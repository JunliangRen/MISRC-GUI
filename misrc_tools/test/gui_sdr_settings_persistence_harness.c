/*
 * Exercise production settings defaults, JSON serialization and reload.
 * --config path selection is retained, but fopen/fclose are redirected to a
 * private tmpfile. The harness never opens or changes the user's settings.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *fixture_open(const char *path, const char *mode);
static int fixture_close(FILE *file);
#define fopen fixture_open
#define fclose fixture_close
#include "../misrc_gui/core/gui_settings.c"
#undef fclose
#undef fopen

static const char fixture_path[] = "gui_sdr_settings_fixture_only.json";
static FILE *fixture_file;
static int checks, failures, file_opens;
static const char *test_case;

static void expect_true(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL [%s]: %s\n", test_case, message);
        failures++;
    }
}

static bool new_fixture(void)
{
    if (fixture_file) fclose(fixture_file);
    fixture_file = tmpfile();
    expect_true(fixture_file != NULL, "private temporary settings file is available");
    return fixture_file != NULL;
}

static FILE *fixture_open(const char *path, const char *mode)
{
    file_opens++;
    bool correct_path = path && strcmp(path, fixture_path) == 0;
    expect_true(correct_path, "production I/O uses the explicit test --config override");
    if (!correct_path) return NULL; // Never fall back to any real settings path.
    if (strcmp(mode, "w") == 0) {
        if (!new_fixture()) return NULL;
    } else if (strcmp(mode, "r") != 0) {
        expect_true(false, "settings use the expected read/write mode");
        return NULL;
    }
    if (fixture_file) rewind(fixture_file);
    return fixture_file;
}

static int fixture_close(FILE *file)
{
    expect_true(file != NULL && file == fixture_file, "only the private fixture is closed");
    return file ? fflush(file) : EOF;
}

static bool set_json(const char *json)
{
    if (!new_fixture()) return false;
    expect_true(fputs(json, fixture_file) >= 0 && fflush(fixture_file) == 0,
                "settings fixture contents are written completely");
    return true;
}

static void prepare_shared_settings(gui_settings_t *settings, bool use_flac)
{
    gui_settings_init_defaults(settings);
    settings->capture_a = false;
    settings->capture_b = true;
    settings->enable_resample_a = true;
    settings->enable_resample_b = false;
    settings->resample_rate_a = 14300.0f;
    settings->resample_rate_b = 20000.0f;
    settings->resample_quality_a = 2;
    settings->resample_quality_b = 4;
    settings->resample_gain_a = -1.5f;
    settings->resample_gain_b = 2.5f;
    settings->use_flac = use_flac;
    settings->rf_bits_a = 8;
    settings->rf_bits_b = use_flac ? 12 : 16;
    settings->misrc_mode = true;
    settings->misrc_v15_v25_ab_swap = false;
    settings->stop_on_dropout = true;
    settings->cxadc_tenbit_mode_card[0] = true;
    settings->cxadc_tenbit_mode_card[1] = false;
#ifdef ENABLE_DDD
    settings->ddd_decimation = DDD_DECIMATION_HALF_RATE;
#endif
}

static void check_shared_settings(const gui_settings_t *actual, const gui_settings_t *expected)
{
    expect_true(actual->capture_a == expected->capture_a && actual->capture_b == expected->capture_b,
                "SDR output mode does not overwrite independent A/B capture preferences");
    expect_true(actual->enable_resample_a == expected->enable_resample_a &&
                actual->enable_resample_b == expected->enable_resample_b &&
                actual->resample_rate_a == expected->resample_rate_a &&
                actual->resample_rate_b == expected->resample_rate_b,
                "other devices retain their resampling enables and rates");
    expect_true(actual->resample_quality_a == expected->resample_quality_a &&
                actual->resample_quality_b == expected->resample_quality_b &&
                actual->resample_gain_a == expected->resample_gain_a &&
                actual->resample_gain_b == expected->resample_gain_b,
                "other devices retain resampling quality and gain");
    expect_true(actual->use_flac == expected->use_flac && actual->rf_bits_a == expected->rf_bits_a &&
                actual->rf_bits_b == expected->rf_bits_b,
                "fixed 16-bit SDR RF output does not rewrite global file format or channel bit depths");
    expect_true(actual->misrc_mode == expected->misrc_mode &&
                actual->misrc_v15_v25_ab_swap == expected->misrc_v15_v25_ab_swap &&
                actual->stop_on_dropout == expected->stop_on_dropout,
                "MISRC mapping and capture safety preferences are retained");
    expect_true(actual->cxadc_tenbit_mode_card[0] == expected->cxadc_tenbit_mode_card[0] &&
                actual->cxadc_tenbit_mode_card[1] == expected->cxadc_tenbit_mode_card[1],
                "CXADC card modes are retained");
#ifdef ENABLE_DDD
    expect_true(actual->ddd_decimation == expected->ddd_decimation,
                "DdD hardware decimation remains independent of RTL-SDR output mode");
#endif
}

int main(void)
{
    gui_settings_t settings, expected, saved;
    test_case = "override and defaults";
    gui_settings_set_override_path(fixture_path);
    expect_true(gui_settings_override_active(), "the same override used by --config is active");
    gui_settings_init_defaults(&settings);
    expect_true(settings.rtlsdr_record_mode == 0 && settings.rtlsdr_direct_sampling == 0,
                "fresh installs select native I/Q and normal tuner input");
    expect_true(settings.rtlsdr_freq_hz == UINT64_C(100000000) &&
                settings.rtlsdr_sample_rate_hz == 2400000,
                "new recording mode does not change established hardware frequency/rate defaults");
    settings.rtlsdr_record_mode = 1;
    settings.rtlsdr_direct_sampling = 2;
    gui_settings_load(&settings); // No temporary file yet: exercise absent settings.
    expect_true(settings.rtlsdr_record_mode == 0 && settings.rtlsdr_direct_sampling == 0,
                "absent config restores safe defaults instead of retaining stale RF mode");

    test_case = "older settings without SDR output keys";
    if (!set_json("{\n\"capture_a\": false,\n\"capture_b\": true,\n"
                  "\"enable_resample_a\": true,\n\"enable_resample_b\": false,\n"
                  "\"resample_rate_a\": 14300,\n\"resample_rate_b\": 20000,\n"
                  "\"rf_bits_a\": 8,\n\"rf_bits_b\": 12,\n\"use_flac\": true,\n"
                  "\"ddd_decimation\": 2\n}\n")) return 1;
    gui_settings_load(&settings);
    expect_true(settings.rtlsdr_record_mode == 0 && settings.rtlsdr_direct_sampling == 0,
                "old settings files continue in native tuner mode without migration");
    expect_true(!settings.capture_a && settings.capture_b && settings.enable_resample_a &&
                !settings.enable_resample_b && settings.resample_rate_a == 14300.0f &&
                settings.resample_rate_b == 20000.0f && settings.rf_bits_a == 8 && settings.rf_bits_b == 12,
                "old capture, resampling and bit-depth choices survive missing new keys");
#ifdef ENABLE_DDD
    expect_true(settings.ddd_decimation == DDD_DECIMATION_HALF_RATE,
                "old DdD half-rate preference survives missing RTL-SDR keys");
#endif

    for (int mode = 0; mode <= 1; mode++) {
        for (int direct = 0; direct <= 2; direct++) {
            test_case = "valid SDR options round trip";
            prepare_shared_settings(&settings, direct != 0);
            settings.rtlsdr_record_mode = mode;
            settings.rtlsdr_direct_sampling = direct;
            settings.rtlsdr_freq_hz = 1600000;
            settings.rtlsdr_sample_rate_hz = 2048000;
            settings.rtlsdr_gain_mode = 1;
            settings.rtlsdr_gain_tenths_db = 123;
            settings.rtlsdr_agc = false;
            settings.rtlsdr_offset_corr = true;
            saved = settings;
            gui_settings_save(&settings);
            if (!fixture_file) return 1;
            expect_true(memcmp(&settings, &saved, sizeof(settings)) == 0,
                        "saving RTL-SDR output preferences never mutates any in-memory settings");

            char content[32769];
            rewind(fixture_file);
            size_t length = fread(content, 1, sizeof(content) - 1, fixture_file);
            content[length] = '\0';
            char mode_key[64], direct_key[64];
            snprintf(mode_key, sizeof(mode_key), "\"rtlsdr_record_mode\": %d", mode);
            snprintf(direct_key, sizeof(direct_key), "\"rtlsdr_direct_sampling\": %d", direct);
            expect_true(strstr(content, mode_key) && strstr(content, direct_key),
                        "saved JSON contains the exact selected recording mode and input path");
            memset(&expected, 0xA5, sizeof(expected));
            gui_settings_load(&expected);
            expect_true(expected.rtlsdr_record_mode == mode && expected.rtlsdr_direct_sampling == direct,
                        "all six native/RF and tuner/direct-I/direct-Q combinations reload correctly");
            expect_true(expected.rtlsdr_freq_hz == settings.rtlsdr_freq_hz &&
                        expected.rtlsdr_sample_rate_hz == settings.rtlsdr_sample_rate_hz &&
                        expected.rtlsdr_gain_mode == settings.rtlsdr_gain_mode &&
                        expected.rtlsdr_gain_tenths_db == settings.rtlsdr_gain_tenths_db &&
                        expected.rtlsdr_agc == settings.rtlsdr_agc &&
                        expected.rtlsdr_offset_corr == settings.rtlsdr_offset_corr,
                        "existing RTL-SDR hardware settings retain their chosen values");
            check_shared_settings(&expected, &settings);
        }
    }

    const int invalid_modes[] = {-1, 2, 99};
    const int invalid_inputs[] = {-1, 3, 99};
    for (size_t i = 0; i < sizeof(invalid_modes) / sizeof(invalid_modes[0]); i++) {
        char json[160];
        test_case = "invalid SDR enums";
        snprintf(json, sizeof(json), "{\n\"rtlsdr_record_mode\": %d,\n"
                 "\"rtlsdr_direct_sampling\": %d\n}\n", invalid_modes[i], invalid_inputs[i]);
        if (!set_json(json)) return 1;
        settings.rtlsdr_record_mode = 1;
        settings.rtlsdr_direct_sampling = 2;
        gui_settings_load(&settings);
        expect_true(settings.rtlsdr_record_mode == 0 && settings.rtlsdr_direct_sampling == 0,
                    "out-of-range persisted enums fall back to native tuner mode");
    }

    test_case = "independent SDR enum validation";
    if (!set_json("{\n\"rtlsdr_record_mode\": 2,\n\"rtlsdr_direct_sampling\": 2\n}\n")) return 1;
    gui_settings_load(&settings);
    expect_true(settings.rtlsdr_record_mode == 0 && settings.rtlsdr_direct_sampling == 2,
                "invalid recording mode does not erase a valid direct-Q input choice");
    if (!set_json("{\n\"rtlsdr_record_mode\": 1,\n\"rtlsdr_direct_sampling\": 3\n}\n")) return 1;
    gui_settings_load(&settings);
    expect_true(settings.rtlsdr_record_mode == 1 && settings.rtlsdr_direct_sampling == 0,
                "invalid input path does not erase a valid RF recording mode choice");

    test_case = "nonnumeric SDR enums";
    if (!set_json("{\n\"rtlsdr_record_mode\": \"invalid\",\n"
                  "\"rtlsdr_direct_sampling\": \"invalid\"\n}\n")) return 1;
    gui_settings_load(&settings);
    expect_true(settings.rtlsdr_record_mode == 0 && settings.rtlsdr_direct_sampling == 0,
                "nonnumeric output/input selections cannot enable RF or direct sampling");
    expect_true(file_opens > 0 && gui_settings_override_active(),
                "all settings operations completed with the test-only override active");
    if (fixture_file) fclose(fixture_file);
    fixture_file = NULL;
    gui_settings_set_override_path(NULL);
    printf("SDR settings persistence: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
