/*
 * cr2_key_check - the CR2 row/slice mapping, walked with counters, must give
 * the same (slice, row, col) as the division form it replaced. Exhaustive over
 * a grid of widths, slice widths/lasts/counts and the first 20 JPEG rows.
 *
 *   cc -O2 -o cr2_key_check cr2_key_check.c && ./cr2_key_check
 */
/* Does the incremental mapping give the same (i,row,col) as the divisions? */
#include <stdio.h>
#include <stdlib.h>

static void old_map(long jrow, int jwide, int jcol, int high, int slice_width,
                    int slice_last, int slice_count, int *oi, int *orow, int *ocol)
{
    long jidx = (long)jrow * jwide + jcol;
    int i = (int)(jidx / ((long)slice_width * high));
    int j = (i >= slice_count);
    if (j) i = slice_count;
    jidx -= (long)i * slice_width * high;
    int w = j ? slice_last : slice_width;
    *oi = i;
    *orow = (int)(jidx / w);
    *ocol = (int)(jidx % w) + i * slice_width;
}

int main(void)
{
    int fails = 0, cases = 0;
    int widths[] = {1668, 1704, 1748};
    int highv[]  = {8, 16, 24};
    int sw[] = {1668, 1672, 1696};
    int sl[] = {900, 1200, 1668};
    int sc[] = {1, 2, 3};
    for (int a = 0; a < 3; a++)
    for (int b = 0; b < 3; b++)
    for (int c = 0; c < 3; c++)
    for (int d = 0; d < 3; d++)
    for (int e = 0; e < 3; e++) {
        int jwide = widths[a], high = highv[b], slice_width = sw[c],
            slice_last = sl[d], slice_count = sc[e];
        for (long jrow = 0; jrow < 20; jrow++) {
            /* incremental state, seeded once per row */
            long jidx0 = jrow * jwide;
            int i = (int)(jidx0 / ((long)slice_width * high));
            int j = (i >= slice_count);
            if (j) i = slice_count;
            long step = (long)slice_width * high;
            long k = jidx0 - (long)i * step;      /* offset inside the slice */
            int w = j ? slice_last : slice_width;
            int row = (int)(k / w), coff = (int)(k % w);
            for (int jcol = 0; jcol < jwide; jcol++) {
                int oi, orow, ocol;
                old_map(jrow, jwide, jcol, high, slice_width, slice_last, slice_count, &oi, &orow, &ocol);
                int ni = i, nrow = row, ncol = coff + i * slice_width;
                cases++;
                if (oi != ni || orow != nrow || ocol != ncol) {
                    if (fails < 5)
                        printf("MISMATCH jrow %ld col %d: old (%d,%d,%d) new (%d,%d,%d)\n",
                               jrow, jcol, oi, orow, ocol, ni, nrow, ncol);
                    fails++;
                }
                k++;
                if (k == step && i < slice_count) {   /* next slice */
                    i++; k = 0; row = 0; coff = 0;
                    w = (i >= slice_count) ? slice_last : slice_width;
                } else if (++coff == w) {
                    coff = 0; row++;
                }
            }
        }
    }
    printf("%d cases, %d mismatches\n", cases, fails);
    return fails != 0;
}
