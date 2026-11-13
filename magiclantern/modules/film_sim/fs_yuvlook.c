/**
 * The shot-time look: the same idea as Florin's filmyuv_apply, on our recipes.
 *
 * What his code does
 * ------------------
 * Disassembling work_000 (0xa0f8) -- one of the eight per-pixel workers
 * filmyuv_apply dispatches to -- the whole loop is three table lookups and
 * about ten multiplies per UYVY pair:
 *
 *      lr  = *src++;                            U | Y0<<8 | V<<16 | Y1<<24
 *      r3  = (lr >> 8) & 0xff;                  Y0
 *      r7  = ctx[lr >> 24];                     Y1 through a 256-byte table
 *      r8  = ctx[r3];                            Y0 through the same table
 *      idx = ((V^80)>>3)*32 + ((U^80)>>3);      5 bits of each, 32x32
 *      cg  = ctx[0x500 + idx];                  chroma gain
 *      ip  = *(uint32_t*)(ctx + 0x100 + ((r7+r8)>>1)*4);
 *           byte0 = gain (Q6), byte1 = U offset, byte2 = V offset
 *      U' = (byte of ip, signed) + (cg * (U signed) * gain) >> 6   clamp +-127
 *      V' = (byte of ip, signed) + (cg * (V signed) * gain) >> 6   clamp +-127
 *
 * So his film look is a SEPARABLE model: a luma curve, a chroma gain and
 * offset per luminance, and a chroma gain per (U,V). It is not a 3D LUT over
 * RGB at shot time -- the .CUB is baked down to exactly those three tables
 * beforehand. That is why it costs ~1 s on 22 Mpx instead of a minute.
 *
 * It is also, very neatly, the shape of our own recipe: CURVE is the luma
 * table, SPLIT_SH/SPLIT_HI are chroma offsets weighted by luma, SAT is the
 * gain, and HUE_SAT per sector is the (U,V) table. So the same three tables
 * can be baked from a .fs file and applied to the frame Canon is about to
 * encode -- one JPEG encode, no decode, no second generation.
 *
 * What is lost
 * ------------
 * Grain and halation. Grain needs a random field per pixel and halation needs
 * a neighbourhood; neither fits a separable lookup, and Florin dropped both
 * from this path for the same reason. They stay in the develop path.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifndef SE_HOSTED
#include <dryos.h>
#include <mem.h>
#include <fio-ml.h>
#endif

#define SE_YUV_STOCK_MAX (16 * 1024)

#include "fs_yuvlook.h"
#include "se/engine/look.h"
#include "se/engine/stock.h"

/* The tables themselves, baked in look.c by running the real grade. */
static se_yuvtbl tbl;
/* The same look as a joint (y,u,v) lattice, for the LiveView filter. */
static se_yuv3   tbl3;
static int       baked;
static char      baked_name[SE_STOCK_NAME];

/* Exact (engine) path, defined below; prepared when a stock is baked. */
static void eng_prepare(const se_look *lk);


/*
 * Load a FLUT cube for the bake. The develop path has its own loader in
 * se_cam.c bound to a module-level path; this one is handed the path, so the
 * two cannot disagree about which cube a bake used.
 *
 * A missing or malformed file is not fatal: the bake runs without it and the
 * caller logs, because a stock whose colour lives in the cube would otherwise
 * silently lose that colour in LiveView -- which is the bug this fixes.
 */
