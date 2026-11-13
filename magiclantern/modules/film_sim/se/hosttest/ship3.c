/* ship3 - apply the joint (y,u,v) lattice to a frame, like the camera could. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../fs_yuvlook.h"
#include "../../fs_core.h"

static unsigned char *read_ppm(const char *p, int *w, int *h)
{
    FILE *f = fopen(p, "rb"); char m[3] = {0}; int mx; unsigned char *b;
    if (!f) return NULL;
    if (fscanf(f, "%2s %d %d %d", m, w, h, &mx) != 4 || strcmp(m, "P6")) { fclose(f); return NULL; }
    fgetc(f);
    b = malloc((size_t)*w * *h * 3);
    if (!b || fread(b, 1, (size_t)*w * *h * 3, f) != (size_t)*w * *h * 3) { free(b); fclose(f); return NULL; }
    fclose(f); return b;
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec + t.tv_nsec/1e9; }

int main(int argc, char **argv)
{
    int W, H; unsigned char *rgb, *yuv, *out;
    if (argc < 4) { fprintf(stderr, "usage: %s in.ppm stock.fs out.ppm\n", argv[0]); return 2; }
    rgb = read_ppm(argv[1], &W, &H); if (!rgb) return 1;
    if (fs_yuvlook_bake(argv[2], 100) != 0) { fprintf(stderr, "bake failed\n"); return 1; }
    const se_yuv3 *t3 = fs_yuvlook_uv3();
    if (!t3) { fprintf(stderr, "no lattice\n"); return 1; }
    yuv = malloc((size_t)W*H*2); out = malloc((size_t)W*H*3);
    for (int y=0;y<H;y++) for (int x=0;x+1<W;x+=2) {
        const unsigned char *a = rgb + ((size_t)y*W+x)*3, *b = a+3;
        int y0=(int)(0.299*a[0]+0.587*a[1]+0.114*a[2]+0.5), y1=(int)(0.299*b[0]+0.587*b[1]+0.114*b[2]+0.5);
        int r=(a[0]+b[0])/2, g=(a[1]+b[1])/2, bl=(a[2]+b[2])/2;
        int u=(int)(-0.168736*r-0.331264*g+0.5*bl+128.5)-128, v=(int)(0.5*r-0.418688*g-0.081312*bl+128.5)-128;
        unsigned char *p = yuv + ((size_t)y*W+x)*2;
        p[0]=(unsigned char)(signed char)u; p[1]=(unsigned char)y0; p[2]=(unsigned char)(signed char)v; p[3]=(unsigned char)y1;
    }
    unsigned rnd = 1; double t0 = now();
    fs_core_process_rows3(t3->y, t3->v, yuv, yuv, W*2, 0, H, 0, &rnd);
    double t1 = now();
    for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
        const unsigned char *p = yuv + ((size_t)y*W + ((size_t)x & ~(size_t)1))*2;
        int Y = (x&1)?p[3]:p[1], U=(int)(signed char)p[0], V=(int)(signed char)p[2];
        int r=(int)(Y+1.402*V+0.5), g=(int)(Y-0.344136*U-0.714136*V+0.5), b=(int)(Y+1.772*U+0.5);
        unsigned char *o = out + ((size_t)y*W+x)*3;
        o[0]=(unsigned char)(r<0?0:r>255?255:r); o[1]=(unsigned char)(g<0?0:g>255?255:g); o[2]=(unsigned char)(b<0?0:b>255?255:b);
    }
    { FILE *f=fopen(argv[3],"wb"); fprintf(f,"P6\n%d %d\n255\n",W,H); fwrite(out,1,(size_t)W*H*3,f); fclose(f); }
    printf("%s %dx%d lattice %.2f ms\n", argv[3], W, H, (t1-t0)*1000.0);
    return 0;
}
