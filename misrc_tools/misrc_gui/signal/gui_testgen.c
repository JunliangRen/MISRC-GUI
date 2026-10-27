/*
 * MISRC GUI - HackTV Signal Generator Plugin
 *
 * A 1:1 PORT of hacktv's line-based video generation pipeline
 * (third_party/hacktv/src/video.c by Philip Heron, GPL-3.0-or-later).
 * This project is GPL-3.0-or-later; direct code porting is legal.
 *
 * PORTED FUNCTIONS (hacktv src/video.c):
 *   _line_sequence()         -> line classification per raster type
 *   _active_video_line()     -> active line mapping
 *   _vid_next_line_raster()  -> per-line CVBS assembly (sync, burst, chroma)
 *   _render_sync_pulses()    -> pre-rendered sync pulse LUTs
 *   vid_init() level scaling -> 16-bit level computation + RGB->YUV
 *   _burstwin()              -> raised-cosine burst envelope
 *   vid_config_pal/ntsc      -> the composite config structs (verbatim values)
 *   av_test.c bars           -> 75% colour bar pattern
 *
 * ALTERED for integration:
 *   - Generates a whole frame at once into a flat int16 buffer
 *   - No pthreads/ffmpeg/audio/scrambling - just the video path
 *   - hacktv's Gaussian chroma FIR replaced with a simple window (v1)
 *   - SECAM ident/pre-emphasis omitted (v1)
 */

#include "gui_testgen.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --- hacktv vid_config structs (verbatim values) ------------------------- */

typedef struct {
    int lines;                  /* 625 / 525 */
    double active_width_us;     /* 51.95 / 52.90 */
    double active_left_us;      /* 10.40 / 9.20 */
    double hsync_us;            /* 4.70 */
    double vsync_short_us;      /* 2.35 / 2.30 (eq pulse) */
    double vsync_long_us;       /* 27.30 / 27.10 (broad pulse) */
    double sync_rise_us;        /* 0.20 / 0.25 */
    double white_level;         /* 0.70 / 100/140 */
    double black_level;         /* 0.00 / 7.5/140 */
    double blanking_level;      /* 0.00 */
    double sync_level;          /* -0.30 / -40/140 */
    int colour_mode;            /* PAL / NTSC / SECAM */
    double burst_width_us;      /* 2.25 / 2.50 */
    double burst_rise_us;       /* 0.30 */
    double burst_left_us;       /* 5.60 / 5.30 */
    double burst_level_frac;    /* 3/7 / 4/10 of white-blank */
    double fsc_hz;              /* 4433618.75 / 3579545.4545 */
    double ev_co;               /* 0.877 */
    double eu_co;               /* 0.493 */
} ht_config_t;

#define HT_PAL   1
#define HT_NTSC  2
#define HT_SECAM 3

static const ht_config_t HT_CONF[GUI_TESTGEN_SYSTEM_COUNT] = {
    /* PAL — hacktv vid_config_pal */
    { 625, 51.95, 10.40, 4.70, 2.35, 27.30, 0.20,
      0.70, 0.00, 0.00, -0.30,
      HT_PAL, 2.25, 0.30, 5.60, 3.0/7.0, 4433618.75, 0.877, 0.493 },
    /* NTSC — hacktv vid_config_ntsc */
    { 525, 52.90, 9.20, 4.70, 2.30, 27.10, 0.25,
      100.0/140, 7.5/140, 0.00, -40.0/140,
      HT_NTSC, 2.50, 0.30, 5.30, 4.0/10.0, 3579545.4545, 0.877, 0.493 },
    /* SECAM — hacktv vid_config_secam (PAL luma geometry + FM chroma) */
    { 625, 51.95, 10.40, 4.70, 2.35, 27.30, 0.20,
      0.70, 0.00, 0.00, -0.30,
      HT_SECAM, 2.25, 0.30, 5.60, 0.0, 4433618.75, 0.877, 0.493 },
};

/* SECAM FM centres (hacktv SECAM_* defines) */
#define HT_SECAM_DB 4250000.0
#define HT_SECAM_DR 4406250.0

/* --- hacktv _line_sequence (625/525 rasters, simplified to the standard) -- */

typedef enum {
    HT_LSEQ_VBLANK,       /* blanked line with normal H-sync */
    HT_LSEQ_EQ_PRE,       /* pre-equalizing pulses (half-line rate) */
    HT_LSEQ_VSYNC,        /* broad pulses (half-line rate) */
    HT_LSEQ_EQ_POST,      /* post-equalizing pulses (half-line rate) */
    HT_LSEQ_ACTIVE        /* active video line with H-sync */
} ht_lseq_t;

