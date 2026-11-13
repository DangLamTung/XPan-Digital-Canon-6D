/**
 * Film simulation core - pure C, no Magic Lantern dependencies.
 *
 * Shared by the module (modules/film_sim/film_sim.c) and the host
 * simulator (tools/sim), so what you see in the simulation is exactly
 * what the camera runs.
 *
 * Chroma convention: on DIGIC 4/5 (6D included) the UYVY buffers hold U and V
 * as two's complement signed bytes, neutral = 0x00 -- see imgconv.h
 * UYVY_GET_U() and imgconv.c, which casts them to int8_t before use.
 *
 * Pipeline, per pixel (UYVY 4:2:2, signed U/V, neutral = 0):
 *
 *   Y  -> filmic S-curve -> shadow lift -> highlight shoulder
 *   UV -> hue matrix -> per-luma saturation -> split toning
 *   Y  -> grain
 */

#ifndef FS_CORE_H
#define FS_CORE_H

#include <string.h>

enum
{
    FS_PRESET_NEUTRAL = 0,
    FS_PRESET_FUJI_SUPERIA,     /* Fuji Superia 400 */
    FS_PRESET_FUJI_CLASSIC_NEG, /* Fuji Classic Negative */
    FS_PRESET_KODAK_GOLD,       /* Kodak Gold 200 */
    FS_PRESET_FUJI_PRO400H,     /* Fuji Pro 400H / Cool Neg */
    FS_PRESET_KODAK_PORTRA,     /* Kodak Portra 400 / Warm Neg */
    FS_PRESET_FUJI_VELVIA,      /* Fuji Velvia 50 / Vivid Slide */
    FS_PRESET_CINEMA,           /* Kodak Vision3 / CineStill 800T */
    FS_PRESET_BW,               /* Kodak Tri-X 400 / B&W Classic */
    FS_PRESET_TUNGSTEN,         /* Tungsten 64T */
    FS_PRESET_COUNT
};

typedef struct
{
    int contrast;       /* -100..100 */
    int black;          /*    0..100, shadow lift */
    int shoulder;       /*    0..100, highlight compression */
    int saturation;     /*    0..200, 100 = unchanged */
    int hue;            /* -180..180 degrees */
    int shadow_cb;      /*  -32..32, chroma offset in shadows */
    int shadow_cr;
    int high_cb;        /*  -32..32, chroma offset in highlights */
    int high_cr;
    int hi_desat;       /*    0..100, desaturate highlights */
} fs_params_t;

static const fs_params_t fs_presets[FS_PRESET_COUNT] =
{
    /*                          con  blk  sho  sat  hue  s_cb s_cr  h_cb h_cr  hds */
    /* Neutral             */ {   0,   0,   0, 100,   0,    0,   0,    0,   0,   0 },
    /* Fuji Superia 400    */ {  32,  10,  30,  92,   5,   10, -12,   -4,   6,  40 },
    /* Fuji Classic Neg    */ {  28,  15,  20,  88,  -4,    4,   6,   -6,  10,  45 },
    /* Kodak Gold 200      */ {  18,  12,  28, 106,  -6,   -4,   9,   -8,  14,  18 },
    /* Fuji Pro 400H       */ {  18,   8,  25,  96,   3,    6,  -6,    2,  -3,  30 },
    /* Kodak Portra 400    */ {  15,  10,  25,  90,  -3,   -2,   3,   -4,   8,  35 },
    /* Fuji Velvia 50      */ {  45,   0,  10, 140,   0,   -8,   2,    8,  -2,   0 },
    /* Cinema 800T         */ {  25,  12,  35,  85,  -6,   14, -18,  -10,  14,  45 },
    /* B&W Tri-X 400       */ {  50,  10,  30,   0,   0,    0,   0,    0,   0,   0 },
    /* Tungsten 64T        */ {  18,  14,  30,  80,   8,   12, -14,   -8,  16,  50 },
};

static const char * const fs_preset_names[FS_PRESET_COUNT] =
{
    "Neutral",
    "Fuji Superia 400",
    "Fuji Classic Neg",
    "Kodak Gold 200",
    "Fuji Pro 400H",
    "Kodak Portra 400",
    "Fuji Velvia 50",
    "Cinema 800T",
    "B&W Tri-X 400",
    "Tungsten 64T",
};

typedef struct
{
    unsigned char curve[256];   /* luma response, 8-bit (3D path) */
    unsigned short curve16[256];/* same curve at 16-bit, for dithering */
    unsigned char sat[256];     /* saturation, 7.7 fixed point (128 = 1.0) */
    short cb_off[256];          /* split toning, Cb */
    short cr_off[256];          /* split toning, Cr */
    short matrix[4];            /* hue rotation, 8.8 fixed point */
    int grain_amp;              /* 0..85 */
} fs_lut_t;

