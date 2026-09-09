/*
 * MISRC GUI - Streaming RTL-SDR I/Q to real RF conversion
 *
 * One state belongs to one recording worker. Input is unsigned 8-bit I,Q pairs,
 * before channel swapping or display processing. Output is mono signed 16-bit
 * at 8 MSPS; the writer is responsible for little-endian RAW serialization.
 */
#ifndef GUI_SDR_RF_H
#define GUI_SDR_RF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GUI_SDR_RF_OUTPUT_RATE_HZ 8000000u
#define GUI_SDR_RF_OUTPUT_GAIN 0.5

typedef struct gui_sdr_rf gui_sdr_rf_t;
typedef bool (*gui_sdr_rf_emit_t)(void *opaque, const int16_t *samples, size_t count);

// Restrict this export to RTL rates with a positive RF band below 4 MHz.
// This checks frequency geometry, not the tuner's ability to receive that band.
bool gui_sdr_rf_validate(uint32_t input_rate_hz, uint64_t center_hz,
                         char *error, size_t error_size);

// VHQ band-limited interpolation, then Re((I + jQ) * exp(j*2*pi*center*t)).
// The interpolation filter has a transition band at the input Nyquist edges.
// A fixed 0.5 gain leaves 6 dB headroom; this does not restore missing bandwidth.
gui_sdr_rf_t *gui_sdr_rf_create(uint32_t input_rate_hz, uint64_t center_hz,
                               char *error, size_t error_size);

// Consumes every input pair on success. Memory use is independent of block size.
// The callback must consume/copy its buffer before returning; false stops the
// stream permanently. No phase or filter state is reset between calls.
bool gui_sdr_rf_process(gui_sdr_rf_t *state, const uint8_t *iq, size_t pairs,
                        gui_sdr_rf_emit_t emit, void *opaque);

// Drain the filter at normal EOF. Repeated successful finish calls are harmless.
// On a discontinuity destroy without finishing; do not synthesize a clean tail.
bool gui_sdr_rf_finish(gui_sdr_rf_t *state, gui_sdr_rf_emit_t emit, void *opaque);

const char *gui_sdr_rf_error(const gui_sdr_rf_t *state);
void gui_sdr_rf_destroy(gui_sdr_rf_t *state);

#endif // GUI_SDR_RF_H