static int bake_cube_load(const char *path, se_lut3 *cube, uint16_t **hold)
{
    unsigned char h[8];
    uint32_t size, need;
    FILE *f;

    *hold = NULL;
    cube->size = 0;
    cube->data = NULL;
    if (!path || !path[0]) return 0;

#ifdef SE_HOSTED
    f = fopen(path, "rb");
#else
    f = FIO_OpenFile(path, O_RDONLY | O_SYNC);
#endif
    if (!f) return -1;
#ifdef SE_HOSTED
    if (fread(h, 1, sizeof h, f) != sizeof h) { fclose(f); return -1; }
#else
    if (FIO_ReadFile(f, h, sizeof h) != (int)sizeof h) { FIO_CloseFile(f); return -1; }
#endif
    if (h[0] != 'F' || h[1] != 'L' || h[2] != 'U' || h[3] != 'T') {
#ifdef SE_HOSTED
        fclose(f);
#else
        FIO_CloseFile(f);
#endif
        return -1;
    }
    size = (uint32_t)(h[4] | (h[5] << 8));
    need = (uint32_t)se_lut3_bin_bytes((int)size);
    if (!need || need != 8u + size * size * size * 6u || size > SE_LUT3_MAX_SIZE) {
#ifdef SE_HOSTED
        fclose(f);
#else
        FIO_CloseFile(f);
#endif
        return -1;
    }
    *hold = malloc(need - 8u);
    if (!*hold) {
#ifdef SE_HOSTED
        fclose(f);
#else
        FIO_CloseFile(f);
#endif
        return -1;
    }
#ifdef SE_HOSTED
    if (fread(*hold, 1, need - 8u, f) != need - 8u) { fclose(f); free(*hold); *hold = NULL; return -1; }
    fclose(f);
#else
    /* 8 KB at a time, like the .fs read above: FIO_ReadFile() asserts on a
     * single read larger than that into ordinary cacheable memory, and a 17^3
     * cube is 29 478 bytes -- the assert would fire the first time a stock
     * with its own cube was baked. */
    {
        size_t got = 0, want = need - 8u;
        while (got < want) {
            int chunk = (want - got) > 8192u ? 8192 : (int)(want - got);
            int r = FIO_ReadFile(f, (char *)*hold + got, chunk);
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (got != want) {
            FIO_CloseFile(f); free(*hold); *hold = NULL; return -1;
        }
    }
    FIO_CloseFile(f);
#endif
    cube->size = (int)size;
    cube->data = *hold;
    return 1;
}

/* Saturation slider for the bake. Develop has always honoured
 * film.sim.satboost through se_cam_set_satboost(); the bake never did, so the
 * slider moved the file and not the panel. */
static int bake_satboost = 100;
void fs_yuvlook_set_satboost(int s) { bake_satboost = (s < 20) ? 20 : (s > 200 ? 200 : s); }

int fs_yuvlook_bake_lut(const char *stock_path, int strength, const char *lut_path)
{
    se_stock st;
    se_look  lk;
    char    *text;
    size_t   got = 0;
    FILE    *f;

    baked = 0;
#ifdef SE_HOSTED
    f = fopen(stock_path, "rb");
#else
    f = FIO_OpenFile(stock_path, O_RDONLY | O_SYNC);
#endif
    if (!f)
        return -1;
    text = malloc(SE_YUV_STOCK_MAX);
    if (!text) {
#ifdef SE_HOSTED
        fclose(f);
#else
        FIO_CloseFile(f);
#endif
        return -1;
    }
#ifdef SE_HOSTED
    got = fread(text, 1, SE_YUV_STOCK_MAX, f);
    fclose(f);
#else
    /* 8 KB at a time: FIO_ReadFile() asserts on more than that when the
     * destination is ordinary cacheable memory (src/fio-ml.c). */
    while (got + 8192 <= SE_YUV_STOCK_MAX) {
        int n = FIO_ReadFile(f, text + got, 8192);
        if (n <= 0) break;
        got += (size_t)n;
    }
    FIO_CloseFile(f);
#endif
    if (!got || se_stock_parse(text, got, &st) != SE_OK) {
        free(text);
        return -1;
    }
    free(text);

    if (!st.name[0]) {
        /* An empty NAME means the parse found nothing it recognised -- which
         * is what happened when this ran against an empty stock path and
         * silently produced identity tables. Refuse instead. */
        return -1;
    }

    se_look_from_stock(&st, 1, &lk);
    se_look_set_strength(&lk, strength);
    se_look_set_satboost(&lk, bake_satboost);
    /*
     * No se_look_set_grain_size() here on purpose: the bakes only sample
     * grade_px(), and grain_row() runs in se_look_rows() -- the develop path.
     * Setting it made no difference to tbl or tbl3 and only implied that
     * Grain Size reached LiveView. LiveView's grain is fs_lut.grain_amp, from
     * the Film Look grain amount; the stock's GRAIN_SIZE shapes developed
     * files only.
     */

    {
        se_lut3 cube;
        uint16_t *hold = NULL;
        int got = bake_cube_load(lut_path, &cube, &hold);
        if (got < 0 && lut_path && lut_path[0])
            se_log("bake: %s unreadable, LiveView has the .fs only", lut_path);
        se_look_set_bake_cube(got > 0 ? &cube : NULL);
        se_look_bake_yuv(&lk, &tbl);
        eng_prepare(&lk);
        se_look_bake_yuv3(&lk, &tbl3);
        se_look_set_bake_cube(NULL);
        free(hold);
    }
    baked = 1;
    snprintf(baked_name, sizeof baked_name, "%s", st.name);
    return 0;
}

int fs_yuvlook_bake(const char *stock_path, int strength)
{
    return fs_yuvlook_bake_lut(stock_path, strength, NULL);
}

int fs_yuvlook_ready(void) { return baked; }
const char *fs_yuvlook_name(void) { return baked ? baked_name : ""; }

/* The baked tables, for host tools that plot or compare them. The camera path
 * never needs this -- it only ever calls fs_yuvlook_apply(). */
const se_yuvtbl *fs_yuvlook_table(void) { return baked ? &tbl : NULL; }

/* The same look as a joint (y,u,v) lattice. NULL until a bake succeeds. */
const se_yuv3 *fs_yuvlook_uv3(void) { return baked ? &tbl3 : NULL; }

/*
 * Apply to a UYVY frame in place.
 *
 * Three table lookups and two multiplies per pixel, no floating point, no
 * allocation: this runs inside the shot, before Canon's encoder reads the
 * buffer. `stride` is in bytes and must be at least width*2.
 */
/*
 * How much of the look the shot path applies. One core, ~1 s per frame at the
 * full grade, and the grade sits in Canon's pipeline, so the shutter waits for
 * it -- this is the dial for that.
 *
 *   0 HIGH   luma curve + per-hue chroma cells + chroma matrix   (the look)
 *   1 NORMAL luma curve + one global chroma gain + matrix        (no per-hue)
 *   2 FAST   luma curve only, chroma untouched                   (tone only)
 *
 * The bake is the same in all three: the cells are computed and then ignored,
 * so switching costs nothing but a rebake that is already done.
 */
static int gradeq;
void fs_yuvlook_set_quality(int q) { gradeq = q < 0 ? 0 : (q > 2 ? 2 : q); }
int  fs_yuvlook_quality(void) { return gradeq; }

/*
 * Ordered dither for the shot path.
 *
 * The baked curve is 8-bit, and for a contrasty stock 64 of its 255 steps are
 * flat -- two input levels land on one output code. Without noise those steps
 * are visible bands, which is exactly what the JPEG showed while LiveView (a
 * different path, with its own dither) looked clean.
 */
static const unsigned char fs_yuv_dither[8][8] = {
    {  0, 128,  32, 160,   8, 136,  40, 168},
    {192,  64, 224,  96, 200,  72, 232, 104},
    { 48, 176,  16, 144,  56, 184,  24, 152},
    {240, 112, 208,  80, 248, 120, 216,  88},
    { 12, 140,  44, 172,   4, 132,  36, 164},
    {204,  76, 236, 108, 196,  68, 228, 100},
    { 60, 188,  28, 156,  52, 180,  20, 148},
    {252, 124, 220,  92, 244, 116, 212,  84},
};

/* +-1 only where the curve is flat at this input, so smooth ramps stay clean. */
static int fs_yuv_flat_dither(int in, int d) {
    if (in < 0 || in > 254) return 0;
    if (tbl.y[in] != tbl.y[in + 1]) return 0;
    return (d >= 192) - (d < 64);
}


/* ------------------------------------------------------------------------
 * Exact path: run the real engine on the shot, not its sampled tables.
 *
 * The baked path samples the look into 256 luma codes and a chroma grid; that
 * sampling is the banding, because a contrasty stock collapses a quarter of
 * the codes onto each other. Here each row of the YUV frame is converted to
 * RGB, pushed through the same se_look_rows() the develop uses, and converted
 * back with the engine's own 16->8 dither. Same look as a developed frame, at
 * the cost of per-pixel work inside the shot.
 *
 * Nothing here allocates: the arena and the row buffers are static and the run
 * is created when the stock is baked, outside the capture path.
 */
static int eng_drop(void *ctx, const uint8_t *row, int width) { (void)ctx; (void)row; (void)width; return 0; }
static void eng_prepare(const se_look *lk);
static se_look_run *eng_run;
static se_arena     eng_arena;
/* The engine run needs far more than the develop's 192 KB at full width: the
 * halation ring is (2r+1) rows wide. 800 KB holds it at 5760 px with r=21,
 * close to what a develop uses; the tables never needed it at all. */
static unsigned char eng_arena_mem[800 * 1024];
static unsigned char eng_rgb[5760 * 3];
static unsigned char *eng_dst;
static int eng_stride, eng_row;

int fs_yuvlook_engine_ready(void) { return eng_run != NULL; }

/* ---- LiveView form: the same engine, colour only ----------------------
 *
 * Halation and grain are the expensive stages and neither is what a panel is
 * for; with them off the run is small enough to prepare per width and the
 * per-frame cost drops out of the seconds. Same code path as the develop, so
 * what the panel shows is the look, not a table standing in for it.
 */
static int eng_emit(void *ctx, const uint8_t *row, int width);
static se_look      lv_look;
static int          lv_have;
static se_look_run *lv_run;
static se_arena     lv_arena;
static unsigned char lv_mem[96 * 1024];
static unsigned char lv_rgb[2048 * 3];
static int          lv_w;

int fs_yuvlook_engine_lv_ready(void) { return lv_have; }

uint32_t fs_yuvlook_apply_engine_lv(unsigned char *buf, int width, int height, int stride)
{
    int y;
    if (!lv_have || !buf || width < 2 || width > 2048) return 0;
    if (!lv_run || lv_w != width) {
        if (lv_run) { se_look_end(lv_run, eng_drop, NULL); lv_run = NULL; }
        se_arena_init(&lv_arena, lv_mem, sizeof lv_mem);
        se_look_fit(&lv_look, width, sizeof lv_mem);
        if (se_look_begin(&lv_run, &lv_look, width, &lv_arena) != SE_OK) {
            lv_run = NULL;
            return 0;
        }
        lv_w = width;
    }
    eng_dst = buf; eng_stride = stride; eng_row = 0;
    for (y = 0; y < height; y++) {
        unsigned char *src = buf + (size_t)y * (size_t)stride;
        int x;
        for (x = 0; x < width; x++) {
            int Y = src[x * 2 + 1];
            int U = (int)(signed char)src[x * 2 + 0];
            int V = (int)(signed char)src[x * 2 + 2];
            int r = Y + ((183 * V) >> 8);
            int g = Y - ((44 * U + 91 * V) >> 8);
            int b = Y + ((227 * U) >> 8);
            lv_rgb[x * 3 + 0] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r);
            lv_rgb[x * 3 + 1] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g);
            lv_rgb[x * 3 + 2] = (unsigned char)(b < 0 ? 0 : b > 255 ? 255 : b);
        }
        se_look_rows(lv_run, lv_rgb, 1, eng_emit, NULL);
    }
    se_look_end(lv_run, eng_emit, NULL);
    return 1;              /* graded: the caller must not fall back */
}

