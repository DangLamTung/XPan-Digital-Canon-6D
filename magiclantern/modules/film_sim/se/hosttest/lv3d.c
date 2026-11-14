/*
 * lv3d - the LiveView look as a joint (y,u,v) lattice, against the forms it
 * could replace, on the panel's own geometry.
 *
 * The LiveView directory grades a 1440-byte UYVY row, 480 of them. This reads
 * a 720x480 P6 PPM (two pixels per UYVY word), builds that plane, bakes every
 * form from ONE .fs recipe, and reports two things per form:
 *
 *   accuracy  per-channel error against se_look_rows -- the develop
 *   cost      ms/frame on this machine, best of several passes
 *
 *   ./lv3d input.ppm stock.fs
 *
 * Nothing here is measured on the camera. The cost RATIO is what carries over;
 * the absolute milliseconds are this Mac's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../engine/se.h"
#include "../engine/look.h"
#include "../engine/stock.h"
#include "../../fs_core.h"
#include "../../fs_yuvlook.h"

/* The module header and the engine must agree on the lattice, or the filter
 * would index a buffer the bake never filled. */
typedef char fs_lv3d_n_check[(SE_YUV3_NY == FS_UV3_NY && SE_YUV3_NC == FS_UV3_NC) ? 1 : -1];

#define PITCH 1440                 /* bytes a row, UYVY              */
#define PIX   (PITCH / 2)          /* pixels a row                   */
#define H     480
#define RGBP  (PIX * 3)            /* bytes of RGB a row             */

static unsigned char plane[PITCH * H];
static unsigned char in_rgb[RGBP * H];
static unsigned char truth[RGBP * H];

static int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void rgb_to_ycc(const unsigned char *p, int *Y, int *U, int *V)
{
    *Y = (77 * p[0] + 150 * p[1] + 29 * p[2] + 128) >> 8;
    *U = (-43 * p[0] - 85 * p[1] + 128 * p[2] + 128) >> 8;
    *V = (128 * p[0] - 107 * p[1] - 21 * p[2] + 128) >> 8;
}

/* UYVY -> interleaved RGB, `npix` pixels. The same conversion tableerr and
 * lvgrade use, so the numbers stay comparable between tools. */
static void unpack(const unsigned char *s, unsigned char *o, int npix)
{
    for (int x = 0; x + 1 < npix; x += 2) {
        int u = (signed char)s[0], y0 = s[1], v = (signed char)s[2], y1 = s[3];
        o[0] = clamp8(y0 + ((359 * v + 128) >> 8));
        o[1] = clamp8(y0 - ((88 * u + 183 * v) >> 8));
        o[2] = clamp8(y0 + ((454 * u) >> 8));
        o[3] = clamp8(y1 + ((359 * v) >> 8));
        o[4] = clamp8(y1 - ((88 * u + 183 * v) >> 8));
        o[5] = clamp8(y1 + ((454 * u) >> 8));
        s += 4;
        o += 6;
    }
}

