/*
 * look.h - the film look, applied to 8-bit sRGB rows.
 *
 * WHAT THIS IS FOR. The camera's fast path grades the full-size JPEG that
 * DIGIC already made and embedded in the CR2. Until now that grade was a
 * per-channel 256-entry LUT and nothing else, applied by a `look_rows`
 * written out twice -- once in ml/payload/film.c and once in
 * tools/previewcheck.c. One implementation, here, because the desktop
 * preview is only worth having if it is the same code the camera runs.
 *
 * WHAT A PER-CHANNEL CURVE CANNOT DO, which is why this exists at all:
 *
 *   Saturation. A curve on R cannot know what G and B are doing, so it
 *   cannot pull a pixel toward or away from its own luma. "Portra, but
 *   more pastel" is simply unreachable with curves, and so is Velvia's
 *   punch beyond what steepening the curves fakes.
 *
 *   Split toning. Warm highlights with cool shadows means the tint has to
 *   depend on the pixel's brightness, which again is cross-channel.
 *
 *   Halation. The orange bloom around a highlight is light scattering off
 *   the film base and back through the emulsion -- it is a neighbourhood
 *   effect, and a LUT has no neighbours. It is also the whole character of
 *   a remjet-stripped motion picture stock.
 *
 * ORDER, and it matters: curves, then per-hue saturation, then global
 * saturation, then split tone, then halation, then grain. Halation comes after the grade because it models
 * light inside the film, which has already been through the response
 * curve; grain comes last because it is the physical emulsion and sits on
 * top of everything, including the glow.
 *
 * EVERYTHING IS INTEGER. ARM946E-S has no FPU and no hardware divide, and
 * the output has to be byte-identical between this desktop build and the
 * camera -- that identity is how the project knows a camera frame is
 * correct. Floats would be identical in practice here, but "in practice"
 * is not the standard the develop is held to.
 */
#ifndef SE_LOOK_H_INCLUDED
#define SE_LOOK_H_INCLUDED

#include "se.h"
#include "stock.h"
#include "lut.h"

/*
 * Chroma at which the per-hue controls reach full strength, 0..255.
 *
 * A pixel with no colour in it has no hue, so it must not receive a
 * hue-specific correction -- and below this, the correction fades out.
 *
 * Without that, the whole thing shatters flat areas. Measured on one
 * patch of pale sky through a window: median chroma 6, 70% of pixels at
 * 12 or less, and the hue classifier scattering them across ALL SIX
 * sectors -- 3219 px called red, 2134 cyan, 752 yellow. Each then got a
 * different rotation, saturation and luminance, and a smooth grey
 * gradient came out as hard posterised blotches of blue and orange.
 *
 * The hue of a near-neutral pixel is not wrong so much as meaningless:
 * one step of quantisation in a byte moves it a whole sector. 40 is
 * about 16% saturation -- below that nothing is really a colour, and
 * above it real colours are unaffected.
 */
/* Chroma at which the per-hue stage reaches full strength. It used to be 40,
 * but a real landscape sits at a mean chroma of ~18, so that knee was fading
 * nearly the whole frame out -- see the note at the fade in look.c. */
#define SE_HUE_FULL 20

/*
 * Grain amplitude, as a percentage of full range at GRAIN 100.
 *
 * Shared rather than written twice because tools/lookcheck.c computes the
 * look a second time, longhand, to check this one -- and the duplicated
 * logic there is the point of that test, but a duplicated CONSTANT is just
 * a way for the two to disagree about what they are comparing. They did:
 * this was changed from 72 to 38 here and not there, and 42 passing cases
 * turned into a row-by-row diff that looked like a ring buffer bug.
 */
#define SE_GRAIN_AMP 62