static int eng_emit(void *ctx, const uint8_t *row, int width)
{
    unsigned char *p = eng_dst + (size_t)eng_row * (size_t)eng_stride;
    int x;
    (void)ctx;
    for (x = 0; x + 1 < width; x += 2) {
        const unsigned char *a = row + (size_t)x * 3;
        const unsigned char *b = row + (size_t)(x + 1) * 3;
        int y0 = (77 * a[0] + 150 * a[1] + 29 * a[2] + 128) >> 8;
        int y1 = (77 * b[0] + 150 * b[1] + 29 * b[2] + 128) >> 8;
        int r = (a[0] + b[0]) / 2, g = (a[1] + b[1]) / 2, bl = (a[2] + b[2]) / 2;
        int u = ((-43 * r - 85 * g + 128 * bl + 128) >> 8);
        int v = ((128 * r - 107 * g - 21 * bl + 128) >> 8);
        p[x * 2 + 0] = (unsigned char)(signed char)(u < -128 ? -128 : u > 127 ? 127 : u);
        p[x * 2 + 1] = (unsigned char)(y0 < 0 ? 0 : y0 > 255 ? 255 : y0);
        p[x * 2 + 2] = (unsigned char)(signed char)(v < -128 ? -128 : v > 127 ? 127 : v);
        p[x * 2 + 3] = (unsigned char)(y1 < 0 ? 0 : y1 > 255 ? 255 : y1);
    }
    eng_row++;
    return 0;
}

