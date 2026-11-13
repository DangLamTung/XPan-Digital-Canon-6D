/* Time the exact (engine) path on a full 6D frame, on the host. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../fs_yuvlook.h"

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec + t.tv_nsec/1e9; }

int main(int argc, char **argv)
{
    const char *stock = argc > 1 ? argv[1] : "../../films/velvia.fs";
    int W = 5472, H = 3648;
    if (fs_yuvlook_bake(stock, 100) != 0) { printf("bake failed\n"); return 1; }
    printf("engine ready: %d\n", fs_yuvlook_engine_ready());
    unsigned char *buf = malloc((size_t)W * H * 2);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x += 2) {
            unsigned char *p = buf + ((size_t)y * W + x) * 2;
            int v = 40 + (y * 150 / H);
            p[0] = 0; p[1] = (unsigned char)v; p[2] = 0; p[3] = (unsigned char)(v + 4);
        }
    double t0 = now();
    fs_yuvlook_apply_engine(buf, W, H, W * 2);
    double t1 = now();
    double t2 = now();
    fs_yuvlook_apply(buf, W, H, W * 2);
    double t3 = now();
    printf("engine %.2f s   baked %.2f s   ratio %.1fx\n", t1-t0, t3-t2, (t1-t0)/(t3-t2));
    return 0;
}