__attribute__((always_inline)) static inline int fs_clamp_i(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int fs_sin_deg(int deg)
{
    while (deg > 180) deg -= 360;
    while (deg < -180) deg += 360;
    int sign = 1;
    if (deg > 90) deg = 180 - deg;
    else if (deg < -90) deg = -180 - deg;
    if (deg < 0) { deg = -deg; sign = -1; }
    float x = deg * 0.01745329252f;
    float x2 = x * x;
    /* Five terms stop one unit short at 90 degrees, which is where cos() is
     * evaluated: fs_cos_deg(0) came out as 257 instead of 256, so the hue
     * matrix carried a 0.4% gain and the "Neutral" preset shifted every
     * negative chroma sample by one level. The seventh term costs nothing
     * outside this loop and puts sin(90) at 255.94. */
    return sign * (int)((x - x * x2 / 6.0f + x * x2 * x2 / 120.0f
                         - x * x2 * x2 * x2 / 5040.0f) * 256.0f + 0.5f);
}

static int fs_cos_deg(int deg)
{
    return fs_sin_deg(deg + 90);
}

/*
 * Saturation on top of the stock's own value, 100 = as the table is written.
 *
 * The module sets this from its Saturation slider so the preview and the
 * developed file obey the same control. It used to reach only the develop
 * engine (se_look_set_satboost in the camera glue), while this file -- the one
 * that draws LiveView -- never saw it, so moving the slider changed the written
 * file and left the panel alone even though the menu said otherwise.
 *
 * Per translation unit, like the rest of this header: the tools and the test
 * frames keep 100.
 */
static int fs_core_satboost = 100;

/* 8x8 ordered dither, scaled to one 8-bit step in 16-bit units. The develop
 * path has used this since the beginning (se_develop_dither_row); the in-shot
 * YUV grade did not, which is what made skies band. */
static const unsigned char fs_core_dither[8][8] = {
    {  0, 128,  32, 160,   8, 136,  40, 168},
    {192,  64, 224,  96, 200,  72, 232, 104},
    { 48, 176,  16, 144,  56, 184,  24, 152},
    {240, 112, 208,  80, 248, 120, 216,  88},
    { 12, 140,  44, 172,   4, 132,  36, 164},
    {204,  76, 236, 108, 196,  68, 228, 100},
    { 60, 188,  28, 156,  52, 180,  20, 148},
    {252, 124, 220,  92, 244, 116, 212,  84},
};

static void fs_core_build_luts(fs_lut_t *lut, const fs_params_t *p,
                               int strength, int grain)
{
    int s = fs_clamp_i(strength, 0, 100);

    float contrast = p->contrast / 100.0f;
    float lift     = p->black / 100.0f * 0.20f;
    float shoulder = p->shoulder / 100.0f * 0.5f;

    for (int i = 0; i < 256; i++)
    {
        float x = i / 255.0f;

        /* filmic S-curve: 0 and 1 stay fixed, slope 2 at mid grey.
         * Rational form on purpose: no libm, and no x*x*x (gcc with
         * -ffast-math turns that into __powisf2, which the camera
         * firmware does not export). */
        float xr = 1.0f - x;
        float sc = (x * x) / (x * x + xr * xr);
        float v = x + contrast * (sc - x);

        /* shadow lift */
        v = v + lift * (1.0f - v) * (1.0f - v);

        /* highlight shoulder (rational, no libm) */
        v = v * (1.0f + shoulder) / (1.0f + shoulder * v);

        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);

        int y = (int)(v * 255.0f + 0.5f);

        /* blend toward the identity curve with the strength setting */
        lut->curve[i] = (y * s + i * (100 - s)) / 100;
        lut->curve16[i] = (unsigned short)(lut->curve[i] * 256);
    }

    float sat = (1.0f + (p->saturation / 100.0f - 1.0f) * s / 100.0f)
                * (float)fs_clamp_i(fs_core_satboost, 20, 200) / 100.0f * 128.0f;
    float hds = p->hi_desat / 100.0f * s / 100.0f;

    float scb = p->shadow_cb * s / 100.0f;
    float scr = p->shadow_cr * s / 100.0f;
    float hcb = p->high_cb   * s / 100.0f;
    float hcr = p->high_cr   * s / 100.0f;

    for (int i = 0; i < 256; i++)
    {
        float t = i / 255.0f;
        float sh = (1.0f - t) * (1.0f - t);
        float hi = t * t;

        float si = sat;
        /* Highlight desaturation rolloff (prevents neon digital highlights) */
        if (t > 0.6f) si *= 1.0f - hds * (t - 0.6f) / 0.4f;
        /* Deep shadow toe desaturation (prevents chroma noise blotches in deep shadows) */
        if (t < 0.12f) si *= t / 0.12f;

        lut->sat[i] = (unsigned char)fs_clamp_i((int)(si + 0.5f), 0, 255);
        lut->cb_off[i] = (short)fs_clamp_i((int)(scb * sh + hcb * hi), -128, 127);
        lut->cr_off[i] = (short)fs_clamp_i((int)(scr * sh + hcr * hi), -128, 127);
    }

    int co = fs_cos_deg(p->hue * s / 100);
    int sn = fs_sin_deg(p->hue * s / 100);

    lut->matrix[0] = (short) co;
    lut->matrix[1] = (short)-sn;
    lut->matrix[2] = (short) sn;
    lut->matrix[3] = (short) co;

    lut->grain_amp = fs_clamp_i(grain, 0, 100) * 255 / 100 / 3;
}

static inline int fs_chroma_signed(unsigned int byte)
{
    return (signed char) byte;
}

/* always_inline, because at -Os gcc turns every call of this into a `bl`:
 * one function call per UYVY pair, 172,800 of them per LiveView frame. The
 * ARM listing of the LV loop showed exactly that, with the matrix coefficients
 * and table pointers spilled to the stack around each call. */
__attribute__((always_inline)) static inline unsigned int fs_pack_px(int u, int y0, int v, int y1)
{
    return ((unsigned int)(unsigned char)(signed char) fs_clamp_i(u, -128, 127))
         | (unsigned int)(fs_clamp_i(y0, 0, 255) <<  8)
         | ((unsigned int)(unsigned char)(signed char) fs_clamp_i(v, -128, 127) << 16)
         | (unsigned int)(fs_clamp_i(y1, 0, 255) << 24);
}