/* Tunables, resolved from a stock once and then used per pixel. */
typedef struct {
    uint8_t  map[3][256];    /* the per-channel curves, baked            */

    int      sat;            /* 256 = unchanged, 0 = grey, 512 = double  */

    /* 3x3 colour matrix, 8.8 fixed, row major; mat_any = 0 costs nothing. */
    int      mat[9];
    int      mat_any;

    /*
     * Saturation per hue sector: R, Y, G, C, B, M, 256 = unchanged,
     * interpolated between neighbours so nothing bands at a boundary.
     *
     * SE_HUE_SECTORS of them (twelve, 30 degrees apart), 256 = unchanged,
     * interpolated between neighbours so nothing bands at a boundary.
     *
     * This is the control that separates a film simulation from a tint.
     * Classic Chrome's whole character is dull greens and yellows over
     * reds that keep their punch; Eterna drains everything except what is
     * already dark. A single global saturation cannot say either of those
     * -- it moves every colour by the same amount -- and per-channel
     * curves cannot either, because a curve on R does not know what G and
     * B are doing at that pixel.
     */
    int      hue_sat[SE_HUE_SECTORS];

    /*
     * Hue rotation per sector, in 1536ths of a turn, signed. Positive
     * moves toward the next sector, clockwise through the wheel.
     *
     * This is the control that stops a stock reading as a filter laid
     * over the picture. Velvia does not merely saturate a green -- it
     * moves it toward blue-green and lets the blues go violet. Classic
     * Chrome takes greens toward olive. Those are different COLOURS, not
     * stronger ones, and no amount of saturation or curve will produce
     * them.
     */
    int      hue_rot[SE_HUE_SECTORS];

    /*
     * Luminance per sector, 256 = unchanged.
     *
     * Film layers have different speeds, so a colour does not just shift
     * -- it renders darker or lighter than its luminance says it should.
     * Velvia's blues and greens going DENSE is this, and it is most of
     * why its shadows look the way they do. Classic Chrome's greens sit
     * down in the frame for the same reason.
     */
    int      hue_lum[SE_HUE_SECTORS];

    int      hue_any;        /* 0 when nothing per-hue is set: skip it all */

    /*
     * Fuji's COLOR_CHROME and FX_BLUE as strengths in 0..256 (weak 128,
     * strong 256). Applied to chroma that is ALREADY there, in proportion to
     * how much there is, so a dull colour is left dull -- that is the whole
     * difference between these and the global saturation.
     */
    int      chrome;         /* 0 = off */
    int      fx_blue;        /* 0 = off, only hues around blue */

    /* Split tone. Signed per channel, in 0..255 units at full strength.
     * The shadow tint is weighted by (255 - luma) and the highlight tint
     * by luma, so they meet in the middle rather than fighting. */
    int      sh[3];
    int      hi[3];

    /* Halation. amount 0 disables it and costs nothing. */
    int      hal_amount;     /* 0..100, how much glow is added back      */
    int      hal_thresh;     /* luma above which a pixel glows, 0..255   */
    int      hal_radius;     /* glow radius in pixels, 1..48             */
    int      hal_tint[3];    /* its colour, 0..255; red for remjet-less */

    /* Grain. The raw path has had this since the beginning; the JPEG path
     * never did, which meant the stocks' own GRAIN line did nothing at all
     * on the only path the camera has ever finished a frame on. */
    /*
     * How much of the look to apply, 0..100. The menu has had a Strength
     * slider since the beginning, but it only ever reached the LiveView path
     * (fs_core_build_luts) -- a developed file was always 100% whatever it
     * said. That is a lie in the UI, so it is honoured here too: the graded
     * pixel is blended back toward the one that came in, and the grain and
     * halo amounts scale with it.
     */
    int      strength;       /* 0..100, 100 = the stock as written       */
    int      grain;          /* 0..100, 0 = none                         */
    int      grain_cell;     /* 1..9 pixels at full size                 */
    int      grain_scale;    /* the decode scale the cell was divided by, so an
                              * override can recompute it the same way      */
} se_look;

/* Resolve a stock into the above. `scale` is the decode scale (1, 2 or 4):
 * grain and halation are in output pixels, so both shrink with the image
 * or a half-size frame would come out twice as grainy. */
void se_look_from_stock(const se_stock *s, int scale, se_look *lk);

/* How much of the look to apply, 0..100. After se_look_from_stock. */
void se_look_set_strength(se_look *lk, int strength);

/* Saturation on top of the stock's own, 100 = as written. Applies to the
 * baked luma/chroma tables as well, so both paths move together. */
void se_look_set_satboost(se_look *lk, int boost);

/* Override the stock's GRAIN_SIZE (0..100). -1 leaves the stock's own value.
 * The cell is recomputed against the same decode scale the stock used, so a
 * half-size frame stays half as grainy. */
void se_look_set_grain_size(se_look *lk, int size);

/* --------------------------------------------------- shot-time tables -- */
/*
 * The same look, flattened into the three tables a UYVY frame can be graded
 * with in one pass: a luma table, a chroma gain plus U/V offsets per luma, and
 * a chroma gain per (U,V) cell.
 *
 * These are BAKED BY RUNNING THE GRADE, not by approximating it from the
 * recipe: every table entry comes out of grade_row() on a synthetic pixel, so
 * the hue rotation, the chroma rolloff and the gamut fit are all in there.
 * That is the difference between an approximation that drifts and one that
 * cannot: an earlier hand-derived version was 7.6/8.0/13.8 levels of mean
 * error away from the develop on a real frame, because it had to guess how the
 * per-channel curves combine.
 */
#define SE_YUV_GRID  64                    /* cells per chroma axis        */
#define SE_YUV_CELLS (SE_YUV_GRID*SE_YUV_GRID) /* 64 x 64 over U,V         */
typedef struct {
    uint8_t y[256];                        /* graded luma at each luma      */
    uint8_t gain[256];                     /* chroma gain, Q6, 64 = unity   */
    int8_t  uoff[256], voff[256];          /* chroma offsets at each luma   */
    int16_t matrix[4];                     /* 2x2 chroma matrix, Q8, 256 = 1.0 */
    uint8_t cg[SE_YUV_CELLS];              /* extra gain per hue cell, Q6   */
    int8_t  dy[SE_YUV_CELLS];              /* extra luma per hue cell       */
    int     strength;                      /* 0..100 applied when grading   */
} se_yuvtbl;

