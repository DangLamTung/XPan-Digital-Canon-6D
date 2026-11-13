/* Print the baked capture curve for a stock: where it clips and where it flattens. */
#include <stdio.h>
#include <string.h>
#include "../engine/look.h"
#include "../engine/stock.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "../../films/velvia.fs";
    char text[16384];
    FILE *f = fopen(path, "rb");
    size_t n;
    se_stock st;
    se_look lk;
    static se_yuvtbl t;

    if (!f) { perror(path); return 1; }
    n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    if (se_stock_parse(text, n, &st) != SE_OK) { printf("parse failed\n"); return 1; }
    se_look_from_stock(&st, 1, &lk);
    se_look_bake_yuv(&lk, &t);

    int clip_hi = 0, clip_lo = 0, flat = 0;
    for (int i = 0; i < 256; i++) {
        if (t.y[i] == 255) clip_hi++;
        if (t.y[i] == 0) clip_lo++;
        if (i < 255 && t.y[i] == t.y[i + 1]) flat++;
    }
    printf("%s: curve clips %d levels to 255, %d to 0, %d flat steps\n",
           path, clip_hi, clip_lo, flat);
    printf("in->out: ");
    for (int i = 0; i <= 255; i += 16) printf("%d->%d ", i, t.y[i]);
    printf("\n");
    printf("uoff/voff at 32/64/128/192: %d/%d %d/%d %d/%d %d/%d\n",
           t.uoff[32], t.voff[32], t.uoff[64], t.voff[64],
           t.uoff[128], t.voff[128], t.uoff[192], t.voff[192]);
    return 0;
}
