/*
 * div_eq - the cube interpolation used to accumulate in int64_t and divide by
 * 65535; it now accumulates in uint32_t. The coefficients are <= 65535 and the
 * weights sum to exactly 65535, so the sum cannot exceed 65535^2 and fits a
 * uint32_t; this checks the two give the same quotient.
 *
 *   cc -O2 -o div_eq div_eq.c && ./div_eq
 */
/* int64 accumulation vs uint32 accumulation for the cube interpolation:
   do they give the same quotient for every legal weight set? */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
int main(void)
{
    unsigned seed = 12345; int fails = 0;
    for (long t = 0; t < 2000000; t++) {
        int c0 = rand_r(&seed) % 65536, ca = rand_r(&seed) % 65536;
        int cb = rand_r(&seed) % 65536, c1 = rand_r(&seed) % 65536;
        int wa = rand_r(&seed) % 257, wb = rand_r(&seed) % 257, wc = rand_r(&seed) % 257;
        if (wa + wb + wc > 65535) continue;
        int w0 = 65535 - wa - wb - wc;
        int64_t v64 = (int64_t)c0 * w0 + (int64_t)ca * wa + (int64_t)cb * wb + (int64_t)c1 * wc;
        uint32_t v32 = (uint32_t)c0 * (uint32_t)w0 + (uint32_t)ca * (uint32_t)wa
                     + (uint32_t)cb * (uint32_t)wb + (uint32_t)c1 * (uint32_t)wc;
        if (v64 / 65535 != (int64_t)(v32 / 65535u)) {
            if (fails < 3) printf("mismatch c=(%d %d %d %d) w=(%d %d %d)\n", c0, ca, cb, c1, wa, wb, wc);
            fails++;
        }
    }
    printf("%d mismatches\n", fails);
    return fails != 0;
}
