/*
 * CPU-only checks of the production RTL-SDR capture loop and stop lifecycle.
 * Scripted USB reads and queues do not open hardware or start real threads.
 * LTO discards unrelated enumeration functions from the include.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../misrc_gui/input/gui_capture.h"
#include "../common/threading.h"

static int test_join(thrd_t thread, int *result);
static int test_create(thrd_t *thread, int (*func)(void *), void *arg, int priority);
static void *test_malloc(size_t bytes);
#undef thrd_join
#define thrd_join test_join
#define thrd_create_with_priority test_create
#define thrd_set_priority(priority) ((void)(priority))
#define thrd_sleep_ms(ms) ((void)(ms))
#define get_time_ms() UINT64_C(1234)
#define malloc test_malloc

#include "../misrc_gui/input/gui_rtlsdr.c"
#undef malloc

volatile atomic_int do_exit = 0;
static gui_app_t test_app;
static int checks, failures;
static const char *test_case;
static int error_logs, summary_logs, write_count, signal_count;
static int joins, closes, extract_stops, display_stops;
static int allocation_count, fail_allocation;
static char last_error[256], last_summary[256];
static uint8_t queue_storage[RTL_WRITE_BYTES];
static int rate_result, frequency_result, direct_result, reset_result;
static int configured_direct, direct_calls, frequency_calls, settings_saves;
static uint32_t configured_rate, configured_frequency;
static bool override_rate, override_frequency, override_direct, record_finalizing;
static uint32_t returned_rate, returned_frequency;
static int returned_direct;
static char last_status[160];
#if LIBSOXR_ENABLED
static int rf_pushes, rf_capture_errors;
static size_t rf_bytes, rf_last_bytes;
static bool rf_bytes_match;
static char last_rf_error[192];
#endif

typedef struct {
    int result;
    int bytes;
    bool queue_full;
    int shutdown; // 1: capture stop, 2: application exit while reading
} read_step_t;

static const read_step_t *read_steps;
static size_t read_step_count, read_index;
static bool queue_full;

static uint8_t input_byte(size_t index)
{
    // Distinct I/Q values also detect channel swapping or sign conversion.
    return (uint8_t)(index * 7 + 19);
}

static void *test_malloc(size_t bytes)
{
    allocation_count++;
    return allocation_count == fail_allocation ? NULL : malloc(bytes);
}

static void expect_true(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL [%s]: %s\n", test_case, message);
        failures++;
    }
}

// Match the real logger's counter semantics even when no recording log is open.
void gui_record_log_capture_event(gui_app_t *app, const char *level, const char *message,
                                  gui_error_class_t error_class, uint32_t error_count)
{
    if (strcmp(level, "ERROR") == 0 || strcmp(level, "CRITICAL") == 0) {
        uint32_t increment = error_count > 0 ? error_count : 1;
        if (error_class == GUI_ERROR_CLASS_SYSTEM) gui_app_count_system_errors(app, increment);
        if (error_class == GUI_ERROR_CLASS_PARSER) gui_app_count_parser_errors(app, increment);
        error_logs++;
        snprintf(last_error, sizeof(last_error), "%s", message);
    } else {
        summary_logs++;
        snprintf(last_summary, sizeof(last_summary), "%s", message);
    }
}

void gui_capture_request_dropout_stop(gui_app_t *app, gui_dropout_reason_t reason)
{
    if (!app || !app->settings.stop_on_dropout) return;
    atomic_store(&app->dropout_stop_reason, reason);
    atomic_store(&app->dropout_stop_requested, true);
}

int rtlsdr_read_sync(rtlsdr_dev_t *dev, void *buf, int len, int *n_read)
{
    expect_true(dev == s_rtlsdr_dev && len == RTL_READ_BYTES, "read uses the configured device and block size");
    if (read_index == read_step_count) {
        atomic_store(&s_rtlsdr_running, false);
        *n_read = 0;
        return -99; // Normal test termination must not be reported as a USB error.
    }
    read_step_t step = read_steps[read_index++];
    *n_read = step.bytes;
    queue_full = step.queue_full;
    for (int i = 0; i < step.bytes && i < len; i++) {
        ((uint8_t *)buf)[i] = input_byte((size_t)i);
    }
    if (step.shutdown == 1) atomic_store(&s_rtlsdr_running, false);
    if (step.shutdown == 2) atomic_store(&do_exit, 1);
    return step.result;
}

#if LIBSOXR_ENABLED
void gui_rtlsdr_record_push(gui_app_t *app, const uint8_t *iq, size_t bytes)
{
    rf_pushes++;
    rf_bytes += bytes;
    rf_last_bytes = bytes;
    rf_bytes_match = rf_bytes_match && app == &test_app && bytes <= RTL_READ_BYTES && !(bytes & 1);
    for (size_t i = 0; i < bytes && i < RTL_READ_BYTES; i++) {
        if (iq[i] != input_byte(i)) rf_bytes_match = false;
    }
}

void gui_rtlsdr_record_capture_error(gui_app_t *app, const char *reason)
{
    expect_true(app == &test_app, "RF recording errors retain the capture app");
    rf_capture_errors++;
    snprintf(last_rf_error, sizeof(last_rf_error), "%s", reason);
}
#endif

int rtlsdr_open(rtlsdr_dev_t **dev, uint32_t index)
{
    expect_true(index == 2, "open passes the selected USB device index");
    *dev = (rtlsdr_dev_t *)(uintptr_t)1;
    return 0;
}

int rtlsdr_set_sample_rate(rtlsdr_dev_t *dev, uint32_t rate)
{
    (void)dev;
    if (!rate_result) configured_rate = rate;
    return rate_result;
}

uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t *dev)
{
    (void)dev;
    return override_rate ? returned_rate : configured_rate;
}

int rtlsdr_set_center_freq(rtlsdr_dev_t *dev, uint32_t frequency)
{
    (void)dev;
    frequency_calls++;
    if (!frequency_result) configured_frequency = frequency;
    return frequency_result;
}

uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t *dev)
{
    (void)dev;
    return override_frequency ? returned_frequency : configured_frequency;
}

int rtlsdr_set_direct_sampling(rtlsdr_dev_t *dev, int input)
{
    (void)dev;
    direct_calls++;
    if (!direct_result) configured_direct = input;
    return direct_result;
}

int rtlsdr_get_direct_sampling(rtlsdr_dev_t *dev)
{
    (void)dev;
    return override_direct ? returned_direct : configured_direct;
}

int rtlsdr_reset_buffer(rtlsdr_dev_t *dev) { (void)dev; return reset_result; }
int rtlsdr_set_agc_mode(rtlsdr_dev_t *dev, int on) { (void)dev; (void)on; return 0; }
int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *dev, int manual) { (void)dev; (void)manual; return 0; }
int rtlsdr_set_tuner_gain(rtlsdr_dev_t *dev, int gain) { (void)dev; (void)gain; return 0; }
int rtlsdr_set_offset_tuning(rtlsdr_dev_t *dev, int on) { (void)dev; (void)on; return 0; }
bool gui_record_is_finalizing(void) { return record_finalizing; }
void gui_settings_save(const gui_settings_t *settings)
{
    expect_true(settings == &test_app.settings, "frequency changes save only the current settings");
    settings_saves++;
}

void *bufmgr_write_begin(buffer_manager_t *mgr, buffer_id_t id,
                         size_t bytes, const backpressure_policy_t *policy)
{
    expect_true(mgr == &test_app.buffers && id == BUF_CAPTURE_RF &&
                bytes == RTL_WRITE_BYTES && policy == NULL, "capture retains the existing fixed-size queue contract");
    expect_true(atomic_load(&test_app.stream_synced), "valid reads restore stream synchronization");
    return queue_full ? NULL : queue_storage;
}

void bufmgr_write_end(buffer_manager_t *mgr, buffer_id_t id, size_t bytes)
{
    (void)mgr; (void)id; (void)bytes;
    write_count++;
}

void bufmgr_signal_data(buffer_manager_t *mgr, buffer_id_t id)
{
    (void)mgr; (void)id;
    signal_count++;
}

void bufmgr_reset_stats(buffer_manager_t *mgr, buffer_id_t id) { (void)mgr; (void)id; }
int bufmgr_ensure_init(buffer_manager_t *mgr, buffer_id_t id) { (void)mgr; (void)id; return 0; }
int gui_extract_start(gui_app_t *app) { (void)app; return 0; }
void gui_extract_stop(void) { extract_stops++; }
int gui_display_thread_start(display_thread_t *dt, gui_app_t *app, buffer_manager_t *mgr)
{
    (void)dt; (void)app; (void)mgr;
    return 0;
}
void gui_display_thread_stop(display_thread_t *dt) { (void)dt; display_stops++; }
void gui_app_set_status(gui_app_t *app, const char *message)
{
    (void)app;
    snprintf(last_status, sizeof(last_status), "%s", message);
}
int rtlsdr_close(rtlsdr_dev_t *dev) { (void)dev; closes++; return 0; }

static int test_create(thrd_t *thread, int (*func)(void *), void *arg, int priority)
{
    expect_true(func == rtlsdr_capture_thread && arg == &test_app &&
                priority == THRD_PRIORITY_CRITICAL, "start wires the real capture worker");
    *thread = (thrd_t)(uintptr_t)1;
    return thrd_success;
}

static int test_join(thrd_t thread, int *result)
{
    (void)result;
    expect_true((uintptr_t)thread == 1, "stop joins the retained worker handle");
    joins++;
    return thrd_success;
}

static void reset_test(const char *name, bool stop_on_dropout)
{
    test_case = name;
    memset(&test_app, 0, sizeof(test_app));
    test_app.settings.stop_on_dropout = stop_on_dropout;
    test_app.display_thread = (display_thread_t *)(uintptr_t)1;
    atomic_store(&do_exit, 0);
    s_rtlsdr_dev = NULL;
    s_rtlsdr_thread = NULL;
    s_rtlsdr_source_valid = false;
    s_rtlsdr_rate_hz = s_rtlsdr_center_hz = 0;
    atomic_store(&s_rtlsdr_running, false);
    error_logs = summary_logs = write_count = signal_count = 0;
    joins = closes = extract_stops = display_stops = 0;
    allocation_count = fail_allocation = 0;
    last_error[0] = last_summary[0] = '\0';
    last_status[0] = '\0';
    rate_result = frequency_result = direct_result = reset_result = 0;
    configured_direct = direct_calls = frequency_calls = settings_saves = 0;
    configured_rate = configured_frequency = 0;
    override_rate = override_frequency = override_direct = record_finalizing = false;
    returned_rate = returned_frequency = 0;
    returned_direct = 0;
    test_app.settings.rtlsdr_sample_rate_hz = RTL_DEFAULT_RATE_HZ;
    test_app.settings.rtlsdr_freq_hz = 1600000;
#if LIBSOXR_ENABLED
    rf_pushes = rf_capture_errors = 0;
    rf_bytes = rf_last_bytes = 0;
    rf_bytes_match = true;
    last_rf_error[0] = '\0';
#endif
}

static void run_capture(const read_step_t *steps, size_t count)
{
    read_steps = steps;
    read_step_count = count;
    read_index = 0;
    s_rtlsdr_dev = (rtlsdr_dev_t *)(uintptr_t)1;
    expect_true(gui_rtlsdr_start(&test_app) == 0, "capture starts with stubbed resources");
    expect_true(atomic_load(&test_app.error_count) == 0 &&
                atomic_load(&test_app.rb_drop_count) == 0 &&
                atomic_load(&test_app.total_samples) == 0 &&
                !atomic_load(&test_app.dropout_stop_requested), "start resets shared diagnostics");
    expect_true(rtlsdr_capture_thread(&test_app) == 0, "scripted capture exits normally");
    expect_true(!gui_rtlsdr_is_running(&test_app) && !atomic_load(&test_app.stream_synced),
                "worker clears running and sync on exit");
}

static void expect_errors(unsigned int count)
{
    expect_true(atomic_load(&test_app.error_count) == count, "Errors counts each event once");
    expect_true(atomic_load(&test_app.system_error_count) == count, "events are system errors");
    expect_true(atomic_load(&test_app.parser_error_count) == 0 &&
                atomic_load(&test_app.missed_frame_count) == 0, "no invented parser errors or missed hardware frames");
}

static void check_stop(void)
{
    gui_rtlsdr_stop(&test_app);
    expect_true(joins == 1 && closes == 1 && extract_stops == 1 && display_stops == 1,
                "stopped worker still gets joined, closed, and extraction/display cleanup");
    expect_true(!test_app.is_capturing && s_rtlsdr_dev == NULL && s_rtlsdr_thread == NULL,
                "stop clears capture resources");
    gui_rtlsdr_stop(&test_app);
    expect_true(joins == 1 && closes == 1 && extract_stops == 1 && display_stops == 1,
                "repeated stop does not repeat cleanup");
}

static void check_rf_source_configuration(void)
{
    uint32_t rate = 111, center = 222;
    reset_test("RF source lifecycle", false);
    test_app.settings.rtlsdr_record_mode = 1;
    test_app.settings.rtlsdr_direct_sampling = 2;
    override_rate = true;
    returned_rate = 2399999; // The converter must use the driver's configured value.
    expect_true(!gui_rtlsdr_get_rf_source(&test_app, &rate, &center) && rate == 111 && center == 222,
                "closed source does not return invented configuration");
    expect_true(gui_rtlsdr_open(&test_app, 2) == 0 && direct_calls == 1 && configured_direct == 2,
                "RF capture can explicitly configure direct Q input");
    expect_true(!gui_rtlsdr_get_rf_source(&test_app, &rate, &center),
                "opened but not running device cannot start a continuous RF export");
    atomic_store(&s_rtlsdr_running, true);
    expect_true(gui_rtlsdr_get_rf_source(&test_app, &rate, &center) && rate == returned_rate && center == 1600000,
                "running source reports driver rate and verified center frequency");
    expect_true(!gui_rtlsdr_get_rf_source(NULL, &rate, &center) &&
                !gui_rtlsdr_get_rf_source(&test_app, NULL, &center) &&
                !gui_rtlsdr_get_rf_source(&test_app, &rate, NULL), "RF source query rejects missing arguments");
    gui_rtlsdr_close(&test_app);
    expect_true(!gui_rtlsdr_get_rf_source(&test_app, &rate, &center) && closes == 1 &&
                !s_rtlsdr_source_valid && !s_rtlsdr_rate_hz && !s_rtlsdr_center_hz,
                "close clears cached source configuration even before running flag changes");

    const char *failure_names[] = {
        "sample-rate setter failure", "frequency setter failure", "direct-input setter failure",
        "direct-input readback mismatch", "USB buffer reset failure", "zero source rate",
        "frequency readback mismatch", "invalid direct-input setting"
    };
    for (int mode = 0; mode <= 1; mode++) {
        for (int failure = 0; failure < 8; failure++) {
            reset_test(failure_names[failure], false);
            test_app.settings.rtlsdr_record_mode = mode;
            switch (failure) {
            case 0: rate_result = -1; break;
            case 1: frequency_result = -1; break;
            case 2:
                test_app.settings.rtlsdr_direct_sampling = 2;
                direct_result = -1;
                break;
            case 3:
                override_direct = true;
                returned_direct = 1;
                break;
            case 4: reset_result = -1; break;
            case 5: override_rate = true; returned_rate = 0; break;
            case 6: override_frequency = true; returned_frequency = 100000000; break;
            case 7: test_app.settings.rtlsdr_direct_sampling = 3; break;
            }
            int result = gui_rtlsdr_open(&test_app, 2);
            expect_true(result == (mode ? -1 : 0),
                        "RF setup fails closed while native capture preserves warning-only opens");
            atomic_store(&s_rtlsdr_running, true);
            expect_true(!gui_rtlsdr_get_rf_source(&test_app, &rate, &center),
                        "failed or inconsistent hardware configuration cannot feed RF conversion");
            if (mode) {
                expect_true(s_rtlsdr_dev == NULL && closes == 1 && strstr(last_status, "setup failed"),
                            "RF setup failure closes the device and explains the rejected configuration");
            } else {
                expect_true(s_rtlsdr_dev != NULL && closes == 0,
                            "native capture is not made dependent on RF source verification");
            }
            gui_rtlsdr_close(&test_app);
        }
    }

    reset_test("native defaults", false);
    test_app.settings.rtlsdr_sample_rate_hz = 0;
    expect_true(gui_rtlsdr_open(&test_app, 2) == 0 && configured_rate == RTL_DEFAULT_RATE_HZ &&
                direct_calls == 0, "native defaults retain the existing rate and tuner path");
    gui_rtlsdr_close(&test_app);
}

static void check_rf_retune_guard(void)
{
    uint32_t rate, center;
    reset_test("RF live retune guard", false);
    test_app.settings.rtlsdr_record_mode = 1;
    expect_true(gui_rtlsdr_open(&test_app, 2) == 0, "retune fixture opens a verified source");
    atomic_store(&s_rtlsdr_running, true);
    int open_frequency_calls = frequency_calls;
    test_app.is_recording = true;
    expect_true(gui_rtlsdr_set_frequency(&test_app, 1700000) == -1 &&
                test_app.settings.rtlsdr_freq_hz == 1600000 && frequency_calls == open_frequency_calls &&
                settings_saves == 0, "active RF recording refuses tuning before settings or hardware mutation");
    test_app.is_recording = false;
    record_finalizing = true;
    expect_true(gui_rtlsdr_set_frequency(&test_app, 1700000) == -1 &&
                test_app.settings.rtlsdr_freq_hz == 1600000 && frequency_calls == open_frequency_calls &&
                settings_saves == 0, "RF finalization retains the source tuning lock");
    record_finalizing = false;
    expect_true(gui_rtlsdr_set_frequency(&test_app, 1700000) == 0 && settings_saves == 1 &&
                gui_rtlsdr_get_rf_source(&test_app, &rate, &center) && center == 1700000,
                "idle RF mode permits verified retuning and updates the next RF export source");
    expect_true(gui_rtlsdr_set_frequency(&test_app, 0) == -1 &&
                gui_rtlsdr_set_frequency(&test_app, UINT64_C(4294967296)) == -1 && settings_saves == 1,
                "invalid frequencies cannot wrap or be persisted");
    frequency_result = -1;
    expect_true(gui_rtlsdr_set_frequency(&test_app, 1800000) == -1 &&
                !gui_rtlsdr_get_rf_source(&test_app, &rate, &center),
                "failed live retune invalidates the RF source");
    gui_rtlsdr_close(&test_app);

    reset_test("RF retune readback mismatch", false);
    test_app.settings.rtlsdr_record_mode = 1;
    expect_true(gui_rtlsdr_open(&test_app, 2) == 0, "retune mismatch fixture opens a verified source");
    atomic_store(&s_rtlsdr_running, true);
    override_frequency = true;
    returned_frequency = 100000000;
    gui_rtlsdr_set_frequency(&test_app, 1700000);
    expect_true(!gui_rtlsdr_get_rf_source(&test_app, &rate, &center),
                "driver readback mismatch prevents exporting with an assumed RF center");
    gui_rtlsdr_close(&test_app);

    reset_test("native live tuning retained", false);
    expect_true(gui_rtlsdr_open(&test_app, 2) == 0, "native retune fixture opens");
    atomic_store(&s_rtlsdr_running, true);
    test_app.is_recording = true;
    record_finalizing = true;
    expect_true(gui_rtlsdr_set_frequency(&test_app, 1700000) == 0 && configured_frequency == 1700000 &&
                settings_saves == 1, "RF-specific tuning lock does not change native I/Q recording controls");
    gui_rtlsdr_close(&test_app);
}

#if LIBSOXR_ENABLED
static void check_rf_input_delivery(void)
{
    const read_step_t shorts[] = {{0, 10, false, 0}, {0, 6, false, 0}};
    reset_test("RF exact paired short reads", false);
    test_app.settings.rtlsdr_record_mode = 1;
    run_capture(shorts, 2);
    expect_true(rf_pushes == 2 && rf_bytes == 16 && rf_last_bytes == 6 && rf_bytes_match,
                "RF writer receives the exact unsigned paired I/Q bytes with no padding or channel conversion");
    expect_true(rf_capture_errors == 0 && write_count == 2,
                "valid short RF reads retain the separate fixed-block display path");
    check_stop();

    const read_step_t malformed[] = {{0, 3, false, 0}, {0, RTL_READ_BYTES + 2, false, 0}, {0, 8, false, 0}};
    reset_test("RF malformed read rejection", false);
    test_app.settings.rtlsdr_record_mode = 1;
    run_capture(malformed, 3);
    expect_errors(2);
    expect_true(rf_capture_errors == 2 && strstr(last_rf_error, "malformed I/Q read") &&
                rf_pushes == 1 && rf_bytes == 8 && rf_bytes_match,
                "odd or oversized USB reads are reported, never repaired or sent to the RF converter");
    expect_true(error_logs == 2 && strstr(last_error, "malformed I/Q read"),
                "malformed USB data is visible in common diagnostics as well as the RF writer");
    check_stop();

    reset_test("RF malformed read stop gate", true);
    test_app.settings.rtlsdr_record_mode = 1;
    run_capture(malformed, 3);
    expect_errors(1);
    expect_true(read_index == 1 && rf_capture_errors == 1 && rf_pushes == 0 && write_count == 0 &&
                atomic_load(&test_app.dropout_stop_requested) &&
                atomic_load(&test_app.dropout_stop_reason) == GUI_DROPOUT_DEVICE_ERROR,
                "malformed data obeys the common dropout gate before another display or RF block is published");
    check_stop();

    const read_step_t usb_error[] = {{-7, 0, false, 0}, {0, 8, false, 0}};
    reset_test("RF USB discontinuity notification", false);
    test_app.settings.rtlsdr_record_mode = 1;
    run_capture(usb_error, 2);
    expect_true(rf_capture_errors == 1 && strstr(last_rf_error, "USB read failed") && rf_pushes == 1,
                "RF writer learns of USB discontinuities even when generic stop-on-dropout is disabled");
    check_stop();

    const read_step_t queue_error[] = {{0, 8, true, 0}};
    for (int mode = 0; mode <= 1; mode++) {
        for (int stop = 0; stop <= 1; stop++) {
            reset_test("display queue loss is not RF source loss", stop != 0);
            test_app.settings.rtlsdr_record_mode = mode;
            run_capture(queue_error, 1);
            expect_errors(1);
            expect_true(rf_pushes == 1 && rf_bytes == 8 && rf_bytes_match && rf_capture_errors == 0,
                        "full display/native queue does not falsely invalidate the already accepted raw RF block");
            expect_true(write_count == 0 && atomic_load(&test_app.rb_drop_count) == 1 &&
                        strstr(last_error, "capture queue full"),
                        "separate display/native queue loss remains counted and reported");
            expect_true(atomic_load(&test_app.dropout_stop_requested) == (stop != 0) &&
                        atomic_load(&test_app.dropout_stop_reason) ==
                            (stop ? GUI_DROPOUT_BACKPRESSURE : GUI_DROPOUT_NONE),
                        "both native and RF modes retain the selected common backpressure stop policy");
            check_stop();
        }
    }
}
#endif

int main(void)
{
    const read_step_t recover[] = {{-7, 0, false, 0}, {0, RTL_READ_BYTES, false, 0}, {0, 10, false, 0}};
    reset_test("USB recovery", false);
    run_capture(recover, sizeof(recover) / sizeof(recover[0]));
    expect_errors(1);
    expect_true(error_logs == 1 && strstr(last_error, "hardware sample loss unknown"), "USB error does not invent a sample-loss count");
    expect_true(write_count == 2 && signal_count == 2 && read_index == 3, "disabled stop gate permits valid data after a USB error");
    expect_true(atomic_load(&test_app.total_samples) == RTL_PAIRS_PER_READ + 5 &&
                atomic_load(&test_app.samples_a) == RTL_PAIRS_PER_READ + 5 &&
                atomic_load(&test_app.samples_b) == RTL_PAIRS_PER_READ + 5, "counts real I/Q pairs, not short-read padding");
    expect_true(!atomic_load(&test_app.dropout_stop_requested), "disabled stop gate leaves capture usable");
    expect_true(summary_logs == 1 && strstr(last_summary, "2 batches, 1 USB read failures") &&
                strstr(last_summary, "0 blocks / 0 I/Q pairs"), "summary reports USB and software queue totals separately");
    check_stop();

    // Reuse app state to check both shared counters and per-worker summary reset.
    run_capture(NULL, 0);
    expect_errors(0);
    expect_true(strstr(last_summary, "0 batches, 0 USB read failures") &&
                strstr(last_summary, "0 blocks / 0 I/Q pairs"), "next capture does not inherit previous summary totals");
    gui_rtlsdr_stop(&test_app);

    reset_test("USB stop gate", true);
    run_capture(recover, sizeof(recover) / sizeof(recover[0]));
    expect_errors(1);
    expect_true(read_index == 1 && write_count == 0 && atomic_load(&test_app.dropout_stop_requested) &&
                atomic_load(&test_app.dropout_stop_reason) == GUI_DROPOUT_DEVICE_ERROR,
                "USB failure requests device-error stop and does not read another block");
    check_stop();

    const read_step_t queue[] = {{0, 8, true, 0}, {0, 6, true, 0}, {0, 10, false, 0}};
    reset_test("queue recovery", false);
    run_capture(queue, sizeof(queue) / sizeof(queue[0]));
    expect_errors(2);
    expect_true(atomic_load(&test_app.rb_drop_count) == 2 && write_count == 1 && read_index == 3,
                "queue drops are counted and later blocks still arrive");
    expect_true(strstr(last_error, "3 I/Q pairs") && strstr(last_summary, "2 blocks / 7 I/Q pairs"),
                "software loss counts only the actual pairs in dropped short reads");
    expect_true(strstr(last_summary, "hardware sample loss unknown") &&
                atomic_load(&test_app.total_samples) == 12, "received samples stay separate from known software loss");
    check_stop();

    reset_test("queue stop gate", true);
    run_capture(queue, sizeof(queue) / sizeof(queue[0]));
    expect_errors(1);
    expect_true(read_index == 1 && atomic_load(&test_app.rb_drop_count) == 1 &&
                atomic_load(&test_app.dropout_stop_requested) &&
                atomic_load(&test_app.dropout_stop_reason) == GUI_DROPOUT_BACKPRESSURE,
                "queue failure requests backpressure stop without inventing device failure");
    check_stop();

    reset_test("log throttling", false);
    atomic_store(&s_rtlsdr_running, true);
    for (uint64_t i = 1; i <= 1001; i++) {
        rtl_report_capture_error(&test_app, "scripted repeated error", GUI_DROPOUT_DEVICE_ERROR, i);
    }
    expect_errors(1001);
    expect_true(error_logs == 6 && !atomic_load(&test_app.dropout_stop_requested) &&
                atomic_load(&s_rtlsdr_running), "suppressed events 6..999 and 1001 still count without stopping");

    for (int shutdown = 1; shutdown <= 2; shutdown++) {
        read_step_t during_stop[] = {{-4, 0, false, shutdown}};
        reset_test(shutdown == 1 ? "capture shutdown" : "application shutdown", true);
        run_capture(during_stop, 1);
        expect_errors(0);
        expect_true(error_logs == 0 && !atomic_load(&test_app.dropout_stop_requested),
                    "shutdown-induced read result is not a new dropout");
        check_stop();
    }

    const read_step_t empty_read[] = {{0, 0, false, 0}, {0, 8, false, 0}};
    reset_test("empty read", false);
    run_capture(empty_read, 2);
    expect_errors(0);
    expect_true(write_count == 1 && atomic_load(&test_app.total_samples) == 4,
                "empty read does not invent samples, queue loss, or transport errors");
    check_stop();

    for (int allocation = 1; allocation <= 2; allocation++) {
        reset_test(allocation == 1 ? "input allocation failure" : "packed allocation failure", false);
        fail_allocation = allocation;
        s_rtlsdr_dev = (rtlsdr_dev_t *)(uintptr_t)1;
        expect_true(gui_rtlsdr_start(&test_app) == 0, "allocation fixture starts with stubbed resources");
        expect_true(rtlsdr_capture_thread(&test_app) == -1, "missing capture buffer exits the worker");
        expect_errors(1);
        expect_true(error_logs == 1 && strstr(last_error, "allocate capture buffers"),
                    "allocation failure reports a system error once");
        expect_true(!gui_rtlsdr_is_running(&test_app) &&
                    atomic_load(&test_app.dropout_stop_requested) &&
                    atomic_load(&test_app.dropout_stop_reason) == GUI_DROPOUT_DEVICE_ERROR,
                    "unusable worker forces device-error stop despite disabled dropout setting");
        check_stop();
    }

    check_rf_source_configuration();
    check_rf_retune_guard();
#if LIBSOXR_ENABLED
    check_rf_input_delivery();
#endif

    printf("RTL-SDR diagnostics: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
