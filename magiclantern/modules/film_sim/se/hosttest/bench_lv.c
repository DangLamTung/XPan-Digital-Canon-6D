#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../fs_yuvlook.h"
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec + t.tv_nsec/1e9; }
int main(int argc, char **argv)
{
    int W = argc > 1 ? atoi(argv[1]) : 1056;
    int H = argc > 2 ? atoi(argv[2]) : 704;
    int N = argc > 3 ? atoi(argv[3]) : 20;
    if (fs_yuvlook_bake("../../films/velvia.fs", 100) != 0) { printf("bake failed\n"); return 1; }
    unsigned char *buf = malloc((size_t)W * H * 2);
    for (int i = 0; i < W * H * 2; i++) buf[i] = (unsigned char)(40 + (i % 150));
    double t0 = now();
    for (int i = 0; i < N; i++) fs_yuvlook_apply_engine_lv(buf, W, H, W * 2);
    double t1 = now();
    printf("%dx%d engine LV (colour only): %.1f ms/frame (host)\n", W, H, (t1-t0)*1000.0/N);
    t0 = now();
    for (int i = 0; i < N; i++) fs_yuvlook_apply(buf, W, H, W * 2);
    t1 = now();
    printf("%dx%d baked : %.1f ms/frame (host)\n", W, H, (t1-t0)*1000.0/N);
    return 0;
}