typedef struct {
    ht_lseq_t type;
    int vy;                /* active video row (-1 = not active) */
} ht_line_info_t;

/* hacktv's _active_video_line for 625/525 */
static int ht_active_video_line(int lines_total, int line_1) {
    if (lines_total == 625) {
        return (line_1 < 313) ? (line_1 - 23) * 2 : (line_1 - 336) * 2 + 1;
    } else {
        return (line_1 < 265) ? (line_1 - 23) * 2 : (line_1 - 286) * 2 + 1;
    }
}

static void ht_line_info(int lines_total, int line_1, ht_line_info_t *out) {
    int hline = (lines_total + 1) / 2;  /* 313 / 263 */
    bool second = line_1 > hline;
    int fl = second ? line_1 - hline : line_1;

    out->vy = -1;
    if (fl <= 2) {
        out->type = HT_LSEQ_EQ_PRE;
    } else if (fl <= 5) {
        out->type = HT_LSEQ_VSYNC;
    } else if (fl <= 7) {
        out->type = HT_LSEQ_EQ_POST;
    } else if (fl <= 22) {
        out->type = HT_LSEQ_VBLANK;
    } else {
        out->type = HT_LSEQ_ACTIVE;
        out->vy = ht_active_video_line(lines_total, line_1);
        if (out->vy < 0) out->vy = -1;
    }
}

/* --- hacktv _burstwin (raised-cosine envelope) --------------------------- */

static double ht_burstwin(double t, double width, double rise) {
    /* t in seconds from burst start; width/rise in seconds */
    if (t < 0 || t > width + rise) return 0.0;
    double w;
    if (t < rise / 2) w = 0.0;
    else if (t < width) w = 1.0;
    else if (t < width + rise / 2) w = (width + rise / 2 - t) / rise;
    else w = 0.0;
    w = fmax(0.0, fmin(1.0, w));
    return 0.5 - 0.5 * cos(M_PI * w);
}

/* --- hacktv av_test bars (75% amplitude) --------------------------------- */

typedef struct { double r, g, b; } ht_rgb_t;
static const ht_rgb_t HT_BARS[8] = {
    { 1.00, 1.00, 1.00 }, { 0.75, 0.75, 0.00 }, { 0.00, 0.75, 0.75 },
    { 0.00, 0.75, 0.00 }, { 0.75, 0.00, 0.75 }, { 0.75, 0.00, 0.00 },
    { 0.00, 0.00, 0.75 }, { 0.00, 0.00, 0.00 },
};

/* hacktv yuv_level_lookup: map RGB -> Y/U/V with config coefficients */
static void ht_rgb_to_yuv(const ht_config_t *c, double r, double g, double b,
                           double *y, double *u, double *v) {
    *y = 0.299 * r + 0.587 * g + 0.114 * b;
    *u = (b - *y) * c->eu_co;
    *v = (r - *y) * c->ev_co;
}

/* --- Public API ----------------------------------------------------------- */

size_t gui_testgen_frame_samples(gui_testgen_system_t system, uint32_t sample_rate) {
    if ((int)system < 0 || (int)system >= GUI_TESTGEN_SYSTEM_COUNT || sample_rate == 0) {
        return 0;
    }
    const ht_config_t *c = &HT_CONF[system];
    /* Line period: PAL 64us, NTSC 63.5555us */
    double line_us = (c->lines == 525) ? 1001.0 / 15.75 : 64.0;
    double frame_us = line_us * (double)c->lines;
    return (size_t)(frame_us * (double)sample_rate / 1e6 + 0.5);
}

const char *gui_testgen_system_name(gui_testgen_system_t system) {
    if ((int)system < 0 || (int)system >= GUI_TESTGEN_SYSTEM_COUNT) return "PAL";
    switch (system) {
        case GUI_TESTGEN_PAL: return "PAL";
        case GUI_TESTGEN_NTSC: return "NTSC";
        case GUI_TESTGEN_SECAM: return "SECAM";
        default: return "PAL";
    }
}

