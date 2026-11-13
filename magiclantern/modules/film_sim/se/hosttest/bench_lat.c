/* Stage cost of the lattice inner loop, on the host. Compile with -DSTAGE=n:
 * 1 pass only, 2 +index, 3 +tetra choice, 4 +interpolation, 5 full. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../fs_core.h"
#ifndef STAGE
#define STAGE 5
#endif
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
int main(int argc, char **argv)
{
    int W = 1056, H = 704, reps = argc > 1 ? atoi(argv[1]) : 20;
    unsigned char *buf = malloc((size_t)W*H*2);
    for (int i = 0; i < W*H*2; i++) buf[i] = (unsigned char)((i*37) & 0xFF);
    int ky=3, ku=FS_UV3_NY*3, kv=FS_UV3_NY*FS_UV3_NC*3;
    static short uv[FS_UV3_NY*FS_UV3_NC*FS_UV3_NC*3];
    for (int i = 0; i < (int)(sizeof uv/sizeof uv[0]); i++) uv[i] = (short)((i*13) & 0xFF);
    double t0 = now();
    unsigned sum = 0;
    for (int r = 0; r < reps; r++) {
        unsigned *sp = (unsigned *)buf;
        for (int i = 0; i < W*H/2; i++) {
            unsigned px = sp[i];
            int u = (int)(signed char)px, v = (int)(signed char)(px>>16);
            int ya = (px>>8)&0xFF, yb = (px>>24)&0xFF;
            int ym = (ya+yb)>>1;
            int ty = ym*FS_UV3_SY, tu=(u+128)*FS_UV3_SC, tv=(v+128)*FS_UV3_SC;
            int iy=ty>>16, iu=tu>>16, iv=tv>>16;
            int fy=(ty>>8)&0xFF, fu=(tu>>8)&0xFF, fv=(tv>>8)&0xFF;
            if (iy>FS_UV3_NY-2){iy=FS_UV3_NY-2;fy=256;}
            if (iu>FS_UV3_NC-2){iu=FS_UV3_NC-2;fu=256;}
            if (iv>FS_UV3_NC-2){iv=FS_UV3_NC-2;fv=256;}
            const short *base = uv + iy*ky + iu*ku + iv*kv;
            int wa=0, wb=0, wc=0;
#if STAGE >= 3
            const short *ca, *cb2;
            if (fy>=fv) { if (fv>=fu){ca=base+ky;cb2=ca+kv;wa=fy-fv;wb=fv-fu;wc=fu;}
                          else if (fy>=fu){ca=base+ky;cb2=ca+ku;wa=fy-fu;wb=fu-fv;wc=fv;}
                          else {ca=base+ku;cb2=ca+ky;wa=fu-fy;wb=fy-fv;wc=fv;} }
            else { if (fu>=fv){ca=base+ku;cb2=ca+kv;wa=fu-fv;wb=fv-fy;wc=fy;}
                   else if (fu>=fy){ca=base+kv;cb2=ca+ku;wa=fv-fu;wb=fu-fy;wc=fy;}
                   else {ca=base+kv;cb2=ca+ky;wa=fv-fy;wb=fy-fu;wc=fu;} }
#endif
#if STAGE >= 4
            for (int c=0;c<3;c++) {
                int v000=base[c], v111=base[c+ky+ku+kv];
                sum += (unsigned)((v000*(256-wa-wb-wc)+ca[c]*wa+cb2[c]*wb+v111*wc)>>8);
            }
#endif
#if STAGE >= 2
            sum += (unsigned)(iy+iu+iv+fy+fu+fv);
#endif
        }
    }
    double t1 = now();
    printf("stage %d: %.2f ms/frame (sum %u)\n", STAGE, (t1-t0)*1000.0/reps, sum);
    return 0;
}
