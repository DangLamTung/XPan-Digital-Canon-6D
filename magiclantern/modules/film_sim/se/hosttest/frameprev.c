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
    int scale = argc > 2 ? atoi(argv[2]) : 3;
    unsigned bg = argc > 3 ? (unsigned)strtoul(argv[3], 0, 16) : 0x1a1a1a;
    const char *top = argc > 4 ? argv[4] : "CINESTILL 50D  12A";
    const char *bot = argc > 5 ? argv[5] : "EOS 6D  F2.8  1/500  ISO 100";
    se_frame f;
    unsigned char *img;
    int band_top, band_bot, y, x, i;
    FILE *o;

    if (argc < 2) { fprintf(stderr, "usage: frameprev out.ppm [scale] [bg] \"top\" \"bottom\"\n"); return 2; }

    memset(&f, 0, sizeof f);
    f.on = 1;
    f.scale = scale < 1 ? 1 : (scale > 4 ? 4 : scale);
    f.top = 7 * f.scale + 8 * f.scale;      /* glyph box + padding */
    f.bottom = f.top;
    f.bg[0] = (uint8_t)((bg >> 16) & 0xFF);
    f.bg[1] = (uint8_t)((bg >> 8) & 0xFF);
    f.bg[2] = (uint8_t)(bg & 0xFF);
    se_frame_contrast(f.bg, f.fg);
    snprintf(f.line1, sizeof f.line1, "%s", top);
    snprintf(f.line2, sizeof f.line2, "%s", bot);

    band_top = f.top;
    band_bot = f.bottom;
    (void)band_bot;

    img = malloc((size_t)W * (size_t)(H + band_top + band_bot) * 3);
    if (!img) return 1;

    /* a picture: sky-to-foreground gradient with a few blocks, so the bands
     * are seen against something with tone in it */
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            int t = y * 255 / H;
            unsigned char *p = img + ((size_t)(y + band_top) * W + x) * 3;
            p[0] = (uint8_t)(40 + t / 2);
            p[1] = (uint8_t)(70 + t / 2);
            p[2] = (uint8_t)(110 - t / 4);
            if (((x / 90) + (y / 70)) % 7 == 0) { p[0] += 30; p[1] += 25; p[2] += 15; }
        }
    }

    {
        unsigned char *row = malloc((size_t)W * 3);
        if (!row) return 1;
        for (i = 0; i < band_top; i++) {
            if (i == band_top - 1) se_frame_edge_row(row, W, &f);
            else {
                se_frame_fill_row(row, W, &f);
                se_frame_text_row(row, W, &f, i, se_frame_text_top(band_top, f.scale), f.line1);
            }
            memcpy(img + (size_t)i * W * 3, row, (size_t)W * 3);
        }
        for (i = 0; i < band_bot; i++) {
            if (i == 0) se_frame_edge_row(row, W, &f);
            else {
                se_frame_fill_row(row, W, &f);
                se_frame_text_row(row, W, &f, i, se_frame_text_top(band_bot, f.scale), f.line2);
            }
            memcpy(img + (size_t)(band_top + H + i) * W * 3, row, (size_t)W * 3);
        }
        free(row);
    }

    o = fopen(argv[1], "wb");
    if (!o) return 1;
    fprintf(o, "P6\n%d %d\n255\n", W, H + band_top + band_bot);
    fwrite(img, 1, (size_t)W * (size_t)(H + band_top + band_bot) * 3, o);
    fclose(o);
    free(img);
    printf("wrote %s  %dx%d  scale %d  band %d\n", argv[1], W, H + band_top + band_bot, f.scale, band_top);
    return 0;
}