/* Aspect mode -> ratio. 0 means "no bars". */
static int fs_core_aspect_ratio(int mode, int *num, int *den)
{
    switch (mode) {
    case 1: *num = 1;  *den = 1;  return 1;   /* 1:1   */
    case 2: *num = 5;  *den = 4;  return 1;   /* 4:5   */
    case 3: *num = 16; *den = 9;  return 1;   /* 9:16  */
    case 4: *num = 9;  *den = 16; return 1;   /* 16:9  */
    case 5: *num = 24; *den = 65; return 1;   /* XPan 65:24 */
    /* Cinemascope: the 2.39:1 of an anamorphic print. Stated as 100:239 rather
     * than a float, because the bars are placed with integer maths. */
    case 6: *num = 100; *den = 239; return 1;
    default: return 0;
    }
}

/*
 * Black bars for a num:den frame inside a width_px wide, row0..row1 tall area of
 * a UYVY image. Shared by the LiveView overlay and the in-shutter burn, so the
 * preview and the file cannot disagree about where the frame is.
 *
 * Chroma is a signed byte with 0 neutral on these buffers, so black is a zero
 * fill. A UYVY word carries the chroma of two pixels, so the side-bar edges are
 * rounded to even pixels: an odd edge zeroes the U of one pair and leaves its V,
 * which paints a one-column colour error down the whole bar.
 */
static void fs_core_aspect_bars(void *dst, int pitch, int width_px,
                                int row0, int row1, int num, int den)
{
    int active = row1 - row0;
    int box_h, box_w, left, right;

    if (!dst || pitch <= 0 || width_px <= 0 || active <= 0 || num <= 0 || den <= 0)
        return;

    box_h = (width_px * num) / den;
    if (box_h <= 0)
        return;

    if (box_h < active) {
        int top = row0 + (active - box_h) / 2;
        int bot = top + box_h;
        for (int y = row0; y < top; y++)
            memset((char *)dst + (size_t)y * pitch, 0, (size_t)pitch);
        for (int y = bot; y < row1; y++)
            memset((char *)dst + (size_t)y * pitch, 0, (size_t)pitch);
        return;
    }

    box_w = (active * den) / num;
    if (box_w <= 0 || box_w >= width_px)
        return;
    left = ((width_px - box_w) / 2) & ~1;
    right = (left + box_w + 1) & ~1;
    if (right > width_px)
        right = width_px;
    for (int y = row0; y < row1; y++) {
        unsigned char *r = (unsigned char *)dst + (size_t)y * pitch;
        if (left > 0)
            memset(r, 0, (size_t)left * 2);
        if (right < width_px)
            memset(r + right * 2, 0, (size_t)(width_px - right) * 2);
    }
}

/* Process rows [row0, row1) of a UYVY frame.  pitch is in bytes. */
static void fs_core_process_rows(const fs_lut_t *lut, const void *src, void *dst,
                                 int pitch, int row0, int row1, unsigned int *rnd_state)
{
    int words = pitch >> 2;
    unsigned int rnd = *rnd_state;

    const unsigned int * sp = (const unsigned int *)src + (unsigned int)row0 * words;
    unsigned int * dp = (unsigned int *)dst + (unsigned int)row0 * words;

    /*
     * Everything the loop needs is copied out of `lut` first.
     *
     * `dp` writes into the caller's buffer, and the compiler cannot prove that
     * buffer does not overlap `lut`, so with `lut->x` written inline it reloads
     * the pointer and the value after every single store -- five table loads
     * and four matrix coefficients per pixel pair, re-read for no reason. The
     * locals below are loop-invariant by construction, and the output is
     * unchanged: same arithmetic, same order.
     */
    const unsigned short *curve16 = lut->curve16;
    const unsigned char *sat_t = lut->sat;
    const short *cb_t = lut->cb_off;
    const short *cr_t = lut->cr_off;
    const int m0 = lut->matrix[0], m1 = lut->matrix[1];
    const int m2 = lut->matrix[2], m3 = lut->matrix[3];
    const int grain = lut->grain_amp;
    /* A stock with no hue rotation has matrix = [256,0,0,256], where the
     * multiply is the identity. Most B&W and flat stocks do, and the LiveView
     * path is arithmetic-bound (89 ms of a 101 ms pass), so skip it. */
    const int idmat = (m0 == 256 && m1 == 0 && m2 == 0 && m3 == 256);

    for (int y = row0; y < row1; y++)
    {
        if (!grain)
        {
            for (int i = 0; i < words; i++)
            {
                unsigned int px = sp[i];

                int u  = fs_chroma_signed(px);
                int ya = (px >>  8) & 0xFF;
                int v  = fs_chroma_signed(px >> 16);
                int yb = (px >> 24) & 0xFF;

                int oa16 = curve16[ya];
                int ob16 = curve16[yb];
                int ym = (oa16 + ob16) >> 9;
                int dx = (i * 2) & 7;
                int dy = y & 7;
                int oa = (oa16 + fs_core_dither[dy][dx]) >> 8;
                int ob = (ob16 + fs_core_dither[dy][dx + 1]) >> 8;

                int nu, nv;
                if (idmat) {
                    nu = u;
                    nv = v;
                } else {
                    nu = ((m0 * u + m1 * v) >> 8);
                    nv = ((m2 * u + m3 * v) >> 8);
                }

                int sat = sat_t[ym];
                nu = ((nu * sat) >> 7) + cb_t[ym];
                nv = ((nv * sat) >> 7) + cr_t[ym];

                dp[i] = fs_pack_px(nu, oa, nv, ob);
            }
        }
        else
        {
            for (int i = 0; i < words; i++)
            {
                unsigned int px = sp[i];

                int u  = fs_chroma_signed(px);
                int ya = (px >>  8) & 0xFF;
                int v  = fs_chroma_signed(px >> 16);
                int yb = (px >> 24) & 0xFF;

                int oa16 = curve16[ya];
                int ob16 = curve16[yb];
                int ym = (oa16 + ob16) >> 9;
                int dx = (i * 2) & 7;
                int dy = y & 7;
                int oa = (oa16 + fs_core_dither[dy][dx]) >> 8;
                int ob = (ob16 + fs_core_dither[dy][dx + 1]) >> 8;

                int nu = ((m0 * u + m1 * v) >> 8);
                int nv = ((m2 * u + m3 * v) >> 8);

                int sat = sat_t[ym];
                nu = ((nu * sat) >> 7) + cb_t[ym];
                nv = ((nv * sat) >> 7) + cr_t[ym];

                /* xorshift32, independent samples for both pixels in the 4:2:2 pair */
                rnd ^= rnd << 13;
                rnd ^= rnd >> 17;
                rnd ^= rnd << 5;
                int g0 = (((int)(rnd & 0xFF) - 128) * grain) >> 7;
                int g1 = (((int)((rnd >> 8) & 0xFF) - 128) * grain) >> 7;
                oa = fs_clamp_i(oa + g0, 0, 255);
                ob = fs_clamp_i(ob + g1, 0, 255);

                dp[i] = fs_pack_px(nu, oa, nv, ob);
            }
        }

        sp += words;
        dp += words;
    }

    *rnd_state = rnd;
}

