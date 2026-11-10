/*
 * stock.h - a film stock: per-channel tone response, and nothing else.
 *
 * This is deliberately the exact set of operations that can be applied to
 * MOSAICED sensor data, where every sample is one known colour and no
 * pixel has all three. That constraint is what lets DIGIC do the expensive
 * work: we rewrite the raw buffer in place, then Canon's hardware
 * demosaics, matrixes and JPEGs it at full speed.
 *
 * Expressible here:   per-channel curves, white balance, exposure,
 *                     black point, contrast, matte lift, highlight rolloff
 * Not expressible:    3D LUTs, HSL, saturation, hue rotation -- anything
 *                     that needs R, G and B at the same pixel
 *
 * That is less limiting than it sounds. Film is three emulsion layers, each
 * with its own characteristic curve; per-channel response IS the mechanism.
 *
 * A stock bakes down to three 1D tables. On the camera that is one lookup
 * per sample: ~13 M lookups, seconds rather than minutes.
 */
#ifndef SE_STOCK_H
#define SE_STOCK_H

#include "se.h"
#include "grade.h"
#include "lut.h"

/*
 * What generation of the stock format this engine speaks.
 *
 *   1  per-channel curves, WB, exposure, grain
 *   2  + SAT, SPLIT_SH/HI, HALATION, HAL_TINT, GRAIN_SIZE
 *   3  + HUE_SAT, HUE_ROT, HUE_LUM, and the [RAW] override block
 *
 * This exists because unknown keys are ignored on purpose, so a stock
 * written by something newer than the engine reading it produces no
 * error and the wrong picture. Six changes to engine/look.c in one
 * session each altered the output of an unchanged .fs file, and two of
 * them added keys. Someone tuning a stock in one place and shooting it
 * in another has no way to tell, and will assume they mis-tuned it.
 *
 * So a file may say which generation it was written for, and the engine
 * says so when it cannot keep up.
 */
/*
 *   4  + MATRIX, the 3x3 colour matrix
 */
#define SE_STOCK_VERSION 4

#define SE_STOCK_NAME 24
#define SE_STOCK_DESC 48

/*
 * What the raw path may do differently.
 *
 * The two develop paths do not see the same picture. The JPEG path grades
 * the preview DIGIC already made -- which has been through the camera's
 * Picture Style, so it arrives contrasty and saturated. The raw path
 * rewrites scene-linear mosaiced samples, and DIGIC applies its Picture
 * Style AFTERWARDS, on top of our work. One set of curve points cannot be
 * right for both, and until now a stock had only one set: the same points
 * evaluated in the linear domain for raw and the sRGB domain for JPEG.
 *
 * So a stock can override, for raw only, exactly the things the raw path
 * is physically able to do. Saturation, the per-hue controls, split
 * toning and halation are absent from this struct on purpose -- they need
 * all three channels at one pixel, and before demosaic no pixel has them.
 *
 * Only the raw side exists, not a matching JPEG side: the top level of
 * the file IS the JPEG look, because that is the path the camera uses
 * unless told otherwise. `[RAW]` says "and when it comes from the sensor,
 * these instead".
 */
typedef struct {
    se_curve master, r, g, b;    /* n < 2 means "no override"      */
    float exposure;   int exposure_set;
    float grain;      int grain_set;
    float grain_size; int grain_size_set;
    int   wb_preset;  int wb_set;
} se_stock_raw;

/*
 * Hue sectors per stock: twelve, 30 degrees apart, so every bead on a
 * 12-colour wheel has a knot of its own. A file that writes six values is
 * expanded on load (see parse_hue_vec in stock.c), so nothing already on a
 * card changes meaning.
 */
#define SE_HUE_SECTORS 12