static int read_ppm(const char *path, unsigned char *rgb, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    char magic[3] = {0};
    int maxv;

    if (!f) return -1;
    if (fscanf(f, "%2s %d %d %d", magic, w, h, &maxv) != 4 ||
        strcmp(magic, "P6") || maxv != 255 || fgetc(f) != '\n') {
        fclose(f);
        return -1;
    }
    if (fread(rgb, 1, (size_t)(*w) * (*h) * 3, f) != (size_t)(*w) * (*h) * 3) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/* A sink that ADVANCES: se_look_rows() emits one call per row, and a sink
 * that always writes to the same address would leave only the last row in the
 * buffer (which is exactly what made the first run of this tool compare three
 * forms against a nearly empty truth). */
static int sink_row(void *ctx, const unsigned char *row, int width)
{
    unsigned char **p = ctx;
    memcpy(*p, row, (size_t)width * 3);
    *p += (size_t)width * 3;
    return 0;
}

/* The develop: the same pixels through se_look_rows. */
static int develop(se_look *lk, unsigned char *in, unsigned char *out,
                   int npix, int nrows)
{
    size_t need = se_look_bytes(lk, npix);
    void *mem = malloc(need + 64);
    se_arena ar;
    se_look_run *run;
    unsigned char *dst = out;
    int rc = -1;

    if (!mem) return -1;
    se_arena_init(&ar, mem, need + 64);
    if (se_look_begin(&run, lk, npix, &ar) != SE_OK) goto done;
    if (se_look_rows(run, in, nrows, sink_row, &dst) != SE_OK) goto done;
    se_look_end(run, sink_row, &dst);
    rc = 0;
done:
    free(mem);
    return rc;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

typedef struct { long n; double sum; int max; double sy, scb, scr, smag; } stat;

static void stat_add(stat *s, int d) { s->n++; s->sum += d; if (d > s->max) s->max = d; }

static void stat_show(const char *nm, const stat *s)
{
    if (!s->n) { printf("  %-38s (no samples)\n", nm); return; }
    printf("  %-38s mean %5.2f  max %3d   [dY %4.2f dCb %4.2f dCr %4.2f |C| %4.1f]\n",
           nm, s->sum / (double)s->n, s->max,
           s->sy / (double)s->n, s->scb / (double)s->n, s->scr / (double)s->n,
           s->smag / (double)s->n);
}

/* Score a graded plane against the develop, per channel. */
static void score(const unsigned char *graded, stat *s)
{
    static unsigned char got[RGBP * H];
    int y, x, i;

    for (y = 0; y < H; y++)
        unpack(graded + (size_t)y * PITCH, got + (size_t)y * RGBP, PIX);
    for (y = 0; y < H; y++)
        for (x = 0; x < RGBP; x += 3)
            for (i = 0; i < 3; i++) {
                int a = got[(size_t)y * RGBP + x + i];
                int b = truth[(size_t)y * RGBP + x + i];
                stat_add(s, a > b ? a - b : b - a);
            }

    /* where the error lives: luma, Cb, Cr -- and how much chroma there was */
    for (y = 0; y < H; y++)
        for (x = 0; x < RGBP; x += 3) {
            const unsigned char *g = got + (size_t)y * RGBP + x;
            const unsigned char *t = truth + (size_t)y * RGBP + x;
            int Yg, Ug, Vg, Yt, Ut, Vt;
            rgb_to_ycc(g, &Yg, &Ug, &Vg);
            rgb_to_ycc(t, &Yt, &Ut, &Vt);
            s->sy  += Yg > Yt ? Yg - Yt : Yt - Yg;
            s->scb += Ug > Ut ? Ug - Ut : Ut - Ug;
            s->scr += Vg > Vt ? Vg - Vt : Vt - Vg;
            s->smag += (Ut < 0 ? -Ut : Ut) + (Vt < 0 ? -Vt : Vt);
        }
}

static int load_stock(const char *path, se_stock *st)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *text;
    size_t got;
    int rc;

    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) { fclose(f); return -1; }
    text = malloc((size_t)sz);
    if (!text) { fclose(f); return -1; }
    got = fread(text, 1, (size_t)sz, f);
    fclose(f);
    rc = se_stock_parse(text, got, st);
    free(text);
    return rc == SE_OK && st->name[0] ? 0 : -1;
}

/* (Y, Cb, Cr) -> RGB, the frame's own integer conversion. */
static void ycc_to_rgb(int y, int u, int v, unsigned char *o)
{
    o[0] = clamp8(y + ((359 * v) >> 8));
    o[1] = clamp8(y - ((88 * u + 183 * v) >> 8));
    o[2] = clamp8(y + ((454 * u) >> 8));
}

