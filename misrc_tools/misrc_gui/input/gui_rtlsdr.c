/*
 * MISRC GUI - RTL-SDR (RTL2832) Device Support Implementation
 *
 * Local USB via librtlsdr. Mirrors the FX3 backend lifecycle
 * (enumerate/open/start/stop/is_running) and the playback/CXADC capture-feed
 * pattern: pack 8-bit I/Q into the hsdaoh 32-bit capture format and write
 * BUF_CAPTURE_RF, letting the shared extraction thread produce display +
 * record + stats. Demodulation is handled by the separate Demod panel.
 */

#ifdef ENABLE_RTLSDR

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdatomic.h>

// rtl-sdr.h only pulls stdint.h + rtl-sdr_export.h (no libusb.h, no Windows
// header conflict), so it is safe to include before raylib/gui headers.
#include <rtl-sdr.h>

#include "gui_rtlsdr.h"
#include "gui_capture.h"
#include "../core/gui_app.h"
#include "../output/gui_record.h"
#if LIBSOXR_ENABLED
#include "../output/gui_rtlsdr_record.h"
#endif
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"

//-----------------------------------------------------------------------------
// Constants
//-----------------------------------------------------------------------------

// One rtlsdr_read_sync call reads this many bytes (must be a multiple of 512).
// 131072 bytes = 65536 interleaved I/Q pairs = 65536 packed uint32 samples.
#define RTL_READ_BYTES      (131072)
#define RTL_PAIRS_PER_READ  (RTL_READ_BYTES / 2)        // 65536 I/Q pairs
#define RTL_SAMPLES_PER_BATCH (RTL_PAIRS_PER_READ)     // 65536 packed samples
#define RTL_WRITE_BYTES    (RTL_SAMPLES_PER_BATCH * 4) // 262144 bytes (matches playback)

#define RTL_DEFAULT_RATE_HZ 2400000U

//-----------------------------------------------------------------------------
// State
//-----------------------------------------------------------------------------

static rtlsdr_dev_t *s_rtlsdr_dev = NULL;
static atomic_bool s_rtlsdr_running = false;
static void *s_rtlsdr_thread = NULL;
static uint32_t s_rtlsdr_rate_hz;
static uint32_t s_rtlsdr_center_hz;
static bool s_rtlsdr_source_valid;

//-----------------------------------------------------------------------------
// Raw format encoding (mirrors gui_playback.c / extract.c decoding)
//   Bits 0-11:  Channel A (12-bit, stored as 2047 - sample)
//   Bits 12-19: AUX data (8 bits, 0)
//   Bits 20-31: Channel B (12-bit, stored as 2047 - sample)
//-----------------------------------------------------------------------------

static inline uint32_t rtl_encode_raw_sample(int16_t sample_a, int16_t sample_b) {
    if (sample_a > 2047) sample_a = 2047;
    if (sample_a < -2048) sample_a = -2048;
    if (sample_b > 2047) sample_b = 2047;
    if (sample_b < -2048) sample_b = -2048;
    uint32_t ch_a = (uint32_t)((2047 - sample_a) & 0xFFF);
    uint32_t ch_b = (uint32_t)((2047 - sample_b) & 0xFFF);
    uint32_t aux = 0;
    return ch_a | (aux << 12) | (ch_b << 20);
}

//-----------------------------------------------------------------------------
// Enumeration
//-----------------------------------------------------------------------------

int gui_rtlsdr_enumerate(rtlsdr_device_info_t *devices, int max_devices) {
    if (!devices || max_devices <= 0) return 0;

    uint32_t count = rtlsdr_get_device_count();
    int added = 0;
    for (uint32_t i = 0; i < count && added < max_devices; i++) {
        rtlsdr_device_info_t *dst = &devices[added];
        dst->index = (int)i;

        const char *name = rtlsdr_get_device_name(i);
        snprintf(dst->name, sizeof(dst->name), "%s", name ? name : "RTL-SDR");

        dst->serial[0] = '\0';
        char manufact[256] = {0};
        char product[256] = {0};
        char serial[256] = {0};
        if (rtlsdr_get_device_usb_strings(i, manufact, product, serial) == 0) {
            snprintf(dst->serial, sizeof(dst->serial), "%s", serial);
        }
        added++;
    }
    return added;
}

