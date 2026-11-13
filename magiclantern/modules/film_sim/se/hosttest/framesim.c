/*
 * framesim - compose the film frame on a real photograph, on the host.
 *
 *   ./framesim out.ppm photo.ppm sprite.rgb sw sh "EOS 6D" "CineStill 50D" 2.8 1/200 400
 *
 * Draws through se/frame.c, the same code the develop path calls, so the
 * geometry and the type are the camera's -- only the JPEG encoder differs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../frame.h"

static unsigned char *read_ppm(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    char magic[3] = {0};
    int maxv;
    unsigned char *p;
    if (!f) return NULL;
    if (fscanf(f, "%2s %d %d %d", magic, w, h, &maxv) != 4 || strcmp(magic, "P6")) {
        fclose(f); return NULL;
    }
    fgetc(f);
    p = malloc((size_t)*w * *h * 3);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)*w * *h * 3, f) != (size_t)*w * *h * 3) {
        free(p); fclose(f); return NULL;
    }
    fclose(f);
    return p;
}

int main(int argc, char **argv)
{
    int W, H, sw = 0, sh = 0;
    unsigned char *photo, *sprite = NULL, *canvas, *row;
    se_frame f;
    FILE *fp;
    int y, i;

    if (argc < 11) {
        fprintf(stderr, "usage: %s out.ppm photo.ppm sprite.rgb sw sh model stock ap ss iso\n", argv[0]);
        return 2;
    }
    photo = read_ppm(argv[2], &W, &H);
    if (!photo) { fprintf(stderr, "bad photo %s\n", argv[2]); return 1; }

    sw = atoi(argv[4]); sh = atoi(argv[5]);
    if (sw > 0 && sh > 0) {
        fp = fopen(argv[3], "rb");
        if (fp) {
            sprite = malloc((size_t)sw * sh * 3);
            if (sprite && fread(sprite, 1, (size_t)sw * sh * 3, fp) != (size_t)sw * sh * 3) {
                free(sprite); sprite = NULL;
            }
            fclose(fp);
        }
    }

    memset(&f, 0, sizeof f);
    f.on = 1;
    f.scale = 1;
    f.brand_i = -1;
    f.bg[0] = f.bg[1] = f.bg[2] = 255;
    se_frame_contrast(f.bg, f.fg);
    f.dim[0] = f.dim[1] = f.dim[2] = 120;
    f.brand_colour[0] = 0xCC;
    f.brand_colour[1] = 0x00;
    f.brand_colour[2] = 0x00;
    snprintf(f.model, sizeof f.model, "%s", argv[6]);
    snprintf(f.stock, sizeof f.stock, "%s", argv[7]);
    if (sprite) {
        f.sprite = sprite;
        f.sprite_w = sw;
        f.sprite_h = sh;
        f.sprite_scale = 2;
    }
    snprintf(f.label[0], sizeof f.label[0], "f");
    snprintf(f.label[1], sizeof f.label[1], "SS");
    snprintf(f.label[2], sizeof f.label[2], "ISO");
    snprintf(f.value[0], sizeof f.value[0], "%s", argv[8]);
    snprintf(f.value[1], sizeof f.value[1], "%s", argv[9]);
    snprintf(f.value[2], sizeof f.value[2], "%s", argv[10]);
    if (argc > 15) f.inner_h = atoi(argv[15]);
    if (f.inner_h <= 0) {
        f.inner_h = W / 14;
        if (f.inner_h < 120) f.inner_h = 120;
        if (f.inner_h > 400) f.inner_h = 400;
    }
    if (argc > 16) snprintf(f.lens, sizeof f.lens, "%s", argv[16]);
    if (argc > 11) {
        f.brand_i = atoi(argv[11]);
        f.brand_fg[0] = argc > 14 ? atoi(argv[12]) : 0xED;
        f.brand_fg[1] = argc > 14 ? atoi(argv[13]) : 0x00;
        f.brand_fg[2] = argc > 14 ? atoi(argv[14]) : 0x00;
        if (f.brand_i >= 0) f.sprite = NULL;
    }
    se_frame_layout(&f);

    canvas = malloc((size_t)W * (size_t)(H + f.top + f.bottom) * 3);
    row = malloc((size_t)W * 3);
    if (!canvas || !row) return 1;

    for (i = 0; i < f.top; i++) {
        if (i == f.top - 1) se_frame_edge_row(row, W, &f);
        else se_frame_top_row(row, W, &f, i);
        memcpy(canvas + (size_t)i * W * 3, row, (size_t)W * 3);
    }
    memcpy(canvas + (size_t)f.top * W * 3, photo, (size_t)W * H * 3);
    for (i = 0; i < f.bottom; i++) {
        if (i == 0) se_frame_edge_row(row, W, &f);
        else se_frame_bottom_row(row, W, &f, i);
        memcpy(canvas + (size_t)(f.top + H + i) * W * 3, row, (size_t)W * 3);
    }

    fp = fopen(argv[1], "wb");
    if (!fp) return 1;
    fprintf(fp, "P6\n%d %d\n255\n", W, H + f.top + f.bottom);
    fwrite(canvas, 1, (size_t)W * (size_t)(H + f.top + f.bottom) * 3, fp);
    fclose(fp);
    printf("wrote %s %dx%d (bands %d/%d)\n", argv[1], W, H + f.top + f.bottom, f.top, f.bottom);
    (void)y;
    return 0;
}