/* ======================================================================
 * The joint form of the same look: one lattice indexed by (y, Cb, Cr).
 *
 * The separable tables above multiply a per-luma gain by a per-hue gain, so
 * a change in hue cannot also depend on how bright the pixel is. Measured
 * against the develop on the same 1444 samples, that form is 7.7-29.7
 * levels out; this one is 5.3-10.9 at the same 2 KB (se/hosttest/tableerr).
 *
 * Lattice order is y fastest, then Cb, then Cr -- the same order
 * se_look_bake_yuv3() fills, so each 8-node cell is 16 contiguous bytes.
 * ====================================================================== */
#ifndef FS_UV3_NY
#define FS_UV3_NY 12                   /* must match SE_YUV3_NY in look.h  */
#endif
#ifndef FS_UV3_NC
#define FS_UV3_NC 17                   /* must match SE_YUV3_NC in look.h  */
#endif
/* (nodes-1) * 65536 / 255, so 0..255 maps onto 0..nodes-1 in 16.16. */
#define FS_UV3_SY (((FS_UV3_NY - 1) * 65536 + 254) / 255)
#define FS_UV3_SC (((FS_UV3_NC - 1) * 65536 + 254) / 255)

static void fs_core_process_rows3(const unsigned char *curve, const short *uv,
                                  const void *src, void *dst,
                                  int pitch, int row0, int row1,
                                  int grain, unsigned int *rnd_state)
{
    int words = pitch >> 2;
    unsigned int rnd = *rnd_state;

    const unsigned int * sp = (const unsigned int *)src + (unsigned int)row0 * words;
    unsigned int * dp = (unsigned int *)dst + (unsigned int)row0 * words;

    /* node strides, in shorts: one step in y, in Cb, in Cr */
    const int ky = 3, ku = FS_UV3_NY * 3, kv = FS_UV3_NY * FS_UV3_NC * 3;

    for (int y = row0; y < row1; y++)
    {
        for (int i = 0; i < words; i++)
        {
            unsigned int px = sp[i];

            int u  = fs_chroma_signed(px);
            int ya = (px >>  8) & 0xFF;
            int v  = fs_chroma_signed(px >> 16);
            int yb = (px >> 24) & 0xFF;

            int oa = curve[ya];
            int ob = curve[yb];
            /*
             * Index the lattice by the INPUT luma, not the graded one: the
             * nodes were baked at input luma y, so passing the tone-curved
             * value looks up a different row and the chroma comes out of the
             * wrong cell. Measured on a real frame, that mistake cost more
             * (11.1 levels) than the whole separable form it was meant to
             * beat (7.0).
             */
            int ym = (ya + yb) >> 1;

            int ty = ym * FS_UV3_SY;
            int tu = (u + 128) * FS_UV3_SC;
            int tv = (v + 128) * FS_UV3_SC;
            int iy = ty >> 16, iu = tu >> 16, iv = tv >> 16;
            int fy = (ty >> 8) & 0xFF, fu = (tu >> 8) & 0xFF, fv = (tv >> 8) & 0xFF;

            /* the top node of an axis has no neighbour: interpolate to it */
            if (iy > FS_UV3_NY - 2) { iy = FS_UV3_NY - 2; fy = 256; }
            if (iu > FS_UV3_NC - 2) { iu = FS_UV3_NC - 2; fu = 256; }
            if (iv > FS_UV3_NC - 2) { iv = FS_UV3_NC - 2; fv = 256; }

            const short *base = uv + iy * ky + iu * ku + iv * kv;
            int out3[3];                 /* dy, Cb, Cr */
            /*
             * Tetrahedral, not trilinear. The cube splits into six tetrahedra
             * and the ordering of the three fractions picks one; the sample
             * then needs four corners instead of eight, and three differences
             * instead of seven interpolations. Same table, same result class,
             * roughly half the work -- and this runs on every LiveView frame.
             */
            {
                const short *ca, *cb2;
                int wa, wb, wc;

                if (fy >= fv) {
                    if (fv >= fu)      { ca = base + ky; cb2 = ca + kv; wa = fy - fv; wb = fv - fu; wc = fu; }
                    else if (fy >= fu) { ca = base + ky; cb2 = ca + ku; wa = fy - fu; wb = fu - fv; wc = fv; }
                    else               { ca = base + ku; cb2 = ca + ky; wa = fu - fy; wb = fy - fv; wc = fv; }
                } else {
                    if (fu >= fv)      { ca = base + ku; cb2 = ca + kv; wa = fu - fv; wb = fv - fy; wc = fy; }
                    else if (fu >= fy) { ca = base + kv; cb2 = ca + ku; wa = fv - fu; wb = fu - fy; wc = fy; }
                    else               { ca = base + kv; cb2 = ca + ky; wa = fv - fy; wb = fy - fu; wc = fu; }
                }

                for (int c = 0; c < 3; c++)
                {
                    int v000 = base[c];
                    int v111 = base[c + ky + ku + kv];
                    /* wa + wb + wc == the largest fraction, so the corner at
                     * the origin takes what is left of 256 */
                    out3[c] = (v000 * (256 - wa - wb - wc)
                               + ca[c]  * wa
                               + cb2[c] * wb
                               + v111   * wc) >> 8;
                }
            }

            /* the tone curve carries luma, the node carries what the colour
             * does to it -- per pixel, so the pair's two lumas stay distinct */
            oa = fs_clamp_i(oa + out3[0], 0, 255);
            ob = fs_clamp_i(ob + out3[0], 0, 255);

            if (grain)
            {
                rnd ^= rnd << 13;
                rnd ^= rnd >> 17;
                rnd ^= rnd << 5;
                oa = fs_clamp_i(oa + ((((int)(rnd & 0xFF) - 128) * grain) >> 7), 0, 255);
                ob = fs_clamp_i(ob + ((((int)((rnd >> 8) & 0xFF) - 128) * grain) >> 7), 0, 255);
            }

            dp[i] = fs_pack_px(out3[1], oa, out3[2], ob);
        }

        sp += words;
        dp += words;
    }

    *rnd_state = rnd;
}