//-----------------------------------------------------------------------------
// Open / Close
//-----------------------------------------------------------------------------

int gui_rtlsdr_open(gui_app_t *app, int device_index) {
    if (!app) return -1;
    s_rtlsdr_source_valid = false;
    s_rtlsdr_rate_hz = s_rtlsdr_center_hz = 0;
    if (s_rtlsdr_dev) {
        rtlsdr_close(s_rtlsdr_dev);
        s_rtlsdr_dev = NULL;
    }

    int r = rtlsdr_open(&s_rtlsdr_dev, (uint32_t)device_index);
    if (r < 0 || !s_rtlsdr_dev) {
        fprintf(stderr, "[RTL-SDR] Failed to open device %d (err %d)\n", device_index, r);
        s_rtlsdr_dev = NULL;
        return -1;
    }

    uint32_t rate = app->settings.rtlsdr_sample_rate_hz;
    if (rate == 0) rate = RTL_DEFAULT_RATE_HZ;
    bool rate_ok = rtlsdr_set_sample_rate(s_rtlsdr_dev, rate) == 0;
    if (!rate_ok) {
        fprintf(stderr, "[RTL-SDR] Warning: set_sample_rate(%u) failed\n", rate);
    }

    // Low-frequency reception may require a board's direct I or Q input.
    // Leave the normal tuner path unchanged unless the user selects otherwise.
    int direct = app->settings.rtlsdr_direct_sampling;
    bool input_ok = direct >= 0 && direct <= 2;
    if (input_ok && direct != 0) {
        input_ok = rtlsdr_set_direct_sampling(s_rtlsdr_dev, direct) == 0;
    }
    input_ok = input_ok && rtlsdr_get_direct_sampling(s_rtlsdr_dev) == direct;
    bool freq_ok = app->settings.rtlsdr_freq_hz <= UINT32_MAX &&
        rtlsdr_set_center_freq(s_rtlsdr_dev, (uint32_t)app->settings.rtlsdr_freq_hz) == 0;
    if (!freq_ok) {
        fprintf(stderr, "[RTL-SDR] Warning: set_center_freq(%llu) failed\n",
                (unsigned long long)app->settings.rtlsdr_freq_hz);
    }
    s_rtlsdr_rate_hz = rtlsdr_get_sample_rate(s_rtlsdr_dev);
    s_rtlsdr_center_hz = rtlsdr_get_center_freq(s_rtlsdr_dev);
    s_rtlsdr_source_valid = rate_ok && freq_ok && input_ok && s_rtlsdr_rate_hz > 0 &&
        s_rtlsdr_center_hz == app->settings.rtlsdr_freq_hz;

    // AGC: rtlsdr_set_agc_mode(1=on,0=off). Gain mode: 0=auto,1=manual.
    rtlsdr_set_agc_mode(s_rtlsdr_dev, app->settings.rtlsdr_agc ? 1 : 0);
    if (app->settings.rtlsdr_gain_mode == 1) {
        rtlsdr_set_tuner_gain_mode(s_rtlsdr_dev, 1);  // manual
        rtlsdr_set_tuner_gain(s_rtlsdr_dev, app->settings.rtlsdr_gain_tenths_db);
    } else {
        rtlsdr_set_tuner_gain_mode(s_rtlsdr_dev, 0);  // auto
    }

    rtlsdr_set_offset_tuning(s_rtlsdr_dev, app->settings.rtlsdr_offset_corr ? 1 : 0);
    if (rtlsdr_reset_buffer(s_rtlsdr_dev) < 0) s_rtlsdr_source_valid = false;

    if (app->settings.rtlsdr_record_mode == 1 && !s_rtlsdr_source_valid) {
        gui_app_set_status(app, "RTL-SDR Hi-Fi RF setup failed; check rate, frequency and input path");
        gui_rtlsdr_close(app);
        return -1;
    }

    fprintf(stderr, "[RTL-SDR] Opened device %d: %u Hz, freq %llu Hz, agc=%d, gain_mode=%d\n",
            device_index, rate, (unsigned long long)app->settings.rtlsdr_freq_hz,
            app->settings.rtlsdr_agc ? 1 : 0, app->settings.rtlsdr_gain_mode);
    return 0;
}

