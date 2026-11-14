/*
 * shipgrade - render a frame the way the CAMERA ships it.
 *
 * The develop path runs the engine at 16-bit. The shot path runs the baked
 * YUV table on Canon's 4:2:2 frame. Those are not the same picture, so the
 * calibration was measuring one pipeline while the camera writes another.
 * This tool does the second one on the host: RGB -> UYVY -> the camera's
 * table -> RGB, for any frame and any .fs.
 *
 *   ./shipgrade in.ppm stock.fs out.ppm [strength]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../fs_yuvlook.h"

static unsigned char *read_ppm(const char *p, int *w, int *h)
{
    FILE *f = fopen(p, "rb"); char m[3] = {0}; int mx; unsigned char *b;
    if (!f) return NULL;
    if (fscanf(f, "%2s %d %d %d", m, w, h, &mx) != 4 || strcmp(m, "P6")) { fclose(f); return NULL; }
    fgetc(f);
    b = malloc((size_t)*w * *h * 3);
    if (!b || fread(b, 1, (size_t)*w * *h * 3, f) != (size_t)*w * *h * 3) { free(b); fclose(f); return NULL; }
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    int W, H, x, y;
    unsigned char *rgb, *yuv, *out;
    int strength = argc > 4 ? atoi(argv[4]) : 100;

    if (argc < 4) { fprintf(stderr, "usage: %s in.ppm stock.fs out.ppm [strength]\n", argv[0]); return 2; }
    rgb = read_ppm(argv[1], &W, &H);
    if (!rgb) { fprintf(stderr, "bad input\n"); return 1; }
    int use_table = strcmp(argv[2], "none") != 0;
    if (use_table && fs_yuvlook_bake(argv[2], strength) != 0) { fprintf(stderr, "bake failed\n"); return 1; }

    yuv = malloc((size_t)W * H * 2);
    out = malloc((size_t)W * H * 3);
    for (y = 0; y < H; y++)
        for (x = 0; x + 1 < W; x += 2) {
            const unsigned char *a = rgb + ((size_t)y * W + x) * 3;
            const unsigned char *b = a + 3;
            /* BT.601 full range, same matrix PIL uses: an integer
             * approximation here cost 6 levels of dE on a round trip and
             * made the shipped path look far worse than it is. */
            int y0 = (int)(0.299*a[0] + 0.587*a[1] + 0.114*a[2] + 0.5);
            int y1 = (int)(0.299*b[0] + 0.587*b[1] + 0.114*b[2] + 0.5);
            int r = (a[0] + b[0]) / 2, g = (a[1] + b[1]) / 2, bl = (a[2] + b[2]) / 2;
            int u = (int)(-0.168736*r - 0.331264*g + 0.5*bl + 128.5) - 128;
            int v = (int)(0.5*r - 0.418688*g - 0.081312*bl + 128.5) - 128;
            unsigned char *p = yuv + ((size_t)y * W + x) * 2;
            p[0] = (unsigned char)(signed char)(u < -128 ? -128 : u > 127 ? 127 : u);
            p[1] = (unsigned char)(y0 < 0 ? 0 : y0 > 255 ? 255 : y0);
            p[2] = (unsigned char)(signed char)(v < -128 ? -128 : v > 127 ? 127 : v);
            p[3] = (unsigned char)(y1 < 0 ? 0 : y1 > 255 ? 255 : y1);
        }

    if (use_table) fs_yuvlook_apply(yuv, W, H, W * 2);

    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            /* UYVY pairs: pixel x belongs to the pair starting at x&~1 */
            const unsigned char *p = yuv + ((size_t)y * W + ((size_t)x & ~(size_t)1)) * 2;
            int Y = (x & 1) ? p[3] : p[1];
            int U = (int)(signed char)p[0], V = (int)(signed char)p[2];
            int r = (int)(Y + 1.402*V + 0.5);
            int g = (int)(Y - 0.344136*U - 0.714136*V + 0.5);
            int b = (int)(Y + 1.772*U + 0.5);
            unsigned char *o = out + ((size_t)y * W + x) * 3;
            o[0] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r);
            o[1] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g);
            o[2] = (unsigned char)(b < 0 ? 0 : b > 255 ? 255 : b);
        }

    { FILE *f = fopen(argv[3], "wb");
      fprintf(f, "P6\n%d %d\n255\n", W, H);
      fwrite(out, 1, (size_t)W * H * 3, f);
      fclose(f); }
    printf("%s: %dx%d shipped render\n", argv[3], W, H);
    return 0;
}
