/* A separate bounded producer/consumer path preserves every accepted I/Q pair.
 * Display buffers, mono resampling, channel swaps and 12-bit packing are not
 * involved in the optional 8 MSPS real-RF recording. */
#include "gui_rtlsdr_record.h"
#include "gui_record.h"
#include "../core/gui_app.h"
#include <stdio.h>
#include <string.h>

bool gui_rtlsdr_record_requested(const gui_app_t *app) {
#ifdef ENABLE_RTLSDR
    return app && app->selected_device >= 0 && app->selected_device < app->device_count &&
           app->devices[app->selected_device].type == DEVICE_TYPE_RTLSDR &&
           app->settings.rtlsdr_record_mode == 1;
#else
    (void)app;
    return false;
#endif
}

#if defined(ENABLE_RTLSDR) && LIBSOXR_ENABLED
#include "../input/gui_rtlsdr.h"
#include "../processing/gui_sdr_rf.h"
#include "../../common/flac_writer.h"
#include "../../common/threading.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <inttypes.h>

#define RTL_RF_QUEUE_SLOTS 64u
#define RTL_RF_CHUNK_BYTES 131072u

typedef struct {
    gui_app_t *app;
    uint8_t *queue;
    uint8_t *input;
    uint8_t *raw;
    size_t lengths[RTL_RF_QUEUE_SLOTS];
    unsigned head, count;
    bool accepting, stopping, thread_started;
    atomic_bool failed;
    thrd_t thread;
    gui_sdr_rf_t *converter;
    FILE *file;
    flac_writer_t *flac;
    uint64_t samples, input_pairs, accepted_pairs, discarded_pairs;
} rtl_rf_record_t;

static rtl_rf_record_t s_rf;
/* The lock itself survives every recording, so a late inactive USB callback
 * cannot touch a destroyed mutex or a freed session. No I/O occurs under it. */
static atomic_flag s_rf_lock = ATOMIC_FLAG_INIT;
static atomic_bool s_rf_write_error = ATOMIC_VAR_INIT(false);

static void rtl_rf_lock(void) {
    // USB capture runs at a higher priority than the consumer. Yield on
    // contention so a preempted consumer can release the queue on one CPU.
    while (atomic_flag_test_and_set_explicit(&s_rf_lock, memory_order_acquire)) thrd_sleep_ms(1);
}
static void rtl_rf_unlock(void) {
    atomic_flag_clear_explicit(&s_rf_lock, memory_order_release);
}

static bool rtl_rf_fail_locked(gui_app_t *app, gui_dropout_reason_t reason) {
    if (s_rf.app != app) return false;
    bool first = !atomic_exchange(&s_rf.failed, true);
    s_rf.accepting = false;
    s_rf.stopping = true;
    /* Continuing after a gap would join unrelated phases into plausible RF.
     * This export therefore always fails closed, even with dropout stop off. */
    atomic_store(&app->dropout_stop_reason, reason);
    atomic_store(&app->dropout_stop_requested, true);
    return first;
}

static void rtl_rf_report(gui_app_t *app, const char *message, bool count_error) {
    gui_record_log_capture_event(app, count_error ? "ERROR" : "WARN", message,
                                  count_error ? GUI_ERROR_CLASS_SYSTEM : GUI_ERROR_CLASS_NONE,
                                  count_error ? 1 : 0);
}

static void rtl_rf_fail(gui_app_t *app, const char *message,
                         gui_dropout_reason_t reason, bool count_error) {
    rtl_rf_lock();
    bool first = rtl_rf_fail_locked(app, reason);
    rtl_rf_unlock();
    if (first) rtl_rf_report(app, message, count_error);
}