void gui_rtlsdr_close(gui_app_t *app) {
    (void)app;
    s_rtlsdr_source_valid = false;
    s_rtlsdr_rate_hz = s_rtlsdr_center_hz = 0;
    if (s_rtlsdr_dev) {
        rtlsdr_close(s_rtlsdr_dev);
        s_rtlsdr_dev = NULL;
    }
}

//-----------------------------------------------------------------------------
// Capture thread
//-----------------------------------------------------------------------------

static void rtl_report_capture_error(gui_app_t *app, const char *message,
                                     gui_dropout_reason_t reason,
                                     uint64_t event_count) {
    // Count every event, but limit repeated messages on a failing USB link or
    // a full queue. The recording logger also updates the common Errors total.
    if (event_count <= 5 || event_count % 1000 == 0) {
        fprintf(stderr, "[RTL-SDR] %s\n", message);
        gui_record_log_capture_event(app, "ERROR", message, GUI_ERROR_CLASS_SYSTEM, 1);
    } else {
        gui_app_count_system_errors(app, 1);
    }
    gui_capture_request_dropout_stop(app, reason);
#if LIBSOXR_ENABLED
    // A dropped display/native queue block does not break the separate RF
    // recorder, which already accepted the original I/Q block above packing.
    if (reason != GUI_DROPOUT_BACKPRESSURE) gui_rtlsdr_record_capture_error(app, message);
#endif
    if (app->settings.stop_on_dropout) {
        atomic_store(&s_rtlsdr_running, false);
    }
}

