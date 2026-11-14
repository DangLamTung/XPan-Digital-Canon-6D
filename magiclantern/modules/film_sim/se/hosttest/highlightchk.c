/* Does a near-clipped neutral highlight go pink, and does the desaturation
 * keep it neutral? Raw neutral x, WB gains R 2.0 / G 1.0 / B 1.6, identity
 * camera matrix -- the tricky part of the pipeline, isolated. */
#include <stdio.h>
#include <string.h>
#include "../engine/color.h"

static int run(int mode, int x, int *o)
{
    se_color c;
    uint16_t rgb[3];
    memset(&c, 0, sizeof c);
    c.highlight_mode = mode;
    c.cam_to_srgb_q[0] = 1 << SE_Q; c.cam_to_srgb_q[4] = 1 << SE_Q;
    c.cam_to_srgb_q[8] = 1 << SE_Q;
    rgb[0] = (uint16_t)(x * 2 > 65535 ? 65535 : x * 2);
    rgb[1] = (uint16_t)x;
    rgb[2] = (uint16_t)(x * 16 / 10 > 65535 ? 65535 : x * 16 / 10);
    se_color_matrix_row(&c, rgb, 1);
    o[0] = rgb[0]; o[1] = rgb[1]; o[2] = rgb[2];
    return 0;
}

int main(void)
{
    int xs[] = { 20000, 26000, 30000, 34000, 40000, 50000, 65535 };
    printf("%-8s %-22s %-22s\n", "raw", "clip", "desaturate");
    for (int i = 0; i < 7; i++) {
        int a[3], b[3];
        run(0, xs[i], a);
        run(1, xs[i], b);
        printf("%-8d %4d,%4d,%4d  spread %5d   %4d,%4d,%4d  spread %5d\n",
               xs[i], a[0], a[1], a[2],
               (a[0] > a[1] ? a[0] : a[1]) - (a[2] < a[1] ? a[2] : a[1]),
               b[0], b[1], b[2],
               (b[0] > b[1] ? b[0] : b[1]) - (b[2] < b[1] ? b[2] : b[1]));
    }
    return 0;
}
