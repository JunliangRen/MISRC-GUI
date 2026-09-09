/* Production RTL RF writer with scripted USB admission and worker scheduling.
 * Real libsoxr and RAW/FLAC writers are exercised; no USB device is opened. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../common/threading.h"
#include "../misrc_gui/core/gui_app.h"

static int real_create(thrd_t *thread, int (*function)(void *), void *arg, int priority) {
    return thrd_create_with_priority(thread, function, arg, priority);
}
static int real_join(thrd_t thread, int *result) { return thrd_join(thread, result); }
static void real_sleep(int ms) { thrd_sleep_ms(ms); }

static int test_create(thrd_t *, int (*)(void *), void *, int);
static int test_join(thrd_t, int *);
static FILE *test_open(const char *, const char *);
static int test_close(FILE *);
static size_t test_write(const void *, size_t, size_t, FILE *);
static void test_sleep(int);
#undef thrd_join
#define thrd_join test_join
#define thrd_create_with_priority test_create
#define thrd_set_priority(priority) ((void)(priority))
#define thrd_sleep_ms(ms) test_sleep(ms)
#define fopen test_open
#define fclose test_close
#define fwrite test_write
#include "../misrc_gui/output/gui_rtlsdr_record.c"
#undef fopen
#undef fclose
#undef fwrite

#if LIBFLAC_ENABLED == 1
#include <FLAC/stream_decoder.h>
#endif

static gui_app_t app;
static FILE *output;
static int (*worker)(void *);
static void *worker_arg;
static bool source_ok, fail_open, fail_write, fail_close, fail_thread, disk_full;
static bool real_threads;
static uint32_t source_rate = 2400000, source_center = 1600000;
static int checks, failures, errors, warnings, joins;
static const char *test_name;

static void check(bool condition, const char *message) {
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL [%s]: %s\n", test_name, message);
        failures++;
    }
}

static int test_create(thrd_t *thread, int (*function)(void *), void *arg, int priority) {
    (void)priority;
    if (fail_thread) return -1;
    if (real_threads) return real_create(thread, function, arg, priority);
    worker = function;
    worker_arg = arg;
    *thread = (thrd_t)1;
    return thrd_success;
}
static int test_join(thrd_t thread, int *result) {
    (void)thread;
    joins++;
    if (real_threads) return real_join(thread, result);
    int value = worker(worker_arg);
    if (result) *result = value;
    worker = NULL;
    return thrd_success;
}
static void test_sleep(int ms) { if (real_threads) real_sleep(ms); }
static FILE *test_open(const char *path, const char *mode) {
    (void)path; (void)mode;
    if (fail_open) return NULL;
    output = tmpfile();
    return output;
}
static int test_close(FILE *file) {
    int result = fflush(file);
    return fail_close ? EOF : result;
}
static size_t test_write(const void *data, size_t size, size_t count, FILE *file) {
    if (fail_write) return 0;
    return fwrite(data, size, count, file);
}

bool gui_rtlsdr_get_rf_source(gui_app_t *target, uint32_t *rate, uint32_t *center) {
    check(target == &app, "source lookup uses recording app");
    *rate = source_rate;
    *center = source_center;
    return source_ok;
}
void gui_record_log_capture_event(gui_app_t *target, const char *level, const char *message,
                                  gui_error_class_t error_class, uint32_t count) {
    (void)target; (void)message; (void)error_class; (void)count;
    if (strcmp(level, "ERROR") == 0) errors++;
    if (strcmp(level, "WARN") == 0) warnings++;
}
bool gui_record_check_disk_space_guard(gui_app_t *target, uint32_t frame, char *error, size_t size) {
    (void)target; (void)frame;
    if (disk_full) snprintf(error, size, "fixture disk space guard");
    return disk_full;
}

typedef struct { int16_t *samples; size_t count, capacity; } collected_t;
static bool collect(void *opaque, const int16_t *samples, size_t count) {
    collected_t *result = opaque;
    if (result->count + count > result->capacity) {
        size_t capacity = (result->count + count) * 2;
        int16_t *grown = realloc(result->samples, capacity * sizeof(*samples));
        if (!grown) return false;
        result->samples = grown;
        result->capacity = capacity;
    }
    memcpy(result->samples + result->count, samples, count * sizeof(*samples));
    result->count += count;
    return true;
}

static void reset(const char *name) {
    if (output) fclose(output);
    output = NULL;
    test_name = name;
    memset(&app, 0, sizeof(app));
    app.device_count = 1;
    app.selected_device = 0;
    app.devices[0].type = DEVICE_TYPE_RTLSDR;
    app.settings.rtlsdr_record_mode = 1;
    app.settings.flac_threads = 1;
    app.settings.flac_level = 1;
    app.settings.flac_verification = true;
    source_ok = true;
    source_rate = 2400000;
    source_center = 1600000;
    fail_open = fail_write = fail_close = fail_thread = disk_full = false;
    real_threads = false;
    errors = warnings = joins = 0;
}

static bool start(void) {
    char error[256] = {0};
    bool ok = gui_rtlsdr_record_start(&app, "fixture.s16", error, sizeof(error));
    check(ok, error[0] ? error : "record starts");
    if (ok) gui_rtlsdr_record_begin(&app);
    return ok;
}

static void check_raw(const collected_t *expected) {
    check(output != NULL, "output opened");
    if (!output) return;
    check(fseek(output, 0, SEEK_END) == 0, "output seeks");
    check(ftell(output) == (long)(expected->count * 2), "RAW byte count matches converted prefix");
    rewind(output);
    bool match = true;
    for (size_t i = 0; i < expected->count; i++) {
        int lo = fgetc(output), hi = fgetc(output);
        if (lo < 0 || hi < 0 || (uint16_t)(lo | hi << 8) != (uint16_t)expected->samples[i]) {
            match = false;
            break;
        }
    }
    check(match, "RAW is exact little-endian signed16 DSP output");
}

static uint8_t iq[65536];
static void make_iq(void) {
    for (size_t i = 0; i < sizeof(iq) / 2; i++) {
        double phase = 2.0 * 3.14159265358979323846 * 200000.0 * (double)i / 2400000.0;
        iq[i * 2] = (uint8_t)lround(128.0 + 90.0 * cos(phase));
        iq[i * 2 + 1] = (uint8_t)lround(128.0 + 90.0 * sin(phase));
    }
}

static collected_t reference(const uint8_t *input, size_t bytes, bool flush) {
    collected_t result = {0};
    char error[256] = {0};
    gui_sdr_rf_t *converter = gui_sdr_rf_create(source_rate, source_center, error, sizeof(error));
    check(converter != NULL, "reference converter starts");
    if (converter) {
        check(gui_sdr_rf_process(converter, input, bytes / 2, collect, &result), "reference consumes input");
        if (flush) check(gui_sdr_rf_finish(converter, collect, &result), "reference drains tail");
    }
    gui_sdr_rf_destroy(converter);
    return result;
}

#if LIBFLAC_ENABLED == 1
static collected_t decoded;
static FLAC__StreamDecoderWriteStatus decoded_write(const FLAC__StreamDecoder *decoder,
    const FLAC__Frame *frame, const FLAC__int32 *const buffers[], void *user) {
    (void)decoder; (void)user;
    check(frame->header.channels == 1 && frame->header.bits_per_sample == 16 &&
          frame->header.sample_rate == 8000, "FLAC is mono16 with existing kHz RF header convention");
    int16_t *samples = malloc(frame->header.blocksize * sizeof(*samples));
    if (!samples) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    for (unsigned i = 0; i < frame->header.blocksize; i++) samples[i] = (int16_t)buffers[0][i];
    bool ok = collect(&decoded, samples, frame->header.blocksize);
    free(samples);
    return ok ? FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE : FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
}
static void decoded_error(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *user) {
    (void)decoder; (void)status; (void)user;
    check(false, "FLAC decoding must not report errors");
}
#endif

int main(void) {
    make_iq();
    reset("optional mode validation");
    check(gui_rtlsdr_record_requested(&app), "RTL mode1 requests converted RF");
    app.settings.rtlsdr_record_mode = 0;
    check(!gui_rtlsdr_record_requested(&app), "native remains opt-out");
    app.settings.rtlsdr_record_mode = 1;
    app.devices[0].type = DEVICE_TYPE_SIMULATED;
    check(!gui_rtlsdr_record_requested(&app), "other devices never enter RF path");
    app.devices[0].type = DEVICE_TYPE_RTLSDR;
    char error[256] = {0};
    source_ok = false;
    check(!gui_rtlsdr_record_start(&app, "fixture", error, sizeof(error)), "invalid source cannot open output");
    check(output == NULL, "validation happens before file open");
    source_ok = true;
    source_center = 100000000;
    check(!gui_rtlsdr_record_start(&app, "fixture", error, sizeof(error)), "FM-radio tune is not labeled Hi-Fi RF");
    check(output == NULL, "invalid frequency leaves output untouched");

    reset("normal stop, variable blocks and late callbacks");
    if (start()) {
        gui_rtlsdr_record_push(&app, iq, 2000);
        gui_rtlsdr_record_push(&app, iq + 2000, 23000);
        gui_rtlsdr_record_push(&app, iq + 25000, sizeof(iq) - 25000);
        gui_rtlsdr_record_request_stop(&app);
        gui_rtlsdr_record_push(&app, iq, sizeof(iq));
        uint64_t samples = gui_rtlsdr_record_finish(&app);
        collected_t expected = reference(iq, sizeof(iq), true);
        check(samples == expected.count, "normal stop drains every accepted sample exactly once");
        check(atomic_load(&app.recording_raw_a) == samples * 2 && atomic_load(&app.recording_raw_b) == 0,
              "converted mono updates A bytes only");
        check(joins == 1 && !atomic_load(&app.dropout_stop_requested), "normal stop joins worker without dropout");
        check_raw(&expected);
        free(expected.samples);
        gui_rtlsdr_record_push(&app, iq, sizeof(iq));
        gui_rtlsdr_record_capture_error(&app, "inactive source error");
        check(!atomic_load(&app.dropout_stop_requested), "inactive callbacks cannot poison finished session");
    }

    reset("source discontinuity fails closed");
    if (start()) {
        gui_rtlsdr_record_push(&app, iq, sizeof(iq));
        gui_rtlsdr_record_capture_error(&app, "USB read failed");
        gui_rtlsdr_record_push(&app, iq, sizeof(iq));
        uint64_t samples = gui_rtlsdr_record_finish(&app);
        collected_t expected = reference(iq, sizeof(iq), false);
        check(samples == expected.count, "failure writes prefix without resampler tail");
        check(atomic_load(&app.dropout_stop_requested) &&
              atomic_load(&app.dropout_stop_reason) == GUI_DROPOUT_DEVICE_ERROR,
              "known source gap stops even when generic stop-on-dropout is off");
        check(warnings == 1 && errors == 0, "source diagnostic is not double-counted");
        check_raw(&expected);
        free(expected.samples);
    }

    reset("bounded queue overflow");
    if (start()) {
        for (unsigned i = 0; i < RTL_RF_QUEUE_SLOTS + 1; i++) gui_rtlsdr_record_push(&app, iq, 256);
        check(s_rf.count == RTL_RF_QUEUE_SLOTS, "queue has fixed bounded capacity");
        check(s_rf.accepted_pairs == RTL_RF_QUEUE_SLOTS * 128 && s_rf.discarded_pairs == 128,
              "queue tracks accepted and known discarded I/Q pairs separately");
        gui_rtlsdr_record_finish(&app);
        check(errors == 1 && atomic_load(&app.dropout_stop_reason) == GUI_DROPOUT_BACKPRESSURE,
              "overflow counted once and capture stopped");
    }

    reset("malformed read length");
    if (start()) {
        gui_rtlsdr_record_push(&app, iq, 3);
        check(gui_rtlsdr_record_finish(&app) == 0, "odd I/Q input is rejected, never padded");
        check(errors == 1 && atomic_load(&app.dropout_stop_requested), "malformed read stops export");
    }

    reset("RAW write failure");
    if (start()) {
        gui_rtlsdr_record_push(&app, iq, sizeof(iq));
        fail_write = true;
        gui_rtlsdr_record_finish(&app);
        check(gui_rtlsdr_record_has_write_error(), "write failure persists for finalizing UI");
        check(errors == 1 && atomic_load(&app.dropout_stop_requested), "write failure counted and stops export");
    }

    reset("disk guard");
    if (start()) {
        gui_rtlsdr_record_push(&app, iq, sizeof(iq));
        disk_full = true;
        check(gui_rtlsdr_record_finish(&app) == 0, "disk guard halts before output grows");
        check(atomic_load(&app.dropout_stop_reason) == GUI_DROPOUT_DISK_SPACE, "disk stop reason preserved");
    }

    reset("file finalization failure");
    if (start()) {
        gui_rtlsdr_record_push(&app, iq, sizeof(iq));
        fail_close = true;
        gui_rtlsdr_record_finish(&app);
        check(gui_rtlsdr_record_has_write_error() && errors == 1, "close/flush failure remains visible and counted");
    }

    reset("start failures clean up");
    fail_open = true;
    check(!gui_rtlsdr_record_start(&app, "fixture", error, sizeof(error)), "file open failure reported");
    check(s_rf.app == NULL, "failed open releases session");
    fail_open = false;
    fail_thread = true;
    check(!gui_rtlsdr_record_start(&app, "fixture", error, sizeof(error)), "worker creation failure reported");
    check(s_rf.app == NULL, "failed thread creation releases session");

    reset("restart resets DSP history");
    if (start()) {
        gui_rtlsdr_record_push(&app, iq, sizeof(iq));
        gui_rtlsdr_record_finish(&app);
        collected_t expected = reference(iq, sizeof(iq), true);
        check_raw(&expected);
        free(expected.samples);
    }

#if LIBFLAC_ENABLED == 1
    reset("FLAC round trip");
    app.settings.use_flac = true;
    if (start()) {
        gui_rtlsdr_record_push(&app, iq, sizeof(iq));
        uint64_t samples = gui_rtlsdr_record_finish(&app);
        collected_t expected = reference(iq, sizeof(iq), true);
        check(samples == expected.count && atomic_load(&app.recording_compressed_a) > 0, "FLAC bytes and samples counted");
        rewind(output);
        FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
        check(decoder != NULL, "FLAC decoder allocated");
        if (decoder) {
            check(FLAC__stream_decoder_init_FILE(decoder, output, decoded_write, NULL, decoded_error, NULL) ==
                  FLAC__STREAM_DECODER_INIT_STATUS_OK, "FLAC decoder initializes");
            check(FLAC__stream_decoder_process_until_end_of_stream(decoder), "FLAC decodes entire export");
            check(decoded.count == expected.count && memcmp(decoded.samples, expected.samples, expected.count * 2) == 0,
                  "FLAC preserves full signed16 samples without native 12-bit shift");
            FLAC__stream_decoder_finish(decoder);
            output = NULL; /* init_FILE transfers file ownership to decoder. */
            FLAC__stream_decoder_delete(decoder);
        }
        free(decoded.samples);
        free(expected.samples);
    }
#endif
    reset("real worker queue and stop boundary");
    real_threads = true;
    if (start()) {
        uint8_t input[40000];
        for (unsigned i = 0; i < 20; i++) {
            memcpy(input + i * 2000, iq, 2000);
            gui_rtlsdr_record_push(&app, iq, 2000);
        }
        gui_rtlsdr_record_request_stop(&app);
        gui_rtlsdr_record_push(&app, iq, sizeof(iq));
        uint64_t samples = gui_rtlsdr_record_finish(&app);
        collected_t expected = reference(input, sizeof(input), true);
        check(samples == expected.count && joins == 1, "real worker drains admitted blocks before joining");
        check(!atomic_load(&app.dropout_stop_requested), "concurrent producer/consumer does not introduce loss");
        check_raw(&expected);
        free(expected.samples);
    }
    if (output) fclose(output);
    printf("RTL-SDR RF recording harness: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
