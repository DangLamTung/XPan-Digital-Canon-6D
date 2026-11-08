/**
 * Host simulator for the Magic Lantern film simulation.
 *
 * Compiles the same fs_core.h the camera module uses, feeds it a synthetic
 * 720x480 UYVY frame (the geometry of the 6D LiveView buffer) and writes
 * before/after images, so the look and the per-pixel cost can be checked
 * without a camera.
 *
 * Build:  make -C magiclantern/modules/film_sim/tools/sim
 * Run:    ./filmsim_sim [outdir]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif
#include <time.h>

#include "../../fs_core.h"

#define W 720
#define H 480
#define PITCH (W * 2)

static unsigned char rgb[W * H * 3];
static unsigned char yuv_src[PITCH * H];
static unsigned char yuv_dst[PITCH * H];

static void set_px(int x, int y, int r, int g, int b)
{
    unsigned char *p = &rgb[(y * W + x) * 3];
    p[0] = (unsigned char)fs_clamp_i(r, 0, 255);
    p[1] = (unsigned char)fs_clamp_i(g, 0, 255);
    p[2] = (unsigned char)fs_clamp_i(b, 0, 255);
}

static void fill_rect(int x0, int y0, int x1, int y1, int r, int g, int b)
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            set_px(x, y, r, g, b);
}

static void build_scene(void)
{
    /* colour patches, roughly like a colour checker */
    static const int patches[6][3] =
    {
        { 235, 200, 180 },  /* light skin */
        { 190, 130, 105 },  /* skin */
        { 110, 145,  90 },  /* foliage */
        {  90, 130, 185 },  /* sky */
        { 200, 160,  60 },  /* warm yellow */
        {  55,  55,  55 },  /* dark */
    };

    for (int i = 0; i < 6; i++)
        fill_rect(i * 120, 0, (i + 1) * 120, 240,
                  patches[i][0], patches[i][1], patches[i][2]);

    /* smooth horizontal ramp */
    for (int y = 240; y < 320; y++)
        for (int x = 0; x < W; x++)
        {
            int v = x * 255 / (W - 1);
            set_px(x, y, v, v, v);
        }

    /* 8-step grey wedge, partly overlaid with a strong red patch */
    for (int y = 320; y < 400; y++)
        for (int x = 0; x < W; x++)
        {
            int v = (x / 90) * 255 / 7;
            set_px(x, y, v, v, v);
        }
    fill_rect(0, 320, 200, 400, 220, 40, 40);

    /* high frequency detail (checkerboard) + colour squares */
    for (int y = 400; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            int on = ((x >> 3) ^ (y >> 3)) & 1;
            int v = on ? 210 : 60;
            set_px(x, y, v, v, v);
        }
    fill_rect(420, 400, 540, 480, 40, 70, 190);
    fill_rect(540, 400, 660, 480, 60, 160, 70);
    fill_rect(660, 400, 720, 480, 230, 120, 20);
}

/* BT.601 full range, same coefficients as imgconv.h */
static void rgb_to_uyvy(void)
{
    for (int y = 0; y < H; y++)
    {
        unsigned char *row = &yuv_src[y * PITCH];
        for (int x = 0; x < W; x += 2)
        {
            int r0 = rgb[(y * W + x) * 3 + 0];
            int g0 = rgb[(y * W + x) * 3 + 1];
            int b0 = rgb[(y * W + x) * 3 + 2];
            int r1 = rgb[(y * W + x + 1) * 3 + 0];
            int g1 = rgb[(y * W + x + 1) * 3 + 1];
            int b1 = rgb[(y * W + x + 1) * 3 + 2];

            int y0 = (299 * r0 + 587 * g0 + 114 * b0) / 1000;
            int y1 = (299 * r1 + 587 * g1 + 114 * b1) / 1000;

            /* signed chroma, neutral 0: the camera's UYVY buffers hold
             * two's complement bytes (see imgconv.h UYVY_GET_U) */
            int u0 = (-169 * r0 - 331 * g0 + 500 * b0) / 1000;
            int u1 = (-169 * r1 - 331 * g1 + 500 * b1) / 1000;
            int v0 = ( 500 * r0 - 419 * g0 -  81 * b0) / 1000;
            int v1 = ( 500 * r1 - 419 * g1 -  81 * b1) / 1000;

            int u = (u0 + u1) / 2;
            int v = (v0 + v1) / 2;

            unsigned int px = (unsigned int)(u & 0xFF)
                            | ((unsigned int)(y0 & 0xFF) << 8)
                            | ((unsigned int)(v & 0xFF) << 16)
                            | ((unsigned int)(y1 & 0xFF) << 24);
            memcpy(row + x * 2, &px, 4);
        }
    }
}