/* Called from the bake: build a run for the widest frame the body can hand. */
static void eng_prepare(const se_look *lk)
{
    se_look fitted = *lk;
    if (eng_run) { se_look_end(eng_run, eng_drop, NULL); eng_run = NULL; }
    /* Shrink the halation radius until the run fits this arena, the same way
     * a develop fits its own budget. */
    lv_look = *lk;
    lv_look.hal_amount = 0;          /* the expensive stages, dropped */
    lv_look.grain = 0;
    lv_have = 1;
    if (lv_run) { se_look_end(lv_run, eng_drop, NULL); lv_run = NULL; }

    se_look_fit(&fitted, 5760, sizeof eng_arena_mem);
    se_arena_init(&eng_arena, eng_arena_mem, sizeof eng_arena_mem);
    if (se_look_begin(&eng_run, &fitted, 5760, &eng_arena) != SE_OK)
        eng_run = NULL;
}

uint32_t fs_yuvlook_apply_engine(unsigned char *buf, int width, int height, int stride)
{
    int y;
#ifndef SE_HOSTED
    uint32_t t0 = get_ms_clock();
#endif
    if (!eng_run || !buf || width < 2 || width > 5760) return 0;
    eng_dst = buf; eng_stride = stride; eng_row = 0;
    for (y = 0; y < height; y++) {
        unsigned char *src = buf + (size_t)y * (size_t)stride;
        int x;
        for (x = 0; x < width; x++) {
            int Y = src[x * 2 + 1];
            int U = (int)(signed char)src[x * 2 + 0];
            int V = (int)(signed char)src[x * 2 + 2];
            int r = Y + ((183 * V) >> 8);
            int g = Y - ((44 * U + 91 * V) >> 8);
            int b = Y + ((227 * U) >> 8);
            eng_rgb[x * 3 + 0] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r);
            eng_rgb[x * 3 + 1] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g);
            eng_rgb[x * 3 + 2] = (unsigned char)(b < 0 ? 0 : b > 255 ? 255 : b);
        }
        se_look_rows(eng_run, eng_rgb, 1, eng_emit, NULL);
    }
    se_look_end(eng_run, eng_emit, NULL);
