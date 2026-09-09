/* Exercise SDR labels and preset/edit transitions through the production UI. */
#include <stdio.h>
#include <string.h>
#include "../misrc_gui/ui/gui_ui.c"

static int checks;
static int failures;
static int save_calls;
static gui_settings_t saved_settings;
static char saved_status[128];
static const char *pending_text = "";
static bool enter_pressed;

/* Keep persistence in memory and script keyboard input without a window. */
void gui_settings_save(const gui_settings_t *settings)
{
    save_calls++;
    memcpy(&saved_settings, settings, sizeof(saved_settings));
}

void gui_app_set_status(gui_app_t *app, const char *message)
{
    (void)app;
    snprintf(saved_status, sizeof(saved_status), "%s", message);
}

bool gui_record_is_finalizing(void) { return false; }
bool IsKeyDown(int key) { (void)key; return false; }
bool IsKeyPressed(int key) { return key == KEY_ENTER && enter_pressed; }
bool IsMouseButtonDown(int button) { (void)button; return false; }
bool IsMouseButtonReleased(int button) { (void)button; return false; }
int GetCharPressed(void) { return *pending_text ? (unsigned char)*pending_text++ : 0; }

static void expect_true(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void clobber_stack(void)
{
    volatile char scratch[8192];
    for (size_t i = 0; i < sizeof(scratch); i++) scratch[i] = (char)i;
}

static void check_preset(gui_app_t *app, bool pal)
{
    gui_settings_t expected;
    memcpy(&expected, &app->settings, sizeof(expected));
    expected.rtlsdr_freq_hz = pal ? 1600000 : 1500000;
    int previous_saves = save_calls;

    gui_ui_apply_rtlsdr_hifi_preset(app, pal);

    expect_true(app->settings.rtlsdr_freq_hz == expected.rtlsdr_freq_hz,
                "the selected PAL/NTSC center frequency is applied");
    expect_true(strcmp(s_rtlsdr_freq_str, pal ? "1600000" : "1500000") == 0,
                "the displayed frequency immediately matches the preset");
    expect_true(memcmp(&app->settings, &expected, sizeof(expected)) == 0,
                "a center preset preserves input, rate, recording mode and all other settings");
    expect_true(save_calls == previous_saves + 1 &&
                memcmp(&saved_settings, &expected, sizeof(expected)) == 0,
                "the complete selected settings are saved exactly once");
    expect_true(strcmp(saved_status, pal ? "PAL center preset applied: 1.6 MHz"
                                       : "NTSC center preset applied: 1.5 MHz") == 0,
                "the preset confirmation names the selected standard and frequency");

    gui_ui_handle_active_text_edit(app);
    expect_true(memcmp(&app->settings, &expected, sizeof(expected)) == 0 &&
                save_calls == previous_saves + 1,
                "the next idle text-edit frame cannot overwrite or re-save the preset");
}

static void check_preset_transitions(void)
{
    static gui_app_t app;
    app.device_count = 1;
    app.devices[0].type = DEVICE_TYPE_RTLSDR;
    app.settings_panel_open = true;
    app.settings.rtlsdr_freq_hz = 100000000;
    app.settings.rtlsdr_direct_sampling = 2;
    app.settings.rtlsdr_sample_rate_hz = 1536000;
    app.settings.rtlsdr_record_mode = 1;
    app.settings.rtlsdr_gain_mode = 1;
    app.settings.rtlsdr_gain_tenths_db = 123;
    app.settings.capture_a = true;
    app.settings.capture_b = true;
    app.settings.enable_resample_a = true;
    app.settings.resample_rate_a = 4000.0f;
    app.settings.resample_rate_b = 3200.0f;
    snprintf(app.settings.output_path, sizeof(app.settings.output_path), "preset-test-output");

    gui_ui_clear_text_edit();
    /* Repeated selection stays selected; switching changes only the center. */
    static const bool sequence[] = {false, true, true, false, false, true};
    for (size_t i = 0; i < sizeof(sequence) / sizeof(sequence[0]); i++) {
        check_preset(&app, sequence[i]);
    }

    char *buffer = NULL;
    size_t capacity = 0;
    expect_true(gui_ui_text_field_get_buffer(&app, UI_TEXT_FIELD_RTLSDR_FREQ,
                                             &buffer, &capacity) &&
                buffer == s_rtlsdr_freq_str && capacity == sizeof(s_rtlsdr_freq_str),
                "the editable frequency field resolves to its authoritative text buffer");

    for (int pal = 0; pal <= 1; pal++) {
        app.settings.rtlsdr_freq_hz = 88500000;
        snprintf(s_rtlsdr_freq_str, sizeof(s_rtlsdr_freq_str), "91500000");
        s_active_text_field = UI_TEXT_FIELD_RTLSDR_FREQ;
        s_active_text_cursor = 3;
        s_active_text_selection_anchor = 1;
        s_active_text_drag_selecting = true;
        s_active_text_element_id = (Clay_ElementId){ .id = 123 };
        s_active_text_left_padding = 8.0f;
        s_active_text_right_padding = 8.0f;
        s_active_text_last_click_time = 2.0;
        s_active_text_last_click_element_id = (Clay_ElementId){ .id = 123 };
        s_active_text_backspace_repeat_at = 2.25;

        /* Prove this is a real live edit before testing the preset transition. */
        gui_ui_handle_active_text_edit(&app);
        expect_true(app.settings.rtlsdr_freq_hz == 91500000 &&
                    s_active_text_field == UI_TEXT_FIELD_RTLSDR_FREQ,
                    "a focused frequency buffer feeds the production text-edit handler");

        check_preset(&app, pal != 0);
        expect_true(s_active_text_field == UI_TEXT_FIELD_NONE &&
                    s_active_text_cursor == 0 && s_active_text_selection_anchor == -1 &&
                    !s_active_text_drag_selecting && s_active_text_element_id.id == 0 &&
                    s_active_text_left_padding == 0.0f && s_active_text_right_padding == 0.0f &&
                    s_active_text_last_click_time == -1.0 &&
                    s_active_text_last_click_element_id.id == 0 &&
                    s_active_text_backspace_repeat_at == 0.0,
                    "a preset ends the frequency edit and resets selection, drag and repeat state");
    }

    /* Also preserve the native I/Q and tuner choices on the same transitions. */
    app.settings.rtlsdr_direct_sampling = 0;
    app.settings.rtlsdr_sample_rate_hz = 2400000;
    app.settings.rtlsdr_record_mode = 0;
    check_preset(&app, true);
    check_preset(&app, false);
}

static void check_frequency_keyboard_input(void)
{
    static gui_app_t app;
    app.device_count = 1;
    app.devices[0].type = DEVICE_TYPE_RTLSDR;
    app.settings_panel_open = true;
    app.settings.rtlsdr_freq_hz = 100000000;
    gui_ui_clear_text_edit();
    s_active_text_field = UI_TEXT_FIELD_RTLSDR_FREQ;
    snprintf(s_rtlsdr_freq_str, sizeof(s_rtlsdr_freq_str), "150000");
    s_active_text_cursor = (int)strlen(s_rtlsdr_freq_str);

    int previous_saves = save_calls;
    pending_text = "1";
    gui_ui_handle_active_text_edit(&app);
    expect_true(s_active_text_field == UI_TEXT_FIELD_RTLSDR_FREQ &&
                strcmp(s_rtlsdr_freq_str, "1500001") == 0 &&
                app.settings.rtlsdr_freq_hz == 1500001,
                "typing a digit updates the frequency in the same frame and retains focus");
    expect_true(save_calls == previous_saves + 1 && saved_settings.rtlsdr_freq_hz == 1500001,
                "typing persists the new frequency, including the latest digit");

    previous_saves = save_calls;
    pending_text = "a.-+ Hz";
    gui_ui_handle_active_text_edit(&app);
    expect_true(strcmp(s_rtlsdr_freq_str, "1500001") == 0 &&
                app.settings.rtlsdr_freq_hz == 1500001 && save_calls == previous_saves &&
                s_active_text_field == UI_TEXT_FIELD_RTLSDR_FREQ,
                "non-digits are ignored without saving or ending the frequency edit");

    snprintf(s_rtlsdr_freq_str, sizeof(s_rtlsdr_freq_str), "160000");
    s_active_text_cursor = (int)strlen(s_rtlsdr_freq_str);
    pending_text = "0";
    enter_pressed = true;
    gui_ui_handle_active_text_edit(&app);
    enter_pressed = false;
    expect_true(strcmp(s_rtlsdr_freq_str, "1600000") == 0 &&
                app.settings.rtlsdr_freq_hz == 1600000 &&
                saved_settings.rtlsdr_freq_hz == 1600000 && save_calls > previous_saves,
                "a final digit and Enter in one frame save the complete frequency");
    expect_true(s_active_text_field == UI_TEXT_FIELD_NONE,
                "Enter commits the frequency edit and releases focus");
    previous_saves = save_calls;
    gui_ui_handle_active_text_edit(&app);
    expect_true(app.settings.rtlsdr_freq_hz == 1600000 && save_calls == previous_saves,
                "the committed keyboard frequency survives the following frame");
}

static void check_sample_rate_policy(void)
{
    static const uint32_t native_rates[] = {250000, 1024000, 1200000, 1536000, 2048000, 2400000};
    static const uint32_t rf_rates[] = {1024000, 1200000, 1536000, 2048000, 2400000};
    for (size_t i = 0; i < sizeof(native_rates) / sizeof(native_rates[0]); i++) {
        expect_true(gui_ui_rtlsdr_next_sample_rate(native_rates[i], false) ==
                    native_rates[(i + 1) % (sizeof(native_rates) / sizeof(native_rates[0]))],
                    "Native I/Q retains all six sample rates and the existing cycle order");
    }
    for (size_t i = 0; i < sizeof(rf_rates) / sizeof(rf_rates[0]); i++) {
        expect_true(gui_ui_rtlsdr_next_sample_rate(rf_rates[i], true) ==
                    rf_rates[(i + 1) % (sizeof(rf_rates) / sizeof(rf_rates[0]))],
                    "Hi-Fi RF cycles through the five supported rates without 0.25 MSPS");
    }
    expect_true(gui_ui_rtlsdr_next_sample_rate(0, false) == 1024000 &&
                gui_ui_rtlsdr_next_sample_rate(2000000, false) == 1024000,
                "unknown native rates keep the original fallback behavior");
    expect_true(gui_ui_rtlsdr_next_sample_rate(250000, true) == 2400000 &&
                gui_ui_rtlsdr_next_sample_rate(2000000, true) == 2400000,
                "unknown or unsupported Hi-Fi cycle values select the recommended 2.4 MSPS");
}

static void check_output_mode_transitions(void)
{
    static gui_app_t app;
    app.device_count = 1;
    app.devices[0].type = DEVICE_TYPE_RTLSDR;
    app.settings.rtlsdr_freq_hz = 1600000;
    app.settings.rtlsdr_direct_sampling = 2;
    app.settings.rtlsdr_gain_mode = 1;
    app.settings.rtlsdr_gain_tenths_db = 123;
    app.settings.capture_a = true;
    app.settings.capture_b = true;
    app.settings.enable_resample_a = true;
    app.settings.enable_resample_b = true;
    app.settings.resample_rate_a = 4000.0f;
    app.settings.resample_rate_b = 3200.0f;
    snprintf(app.settings.output_path, sizeof(app.settings.output_path), "output-mode-test");

#if LIBSOXR_ENABLED
    static const uint32_t rates[] = {
        1024000, 1200000, 1536000, 2048000, 2400000, 2000000,
        0, 250000, 1023999, 2400001, UINT32_MAX
    };
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        app.settings.rtlsdr_record_mode = 0;
        app.settings.rtlsdr_sample_rate_hz = rates[i];
        gui_settings_t expected;
        memcpy(&expected, &app.settings, sizeof(expected));
        expected.rtlsdr_record_mode = 1;
        bool adjusted_rate = rates[i] < 1024000 || rates[i] > 2400000;
        if (adjusted_rate) expected.rtlsdr_sample_rate_hz = 2400000;
        int previous_saves = save_calls;

        expect_true(gui_ui_toggle_rtlsdr_output(&app), "enabling Hi-Fi RF reports a mode change");
        expect_true(memcmp(&app.settings, &expected, sizeof(expected)) == 0,
                    "Hi-Fi entry normalizes only unsupported rates and preserves center, input and raw settings");
        expect_true(save_calls == previous_saves + 1 &&
                    memcmp(&saved_settings, &expected, sizeof(expected)) == 0,
                    "Hi-Fi entry persists the final mode and rate together exactly once");
        expect_true(adjusted_rate
                        ? strstr(saved_status, "hardware rate changed to recommended 2.4 MSPS") != NULL
                        : strstr(saved_status, "before connecting") != NULL,
                    "Hi-Fi entry explains any hardware-rate adjustment instead of silently replacing it");

        expected.rtlsdr_record_mode = 0;
        expect_true(gui_ui_toggle_rtlsdr_output(&app) &&
                    memcmp(&app.settings, &expected, sizeof(expected)) == 0,
                    "returning to Native I/Q changes only the recording mode");
        expect_true(save_calls == previous_saves + 2 &&
                    memcmp(&saved_settings, &expected, sizeof(expected)) == 0,
                    "Native I/Q return persists the retained hardware rate and raw options");
    }
#else
    static const uint32_t rates[] = {250000, 1536000, 2400000};
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        app.settings.rtlsdr_record_mode = 0;
        app.settings.rtlsdr_sample_rate_hz = rates[i];
        gui_settings_t expected;
        memcpy(&expected, &app.settings, sizeof(expected));
        int previous_saves = save_calls;
        expect_true(!gui_ui_toggle_rtlsdr_output(&app) &&
                    memcmp(&app.settings, &expected, sizeof(expected)) == 0 &&
                    save_calls == previous_saves,
                    "without soxr, Hi-Fi entry remains unavailable and changes no settings");
        expect_true(strcmp(saved_status, "8 MSPS Hi-Fi RF requires libsoxr") == 0,
                    "the unavailable conversion explains the missing dependency");
        app.settings.rtlsdr_record_mode = 1;
        expect_true(gui_ui_toggle_rtlsdr_output(&app) &&
                    memcmp(&app.settings, &expected, sizeof(expected)) == 0,
                    "a persisted Hi-Fi mode can still return to native without soxr or rate replacement");
        expect_true(save_calls == previous_saves + 1 &&
                    memcmp(&saved_settings, &expected, sizeof(expected)) == 0,
                    "leaving persisted Hi-Fi mode saves once without soxr");
    }