/*
 * A stock's colour can live in a 3D cube rather than in the .fs. Develop
 * applies that cube as its own stage before the look, so the bake has to as
 * well or LiveView and the developed file disagree for every such stock.
 * Set it before baking, clear it after; NULL means "no cube".
 */
void se_look_set_bake_cube(const se_lut3 *c);

void se_look_bake_yuv(const se_look *lk, se_yuvtbl *t);

/* ------------------------------------------------- joint (y,u,v) lattice -- */
/*
 * The LiveView form: a tone curve plus ONE lattice indexed by all three of
 * (y, Cb, Cr) at once, holding the graded (Cb, Cr).
 *
 * This exists because the separable form cannot express the two relations a
 * look actually uses -- hue rotation and chroma that depends on luma and hue
 * together -- and the measurement says so: against the same develop rows on
 * the same samples, the separable shot table is 7.7-29.7 levels out while a
 * joint lattice of the same size is 5.3-10.9 (se/hosttest/tableerr.c).
 *
 * Indexed y fastest, then u, then v -- the order the module's apply loop
 * walks, so its inner interpolation stays in one cache line.
 */
/*
 * The axes are NOT all the same size, and that is measured, not assumed: a
 * photograph uses the whole 0..255 luma range but its chroma sits near zero,
 * so equal steps on Cb and Cr waste most of their nodes on colours no scene
 * contains. A 8^3 lattice costs 11.4 levels mean on a real frame while the
 * separable form gets 7.0; widening only the chroma axes fixes that at the
 * same node count per axis (se/hosttest/lv3d.c).
 */
#ifndef SE_YUV3_NY
#define SE_YUV3_NY 12                      /* nodes along luma              */
#endif
#ifndef SE_YUV3_NC
#define SE_YUV3_NC 17                      /* nodes along Cb, and along Cr  */
#endif
#define SE_YUV3_NODES (SE_YUV3_NY * SE_YUV3_NC * SE_YUV3_NC)
typedef struct {
    uint8_t y[256];                        /* graded luma at each luma      */
    /*
     * Per node: how much brighter or darker this COLOUR grades than grey of
     * the same luma (dy), and where its chroma goes (Cb, Cr).
     *
     * dy is not optional. Without it the lattice fixed chroma (dCb 0.38
     * against the develop) but left luma 3.4x worse than the separable form
     * (dY 3.50 vs 1.04), because a saturated pixel does not grade to the same
     * brightness as a grey one. That is the hue->luma relation the separable
     * form carries as dy[cell].
     */
    int16_t v[SE_YUV3_NODES * 3];          /* dy, Cb, Cr per (y,u,v)        */
} se_yuv3;

void se_look_bake_yuv3(const se_look *lk, se_yuv3 *t);

/* ----------------------------------------------------- streaming ----- */
/*
 * Halation needs rows above and below the one being written, and the
 * decoder hands rows over once, in order, and keeps none. So the run holds
 * a delay line: rows go in, and come out `radius` rows later, by which
 * time their neighbourhood has arrived. Without halation the delay is
 * zero and a row goes straight through.
 */
typedef struct se_look_run se_look_run;

/* How many bytes se_look_begin will take from the arena, so a caller can
 * check it has them before committing. */
size_t se_look_bytes(const se_look *lk, int width);

/*
 * What the look stage may spend, and why there is a cap at all.
 *
 * The delay line is 2*radius+1 rows of graded RGB plus the same number of
 * rows of mask. At 2496 columns and radius 16 that is about 340 KB -- more
 * than the camera's whole decode arena, which the decoder needs roughly
 * width*16*6 of (240 KB at that width). So the look gets a fixed slice and
 * the radius is reduced until it fits: 192 KB leaves radius 8 at full size
 * and 18 at half, and puts the camera's worst-case total at 528 KB, under
 * the 592 KB single run that body has already granted.
 *
 * This lives here, and not in the camera payload where the number came
 * from, because the desktop preview has to reduce the radius *identically*
 * or the two produce different pixels -- and the whole point of the shared
 * look is that a desktop hash proves a camera frame.
 */
#define SE_LOOK_BUDGET (192 * 1024)

/* Reduce the halation radius until the run fits `budget`, disabling
 * halation outright if even radius 1 does not. Returns the final cost, so
 * a caller can size its allocation from it. */
size_t se_look_fit(se_look *lk, int width, size_t budget);

se_err se_look_begin(se_look_run **run, const se_look *lk,
                     int width, se_arena *ar);

/* Each finished row is handed to `emit`, which returns 0 to continue. */
typedef int (*se_look_emit)(void *ctx, const uint8_t *row, int width);

se_err se_look_rows(se_look_run *run, const uint8_t *rgb, int nrows,
                    se_look_emit emit, void *ctx);

/* Flush whatever the delay line is still holding. Must be called, or the
 * encoder is short by `radius` rows. */
se_err se_look_end(se_look_run *run, se_look_emit emit, void *ctx);

/* Look-stage timers; see look.c. */
extern uint32_t se_lk_t_grade, se_lk_t_mask, se_lk_t_glow, se_lk_t_grain;
extern uint32_t se_lk_n_rows;

#endif /* SE_LOOK_H_INCLUDED */