static int rtlsdr_capture_thread(void *ctx) {
    gui_app_t *app = (gui_app_t *)ctx;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    uint8_t *in = (uint8_t *)malloc(RTL_READ_BYTES);
    uint32_t *packed = (uint32_t *)malloc(RTL_WRITE_BYTES);
    if (!in || !packed) {
        rtl_report_capture_error(app, "RTL-SDR failed to allocate capture buffers",
                                 GUI_DROPOUT_DEVICE_ERROR, 1);
        // There is no usable capture thread in this case, even if the user
        // allows recoverable dropouts. Let the UI perform the normal cleanup.
        atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_DEVICE_ERROR);
        atomic_store(&app->dropout_stop_requested, true);
        free(in); free(packed);
        atomic_store(&s_rtlsdr_running, false);
        return -1;
    }

    fprintf(stderr, "[RTL-SDR] Capture thread started\n");
    atomic_store(&app->last_callback_time_ms, get_time_ms());

    uint64_t batch_count = 0;
    uint64_t read_errors = 0;
    uint64_t dropped_batches = 0;
    uint64_t dropped_pairs = 0;
    while (atomic_load(&s_rtlsdr_running) && !atomic_load(&do_exit)) {
        int n_read = 0;
        int r = rtlsdr_read_sync(s_rtlsdr_dev, in, RTL_READ_BYTES, &n_read);
        if (!atomic_load(&s_rtlsdr_running) || atomic_load(&do_exit)) break;
        if (r < 0) {
            char message[192];
            atomic_store(&app->stream_synced, false);
            snprintf(message, sizeof(message),
                     "RTL-SDR USB read failed (error %d, read failures=%" PRIu64
                     "; hardware sample loss unknown)", r, ++read_errors);
            rtl_report_capture_error(app, message, GUI_DROPOUT_DEVICE_ERROR, read_errors);
            if (!atomic_load(&s_rtlsdr_running)) break;
            thrd_sleep_ms(10);
            continue;
        }
        if (n_read <= 0) {
            thrd_sleep_ms(1);
            continue;
        }

#if LIBSOXR_ENABLED
        // Record true paired I/Q before display packing, padding or A/B mapping.
        // Malformed reads cannot be repaired by padding a continuous RF file.
        if (n_read > RTL_READ_BYTES || (n_read & 1)) {
            rtl_report_capture_error(app, "RTL-SDR malformed I/Q read",
                                     GUI_DROPOUT_DEVICE_ERROR, ++read_errors);
            if (!atomic_load(&s_rtlsdr_running)) break;
        } else {
            gui_rtlsdr_record_push(app, in, (size_t)n_read);
        }
#endif

        size_t pairs = (size_t)n_read / 2;
        if (pairs > RTL_PAIRS_PER_READ) pairs = RTL_PAIRS_PER_READ;
        atomic_store(&app->stream_synced, true);

        for (size_t i = 0; i < pairs; i++) {
            int8_t i_s = (int8_t)in[i * 2] - 128;      // I: centered to signed
            int8_t q_s = (int8_t)in[i * 2 + 1] - 128;  // Q: centered to signed
            int16_t a16 = (int16_t)((int)i_s << 4);    // shift into 12-bit field
            int16_t b16 = (int16_t)((int)q_s << 4);
            packed[i] = rtl_encode_raw_sample(a16, b16);
        }
        // Zero-pad the remainder of the batch so the write is a fixed size
        // (matches what the extraction thread expects to read).
        for (size_t i = pairs; i < RTL_SAMPLES_PER_BATCH; i++) {
            packed[i] = rtl_encode_raw_sample(0, 0);
        }

        uint8_t *out = bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF,
                                           RTL_WRITE_BYTES, NULL);
        if (out) {
            memcpy(out, packed, RTL_WRITE_BYTES);
            bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, RTL_WRITE_BYTES);
            bufmgr_signal_data(&app->buffers, BUF_CAPTURE_RF);
        } else {
            char message[224];
            atomic_fetch_add(&app->rb_drop_count, 1);
            dropped_batches++;
            dropped_pairs += pairs;
            snprintf(message, sizeof(message),
                     "RTL-SDR RF capture queue full: dropped %zu I/Q pairs "
                     "(software queue totals: %" PRIu64 " blocks, %" PRIu64 " I/Q pairs)",
                     pairs, dropped_batches, dropped_pairs);
            rtl_report_capture_error(app, message, GUI_DROPOUT_BACKPRESSURE, dropped_batches);
        }

        atomic_fetch_add(&app->total_samples, (uint64_t)pairs);
        atomic_fetch_add(&app->samples_a, (uint64_t)pairs);
        atomic_fetch_add(&app->samples_b, (uint64_t)pairs);
        atomic_store(&app->last_callback_time_ms, get_time_ms());
        batch_count++;
    }

    char summary[256];
    snprintf(summary, sizeof(summary),
             "RTL-SDR capture ended: %" PRIu64 " batches, %" PRIu64
             " USB read failures, software RF queue dropped %" PRIu64
             " blocks / %" PRIu64 " I/Q pairs; hardware sample loss unknown",
             batch_count, read_errors, dropped_batches, dropped_pairs);
    fprintf(stderr, "[RTL-SDR] %s\n", summary);
    gui_record_log_capture_event(app, "INFO", summary, GUI_ERROR_CLASS_NONE, 0);
    atomic_store(&s_rtlsdr_running, false);
    atomic_store(&app->stream_synced, false);
    free(in);
    free(packed);
    return 0;
}

//-----------------------------------------------------------------------------
// Start / Stop
//-----------------------------------------------------------------------------