static bool rtl_rf_emit(void *opaque, const int16_t *samples, size_t count) {
    rtl_rf_record_t *ctx = opaque;
    while (count) {
        size_t n = count > RTL_RF_CHUNK_BYTES / 2 ? RTL_RF_CHUNK_BYTES / 2 : count;
        if (ctx->flac) {
            if (flac_writer_process_int16(ctx->flac, samples, (uint32_t)n) != (int)n) {
                atomic_store(&s_rf_write_error, true);
                return false;
            }
        } else {
            /* RAW is explicitly signed 16-bit little endian on all hosts. */
            for (size_t i = 0; i < n; i++) {
                uint16_t value = (uint16_t)samples[i];
                ctx->raw[i * 2] = (uint8_t)value;
                ctx->raw[i * 2 + 1] = (uint8_t)(value >> 8);
            }
            if (fwrite(ctx->raw, 2, n, ctx->file) != n) {
                atomic_store(&s_rf_write_error, true);
                return false;
            }
        }
        ctx->samples += n;
        atomic_fetch_add(&ctx->app->recording_bytes, n * 2);
        atomic_fetch_add(&ctx->app->recording_raw_a, n * 2);
        samples += n;
        count -= n;
    }
    return true;
}

static void rtl_rf_flac_bytes(void *opaque, size_t bytes) {
    rtl_rf_record_t *ctx = opaque;
    atomic_fetch_add(&ctx->app->recording_compressed_a, bytes);
}

static void rtl_rf_flac_error(void *opaque, flac_writer_error_t error, const char *message) {
    (void)opaque; (void)error; (void)message;
    atomic_store(&s_rf_write_error, true);
}

static int rtl_rf_worker(void *opaque) {
    rtl_rf_record_t *ctx = opaque;
    thrd_set_priority(THRD_PRIORITY_ABOVE);
    if (ctx->flac && flac_writer_apply_thread_affinity(ctx->flac) != FLAC_WRITER_OK) {
        rtl_rf_fail(ctx->app, "RTL-SDR RF: failed to apply FLAC thread affinity",
                    GUI_DROPOUT_DEVICE_ERROR, true);
        return 0;
    }
    for (;;) {
        size_t bytes = 0;
        rtl_rf_lock();
        bool stopping = ctx->stopping;
        if (ctx->count) {
            bytes = ctx->lengths[ctx->head];
            memcpy(ctx->input, ctx->queue + ctx->head * RTL_RF_CHUNK_BYTES, bytes);
            ctx->head = (ctx->head + 1) % RTL_RF_QUEUE_SLOTS;
            ctx->count--;
        }
        rtl_rf_unlock();
        if (!bytes) {
            if (stopping) break;
            thrd_sleep_ms(1);
            continue;
        }
        char disk_error[256] = {0};
        if (gui_record_check_disk_space_guard(ctx->app, 0, disk_error, sizeof(disk_error))) {
            rtl_rf_fail(ctx->app, disk_error, GUI_DROPOUT_DISK_SPACE, false);
            return 0;
        }
        if (!gui_sdr_rf_process(ctx->converter, ctx->input, bytes / 2, rtl_rf_emit, ctx)) {
            char error[320];
            snprintf(error, sizeof(error), "RTL-SDR RF output failed: %s", gui_sdr_rf_error(ctx->converter));
            rtl_rf_fail(ctx->app, error, GUI_DROPOUT_DEVICE_ERROR, true);
            return 0;
        }
        ctx->input_pairs += bytes / 2;
    }
    /* Drain only an intact stream. On known USB loss/queue overflow, write
     * the accepted prefix but do not fabricate a filtered tail over the gap. */
    if (!atomic_load(&ctx->failed) && !gui_sdr_rf_finish(ctx->converter, rtl_rf_emit, ctx)) {
        rtl_rf_fail(ctx->app, "RTL-SDR RF: failed to flush output", GUI_DROPOUT_DEVICE_ERROR, true);
    }
    return 0;
}

bool gui_rtlsdr_record_validate(gui_app_t *app, char *error, size_t error_size) {
    uint32_t rate = 0, center = 0;
    if (!gui_rtlsdr_get_rf_source(app, &rate, &center)) {
        snprintf(error, error_size, "RTL-SDR RF requires a successfully configured live source");
        return false;
    }
    if (app->settings.use_flac && !flac_writer_available()) {
        snprintf(error, error_size, "FLAC support is not available in this build");
        return false;
    }
    if (app->settings.use_flac && app->settings.flac_affinity_enabled &&
        (!flac_writer_affinity_supported() ||
         !flac_writer_validate_affinity_cpu_list(app->settings.flac_affinity_cpu_list, error, error_size))) {
        if (!flac_writer_affinity_supported()) snprintf(error, error_size, "FLAC affinity is only supported on Linux");
        return false;
    }
    return gui_sdr_rf_validate(rate, center, error, error_size);
}