static void read_ppm(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    char magic[3];
    int w = 0, h = 0, maxv = 0;
    if (fscanf(f, "%2s %d %d %d", magic, &w, &h, &maxv) != 4 || strcmp(magic, "P6") != 0)
    {
        fprintf(stderr, "%s: invalid P6 PPM format\n", path);
        exit(1);
    }
    fgetc(f); /* skip single whitespace */
    size_t rd = fread(rgb, 1, sizeof(rgb), f);
    (void)rd;
    fclose(f);
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        perror(path);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    fwrite(rgb, 1, sizeof(rgb), f);
    fclose(f);
}

static void uyvy_to_rgb(const unsigned char *buf)
{
    for (int y = 0; y < H; y++)
    {
        const unsigned char *row = buf + y * PITCH;
        for (int x = 0; x < W; x += 2)
        {
            int u  = (signed char) row[x * 2 + 0];
            int y0 = row[x * 2 + 1];
            int v  = (signed char) row[x * 2 + 2];
            int y1 = row[x * 2 + 3];

            for (int k = 0; k < 2; k++)
            {
                int Y = k ? y1 : y0;
                int r = Y + (1402 * v) / 1000;
                int g = Y - (344 * u) / 1000 - (714 * v) / 1000;
                int b = Y + (1772 * u) / 1000;
                set_px(x + k, y, r, g, b);
            }
        }
    }
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

#include <sys/stat.h>

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : "out";
    const char *infile = argc > 2 ? argv[2] : NULL;
    /* optional strength / grain, defaults match the module's menu defaults */
    int strength = argc > 3 ? atoi(argv[3]) : 100;
    int grain    = argc > 4 ? atoi(argv[4]) : 20;
    char path[600];

    mkdir(outdir, 0755);

    if (infile)
        read_ppm(infile);
    else
        build_scene();

    rgb_to_uyvy();

    /* "before" is what the camera buffer holds after RGB -> UYVY, including
     * the 8-bit 4:2:2 round-trip, so an identity pass can be compared to it */
    uyvy_to_rgb(yuv_src);
    snprintf(path, sizeof(path), "%s/before.ppm", outdir);
    write_ppm(path);

    printf("strength %d%%  grain %d\n", strength, grain);
    printf("%-18s %10s %10s\n", "preset", "ms/frame", "checksum");

    for (int preset = 0; preset < FS_PRESET_COUNT; preset++)
    {
        fs_lut_t lut;
        fs_core_build_luts(&lut, &fs_presets[preset], strength, grain);

        unsigned int rnd = 0x9E3779B9;

        /* warm up + timing */
        const int reps = 30;
        double t0 = now_ms();
        for (int i = 0; i < reps; i++)
        {
            rnd = 0x9E3779B9;
            fs_core_process_rows(&lut, yuv_src, yuv_dst, PITCH, 0, H, &rnd);
        }
        double t1 = now_ms();

        unsigned int sum = 0;
        for (size_t i = 0; i < sizeof(yuv_dst); i++) sum = sum * 31u + yuv_dst[i];

        printf("%-18s %10.3f %10u\n", fs_preset_names[preset], (t1 - t0) / reps, sum);

        uyvy_to_rgb(yuv_dst);
        char name[64];
        snprintf(name, sizeof(name), "%s", fs_preset_names[preset]);
        for (char *c = name; *c; c++) if (*c == ' ') *c = '_';

        snprintf(path, sizeof(path), "%s/%02d-%s.ppm", outdir, preset, name);
        write_ppm(path);
    }

    return 0;
}
