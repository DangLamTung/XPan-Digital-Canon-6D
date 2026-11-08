/**
 * Host harness for the RAW: Develop path.
 *
 * Takes a PPM, turns it into an RGGB Bayer frame the way a sensor would
 * (display RGB -> linear -> inverse of the camera->sRGB matrix -> 12-bit
 * sites on top of a black level), then runs the *same* code the camera
 * module runs: fs_core_dev_wb_from_sums, fs_core_demosaic,
 * fs_core_dev_pixel and fs_core_tiff_header.  One TIFF per Film Look.
 *
 * What this proves: the demosaic, white balance, matrix, transfer curve,
 * film LUT, split tone and TIFF writer all produce a sane image from Bayer
 * data.  What it cannot prove: that raw_info holds the buffer we think it
 * does on the camera - that needs the camera.
 *
 * Build:  make -C magiclantern/modules/film_sim/tools/sim devsim
 * Run:    ./devsim in.ppm outdir [look]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

#include "../../fs_core.h"

/* Canon EOS 6D DNG ColorMatrix1 (XYZ -> camera), as n/10000 rationals. */
static int cam_cm[18] =
{
     4558, 10000,  -848, 10000,  -359, 10000,
    -6077, 10000, 12984, 10000,  3469, 10000,
     -287, 10000,  1038, 10000,  7489, 10000,
};

static int      W, H;
static unsigned char *rgb;
static unsigned short *bayer;
static int      black = 2048, white = 15000;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void read_ppm(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }

    char magic[3];
    int maxv = 0;
    if (fscanf(f, "%2s %d %d %d", magic, &W, &H, &maxv) != 4 || strcmp(magic, "P6") != 0)
    {
        fprintf(stderr, "%s: not a P6 PPM\n", path);
        exit(1);
    }
    fgetc(f);
    rgb = malloc((size_t) W * H * 3);
    if (!rgb || fread(rgb, 1, (size_t) W * H * 3, f) != (size_t) W * H * 3)
    {
        fprintf(stderr, "%s: short read\n", path);
        exit(1);
    }
    fclose(f);
    printf("input %dx%d  (%s)\n", W, H, path);
}

/*
 * Synthesise the sensor frame the way light actually arrives: sRGB -> XYZ ->
 * the camera's own ColorMatrix1 -> 12-bit sites on top of a black level.
 * This is independent of the develop matrix, so a round trip that returns the
 * original colour means the develop path is right.
 */