bool gui_rtlsdr_record_start(gui_app_t *app, const char *path, char *error, size_t error_size) {
    uint32_t rate = 0, center = 0;
    if (!gui_rtlsdr_record_validate(app, error, error_size) ||
        !gui_rtlsdr_get_rf_source(app, &rate, &center)) return false;
    rtl_rf_lock();
    if (s_rf.app) {
        rtl_rf_unlock();
        snprintf(error, error_size, "Previous RTL-SDR RF recording is still active");
        return false;
    }
    memset(&s_rf, 0, sizeof(s_rf));
    atomic_init(&s_rf.failed, false);
    atomic_store(&s_rf_write_error, false);
    s_rf.app = app;
    rtl_rf_unlock();
    s_rf.converter = gui_sdr_rf_create(rate, center, error, error_size);
    s_rf.queue = malloc((size_t)RTL_RF_QUEUE_SLOTS * RTL_RF_CHUNK_BYTES);
    s_rf.input = malloc(RTL_RF_CHUNK_BYTES);
    s_rf.raw = malloc(RTL_RF_CHUNK_BYTES);
    if (!s_rf.converter || !s_rf.queue || !s_rf.input || !s_rf.raw) {
        if (s_rf.converter) snprintf(error, error_size, "Not enough memory for RTL-SDR RF recording");
        gui_rtlsdr_record_finish(app);
        return false;
    }
    s_rf.file = fopen(path, "wb");
    if (!s_rf.file) {
        snprintf(error, error_size, "Failed to open RTL-SDR RF output file");
        gui_rtlsdr_record_finish(app);
        return false;
    }
    if (app->settings.use_flac) {
        flac_writer_config_t config = flac_writer_default_config();
        config.sample_rate = GUI_SDR_RF_OUTPUT_RATE_HZ / 1000;
        config.bits_per_sample = 16;
        config.compression_level = app->settings.flac_level;
        config.verify = app->settings.flac_verification;
        config.num_threads = app->settings.flac_threads;
        config.affinity_enabled = app->settings.flac_affinity_enabled;
        snprintf(config.affinity_cpu_list, sizeof(config.affinity_cpu_list), "%s", app->settings.flac_affinity_cpu_list);
        config.error_cb = rtl_rf_flac_error;
        config.bytes_cb = rtl_rf_flac_bytes;
        config.callback_user_data = &s_rf;
        s_rf.flac = flac_writer_create_stream(s_rf.file, &config);
        if (!s_rf.flac) {
            snprintf(error, error_size, "Failed to create RTL-SDR RF FLAC encoder");
            gui_rtlsdr_record_finish(app);
            return false;
        }
    }
    if (thrd_create_with_priority(&s_rf.thread, rtl_rf_worker, &s_rf, THRD_PRIORITY_ABOVE) != thrd_success) {
        snprintf(error, error_size, "Failed to start RTL-SDR RF writer");
        gui_rtlsdr_record_finish(app);
        return false;
    }
    s_rf.thread_started = true;
    return true;
}

void gui_rtlsdr_record_begin(gui_app_t *app) {
    rtl_rf_lock();
    if (s_rf.app == app && !s_rf.stopping) s_rf.accepting = true;
    rtl_rf_unlock();
}

void gui_rtlsdr_record_request_stop(gui_app_t *app) {
    rtl_rf_lock();
    if (s_rf.app == app) {
        s_rf.accepting = false;
        s_rf.stopping = true;
    }
    rtl_rf_unlock();
}