/* Run the look over npix pixels that are not a frame (bake probes). */
static int run_look_rgb(const se_look *lk, const unsigned char *in,
                        unsigned char *out, int npix)
{
    static unsigned char buf[70000 * 3];
    if (npix > 70000) return -1;
    memcpy(buf, in, (size_t)npix * 3);
    return develop((se_look *)lk, buf, out, npix, 1);
}

/* ======================================================================
 * The Cb/Cr factor array, parameterised.
 *
 * This is the structure the shot table already has -- a 32x32 map of chroma
 * factors measured at one reference luma, times a scalar gain per luma -- but
 * with the map size and a luma-band count exposed, so the question "would a
 * bigger factor array, or a banded one, close the gap to the joint lattice"
 * can be answered instead of argued.
 *
 * bands = 1 is the classic factor array. A banded variant (each luma band with
 * its own map) was implemented and abandoned: normalising each band's gain to
 * its own centre luma blows up in the dark bands, where the ring used to
 * measure gain carries almost no chroma, and the banded runs measured WORSE
 * than the single map (dCb 2.9 against 0.88). That is a fault in this model,
 * not a property of banding, so no banded number is reported here.
 * ====================================================================== */
#define M2_N_MAX 128
#define M2_B_MAX 8

typedef struct {
    int N, B;
    short map[M2_B_MAX][M2_N_MAX * M2_N_MAX * 3];   /* dy, Cb, Cr */
    unsigned char y[256];
    int gain[256];          /* Q6: 64 at the reference luma of its band */
    int uo[256], vo[256];
} m2_t;

static m2_t m2;

static int band_of(int y) { return y * m2.B / 256; }
static int band_centre(int b) { return (b * 256 + 128) / m2.B; }

static int bake_m2(const se_look *lk, int N, int B)
{
    static unsigned char in[70000 * 3], out[70000 * 3];
    int np = 0, y, b, iu, iv, a;
    int idx_neutral, idx_ring, idx_map;

    if (N < 2 || N > M2_N_MAX || B < 1 || B > M2_B_MAX) return -1;
    m2.N = N; m2.B = B;

    /* every probe this model needs, in one row so the look runs once */
    idx_neutral = np;
    for (y = 0; y < 256; y++) {
        in[np * 3 + 0] = (unsigned char)y;
        in[np * 3 + 1] = (unsigned char)y;
        in[np * 3 + 2] = (unsigned char)y;
        np++;
    }
    idx_ring = np;
    for (y = 0; y < 256; y++)
        for (a = 0; a < 8; a++) {
            int u = (int)(40.0 * 0.7071 * (a & 1 ? 1 : -1) * (a & 2 ? 0.5 : 1));
            int v = (int)(40.0 * 0.7071 * (a & 4 ? 1 : -1) * (a & 4 ? 1 : 0.5));
            unsigned char px[3];
            ycc_to_rgb(y, u, v, px);
            in[np * 3 + 0] = px[0]; in[np * 3 + 1] = px[1]; in[np * 3 + 2] = px[2];
            np++;
        }
    idx_map = np;
    for (b = 0; b < B; b++)
        for (iv = 0; iv < N; iv++)
            for (iu = 0; iu < N; iu++) {
                int u  = -128 + iu * 255 / (N - 1);
                int vv = -128 + iv * 255 / (N - 1);
                unsigned char px[3];
                ycc_to_rgb(band_centre(b), u, vv, px);
                in[np * 3 + 0] = px[0]; in[np * 3 + 1] = px[1]; in[np * 3 + 2] = px[2];
                np++;
            }
    if (np > 70000) return -1;
    if (run_look_rgb(lk, in, out, np) != 0) return -1;

    /* tone curve and neutral cast */
    for (y = 0; y < 256; y++) {
        int Y, U, V;
        rgb_to_ycc(out + (size_t)(idx_neutral + y) * 3, &Y, &U, &V);
        m2.y[y] = (unsigned char)Y;
        m2.uo[y] = U;
        m2.vo[y] = V;
    }

    /* one scalar gain per luma: how much chroma survives, averaged over the
     * ring, relative to that band's reference luma */
    for (y = 0; y < 256; y++) {
        int k, sy = 0, sn = 0;
        for (k = 0; k < 8; k++) {
            const unsigned char *g = out + (size_t)(idx_ring + y * 8 + k) * 3;
            int Y, U, V;
            rgb_to_ycc(g, &Y, &U, &V);
            sy += (U < 0 ? -U : U) + (V < 0 ? -V : V);
            sn += 64;      /* the ring radius, |u|+|v| of the probe (approx) */
        }
        m2.gain[y] = sn ? (sy * 64 + sn / 2) / sn : 64;
        if (m2.gain[y] < 1) m2.gain[y] = 1;
    }
    /* normalise each band so its centre luma has gain 64 */
    for (b = 0; b < B; b++) {
        int ref = m2.gain[band_centre(b)];
        if (ref < 1) ref = 1;
        for (y = b * 256 / B; y < (b + 1) * 256 / B && y < 256; y++)
            m2.gain[y] = m2.gain[y] * 64 / ref;
    }

    /* the map itself, in frame units, with the neutral cast removed */
    np = idx_map;
    for (b = 0; b < B; b++)
        for (iv = 0; iv < N; iv++)
            for (iu = 0; iu < N; iu++) {
                int Y, U, V;
                size_t k = ((size_t)b * N * N + (size_t)iv * N + iu) * 3;
                rgb_to_ycc(out + (size_t)np * 3, &Y, &U, &V);
                /* dy: what this colour does to luma, vs grey of the same
                 * luma. Without it a factor array cannot move brightness by
                 * hue, and CLASSIC CHROME stays 3.35 levels out on luma
                 * however fine the (Cb,Cr) map gets. */
                m2.map[b][k]     = (short)(Y - m2.y[band_centre(b)]);
                m2.map[b][k + 1] = (short)(U - m2.uo[band_centre(b)]);
                m2.map[b][k + 2] = (short)(V - m2.vo[band_centre(b)]);
                np++;
            }
    return 0;
}

