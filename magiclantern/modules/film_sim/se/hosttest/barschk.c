/* The one-memset bars must paint exactly what the per-row loop painted. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "fs_core.h"

static void ref(unsigned char *d, int pitch, int w, int row0, int row1, int num, int den)
{
    int active = row1 - row0;
    int box_h = (w * num) / den;
    int top = row0 + (active - box_h) / 2;
    int bot = top + box_h;
    for (int y = row0; y < top; y++) memset(d + (size_t)y * pitch, 0, (size_t)pitch);
    for (int y = bot; y < row1; y++) memset(d + (size_t)y * pitch, 0, (size_t)pitch);
}

int main(void)
{
    int fails = 0, cases = 0;
    int W[] = {720, 1056, 5472};
    int H[] = {480, 704, 3648};
    int num[] = {24, 9, 16};
    int den[] = {65, 16, 9};
    for (int a = 0; a < 3; a++)
    for (int b = 0; b < 3; b++)
    for (int c = 0; c < 3; c++) {
        int w = W[a], h = H[b], pitch = w * 2;
        unsigned char *x = malloc((size_t)pitch * h);
        unsigned char *y = malloc((size_t)pitch * h);
        if (!x || !y) return 2;
        memset(x, 0xAA, (size_t)pitch * h);
        memset(y, 0xAA, (size_t)pitch * h);
        if ((w * num[c]) / den[c] >= h) { free(x); free(y); continue; }  /* letterbox path */
        fs_core_aspect_bars(x, pitch, w, 0, h, num[c], den[c]);
        ref(y, pitch, w, 0, h, num[c], den[c]);
        if (memcmp(x, y, (size_t)pitch * h)) { fails++; printf("MISMATCH %dx%d %d:%d\n", w, h, num[c], den[c]); }
        cases++;
        free(x); free(y);
    }
    printf("%d cases, %d mismatches\n", cases, fails);
    return fails != 0;
}