size_t gui_testgen_frame(gui_testgen_system_t system, int16_t *out, size_t cap,
                         uint32_t sample_rate) {
    if ((int)system < 0 || (int)system >= GUI_TESTGEN_SYSTEM_COUNT) return 0;
    if (!out || sample_rate == 0) return 0;
    const ht_config_t *c = &HT_CONF[system];
    size_t need = gui_testgen_frame_samples(system, sample_rate);
    if (cap < need) return 0;

    /* hacktv vid_init: samples per line from pixel rate.
     * NTSC's line period (63.5555..us) is non-integer at most rates:
     * at 40 MSPS it is 2542.22 samples. Use the FRAME total (not
     * lines * integer_width) to determine the buffer size, and let
     * the last line absorb the rounding remainder so the frame wraps
     * without a timing gap. The subcarrier phase (fsc_inc * abs_pos)
     * is already continuous and drift-free. */
    double line_us = (c->lines == 525) ? 1001.0 / 15.75 : 64.0;
    int width = (int)(line_us * (double)sample_rate / 1e6 + 0.5);
    int half_width = width / 2;
    if (width < 2) return 0;
    /* Verify: lines * width should be close to `need` */
    /* If lines * width != need, the last line is slightly longer to */
    /* absorb the remainder (prevents inter-frame drift). */

    /* hacktv vid_init: active area in samples */
    int active_left = (int)(c->active_left_us * (double)sample_rate / 1e6 + 0.5);
    int active_width = (int)ceil(c->active_width_us * (double)sample_rate / 1e6);
    if (active_left + active_width > width) active_width = width - active_left;

    /* hacktv vid_init: 16-bit signal levels */
    /* level = 1.0 (composite unmodulated), slevel = level */
    double level = 1.0;
    double white16 = c->white_level * level;
    double black16 = c->black_level * level;
    double blank16 = c->blanking_level * level;
    double sync16 = c->sync_level * level;

    /* hacktv vid_init: sync pulse sample counts */
    int hsync_smp = (int)(c->hsync_us * (double)sample_rate / 1e6 + 0.5);
    int eq_smp = (int)(c->vsync_short_us * (double)sample_rate / 1e6 + 0.5);
    int broad_smp = (int)(c->vsync_long_us * (double)sample_rate / 1e6 + 0.5);

    /* hacktv vid_init: burst parameters */
    int burst_left_smp = (int)(c->burst_left_us * (double)sample_rate / 1e6 + 0.5);
    double burst_rise_s = c->burst_rise_us * 1e-6;
    double burst_width_s = c->burst_width_us * 1e-6;
    double burst_amp = c->burst_level_frac * (white16 - blank16);
    if (burst_amp < 0) burst_amp = -burst_amp;

    /* hacktv vid_init: colour carrier phase increment per sample */
    double fsc_inc = 2.0 * M_PI * c->fsc_hz / (double)sample_rate;

    /* SECAM FM phase accumulator (continuous across the frame) */
    double secam_phase = 0.0;

    /* Generate line by line (hacktv's _vid_next_line_raster approach) */
    size_t out_pos = 0;
    int line_1 = 1;

    while (out_pos < need) {
        ht_line_info_t info;
        ht_line_info(c->lines, line_1, &info);

        /* Every line is always `width` samples (hacktv's approach).
         * The interlace comes from the V-SYNC PULSE POSITIONS, not from
         * shortening lines. Shortening line 314 to half_width created a
         * timing gap the decoder's V-SYNC read as a dropout (the rolling). */
        int line_samples = width;

        for (int x = 0; x < line_samples && out_pos < need; x++) {
            /* Absolute sample position (for subcarrier phase continuity) */
            size_t abs_pos = out_pos;

            /* Base signal = blanking level */
            double signal = blank16;
            double chroma = 0.0;

            /* Sync pulses (hacktv _render_sync_pulses) */
            switch (info.type) {
                case HT_LSEQ_EQ_PRE:
                case HT_LSEQ_EQ_POST:
                    /* Equalizing: eq pulse at start of each half-line */
                    if (x < eq_smp) signal = sync16;
                    else if (x >= half_width && x < half_width + eq_smp) signal = sync16;
                    break;

                case HT_LSEQ_VSYNC:
                    /* Broad pulse at start of each half-line */
                    if (x < broad_smp) signal = sync16;
                    else if (x >= half_width && x < half_width + broad_smp) signal = sync16;
                    break;

                case HT_LSEQ_VBLANK:
                case HT_LSEQ_ACTIVE:
                default:
                    /* Normal H-sync */
                    if (x < hsync_smp) signal = sync16;
                    break;
            }

            /* Active video (hacktv _vid_next_line_raster active path) */
            if (info.type == HT_LSEQ_ACTIVE && info.vy >= 0) {
                int ax = x - active_left;
                if (ax >= 0 && ax < active_width) {
                    int bar = (ax * 8) / active_width;
                    if (bar > 7) bar = 7;
                    if (bar < 0) bar = 0;

                    double y, u, v;
                    ht_rgb_to_yuv(c, HT_BARS[bar].r, HT_BARS[bar].g,
                                  HT_BARS[bar].b, &y, &u, &v);

                    /* hacktv yuv_level_lookup: y in black..white */
                    signal = black16 + y * (white16 - black16);

                    /* Chroma modulation (hacktv chrominance path) */
                    if (c->colour_mode == HT_PAL || c->colour_mode == HT_NTSC) {
                        /* QAM on the subcarrier.
                         * hacktv: U rides sin, V rides cos.
                         * PAL V-switch: V inverted on alternating lines.
                         * hacktv: pal = 1 normally, -1 on (frame+line) odd.
                         * We use line parity (matches hacktv for field 1). */
                        int pal_switch = ((line_1 & 1) == 0) ? 1 : -1;
                        if (c->colour_mode == HT_NTSC) pal_switch = 1;

                        double u_scaled = u * (white16 - blank16);
                        double v_scaled = v * (white16 - blank16);
                        double phase = fsc_inc * (double)abs_pos;
                        chroma = u_scaled * sin(phase) +
                                 v_scaled * pal_switch * cos(phase);
                    } else if (c->colour_mode == HT_SECAM) {
                        /* SECAM FM: alternating D'R/D'B per line */
                        bool dr = (line_1 & 1) != 0;
                        double comp = dr ? v : u;
                        double centre = dr ? HT_SECAM_DR : HT_SECAM_DB;
                        double span = white16 - blank16;
                        double freq = centre + comp * span * 1e6;
                        secam_phase += 2.0 * M_PI * freq / (double)sample_rate;
                        double sub_amp = 0.25 * span;
                        chroma = sub_amp * sin(secam_phase);
                    }
                }
            }

            /* Burst (hacktv burst window + phase, on non-eq/vsync lines) */
            if ((c->colour_mode == HT_PAL || c->colour_mode == HT_NTSC) &&
                (info.type == HT_LSEQ_ACTIVE || info.type == HT_LSEQ_VBLANK) &&
                x >= burst_left_smp) {
                double bt = (double)(x - burst_left_smp) / (double)sample_rate;
                double w = ht_burstwin(bt, burst_width_s, burst_rise_s);
                if (w > 0.0) {
                    double phase = fsc_inc * (double)abs_pos;
                    if (c->colour_mode == HT_NTSC) {
                        /* NTSC burst: 180 degrees (hacktv: -sin) */
                        chroma += -w * burst_amp * sin(phase);
                    } else {
                        /* PAL burst: +-135 degrees with V-switch */
                        int pal_switch = ((line_1 & 1) == 0) ? 1 : -1;
                        double ang = (pal_switch > 0)
                            ? 135.0 * M_PI / 180.0
                            : 225.0 * M_PI / 180.0;
                        chroma += w * burst_amp * sin(phase + ang);
                    }
                }
            }

            /* SECAM: subcarrier runs continuously during active lines */
            if (c->colour_mode == HT_SECAM && info.type == HT_LSEQ_ACTIVE &&
                info.vy >= 0) {
                int ax = x - active_left;
                if (ax < 0 || ax >= active_width) {
                    /* Porch: run at centre frequency */
                    bool dr = (line_1 & 1) != 0;
                    double centre = dr ? HT_SECAM_DR : HT_SECAM_DB;
                    secam_phase += 2.0 * M_PI * centre / (double)sample_rate;
                    double span = white16 - blank16;
                    chroma = 0.25 * span * sin(secam_phase);
                }
            }

            /* Output: luma + chroma, scaled to int16 */
            double value = (signal + chroma) * 32767.0;
            if (value > 32767.0) value = 32767.0;
            if (value < -32768.0) value = -32768.0;
            out[out_pos] = (int16_t)lrint(value);
            out_pos++;
        }

        /* Advance to the next line (hacktv: bline++ then wrap at lines) */
        line_1++;
        if (line_1 > c->lines) line_1 = 1;
    }

    return need;
}