static void apply_m2(const void *src, void *dst, int pitch, int row0, int row1, int interp)
{
    const int N = m2.N;
    int words = pitch >> 2;
    const unsigned int *sp = (const unsigned int *)src + (unsigned int)row0 * words;
    unsigned int *dp = (unsigned int *)dst + (unsigned int)row0 * words;

    for (int y = row0; y < row1; y++) {
        for (int i = 0; i < words; i++) {
            unsigned int px = sp[i];
            int u  = (signed char)px;
            int ya = (px >> 8) & 0xFF;
            int vv = (signed char)(px >> 16);
            int yb = (px >> 24) & 0xFF;
            int ym = (ya + yb) >> 1;
            int b  = band_of(ym);
            int tu = (u + 128) * 65536 * (N - 1) / 255;
            int tv = (vv + 128) * 65536 * (N - 1) / 255;
            int iu = tu >> 16, iv = tv >> 16;
            int fu = (tu >> 8) & 0xFF, fv = (tv >> 8) & 0xFF;
            int nu, nv, dy = 0;
            const short *base;

            if (iu > N - 2) { iu = N - 2; fu = 256; }
            if (iv > N - 2) { iv = N - 2; fv = 256; }
            base = m2.map[b] + ((size_t)iv * N + iu) * 3;
            if (!interp) {                      /* nearest cell, no blend */
                dy = base[0];
                nu = base[1];
                nv = base[2];
                goto have;
            }
            {
                int c;
                for (c = 0; c < 3; c++) {
                    int c0 = base[c],            c1 = base[c + 3];
                    int c2 = base[N * 3 + c],    c3 = base[N * 3 + c + 3];
                    int a2 = c0 + (((c1 - c0) * fu) >> 8);
                    int b2 = c2 + (((c3 - c2) * fu) >> 8);
                    int r2 = a2 + (((b2 - a2) * fv) >> 8);
                    if (c == 0) dy = r2; else if (c == 1) nu = r2; else nv = r2;
                }
            }
            goto have2;
            (void)0;
        have:
            fu = fv = 0;
        have2:
            nu = m2.uo[ym] + nu * m2.gain[ym] / 64;
            nv = m2.vo[ym] + nv * m2.gain[ym] / 64;
            dp[i] = ((unsigned int)(unsigned char)(signed char)fs_clamp_i(nu, -128, 127))
                  | ((unsigned int)fs_clamp_i(m2.y[ya] + dy, 0, 255) << 8)
                  | ((unsigned int)(unsigned char)(signed char)fs_clamp_i(nv, -128, 127) << 16)
                  | ((unsigned int)fs_clamp_i(m2.y[yb] + dy, 0, 255) << 24);
        }
        sp += words;
        dp += words;
    }
}

