/**
 * Host renderer for the film-simulation core, on a real photograph.
 *
 * The shipped host tools either synthesise a scene (filmsim_sim) or run the whole
 * raw-develop path including a synthetic Bayer round trip (devsim). This one
 * isolates the *look*: RGB -> UYVY, through fs_core.h exactly as the camera runs
 * it, then back to RGB. That is the form that can be compared against another
 * grading engine on the same pixels.
 *
 * Build:  python -m ziglang cc -O2 -std=c99 -o look_host.exe look_host.c -lm
 * Run:    ./look_host in.ppm out.ppm <preset 0..9> [strength] [grain]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

#include "../../fs_core.h"

static int read_ppm(const char *path, int *w, int *h, unsigned char **rgb)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[3] = {0};
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) { fclose(f); return 0; }
    int width, height, maxval;
    if (fscanf(f, "%d %d %d", &width, &height, &maxval) != 3 || maxval != 255) { fclose(f); return 0; }
    fgetc(f);
    unsigned char *buf = (unsigned char *)malloc((size_t)width * height * 3);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)width * height * 3, f) != (size_t)width * height * 3) {
        free(buf); fclose(f); return 0;
    }
    fclose(f);
    *w = width; *h = height; *rgb = buf;
    return 1;
}

static int write_ppm(const char *path, int w, int h, const unsigned char *rgb)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, (size_t)w * h * 3, f);
    fclose(f);
    return 1;
}

static unsigned char clamp_u8(int v) { return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s in.ppm out.ppm <preset 0..%d> [strength=100] [grain=20]\n",
                argv[0], FS_PRESET_COUNT - 1);
        return 2;
    }
    const char *in_path = argv[1];
    const char *out_path = argv[2];
    int preset = atoi(argv[3]);
    int strength = argc > 4 ? atoi(argv[4]) : 100;
    int grain = argc > 5 ? atoi(argv[5]) : 20;
    if (preset < 0 || preset >= FS_PRESET_COUNT) {
        fprintf(stderr, "preset out of range\n");
        return 2;
    }

    int w = 0, h = 0;
    unsigned char *rgb = NULL;
    if (!read_ppm(in_path, &w, &h, &rgb)) {
        fprintf(stderr, "cannot read %s\n", in_path);
        return 1;
    }

    int pitch = w * 2;                       /* UYVY: two pixels per four bytes */
    unsigned char *yuv = (unsigned char *)malloc((size_t)pitch * h);
    if (!yuv) return 1;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x += 2) {
            int x2 = (x + 1 < w) ? x + 1 : x;
            const unsigned char *p0 = &rgb[((size_t)y * w + x) * 3];
            const unsigned char *p1 = &rgb[((size_t)y * w + x2) * 3];
            double y0 = 0.299 * p0[0] + 0.587 * p0[1] + 0.114 * p0[2];
            double y1 = 0.299 * p1[0] + 0.587 * p1[1] + 0.114 * p1[2];
            double u0 = -0.169 * p0[0] - 0.331 * p0[1] + 0.500 * p0[2];
            double v0 = 0.500 * p0[0] - 0.419 * p0[1] - 0.081 * p0[2];
            unsigned char *q = &yuv[(size_t)y * pitch + x * 2];
            q[0] = (unsigned char)((signed char)lrint(u0));   /* signed chroma, neutral 0 */
            q[1] = clamp_u8((int)lrint(y0));
            q[2] = (unsigned char)((signed char)lrint(v0));
            q[3] = clamp_u8((int)lrint(y1));
        }
    }

    unsigned char *dst = (unsigned char *)malloc((size_t)pitch * h);
    if (!dst) return 1;

    fs_lut_t lut;
    fs_core_build_luts(&lut, &fs_presets[preset], strength, grain);
    unsigned int rnd = 0x12345678u;
    fs_core_process_rows(&lut, yuv, dst, pitch, 0, h, &rnd);

    unsigned char *out = (unsigned char *)malloc((size_t)w * h * 3);
    if (!out) return 1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const unsigned char *q = &dst[(size_t)y * pitch + (x & ~1) * 2];
            int u = (signed char)q[0];
            int v = (signed char)q[2];
            int yy = q[(x & 1) ? 3 : 1];
            double r = yy + 1.402 * v;
            double g = yy - 0.344136 * u - 0.714136 * v;
            double b = yy + 1.772 * u;
            unsigned char *o = &out[((size_t)y * w + x) * 3];
            o[0] = clamp_u8((int)lrint(r));
            o[1] = clamp_u8((int)lrint(g));
            o[2] = clamp_u8((int)lrint(b));
        }
    }

    if (!write_ppm(out_path, w, h, out)) {
        fprintf(stderr, "cannot write %s\n", out_path);
        return 1;
    }
    printf("%s: %dx%d preset %d strength %d grain %d\n", out_path, w, h, preset, strength, grain);
    free(rgb); free(yuv); free(dst); free(out);
    return 0;
}
