/*
 * CPU-only checks for the production digital peak and clipping statistics.
 * LTO discards unrelated extraction/thread/backend code from the included file.
 * These are sample-format fixtures, not hardware overload or RF quality tests.
 */
#include <stdio.h>
#include <string.h>

#include "../misrc_gui/processing/gui_extract.c"

static int checks;
static int failures;
static gui_app_t test_app;

static void expect_true(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void reset_app(device_type_t type)
{
    memset(&test_app, 0, sizeof(test_app));
    test_app.device_count = 1;
    test_app.selected_device = 0;
    test_app.devices[0].type = type;
    atomic_init(&test_app.clip_count_a_pos, 0);
    atomic_init(&test_app.clip_count_a_neg, 0);
    atomic_init(&test_app.clip_count_b_pos, 0);
    atomic_init(&test_app.clip_count_b_neg, 0);
    atomic_init(&test_app.peak_a_pos, 0);
    atomic_init(&test_app.peak_a_neg, 0);
    atomic_init(&test_app.peak_b_pos, 0);
    atomic_init(&test_app.peak_b_neg, 0);
}

static void expect_clips(unsigned int a_pos, unsigned int a_neg,
                         unsigned int b_pos, unsigned int b_neg)
{
    expect_true(atomic_load(&test_app.clip_count_a_pos) == a_pos, "A positive rail count");
    expect_true(atomic_load(&test_app.clip_count_a_neg) == a_neg, "A negative rail count");
    expect_true(atomic_load(&test_app.clip_count_b_pos) == b_pos, "B positive rail count");
    expect_true(atomic_load(&test_app.clip_count_b_neg) == b_neg, "B negative rail count");
}

int main(void)
{
    const device_type_t other_types[] = {
        DEVICE_TYPE_HSDAOH, DEVICE_TYPE_SIMPLE_CAPTURE, DEVICE_TYPE_CXADC,
        DEVICE_TYPE_MISRC_CLOCKGEN, DEVICE_TYPE_SIMULATED, DEVICE_TYPE_PLAYBACK,
#ifdef ENABLE_FX3
        DEVICE_TYPE_FX3,
#endif
#ifdef ENABLE_DDD
        DEVICE_TYPE_DDD,
#endif
    };
    const int16_t boundary_a[] = {2031, 2032, 2046, 2047, -2047, -2048};
    const int16_t boundary_b[] = {-2048, -2048, -2047, 2046, 2032, 2031};
    for (size_t i = 0; i < sizeof(other_types) / sizeof(other_types[0]); i++) {
        reset_app(other_types[i]);
        gui_extract_update_stats(&test_app, boundary_a, boundary_b, 6);
        expect_clips(1, 1, 0, 2);
        expect_true(atomic_load(&test_app.peak_a_pos) == 2047, "12-bit positive peak unchanged");
        expect_true(atomic_load(&test_app.peak_a_neg) == 2048, "12-bit negative peak unchanged");
    }

    // Invalid/no selected device retains the original 12-bit thresholds.
    const int invalid_indices[] = {-1, 1};
    for (size_t i = 0; i < sizeof(invalid_indices) / sizeof(invalid_indices[0]); i++) {
        reset_app(DEVICE_TYPE_HSDAOH);
        test_app.selected_device = invalid_indices[i];
        gui_extract_update_stats(&test_app, boundary_a, boundary_b, 6);
        expect_clips(1, 1, 0, 2);
    }

#ifdef ENABLE_RTLSDR
    reset_app(DEVICE_TYPE_RTLSDR);
    test_app.device_count = 0;
    gui_extract_update_stats(&test_app, boundary_a, boundary_b, 6);
    expect_clips(1, 1, 0, 2);

    reset_app(DEVICE_TYPE_RTLSDR);
    gui_extract_update_stats(&test_app, boundary_a, boundary_b, 6);
    expect_clips(3, 1, 2, 2);

    // Exhaust the complete RTL 8-bit code range, reversing Q to exercise both rails.
    int16_t rtl_i[256], rtl_q[256];
    for (int i = 0; i < 256; i++) {
        rtl_i[i] = (int16_t)((i - 128) * 16);
        rtl_q[i] = (int16_t)((255 - i - 128) * 16);
    }
    reset_app(DEVICE_TYPE_RTLSDR);
    gui_extract_update_stats(&test_app, rtl_i, rtl_q, 256);
    expect_clips(1, 1, 1, 1);
    expect_true(atomic_load(&test_app.peak_a_pos) == 2032, "RTL I peak retains the unmodified sample value");
    expect_true(atomic_load(&test_app.peak_a_neg) == 2048, "RTL I negative peak");
    expect_true(atomic_load(&test_app.peak_b_pos) == 2032, "RTL Q positive peak");
    expect_true(atomic_load(&test_app.peak_b_neg) == 2048, "RTL Q negative peak");

    gui_extract_update_stats(&test_app, rtl_i + 1, rtl_q + 1, 254);
    expect_clips(1, 1, 1, 1);
    expect_true(atomic_load(&test_app.peak_a_pos) == 2016, "RTL value below positive rail does not clip");
    expect_true(atomic_load(&test_app.peak_a_neg) == 2032, "RTL value above negative rail does not clip");

    gui_extract_update_stats(&test_app, rtl_i, rtl_q, 256);
    expect_clips(2, 2, 2, 2);
    gui_extract_update_stats(&test_app, rtl_i, NULL, 256);
    expect_clips(3, 3, 0, 0);
    expect_true(atomic_load(&test_app.peak_b_pos) == 0 &&
                atomic_load(&test_app.peak_b_neg) == 0, "absent B resets both peaks");
#endif

    // Peak window remains the first 1000 samples; clipping still scans the block.
    int16_t long_block[1002] = {0};
    long_block[999] = 123;
    long_block[1000] = 2047;
    long_block[1001] = -2048;
    reset_app(DEVICE_TYPE_HSDAOH);
    gui_extract_update_stats(&test_app, long_block, long_block, 1002);
    expect_clips(1, 1, 1, 1);
    expect_true(atomic_load(&test_app.peak_a_pos) == 123 &&
                atomic_load(&test_app.peak_a_neg) == 0, "A peak window unchanged");
    expect_true(atomic_load(&test_app.peak_b_pos) == 123 &&
                atomic_load(&test_app.peak_b_neg) == 0, "B peak window unchanged");
    gui_extract_update_stats(&test_app, long_block, long_block, 0);
    expect_clips(1, 1, 1, 1);
    expect_true(atomic_load(&test_app.peak_a_pos) == 0 &&
                atomic_load(&test_app.peak_b_pos) == 0, "empty input resets peaks without adding clips");

    printf("Extraction stats: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