void gui_rtlsdr_record_push(gui_app_t *app, const uint8_t *iq, size_t bytes) {
    rtl_rf_lock();
    if (s_rf.app != app || !s_rf.accepting) {
        rtl_rf_unlock();
        return;
    }
    bool invalid = !iq || bytes == 0 || (bytes & 1) || bytes > RTL_RF_CHUNK_BYTES;
    bool full = s_rf.count == RTL_RF_QUEUE_SLOTS;
    if (invalid || full) {
        /* Close admission under the same lock as the queue, before reporting. */
        if (full && !invalid) s_rf.discarded_pairs += bytes / 2;
        bool first = rtl_rf_fail_locked(app, full ? GUI_DROPOUT_BACKPRESSURE : GUI_DROPOUT_DEVICE_ERROR);
        rtl_rf_unlock();
        char error[256];
        if (full) snprintf(error, sizeof(error), "RTL-SDR RF queue overflow: discarded %zu known I/Q pairs; recording stopped", bytes / 2);
        else snprintf(error, sizeof(error), "RTL-SDR RF: invalid I/Q block (%zu bytes); recording stopped", bytes);
        if (first) rtl_rf_report(app, error, true);
        return;
    }
    unsigned tail = (s_rf.head + s_rf.count) % RTL_RF_QUEUE_SLOTS;
    memcpy(s_rf.queue + tail * RTL_RF_CHUNK_BYTES, iq, bytes);
    s_rf.lengths[tail] = bytes;
    s_rf.count++;
    s_rf.accepted_pairs += bytes / 2;
    rtl_rf_unlock();
}

void gui_rtlsdr_record_capture_error(gui_app_t *app, const char *reason) {
    rtl_rf_lock();
    bool active = s_rf.app == app && s_rf.accepting;
    bool first = active && rtl_rf_fail_locked(app, GUI_DROPOUT_DEVICE_ERROR);
    rtl_rf_unlock();
    if (first) {
        char error[320];
        snprintf(error, sizeof(error), "RTL-SDR RF stopped at source discontinuity: %s", reason ? reason : "unknown source error");
        rtl_rf_report(app, error, false);
    }
}

uint64_t gui_rtlsdr_record_finish(gui_app_t *app) {
    gui_rtlsdr_record_request_stop(app);
    if (s_rf.app != app) return 0;
    if (s_rf.thread_started) thrd_join(s_rf.thread, NULL);
    bool finish_failed = s_rf.flac && flac_writer_finish(s_rf.flac) != FLAC_WRITER_OK;
    if (s_rf.file && fclose(s_rf.file) != 0) finish_failed = true;
    if (finish_failed) {
        atomic_store(&s_rf_write_error, true);
        atomic_store(&s_rf.failed, true);
        gui_record_log_capture_event(app, "ERROR", "RTL-SDR RF output could not be finalized; file may be incomplete",
                                      GUI_ERROR_CLASS_SYSTEM, 1);
    }
    uint64_t samples = s_rf.samples;
    char summary[320];
    snprintf(summary, sizeof(summary), "RTL-SDR RF: accepted=%" PRIu64 " processed=%" PRIu64 " discarded=%" PRIu64 " known I/Q pairs, output=%" PRIu64 " real samples at 8000000 Hz; continuity=%s",
             s_rf.accepted_pairs, s_rf.input_pairs, s_rf.discarded_pairs, samples,
             atomic_load(&s_rf.failed) ? "failed (prefix only)" : "intact");
    gui_record_log_capture_event(app, "INFO", summary, GUI_ERROR_CLASS_NONE, 0);
    gui_sdr_rf_destroy(s_rf.converter);
    free(s_rf.queue);
    free(s_rf.input);
    free(s_rf.raw);
    rtl_rf_lock();
    s_rf.app = NULL;
    rtl_rf_unlock();
    return samples;
}

bool gui_rtlsdr_record_has_write_error(void) {
    return atomic_load(&s_rf_write_error);
}

#else
bool gui_rtlsdr_record_validate(gui_app_t *app, char *error, size_t size) {
    (void)app;
    snprintf(error, size, "8 MSPS Hi-Fi RF requires RTL-SDR and libsoxr support");
    return false;
}
bool gui_rtlsdr_record_start(gui_app_t *app, const char *path, char *error, size_t size) {
    (void)path;
    return gui_rtlsdr_record_validate(app, error, size);
}
void gui_rtlsdr_record_begin(gui_app_t *app) { (void)app; }
void gui_rtlsdr_record_request_stop(gui_app_t *app) { (void)app; }
uint64_t gui_rtlsdr_record_finish(gui_app_t *app) { (void)app; return 0; }
bool gui_rtlsdr_record_has_write_error(void) { return false; }
void gui_rtlsdr_record_push(gui_app_t *app, const uint8_t *iq, size_t bytes) { (void)app; (void)iq; (void)bytes; }
void gui_rtlsdr_record_capture_error(gui_app_t *app, const char *reason) { (void)app; (void)reason; }
#endif