static void build_bayer(void)
{
    static const float xyz2srgb[9] =
    {
         3.2406f, -1.5372f, -0.4986f,
        -0.9689f,  1.8758f,  0.0415f,
         0.0557f, -0.2040f,  1.0570f,
    };

    float srgb2xyz[9], m[9];
    if (fs_core_inv3(xyz2srgb, srgb2xyz) != 0) { fprintf(stderr, "xyz invert failed\n"); exit(1); }

    for (int i = 0; i < 9; i++)
        m[i] = (float) cam_cm[i * 2] / (float) cam_cm[i * 2 + 1];

    float tocam[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            tocam[i * 3 + j] = m[i * 3] * srgb2xyz[j]
                             + m[i * 3 + 1] * srgb2xyz[3 + j]
                             + m[i * 3 + 2] * srgb2xyz[6 + j];

    static float lin[256];
    for (int i = 0; i < 256; i++) lin[i] = (float) pow(i / 255.0, 2.2);

    int span = white - black;
    bayer = malloc((size_t) W * H * sizeof(unsigned short));
    if (!bayer) { fprintf(stderr, "out of memory\n"); exit(1); }

    /* find the brightest channel and expose so nothing clips: a synthetic frame
     * that clips its green channel stops being neutral at the top end, which
     * would look like a colour bug in the develop path */
    float peak = 0.0f;
    for (int i = 0; i < W * H; i++)
    {
        const unsigned char * p = rgb + (size_t) i * 3;
        float lr = lin[p[0]], lg = lin[p[1]], lb = lin[p[2]];
        for (int r = 0; r < 3; r++)
        {
            float v = tocam[r * 3] * lr + tocam[r * 3 + 1] * lg + tocam[r * 3 + 2] * lb;
            if (v > peak) peak = v;
        }
    }
    float scale = peak > 0.0001f ? 0.97f / peak : 1.0f;
    printf("peak %.4f -> exposure scale %.3f\n", peak, scale);

    for (int y = 0; y < H; y++)
    {
        for (int x = 0; x < W; x++)
        {
            const unsigned char * p = rgb + ((size_t) y * W + x) * 3;
            float lr = lin[p[0]], lg = lin[p[1]], lb = lin[p[2]];

            float cr = tocam[0] * lr + tocam[1] * lg + tocam[2] * lb;
            float cg = tocam[3] * lr + tocam[4] * lg + tocam[5] * lb;
            float cb = tocam[6] * lr + tocam[7] * lg + tocam[8] * lb;

            int c = ((y & 1) << 1) | (x & 1);          /* RGGB */
            float v = (c == 0) ? cr : (c == 3) ? cb : cg;

            int q = (int) (v * scale * span + 0.5);
            bayer[(size_t) y * W + x] = (unsigned short) fs_clamp_i(q, 0, span) + black;
        }
    }
}

/* row pointer mirrored at the edges, the way the module's window does it:
 * row -1 is row 1 and row H is row H-2, so the CFA parity is preserved */
static const unsigned short * row(int y)
{
    if (y < 0) y = -y;
    if (y >= H) y = 2 * (H - 1) - y;
    if (y < 0) y = 0;
    if (y >= H) y = H - 1;
    return bayer + (size_t) y * W;
}

static int write_look(const char *path, const fs_lut_t *lut, const fs_dev_t *dev)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }

    unsigned char hdr[FS_TIFF_HDR_SIZE];
    fs_core_tiff_header(hdr, W, H);
    if (fwrite(hdr, 1, FS_TIFF_HDR_SIZE, f) != FS_TIFF_HDR_SIZE) { fclose(f); return 1; }

    const int rows = 32;
    unsigned char *strip = malloc((size_t) W * 3 * rows);
    if (!strip) { fclose(f); return 1; }

    int span = white - black, bad = 0, at = 0;
    int inv_span = ((FS_DEV_LINEAR_MAX << 16) + span / 2) / span;

    for (int y = 0; y < H; y++)
    {
        unsigned char *o = strip + (size_t) at * W * 3;
        const unsigned short *pv = row(y - 1), *cu = row(y), *nx = row(y + 1);

        for (int x = 0; x < W; x++)
        {
            int s[3];
            fs_core_demosaic(pv, cu, nx, x, y, W, s);
            fs_core_dev_pixel(lut, dev,
                              ((s[0] - black) * inv_span) >> 16,
                              ((s[1] - black) * inv_span) >> 16,
                              ((s[2] - black) * inv_span) >> 16,
                              o + x * 3);
        }

        if (++at == rows || y == H - 1)
        {
            if (fwrite(strip, 1, (size_t) at * W * 3, f) != (size_t) at * W * 3) bad++;
            at = 0;
        }
    }
    free(strip);
    fclose(f);
    return bad;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s in.ppm outdir [look-index]\n", argv[0]);
        return 1;
    }
    const char *outdir = argv[2];
    int only = argc > 3 ? atoi(argv[3]) : -1;

    mkdir(outdir, 0755);
    read_ppm(argv[1]);
    build_bayer();

    fs_dev_t dev;
    fs_core_dev_init(&dev, cam_cm, 6500, 256, 256);
    printf("bayer %dx%d  black %d  white %d  matrix %s\n",
           W, H, black, white, dev.have_matrix ? "ok" : "identity");
    printf("camera->sRGB Q10: %d %d %d | %d %d %d | %d %d %d\n",
           dev.m[0], dev.m[1], dev.m[2], dev.m[3], dev.m[4], dev.m[5],
           dev.m[6], dev.m[7], dev.m[8]);

    /* stage probe: bayer -> demosaic -> matrix+curve, for one pixel */
    if (getenv("PROBE"))
    {
        int px = atoi(getenv("PROBE")), wx = px % W, wy = px / W;
        int s3[3];
        fs_core_demosaic(row(wy - 1), row(wy), row(wy + 1), wx, wy, W, s3);
        unsigned char o3[3];
        fs_lut_t nl;
        fs_core_build_luts(&nl, &fs_presets[0], 100, 0);
        int iv = ((FS_DEV_LINEAR_MAX << 16) + (white - black) / 2) / (white - black);
        fs_core_dev_pixel(&nl, &dev,
                          ((s3[0] - black) * iv) >> 16,
                          ((s3[1] - black) * iv) >> 16,
                          ((s3[2] - black) * iv) >> 16, o3);
        printf("probe (%d,%d) cfa=%d src=(%d,%d,%d) linear=(%d,%d,%d) out=(%d,%d,%d)\n",
               wx, wy, ((wy & 1) << 1) | (wx & 1),
               rgb[(wy * W + wx) * 3], rgb[(wy * W + wx) * 3 + 1], rgb[(wy * W + wx) * 3 + 2],
               s3[0], s3[1], s3[2], o3[0], o3[1], o3[2]);
    }

    char path[512];
    printf("%-18s %10s %8s\n", "look", "ms/frame", "MP/s");
    double px = (double) W * H;

    for (int i = 0; i < FS_PRESET_COUNT; i++)
    {
        if (only >= 0 && i != only) continue;

        fs_lut_t lut;
        fs_core_build_luts(&lut, &fs_presets[i], 100, 0);

        double t0 = now_ms();
        snprintf(path, sizeof(path), "%s/%02d-%s.tif", outdir, i, i == 0 ? "Neutral" : "look");
        write_look(path, &lut, &dev);
        double dt = now_ms() - t0;

        printf("%-18s %10.1f %8.2f\n", fs_preset_names[i], dt, px / dt / 1000.0);
    }

    /* scale to the 6D sensor so the camera cost can be read off this table */
    printf("\n6D full frame is 5472x3648 = %.1f MP = %.2fx this run\n",
           5472.0 * 3648.0 / 1e6, 5472.0 * 3648.0 / px);

    return 0;
}