#define PASSES 40

int main(int argc, char **argv)
{
    static unsigned char rgb[PIX * H * 3];
    static unsigned char work[PITCH * H];
    static fs_lut_t lut_preset;
    int w = 0, h = 0, x, y, pass;
    se_stock st;
    se_look lk;
    se_yuv3 t3;
    stat s_lat = {0}, s_shot = {0}, s_preset = {0};
    double t_lat = 1e9, t_preset = 1e9, t_shot = 1e9;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <720x480.ppm> <stock.fs>\n", argv[0]);
        return 2;
    }
    if (read_ppm(argv[1], rgb, &w, &h) != 0) {
        fprintf(stderr, "read %s failed\n", argv[1]);
        return 1;
    }
    if (w != PIX || h != H) {
        fprintf(stderr, "%s is %dx%d, expected %dx%d\n", argv[1], w, h, PIX, H);
        return 1;
    }
    if (load_stock(argv[2], &st) != 0) {
        fprintf(stderr, "parse %s failed\n", argv[2]);
        return 1;
    }

    /* the UYVY plane this picture would occupy in VRAM */
    for (y = 0; y < H; y++)
        for (x = 0; x < PIX; x += 2) {
            unsigned char *q = plane + (size_t)y * PITCH + (size_t)x * 2;
            int Y0, U0, V0, Y1, U1, V1;
            rgb_to_ycc(rgb + ((size_t)y * PIX + x) * 3, &Y0, &U0, &V0);
            rgb_to_ycc(rgb + ((size_t)y * PIX + x + 1) * 3, &Y1, &U1, &V1);
            (void)U1; (void)V1;              /* 4:2:2 keeps the left chroma */
            q[0] = (unsigned char)(signed char)U0;
            q[1] = (unsigned char)Y0;
            q[2] = (unsigned char)(signed char)V0;
            q[3] = (unsigned char)Y1;
        }

    /* The look, and the lattice form of it. */
    se_look_from_stock(&st, 1, &lk);
    se_look_set_strength(&lk, 100);
    lk.grain = 0;
    lk.hal_amount = 0;
    se_look_bake_yuv3(&lk, &t3);

    /* The develop: unpack the plane row by row, then grade it whole. */
    for (y = 0; y < H; y++)
        unpack(plane + (size_t)y * PITCH, in_rgb + (size_t)y * RGBP, PIX);
    if (develop(&lk, in_rgb, truth, PIX, H) != 0) {
        fprintf(stderr, "develop failed\n");
        return 1;
    }

    /* --- the joint lattice ------------------------------------------- */
    for (pass = 0; pass < PASSES; pass++) {
        unsigned int rnd = 12345u;
        double t0, dt;
        memcpy(work, plane, sizeof work);
        t0 = now_ms();
        fs_core_process_rows3(t3.y, t3.v, work, work, PITCH, 0, H, 0, &rnd);
        dt = now_ms() - t0;
        if (dt < t_lat) t_lat = dt;
    }
    score(work, &s_lat);

    /* --- the shipping separable shot tables -------------------------- */
    if (fs_yuvlook_bake(argv[2], 100) == 0) {
        double best = 1e9;
        for (pass = 0; pass < PASSES; pass++) {
            double t;
            memcpy(work, plane, sizeof work);
            t = (double)fs_yuvlook_apply(work, PIX, H, PITCH);
            if (t > 0 && t < best) best = t;
        }
        t_shot = best;
        score(work, &s_shot);
    }

    /* --- the LiveView preset form, which is what runs today ----------- */
    fs_core_build_luts(&lut_preset, &fs_presets[FS_PRESET_FUJI_SUPERIA], 100, 0);
    for (pass = 0; pass < PASSES; pass++) {
        unsigned int rnd = 12345u;
        double t0, dt;
        memcpy(work, plane, sizeof work);
        t0 = now_ms();
        fs_core_process_rows(&lut_preset, work, work, PITCH, 0, H, &rnd);
        dt = now_ms() - t0;
        if (dt < t_preset) t_preset = dt;
    }
    score(work, &s_preset);

    printf("%dx%d panel, %s + %s\n", PIX, H, argv[1], st.name);
    printf("\nerror against se_look_rows on the same pixels (levels, per channel)\n");
    {
        char nm[64];
        snprintf(nm, sizeof nm, "joint (y,u,v) lattice, %d B",
                 256 + SE_YUV3_NODES * 3 * 2);
        stat_show(nm, &s_lat);
    }
    stat_show("separable shot tables, 9228 B", &s_shot);
    stat_show("LV preset form, a DIFFERENT look", &s_preset);
    printf("\ncost on this Mac, best of %d passes\n", PASSES);
    printf("  %-30s %7.2f ms/frame\n", "joint (y,u,v) lattice", t_lat);
    printf("  %-30s %7.2f ms/frame\n", "LV preset tables", t_preset);
    printf("  lattice / preset: %.2fx\n", t_lat / t_preset);
    if (t_shot < 1e9)
        printf("  %-30s %7.2f ms/frame\n", "separable shot tables", t_shot);

    /* --- the Cb/Cr factor array, at several sizes and band counts ---- */
    {
        static const int cfg[][3] = { {32,1,1}, {64,1,1}, {128,1,1},
                                      {32,1,0}, {64,1,0}, {128,1,0} };
        /* B is always 1; see the note on the banded variant above. */
        printf("%dx%d panel, %s + %s\n", PIX, H, argv[1], st.name);
        printf("\nCb/Cr factor array vs the joint lattice (same frame, same truth)\n");
        for (unsigned ci = 0; ci < sizeof cfg / sizeof cfg[0]; ci++) {
            stat s2 = {0};
            double best = 1e9;
            int N = cfg[ci][0], B = cfg[ci][1], ip = cfg[ci][2];
            if (bake_m2(&lk, N, B) != 0) { printf("  %dx%d/%d band(s): bake failed\n", N, N, B); continue; }
            for (pass = 0; pass < PASSES; pass++) {
                double t0, dt;
                memcpy(work, plane, sizeof work);
                t0 = now_ms();
                apply_m2(work, work, PITCH, 0, H, ip);
                dt = now_ms() - t0;
                if (dt < best) best = dt;
            }
            score(work, &s2);
            printf("  %3dx%-3d map, %s, %6d B  ", N, N, ip ? "bilinear" : "nearest ",
                   N * N * 3 * 2 * B + 256 + 1024 + 2048);
            printf("mean %5.2f  [dY %4.2f dCb %4.2f dCr %4.2f]  %5.2f ms  %.2fx\n",
                   s2.sum / (double)s2.n, s2.sy / (double)s2.n, s2.scb / (double)s2.n,
                   s2.scr / (double)s2.n, best, best / t_preset);
        }
    }

    return 0;
}