typedef struct {
    /* The format generation this file asked for; 0 when it did not say,
     * which means it predates the key and is read as generation 1. */
    int   version;

    char  name[SE_STOCK_NAME];
    char  desc[SE_STOCK_DESC];

    int   wb_preset;        /* se_wb_preset; -1 = leave as shot */
    float exposure;         /* stops */

    se_curve master;        /* applied to all three channels   */
    se_curve r, g, b;       /* then each channel on its own    */

    /*
     * CURVE_Y, applied after the three per-channel curves: a curve on the
     * pixel's luma, with the colour ratios preserved. It exists because the
     * per-channel curves cannot separate "how bright" from "how warm" -- a
     * move on B to cool the shadows also cools every midtone, and the
     * sky/water regions end up fighting the skin. n < 2 means identity, so
     * every recipe written before this key behaves exactly as it did.
     */
    se_curve luma;

    /*
     * Everything below is applied by engine/look.c on the JPEG path, where
     * all three channels are present at every pixel. The raw path cannot
     * use any of it: it rewrites MOSAICED samples, one colour per pixel,
     * so it has no neighbours and no cross-channel anything. A stock that
     * leans on these will look different developed from raw, and that is a
     * property of the sensor data, not a bug to fix here.
     */

    float grain;            /* 0..100, applied after DIGIC; 0 = none */
    float grain_size;       /* 0..100 -> a 1..5 pixel cell           */

    /*
     * Fuji's two colour-character controls, which a recipe names explicitly
     * and which no curve can express.
     *
     * COLOR_CHROME deepens colours that are ALREADY saturated and leaves dull
     * ones alone -- it is a statement about the shape of the chroma response,
     * not its level, which is why a global saturation cannot stand in for it.
     * FX_BLUE is the same statement restricted to blues.
     *
     * 0 = off, 1 = weak, 2 = strong, the three values a Fuji body offers.
     */
    int color_chrome;
    int fx_blue;

    /* Saturation, 100 = unchanged. `sat_set` distinguishes "SAT 0",
     * which means monochrome, from a stock file that never said SAT. */
    float sat;
    int   sat_set;

    /*
     * Saturation per hue sector, 100 = unchanged. Defaulted in
     * se_stock_init, not by the memset, because 0 here means "make that hue
     * grey" and silence must not mean that.
     *
     * TWELVE sectors, 30 degrees apart, not six. Six is enough to model a
     * stock (a 12-knot fit only takes the residual from 2.1 to 0.8 levels,
     * and the whole hue stage is smaller than that), but not enough to
     * CONTROL one: a 12-colour wheel like the OM System's puts a bead every
     * 30 degrees, and with six knots every bead would sit exactly halfway
     * between two of them and move its neighbours by half as much as
     * itself. A file that still writes six values is expanded here, so
     * nothing already on the card changes meaning.
     */
    float hue_sat[SE_HUE_SECTORS];

    /* Hue rotation per sector, in DEGREES, signed. Positive moves toward
     * the next sector, i.e. clockwise through the wheel. This is what makes
     * a stock a different colour rather than a stronger one. */
    float hue_rot[SE_HUE_SECTORS];

    /* How bright each hue renders, 100 = unchanged. Film layers have
     * different speeds; this is why Velvia's blues go dense. */
    float hue_lum[SE_HUE_SECTORS];

    /* Split tone, signed, in 0..255 output units at full weight. The
     * shadow triple is weighted by (255 - luma), the highlight triple by
     * luma, so a warm-highlight/cool-shadow stock is two lines. */
    float split_sh[3];
    float split_hi[3];

    /* Halation: light scattering off the film base and back up through the
     * emulsion. amount 0 disables it, and then it costs nothing at all --
     * no delay line is allocated. */
    float hal_amount;       /* 0..100                                 */
    float hal_thresh;       /* luma a pixel must beat to glow, 0..255 */
    float hal_radius;       /* pixels, 1..48, at full decode scale    */
    float hal_tint[3];      /* the glow's colour, 0..255              */

    /*
     * A 3x3 colour matrix, applied to the three curves' outputs before the
     * per-hue stage. 8.8 fixed point, 256 = identity, row major.
     *
     * This is the cheapest way to say what a curve cannot: that a colour is
     * pulled toward another channel. A film LUT carries that crosstalk at
     * full resolution; a matrix carries its linear part, and the per-hue
     * tables carry the rest. Nine multiplies per pixel, skipped entirely at
     * identity, which is why it fits the camera's budget where a 3D LUT does
     * not.
     */
    int   matrix[9];
    int   matrix_set;

    /* The same matrix at the white end. The look interpolates between the
     * two by luma, so one stage carries a luma-shaped crosstalk instead of a
     * single linear one. 18 multiplies, still no lattice. */
    int   matrix_hi[9];
    int   matrix_hi_set;

    /* Overrides applied only when developing from the sensor data. */
    se_stock_raw raw;
} se_stock;