/* ======================================================================
 * Raw develop path: sensor Bayer -> 8-bit RGB through the same look.
 *
 * Lives here, not in the module, so tools/sim can run the exact code the
 * camera runs.  Everything is integer; the only float work happens once
 * per frame in fs_core_dev_init().
 *
 * Precision: the sensor value is normalised to 0..FS_DEV_LINEAR_MAX (12 bit)
 * instead of 0..255.  An 8-bit normalisation before the transfer curve is
 * what made the first version band in the shadows - 12 bits is 16x finer
 * and still costs one byte of table per entry.
 * ====================================================================== */

#define FS_DEV_LINEAR_MAX 4095

typedef struct
{
    unsigned char gamma[FS_DEV_LINEAR_MAX + 1];  /* linear -> display-encoded */
    int have_matrix;
    int m[9];                                    /* camera -> sRGB, Q10 */
    int wb_r, wb_b;                              /* white balance, Q8 */
} fs_dev_t;

/*
 * sRGB transfer function, sampled at 65 points and linearly interpolated.
 *
 * The obvious implementation is an integer square root, which is gamma 2.0 -
 * close, but it puts mid grey 8-10 levels below where the camera's own JPEG
 * puts it, so a developed frame reads darker than the preview of the same
 * scene.  The firmware has no powf, so the curve is a table instead of a
 * calculation: 65 anchors, under a level of error, 130 bytes.
 */
static const unsigned char fs_srgb_tab[65] =
{
      0,  34,  49,  61,  71,  79,  86,  93,  99, 105, 110, 115, 120, 124,
    129, 133, 137, 141, 145, 148, 152, 155, 158, 162, 165, 168, 171, 174,
    177, 179, 182, 185, 188, 190, 193, 195, 198, 200, 202, 205, 207, 209,
    212, 214, 216, 218, 220, 223, 225, 227, 229, 231, 233, 235, 237, 239,
    240, 242, 244, 246, 248, 250, 251, 253, 255,
};

static unsigned char fs_core_srgb(int v)
{
    if (v <= 0) return 0;
    if (v >= FS_DEV_LINEAR_MAX) return 255;
    /* position in 1/64ths of the range, in Q8 */
    int t = (v * (64 * 256) + FS_DEV_LINEAR_MAX / 2) / FS_DEV_LINEAR_MAX;
    int k = t >> 8, f = t & 0xFF;
    if (k >= 64) return 255;
    return (unsigned char) ((fs_srgb_tab[k] * (256 - f) + fs_srgb_tab[k + 1] * f) >> 8);
}

/*
 * camera -> sRGB from the DNG ColorMatrix1 (XYZ -> camera, 9 rationals).
 * Inverted and pre-multiplied by the standard XYZ -> sRGB matrix.
 * Returns 0 on success, -1 when the camera did not supply a usable matrix.
 */