#endif
}

int main(void)
{
    static const uint32_t rates[] = {250000, 1024000, 1200000, 1536000, 2048000, 2400000};
    static const char *labels[] = {"0.250 MSPS", "1.024 MSPS", "1.200 MSPS", "1.536 MSPS", "2.048 MSPS", "2.400 MSPS"};
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        const char *rate = gui_ui_rtlsdr_sample_rate_label(rates[i]);
        const char *gain = gui_ui_rtlsdr_gain_label(123);
        clobber_stack();
        expect_true(strcmp(rate, labels[i]) == 0 && strcmp(gain, "Gain: 12.3 dB") == 0,
                    "formatted SDR labels survive stack reuse and other formatting");
    }
    const char *negative = gui_ui_rtlsdr_gain_label(-99);
    (void)gui_ui_rtlsdr_sample_rate_label(2400000);
    clobber_stack();
    expect_true(strcmp(negative, "Gain: -9.9 dB") == 0,
                "negative gain labels retain their sign and decimal precision");
    static gui_app_t app;
    app.device_count = 1;
    app.devices[0].type = DEVICE_TYPE_RTLSDR;
    expect_true(!gui_ui_rtlsdr_rf_mode(&app), "native I/Q is not Hi-Fi RF mode");
    app.settings.rtlsdr_record_mode = 1;
    // Even without soxr, show the persisted mode honestly so it can be disabled.
    expect_true(gui_ui_rtlsdr_rf_mode(&app), "persisted Hi-Fi RF mode remains visible without soxr");
    app.devices[0].type = DEVICE_TYPE_SIMULATED;
    expect_true(!gui_ui_rtlsdr_rf_mode(&app) && !gui_ui_selected_device_is_sdr(&app),
                "a non-SDR device cannot inherit the SDR recording mode");
    app.selected_device = -1;
    expect_true(!gui_ui_rtlsdr_rf_mode(&app), "a negative device selection is not SDR RF mode");
    app.selected_device = 1;
    expect_true(!gui_ui_rtlsdr_rf_mode(&app), "an out-of-range device selection is not SDR RF mode");
    check_preset_transitions();
    check_frequency_keyboard_input();
    check_sample_rate_policy();
    check_output_mode_transitions();
    printf("SDR settings: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