/* Which develop path is asking. */
#define SE_SRC_JPEG 0
#define SE_SRC_RAW  1

/* The grain a stock wants from a given path, honouring any [RAW]
 * override. The curves are picked inside the two bake functions, which
 * already know which path they are -- this is the one value a caller
 * outside them needs. */
float se_stock_grain(const se_stock *s, int src);
float se_stock_grain_size(const se_stock *s, int src);
int   se_stock_wb(const se_stock *s, int src);

void se_stock_init(se_stock *s);

/*
 * Parse the text format:
 *
 *   VERSION   3
 *   NAME      VELVIA
 *   DESC      SATURATED TRANSPARENCY
 *   WB        DAYLIGHT
 *   EXPOSURE  +0.10
 *   CURVE     0,0 22,16 127,127 255,255
 *   CURVE_B   0,0 52,60 255,255
 *   GRAIN     20
 *   GRAIN_SIZE 30
 *   SAT       120
 *   HUE_SAT   110,90,70,62,55,67,80,88,95,100,105,108  ; 12 knots, 30 deg
 *   HUE_ROT   0,-4,-8,-11,-14,-4,6,8,10,5,0,0          ; degrees, + = next
 *   HUE_LUM   104,100,96,92,88,93,98,98,82,91,100,102  ; how bright
 *
 * The hue lines take 12 values, one every 30 degrees, in this order:
 * R, RY, Y, YG, G, GC, C, CB, B, BM, M, MR (100 = unchanged for SAT and LUM).
 * A file that writes six (R Y G C B M) still loads: each written value lands
 * on its 60-degree knot and the midpoint between two is the average of them,
 * which is exactly what tools/hue12.py writes out. Every stock in films/ is
 * shipped in the 12-value form, so a bead on the Colour Creator's wheel lands
 * on a knot and moves only itself.
 *   SPLIT_SH  -6,0,10
 *   SPLIT_HI  10,4,-6
 *   MATRIX    300,-24,-20, -18,280,-6, -4,-30,290  ; 3x3, 256 = identity
 *   MATRIX_HI 280,-14,-10, -10,270,-6, -2,-20,272  ; the same at white
 *   HALATION  40 190 14      ; amount, threshold, radius
 *   HAL_TINT  255,70,30
 *
 * Unknown keys are ignored so a newer stock file still loads on older code.
 *
 * Everything above applies to the JPEG path, which is what the camera
 * uses unless told otherwise. A `[RAW]` line starts a block that
 * overrides, for the sensor-data path only, the handful of things that
 * path can actually do:
 *
 *   [RAW]
 *   CURVE     0,0 20,14 128,132 255,255
 *   EXPOSURE  +0.15
 *   GRAIN     12
 *
 * `[JPEG]` returns to the common block. Keys that need all three colour
 * channels -- SAT, HUE_*, SPLIT_*, HALATION -- are refused inside [RAW]
 * with a warning, because the raw path cannot honour them.
 */
se_err se_stock_parse(const char *text, size_t len, se_stock *s);

/*
 * Bake to three channel tables mapping white-balanced linear 16-bit in to
 * linear 16-bit out. The curves are authored in a display-referred domain
 * (that is where anyone editing them can reason about them), so the bake
 * encodes, applies the curve, and decodes back to linear.
 */
/*
 * Bake the look into three tables.
 *
 * se_stock_bake is linear in, linear out: for the raw pipeline, which hands
 * it scene-linear sensor data.
 *
 * se_stock_bake_srgb is sRGB in, sRGB out: for the preview pipeline, which
 * works on the JPEG DIGIC already made. Using the wrong one of these is not
 * a subtle difference -- it evaluates the curve at the wrong point and
 * gamma-decodes the result, which made every curved stock dark and muddy.
 */
void se_stock_bake(const se_stock *s, se_lut1 out[3]);
void se_stock_bake_srgb(const se_stock *s, se_lut1 out[3]);

/* The sRGB bake straight to the 256-entry byte tables an 8-bit path wants.
 * Same curve, no se_lut1 in between. */
void se_stock_bake_srgb8(const se_stock *s, uint8_t out[3][256]);

#endif /* SE_STOCK_H */
