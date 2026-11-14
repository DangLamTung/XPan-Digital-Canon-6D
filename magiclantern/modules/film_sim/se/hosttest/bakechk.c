/* Checksum of the raw bake, to prove a rewrite of se_stock_bake is bit-exact. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../engine/stock.h"
int main(int argc, char **argv)
{
    char text[65536]; FILE *f = fopen(argv[1], "rb");
    size_t n = fread(text, 1, sizeof text - 1, f); fclose(f); text[n] = 0;
    se_stock st; se_lut1 out[3];
    if (se_stock_parse(text, n, &st) != SE_OK) return 1;
    se_stock_bake(&st, out);
    unsigned long h = 5381;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < SE_LUT1_SIZE; i++)
            h = ((h << 5) + h) ^ out[c].v[i];
    printf("%016lx\n", h);
    return 0;
}
