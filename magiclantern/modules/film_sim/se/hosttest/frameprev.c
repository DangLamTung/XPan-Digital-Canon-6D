/*
 * frameprev - render the film frame on a synthetic image, as a PPM.
 *
 * The compositor is shared with the develop path, so what this draws is what
 * the camera would draw. Used to check the design without a camera.
 *
 *   ./frameprev out.ppm [scale] [bg-hex] "TOP LINE" "BOTTOM LINE"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../frame.h"

int main(int argc, char **argv)
{
    int W = 1600, H = 1067;
    int scale = argc > 2 ? atoi(argv[2]) : 2;
    unsigned bg = argc > 3 ? (unsigned)strtoul(argv[3], 0, 16) : 0xFFFFFF;
    const char *model = argc > 4 ? argv[4] : "EOS 6D";
    se_frame f;
    unsigned char *img, *row;
    int y, x, i;
    FILE *o;

    if (argc < 2) {
        fprintf(stderr, "usage: frameprev out.ppm [scale] [bg] [model]\n");
        return 2;
    }

    memset(&f, 0, sizeof f);
    f.on = 1;
    f.scale = scale;
    se_frame_layout(&f);
    f.bg[0] = (uint8_t)((bg >> 16) & 0xFF);
    f.bg[1] = (uint8_t)((bg >> 8) & 0xFF);
    f.bg[2] = (uint8_t)(bg & 0xFF);
    se_frame_contrast(f.bg, f.fg);
    f.dim[0] = (uint8_t)((f.fg[0] + f.bg[0]) / 2);
    f.dim[1] = (uint8_t)((f.fg[1] + f.bg[1]) / 2);
    f.dim[2] = (uint8_t)((f.fg[2] + f.bg[2]) / 2);
    snprintf(f.model, sizeof f.model, "%s", model);
    f.brand_colour[0] = 0xCC; f.brand_colour[1] = 0x00; f.brand_colour[2] = 0x00;
    snprintf(f.label[0], sizeof f.label[0], "f");
    snprintf(f.label[1], sizeof f.label[1], "SS");
    snprintf(f.label[2], sizeof f.label[2], "ISO");
    snprintf(f.value[0], sizeof f.value[0], "2.8");
    snprintf(f.value[1], sizeof f.value[1], "1/200");
    snprintf(f.value[2], sizeof f.value[2], "400");

    img = malloc((size_t)W * (size_t)(H + f.top + f.bottom) * 3);
    row = malloc((size_t)W * 3);
    if (!img || !row) return 1;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            int t = y * 255 / H;
            unsigned char *p = img + ((size_t)(y + f.top) * W + x) * 3;
            p[0] = (uint8_t)(40 + t / 2);
            p[1] = (uint8_t)(70 + t / 2);
            p[2] = (uint8_t)(110 - t / 4);
            if (((x / 90) + (y / 70)) % 7 == 0) { p[0] += 30; p[1] += 25; p[2] += 15; }
        }
    }
    for (i = 0; i < f.top; i++) {
        if (i == f.top - 1) se_frame_edge_row(row, W, &f);
        else se_frame_top_row(row, W, &f, i);
        memcpy(img + (size_t)i * W * 3, row, (size_t)W * 3);
    }
    for (i = 0; i < f.bottom; i++) {
        int base = f.top + H + i;
        if (i == 0) se_frame_edge_row(row, W, &f);
        else se_frame_bottom_row(row, W, &f, i);
        memcpy(img + (size_t)base * W * 3, row, (size_t)W * 3);
    }

    o = fopen(argv[1], "wb");
    if (!o) return 1;
    fprintf(o, "P6\n%d %d\n255\n", W, H + f.top + f.bottom);
    fwrite(img, 1, (size_t)W * (size_t)(H + f.top + f.bottom) * 3, o);
    fclose(o);
    printf("wrote %s  %dx%d  scale %d  bands %d/%d\n", argv[1], W, H + f.top + f.bottom,
           f.scale, f.top, f.bottom);
    return 0;
}