static int fs_core_dev_matrix(const int * cm, int m_out[9])
{
    float m[3][3], inv[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
        {
            int n = cm[(i * 3 + j) * 2], d = cm[(i * 3 + j) * 2 + 1];
            m[i][j] = (d > 0 && n != 0) ? (float) n / (float) d : 0.0f;
        }
    if (m[0][0] == 0.0f || m[1][1] == 0.0f || m[2][2] == 0.0f) return -1;

    float c00 =  (m[1][1] * m[2][2] - m[1][2] * m[2][1]);
    float c01 = -(m[1][0] * m[2][2] - m[1][2] * m[2][0]);
    float c02 =  (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    float det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    if (det > -0.000001f && det < 0.000001f) return -1;
    float id = 1.0f / det;

    inv[0][0] = c00 * id;
    inv[1][0] = c01 * id;
    inv[2][0] = c02 * id;
    inv[0][1] = -(m[0][1] * m[2][2] - m[0][2] * m[2][1]) * id;
    inv[1][1] =  (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
    inv[2][1] = -(m[0][0] * m[2][1] - m[0][1] * m[2][0]) * id;
    inv[0][2] =  (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
    inv[1][2] = -(m[0][0] * m[1][2] - m[0][2] * m[1][0]) * id;
    inv[2][2] =  (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;

    static const float xyz2srgb[3][3] =
    {
        {  3.2406f, -1.5372f, -0.4986f },
        { -0.9689f,  1.8758f,  0.0415f },
        {  0.0557f, -0.2040f,  1.0570f },
    };

    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
        {
            float v = 0.0f;
            for (int k = 0; k < 3; k++) v += xyz2srgb[i][k] * inv[k][j];
            m_out[i * 3 + j] = (int) (v * 1024.0f + (v < 0 ? -0.5f : 0.5f));
        }
    return 0;
}

static int fs_core_dev_matrix_wb(const int * cm, int kelvin, int m_out[9]);

/*
 * kelvin > 0 gives the fully white-balanced matrix and no separate gains.
 * Without a Kelvin the matrix still has to be normalised somehow, or a neutral
 * subject decodes as strongly green - so daylight is assumed and the caller's
 * bounded scene estimate is applied on top as a correction.  A wrong assumption
 * then costs a slight cast instead of a broken frame.
 */
static void fs_core_dev_init(fs_dev_t * d, const int * color_matrix1,
                             int kelvin, int wb_r, int wb_b)
{
    int i;
    for (i = 0; i <= FS_DEV_LINEAR_MAX; i++)
        d->gamma[i] = fs_core_srgb(i);

    int known = (kelvin >= 2000 && kelvin <= 12000);
    int k = known ? kelvin : 6500;

    d->have_matrix = 0;
    d->wb_r = wb_r;
    d->wb_b = wb_b;

    if (color_matrix1 && fs_core_dev_matrix_wb(color_matrix1, k, d->m) == 0)
    {
        d->have_matrix = 1;
        if (known)
        {
            d->wb_r = 256;      /* the matrix already balances for this light */
            d->wb_b = 256;
        }
        return;
    }

    d->have_matrix = (color_matrix1 && fs_core_dev_matrix(color_matrix1, d->m) == 0);
    if (!d->have_matrix) for (i = 0; i < 9; i++) d->m[i] = 0;
}

/*
 * Camera -> sRGB for the light the camera is actually balanced for.
 *
 * Three things have to happen together, which is why they are one matrix:
 *
 *   1. white balance   x' = x / cam_neutral   (cam_neutral from ColorMatrix1
 *                      applied to the illuminant)
 *   2. colour convert  x' -> linear sRGB
 *   3. normalise       so the illuminant renders as (1,1,1)
 *
 * Doing them separately is what broke the first attempt: applying a WB gain
 * computed in sRGB space leaves the camera-space channel scaling untouched,
 * and applying a camera-space gain without renormalising the colour matrix
 * multiplies the overall brightness by whatever the gain was.
 *
 * Returns 0 on success; -1 when the matrix is unusable and the caller should
 * fall back to plain fs_core_dev_matrix() plus external gains.
 */
static void fs_core_illuminant_xyz(int kelvin, float xyz[3])
{
    if (kelvin < 2000) kelvin = 2000;
    if (kelvin > 12000) kelvin = 12000;

    float t = (float) kelvin, t2 = t * t, t3 = t2 * t;
    float x, y;

    if (kelvin <= 4000)
        x = -0.2661239e9f / t3 - 0.2343589e6f / t2 + 0.8776956e3f / t + 0.179910f;
    else
        x = -3.0258469e9f / t3 + 2.1070379e6f / t2 + 0.2226347e3f / t + 0.240390f;

    float x2 = x * x, x3 = x2 * x;
    if (kelvin <= 4000)
        y = -1.1063814f * x3 - 1.34811020f * x2 + 2.18555832f * x - 0.20219683f;
    else
        y = -0.9549476f * x3 - 1.37418593f * x2 + 2.09137015f * x - 0.16748867f;

    if (y < 0.05f) y = 0.05f;
    xyz[0] = x / y;
    xyz[1] = 1.0f;
    xyz[2] = (1.0f - x - y) / y;
}

/* float throughout: the camera has no FPU, so every double is a library call,
 * and a 3x3 inverse at Q10 does not need more than 24 bits of mantissa. */
static int fs_core_inv3(const float a[9], float out[9])
{
    float c00 =  a[4] * a[8] - a[5] * a[7];
    float c01 = -(a[3] * a[8] - a[5] * a[6]);
    float c02 =  a[3] * a[7] - a[4] * a[6];
    float det = a[0] * c00 + a[1] * c01 + a[2] * c02;
    if (det > -1e-9f && det < 1e-9f) return -1;

    out[0] = c00 / det;
    out[3] = c01 / det;
    out[6] = c02 / det;
    out[1] = -(a[1] * a[8] - a[2] * a[7]) / det;
    out[4] =  (a[0] * a[8] - a[2] * a[6]) / det;
    out[7] = -(a[0] * a[7] - a[1] * a[6]) / det;
    out[2] =  (a[1] * a[5] - a[2] * a[4]) / det;
    out[5] = -(a[0] * a[5] - a[2] * a[3]) / det;
    out[8] =  (a[0] * a[4] - a[1] * a[3]) / det;
    return 0;
}

static int fs_core_dev_matrix_wb(const int * cm, int kelvin, int m_out[9])
{
    static const float xyz2srgb[9] =
    {
         3.2406f, -1.5372f, -0.4986f,
        -0.9689f,  1.8758f,  0.0415f,
         0.0557f, -0.2040f,  1.0570f,
    };

    float xyz[3];
    fs_core_illuminant_xyz(kelvin, xyz);

    float m[9], cam_n[3];
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            int n = cm[(i * 3 + j) * 2], d = cm[(i * 3 + j) * 2 + 1];
            m[i * 3 + j] = (d > 0 && n != 0) ? (float) n / (float) d : 0.0f;
        }
        cam_n[i] = m[i * 3] * xyz[0] + m[i * 3 + 1] * xyz[1] + m[i * 3 + 2] * xyz[2];
        if (cam_n[i] < 1e-4f) return -1;
    }

    /* camera response of the illuminant, normalised to 1 by dividing it out */
    float nmat[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            nmat[i * 3 + j] = m[i * 3 + j] / cam_n[i];

    float ninv[9];
    if (fs_core_inv3(nmat, ninv) != 0) return -1;

    float c[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            c[i * 3 + j] = xyz2srgb[i * 3] * ninv[j]
                         + xyz2srgb[i * 3 + 1] * ninv[3 + j]
                         + xyz2srgb[i * 3 + 2] * ninv[6 + j];

    /* in: divide by the camera's reading of the illuminant, so this matrix
     * takes raw sensor values, not white-balanced ones.
     * out: divide by the illuminant's sRGB, so the illuminant renders white. */
    for (int i = 0; i < 3; i++)
    {
        float ill = xyz2srgb[i * 3] * xyz[0]
                  + xyz2srgb[i * 3 + 1] * xyz[1]
                  + xyz2srgb[i * 3 + 2] * xyz[2];
        if (ill < 1e-4f) return -1;
        for (int j = 0; j < 3; j++)
            c[i * 3 + j] /= ill * cam_n[j];
    }

    for (int i = 0; i < 9; i++)
        m_out[i] = (int) (c[i] * 1024.0f + (c[i] < 0 ? -0.5f : 0.5f));
    return 0;
}

/*
 * White balance gains from the four per-CFA-channel sums of a coarse sample.
 * Q8, 256 = unity.  Bounded much tighter than the ideal, because a scene that
 * is genuinely one colour must not be corrected to grey.
 */
static void fs_core_dev_wb_from_sums(const int sum[4], const int cnt[4],
                                     int * wb_r, int * wb_b)
{
    int gavg = (cnt[1] ? sum[1] / cnt[1] : 1) + (cnt[2] ? sum[2] / cnt[2] : 1);
    gavg = gavg ? gavg / 2 : 1;

    int r = (cnt[0] && sum[0]) ? (gavg * 256) / (sum[0] / cnt[0]) : 256;
    int b = (cnt[3] && sum[3]) ? (gavg * 256) / (sum[3] / cnt[3]) : 256;

    *wb_r = fs_clamp_i(r, 192, 384);
    *wb_b = fs_clamp_i(b, 192, 384);
}

/*
 * Demosaic one pixel from a three-row window of an RGGB Bayer frame.
 *
 * prev / cur / next point at x == 0 of rows y-1, y, y+1.  Rows and columns
 * must be *mirrored* at the frame edge (row -1 is row 1), not clamped: a
 * duplicated neighbour has the wrong CFA colour and the border line goes bad.
 * Output is sensor RGB on the raw scale (black not yet subtracted).
 *
 * Bilinear on purpose: hand-held single frames do not benefit from anything
 * heavier at this stage, and this is the only version both the camera and the
 * host simulator can run.
 */
static inline void fs_core_demosaic(const unsigned short * prev,
                                    const unsigned short * cur,
                                    const unsigned short * next,
                                    int x, int y, int w, int out[3])
{
    /* mirror at the sides rather than clamping: x-2 and x+2 hold the same CFA
     * colour as x, so the mirrored neighbour is the one the demosaic wants */
    int xm = x > 0 ? x - 1 : (w > 2 ? 1 : x);
    int xp = x + 1 < w ? x + 1 : (w > 2 ? w - 2 : x);
    int c  = ((y & 1) << 1) | (x & 1);           /* RGGB: 0=R 1=G 2=G 3=B */
    int centre = cur[x];
    int r, g, b;

    if (c == 0)                                  /* red site */
    {
        r = centre;
        g = (cur[xm] + cur[xp]) / 2;
        b = (prev[xm] + prev[xp] + next[xm] + next[xp]) / 4;
    }
    else if (c == 3)                             /* blue site */
    {
        b = centre;
        g = (cur[xm] + cur[xp]) / 2;
        r = (prev[xm] + prev[xp] + next[xm] + next[xp]) / 4;
    }
    else                                         /* green site */
    {
        g = centre;
        /* In RGGB the green sites form their own checkerboard, so a green
         * pixel's diagonals are green too: the opposite chroma is the pair
         * left/right on one row parity and above/below on the other. */
        int horiz = ((y & 1) == 0);              /* even row: red is horizontal */
        r = horiz ? (cur[xm] + cur[xp]) / 2 : (prev[x] + next[x]) / 2;
        b = horiz ? (prev[x] + next[x]) / 2 : (cur[xm] + cur[xp]) / 2;
    }

    out[0] = r; out[1] = g; out[2] = b;
}

/*
 * One pixel.  Input is linear sensor RGB already scaled to 0..FS_DEV_LINEAR_MAX
 * and black-subtracted; output is display-encoded RGB carrying the film look.
 *
 * Order matters and mirrors the LiveView filter: linear -> matrix -> transfer
 * curve -> film LUT -> hue/saturation/split-tone.  Doing the matrix after the
 * curve mixes channels of already-gamma'd data and is visibly wrong.
 */
static inline void fs_core_dev_pixel(const fs_lut_t * lut, const fs_dev_t * d,
                                     int r, int g, int b, unsigned char out[3])
{
    /* white balance in linear light, before the clamp */
    if (d->wb_r != 256) r = (r * d->wb_r) >> 8;
    if (d->wb_b != 256) b = (b * d->wb_b) >> 8;

    r = fs_clamp_i(r, 0, FS_DEV_LINEAR_MAX);
    g = fs_clamp_i(g, 0, FS_DEV_LINEAR_MAX);
    b = fs_clamp_i(b, 0, FS_DEV_LINEAR_MAX);

    if (d->have_matrix)
    {
        int nr = (d->m[0] * r + d->m[1] * g + d->m[2] * b) >> 10;
        int ng = (d->m[3] * r + d->m[4] * g + d->m[5] * b) >> 10;
        int nb = (d->m[6] * r + d->m[7] * g + d->m[8] * b) >> 10;
        r = fs_clamp_i(nr, 0, FS_DEV_LINEAR_MAX);
        g = fs_clamp_i(ng, 0, FS_DEV_LINEAR_MAX);
        b = fs_clamp_i(nb, 0, FS_DEV_LINEAR_MAX);
    }

    int R = lut->curve[d->gamma[r]];
    int G = lut->curve[d->gamma[g]];
    int B = lut->curve[d->gamma[b]];

    int y  = (77 * R + 150 * G + 29 * B) >> 8;
    int cb = B - y;
    int cr = R - y;

    int ncb = (lut->matrix[0] * cb + lut->matrix[1] * cr) >> 8;
    int ncr = (lut->matrix[2] * cb + lut->matrix[3] * cr) >> 8;

    int sat = lut->sat[y];
    ncb = ((ncb * sat) >> 7) + lut->cb_off[y];
    ncr = ((ncr * sat) >> 7) + lut->cr_off[y];

    int nr = fs_clamp_i(y + ncr, 0, 255);
    int nb = fs_clamp_i(y + ncb, 0, 255);

    /* Green keeps its own detail: cancel only the chroma change, so the luma
     * of the result still equals y.  Setting all three channels from y (what
     * this did first) threw away every green micro-detail.
     * Division by 150 without __aeabi_idiv via fixed-point 27963/2^22. */
    int t2 = (nr - R) * 77 + (nb - B) * 29;
    int sg = 1;
    if (t2 < 0) { t2 = -t2; sg = -1; }
    int dg = -sg * ((t2 * 27963) >> 22);

    out[0] = (unsigned char) nr;
    out[1] = (unsigned char) fs_clamp_i(G + dg, 0, 255);
    out[2] = (unsigned char) nb;
}

/* ---------------------------------------------------------------------
 * Baseline 8-bit RGB TIFF header: 160 bytes, one uncompressed strip, no
 * metadata beyond what a viewer needs.  Also in the core so the host build
 * writes byte-identical files to the camera.
 * ------------------------------------------------------------------- */

#define FS_TIFF_HDR_SIZE 160

static inline void fs_core_put16(unsigned char * p, int v)
{
    p[0] = (unsigned char) (v & 0xFF);
    p[1] = (unsigned char) ((v >> 8) & 0xFF);
}

static inline void fs_core_put32(unsigned char * p, unsigned int v)
{
    p[0] = (unsigned char) (v & 0xFF);
    p[1] = (unsigned char) ((v >> 8) & 0xFF);
    p[2] = (unsigned char) ((v >> 16) & 0xFF);
    p[3] = (unsigned char) ((v >> 24) & 0xFF);
}

static int fs_core_tiff_header(unsigned char * hdr, int w, int h)
{
    enum { NTAGS = 10 };
    int i;
    for (i = 0; i < FS_TIFF_HDR_SIZE; i++) hdr[i] = 0;

    hdr[0] = 'I'; hdr[1] = 'I';
    fs_core_put16(hdr + 2, 42);
    fs_core_put32(hdr + 4, 8);
    fs_core_put16(hdr + 8, NTAGS);

    int bps_off = 10 + NTAGS * 12 + 4;
    unsigned char * t = hdr + 10;

    /* tag, type, count, value.  BitsPerSample is three SHORTs, so it does not
     * fit in the entry and holds a pointer - writing count 1 there makes
     * strict readers (libtiff, Preview) reject the file. */
    struct { int tag, type, count; unsigned int val; } tags[NTAGS] =
    {
        { 256, 3, 1, (unsigned int) w },                      /* ImageWidth        */
        { 257, 3, 1, (unsigned int) h },                      /* ImageLength       */
        { 258, 3, 3, (unsigned int) bps_off },                /* BitsPerSample     */
        { 259, 3, 1, 1 },                                     /* Compression none  */
        { 262, 3, 1, 2 },                                     /* Photometric RGB   */
        { 273, 4, 1, (unsigned int) FS_TIFF_HDR_SIZE },       /* StripOffsets      */
        { 277, 3, 1, 3 },                                     /* SamplesPerPixel   */
        { 278, 4, 1, (unsigned int) h },                      /* RowsPerStrip      */
        { 279, 4, 1, (unsigned int) w * (unsigned int) h * 3 },/* StripByteCounts  */
        { 284, 3, 1, 1 },                                     /* PlanarConfig      */
    };

    for (i = 0; i < NTAGS; i++)
    {
        fs_core_put16(t,      tags[i].tag);
        fs_core_put16(t + 2,  tags[i].type);
        fs_core_put32(t + 4,  (unsigned int) tags[i].count);
        fs_core_put32(t + 8,  tags[i].val);
        t += 12;
    }
    fs_core_put32(t, 0);                                   /* next IFD */

    fs_core_put16(hdr + bps_off,     8);
    fs_core_put16(hdr + bps_off + 2, 8);
    fs_core_put16(hdr + bps_off + 4, 8);
    return 0;
}

#endif