#ifdef SE_HOSTED
    return 0;
#else
    return get_ms_clock() - t0;
#endif
}

uint32_t fs_yuvlook_apply(unsigned char *buf, int width, int height, int stride)
{
#ifndef SE_HOSTED
    uint32_t t0 = get_ms_clock();
#endif
    int y;

    if (!baked || !buf || width < 2 || height < 1 || stride < width * 2)
        return 0;

    for (y = 0; y < height; y++) {
        unsigned char *p = buf + (size_t)y * (size_t)stride;
        int x;
        for (x = 0; x + 1 < width; x += 2, p += 4) {
            /*
             * Chroma is a SIGNED byte on this body: 0 is neutral, not 128.
             * Proven by dumping Canon's own LiveView plane (LV_RAW.BIN):
             * decoded as signed the frame is correct, decoded as (byte-128)
             * it is a mess of magenta and cyan. fs_core_process_rows, the
             * LiveView path, has always used (signed char) -- this function
             * was the odd one out.
             */
            int u   = (int)(signed char)p[0];
            int v   = (int)(signed char)p[2];
            int y0  = p[1];
            int y1  = p[3];
            int yav = (y0 + y1) >> 1;
            /*
             * Nearest cell of a 64x64 chroma grid.
             *
             * This was 32x32 nearest (8-level quantisation, visible banding
             * in a smooth sky), then briefly 32x32 BILINEAR -- which fixed
             * the banding and cost 5.3x on camera: 186 ms -> 992 ms for an
             * 18 MP frame, because it is eight table reads and eight
             * multiplies per pixel pair. The grid is now four times finer on
             * each axis (4-level quantisation) and the lookup is a shift
             * again, so the shot path is as cheap as it was.
             */
            int g    = tbl.gain[yav];
            int cg   = 64, dy = 0;      /* 64 = unity in Q6 */

            if (gradeq < 2) {
                if (gradeq == 0) {
                    int uu = (u + 128 < 0) ? 0 : (u + 128 > 255 ? 255 : u + 128);
                    int vv = (v + 128 < 0) ? 0 : (v + 128 > 255 ? 255 : v + 128);
                    int cell = (vv >> 2) * SE_YUV_GRID + (uu >> 2);
                    cg = tbl.cg[cell];
                    dy = tbl.dy[cell];
                }
                /* NORMAL keeps the luma gain and the matrix, drops the cells. */
                int cgg  = cg * g;
                int ru   = (tbl.matrix[0] * u + tbl.matrix[1] * v) >> 8;
                int rv   = (tbl.matrix[2] * u + tbl.matrix[3] * v) >> 8;
                int nu   = tbl.uoff[yav] + ((ru * cgg) >> 12);
                int nv   = tbl.voff[yav] + ((rv * cgg) >> 12);
                int dith = fs_yuv_dither[y & 7][(x >> 1) & 7];
                int ny0  = tbl.y[y0] + dy + fs_yuv_flat_dither(y0, dith);
                int ny1  = tbl.y[y1] + dy;
                dith = fs_yuv_dither[y & 7][((x >> 1) + 1) & 7];
                ny1 += fs_yuv_flat_dither(y1, dith);

                p[0] = (unsigned char)(signed char)(nu < -128 ? -128 : nu > 127 ? 127 : nu);
                p[2] = (unsigned char)(signed char)(nv < -128 ? -128 : nv > 127 ? 127 : nv);
                p[1] = (unsigned char)(ny0 < 0 ? 0 : ny0 > 255 ? 255 : ny0);
                p[3] = (unsigned char)(ny1 < 0 ? 0 : ny1 > 255 ? 255 : ny1);
            } else {
                /* FAST: chroma bytes are left exactly as Canon wrote them. */
                int dith = fs_yuv_dither[y & 7][(x >> 1) & 7];
                int ny0 = tbl.y[y0] + fs_yuv_flat_dither(y0, dith);
                int ny1 = tbl.y[y1];
                dith = fs_yuv_dither[y & 7][((x >> 1) + 1) & 7];
                ny1 += fs_yuv_flat_dither(y1, dith);
                p[1] = (unsigned char)(ny0 < 0 ? 0 : ny0 > 255 ? 255 : ny0);
                p[3] = (unsigned char)(ny1 < 0 ? 0 : ny1 > 255 ? 255 : ny1);
            }
        }
    }
#ifdef SE_HOSTED
    return 0;
#else
    return get_ms_clock() - t0;
#endif
}