int gui_rtlsdr_start(gui_app_t *app) {
    if (!app) return -1;
    fprintf(stderr, "[RTL-SDR] Starting capture\n");
    if (!s_rtlsdr_dev) {
        fprintf(stderr, "[RTL-SDR] No device open\n");
        return -1;
    }

    bufmgr_reset_stats(&app->buffers, BUF_COUNT);

    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->parser_error_count, 0);
    atomic_store(&app->system_error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    atomic_store(&app->dropout_stop_requested, false);
    atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);

    uint32_t rate = s_rtlsdr_rate_hz ? s_rtlsdr_rate_hz : app->settings.rtlsdr_sample_rate_hz;
    if (rate == 0) rate = RTL_DEFAULT_RATE_HZ;
    atomic_store(&app->sample_rate, rate);

    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) != 0) {
        fprintf(stderr, "[RTL-SDR] Failed to initialize capture ringbuffer\n");
        gui_app_set_status(app, "Failed to initialize capture buffer");
        return -1;
    }

    atomic_store(&s_rtlsdr_running, true);
    app->is_capturing = true;

    // Extraction thread: reads BUF_CAPTURE_RF -> BUF_DISPLAY + BUF_RECORD_* + stats.
    int r = gui_extract_start(app);
    if (r < 0) {
        fprintf(stderr, "[RTL-SDR] Failed to start extraction thread\n");
        gui_app_set_status(app, "Failed to start extraction");
        atomic_store(&s_rtlsdr_running, false);
        app->is_capturing = false;
        return -1;
    }

    // Display thread: processes BUF_DISPLAY for oscilloscope/FFT/waterfall/demod.
    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[RTL-SDR] Failed to start display thread (non-fatal)\n");
        }
    }

    thrd_t thread;
    if (thrd_create_with_priority(&thread,
                                   rtlsdr_capture_thread,
                                   app,
                                   THRD_PRIORITY_CRITICAL) != thrd_success) {
        fprintf(stderr, "[RTL-SDR] Failed to create capture thread\n");
        gui_extract_stop();
        if (app->display_thread) gui_display_thread_stop(app->display_thread);
        atomic_store(&s_rtlsdr_running, false);
        app->is_capturing = false;
        return -1;
    }
    s_rtlsdr_thread = (void *)(uintptr_t)thread;

    gui_app_set_status(app, "RTL-SDR capture running");
    return 0;
}

void gui_rtlsdr_stop(gui_app_t *app) {
    // A failed worker may already have cleared running. Its join/close and
    // extraction cleanup must still happen on the UI thread.
    if (!app || (!s_rtlsdr_thread && !s_rtlsdr_dev)) return;
    fprintf(stderr, "[RTL-SDR] Stopping capture\n");

    app->is_capturing = false;
    atomic_store(&s_rtlsdr_running, false);

    if (s_rtlsdr_thread) {
        thrd_t thread = (thrd_t)(uintptr_t)s_rtlsdr_thread;
        thrd_join(thread, NULL);
        s_rtlsdr_thread = NULL;
    }

    if (app->display_thread) gui_display_thread_stop(app->display_thread);
    gui_extract_stop();

    gui_rtlsdr_close(app);
    atomic_store(&app->stream_synced, false);
    gui_app_set_status(app, "RTL-SDR capture stopped");
}

bool gui_rtlsdr_is_running(gui_app_t *app) {
    (void)app;
    return atomic_load(&s_rtlsdr_running);
}

int gui_rtlsdr_set_frequency(gui_app_t *app, uint64_t hz) {
    if (!app) return -1;
    if (hz == 0 || hz > UINT32_MAX) return -1;
    // A frequency change would corrupt the mapping of the continuous RF export.
    if (app->settings.rtlsdr_record_mode == 1 &&
        (app->is_recording || gui_record_is_finalizing())) return -1;
    // Persist so the next capture start uses the new frequency even if not live.
    app->settings.rtlsdr_freq_hz = hz;
    gui_settings_save(&app->settings);
    // Live retune only if the device is open and capture is running.
    if (s_rtlsdr_dev && atomic_load(&s_rtlsdr_running)) {
        int r = rtlsdr_set_center_freq(s_rtlsdr_dev, (uint32_t)hz);
        if (r < 0) {
            s_rtlsdr_source_valid = false;
            fprintf(stderr, "[RTL-SDR] live retune to %llu Hz failed (err %d)\n",
                    (unsigned long long)hz, r);
            return -1;
        }
        s_rtlsdr_center_hz = rtlsdr_get_center_freq(s_rtlsdr_dev);
        if (s_rtlsdr_center_hz != hz) s_rtlsdr_source_valid = false;
        fprintf(stderr, "[RTL-SDR] live retune to %llu Hz\n", (unsigned long long)hz);
    }
    return 0;
}

bool gui_rtlsdr_get_rf_source(gui_app_t *app, uint32_t *rate_hz, uint32_t *center_hz) {
    if (!app || !rate_hz || !center_hz || !s_rtlsdr_dev || !s_rtlsdr_source_valid ||
        !atomic_load(&s_rtlsdr_running)) return false;
    *rate_hz = s_rtlsdr_rate_hz;
    *center_hz = s_rtlsdr_center_hz;
    return true;
}

#endif // ENABLE_RTLSDR
