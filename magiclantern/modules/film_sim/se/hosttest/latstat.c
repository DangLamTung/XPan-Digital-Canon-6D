/* What range do the lattice's dy/Cb/Cr actually take, per recipe? */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../engine/look.h"
#include "../engine/stock.h"
int main(int argc, char **argv)
{
    char text[131072]; FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    size_t n = fread(text, 1, sizeof text - 1, f); fclose(f); text[n] = 0;
    se_stock st; static se_look lk;
    if (se_stock_parse(text, n, &st) != SE_OK) return 2;
    se_look_from_stock(&st, 1, &lk);
    static se_yuv3 t;
    se_look_bake_yuv3(&lk, &t);
    int mdy = 0, mcb = 0, mcr = 0;
    for (int i = 0; i < (int)(sizeof t.v / sizeof t.v[0]); i++) {
        int dy = (int)(int8_t)(t.v[i] >> 24), cb = (int)(t.v[i] & 0xFF), cr = (int)((t.v[i] >> 16) & 0xFF);
        if (dy < 0) dy = -dy; if (cb < 0) cb = -cb; if (cr < 0) cr = -cr;
        if (dy > mdy) mdy = dy;
        if (cb > mcb) mcb = cb;
        if (cr > mcr) mcr = cr;
    }
    printf("%-24s max|dy| %3d  max|cb| %3d  max|cr| %3d\n", argv[1], mdy, mcb, mcr);
    return 0;
}
