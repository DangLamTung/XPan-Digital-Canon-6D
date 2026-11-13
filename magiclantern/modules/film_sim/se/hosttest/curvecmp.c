#include <stdio.h>
#include <string.h>
#include "../engine/look.h"
#include "../engine/stock.h"
int main(int argc, char **argv) {
    char text[16384]; FILE *f = fopen(argv[1], "rb");
    size_t n = fread(text, 1, sizeof text - 1, f); fclose(f); text[n] = 0;
    se_stock st; static se_look lk; static se_yuvtbl t;
    if (se_stock_parse(text, n, &st) != SE_OK) return 1;
    se_look_from_stock(&st, 1, &lk); se_look_bake_yuv(&lk, &t);
    printf("%-10s", argv[1]);
    int pts[] = {0, 8, 16, 24, 32, 48, 64, 96, 128, 160, 192, 224, 235, 240, 248, 255};
    for (int i = 0; i < 16; i++) printf(" %d->%d", pts[i], t.y[pts[i]]);
    printf("\n");
    return 0;
}
