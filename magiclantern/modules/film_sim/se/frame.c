/*
 * Film-frame compositor -- see frame.h.
 *
 * A 5x7 font, column-major, bit 0 = top row: the same shape as the character
 * generators on the camera's own OSD, which is why the text reads as printed
 * rather than as a scaled bitmap. Only the characters a rebate uses are here --
 * that keeps the table small enough to live in the module's own section.
 */
#include "frame.h"
#include <string.h>

typedef struct { char c; unsigned char col[5]; } glyph;

static const glyph g_font[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
    {'+', {0x08, 0x08, 0x3E, 0x08, 0x08}},
    {'%', {0x62, 0x64, 0x08, 0x13, 0x23}},
};
#define FONT_N ((int)(sizeof g_font / sizeof g_font[0]))

static const glyph *find(char c) {
    int i;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (i = 0; i < FONT_N; i++)
        if (g_font[i].c == c) return &g_font[i];
    return 0;
}


static int ch_width(int scale) { return 6 * scale; }

int se_frame_text_width(const char *text, int scale) {
    int n = 0;
    if (!text || scale < 1) return 0;
    while (text[n]) n++;
    return n * ch_width(scale);
}

/* Draw one row of `text` with its glyph box starting at `ytop`. */
static void glyph_row(uint8_t *row, int width, int y, int ytop, int scale,
                      const char *text, int x0, const uint8_t fg[3]) {
    int gy = (y - ytop) / scale, x, i, gx, sx;
    if (!text || y < ytop || gy < 0 || gy > 6) return;
    x = x0;
    for (i = 0; text[i]; i++) {
        const glyph *gl = find(text[i]);
        if (!gl) { x += ch_width(scale); continue; }
        for (gx = 0; gx < 5; gx++) {
            if (!((gl->col[gx] >> gy) & 1)) continue;
            for (sx = 0; sx < scale; sx++) {
                int px = x + gx * scale + sx;
                if (px < 0 || px >= width) continue;
                row[px * 3 + 0] = fg[0];
                row[px * 3 + 1] = fg[1];
                row[px * 3 + 2] = fg[2];
            }
        }
        x += ch_width(scale);
    }
}

void se_frame_contrast(const uint8_t bg[3], uint8_t fg[3]) {
    int y = (299 * bg[0] + 587 * bg[1] + 114 * bg[2]) / 1000;
    uint8_t v = y > 140 ? 0 : 255;
    fg[0] = fg[1] = fg[2] = v;
}

void se_frame_fill_row(uint8_t *row, int width, const se_frame *f) {
    int x;
    for (x = 0; x < width; x++) {
        row[x * 3 + 0] = f->bg[0];
        row[x * 3 + 1] = f->bg[1];
        row[x * 3 + 2] = f->bg[2];
    }
}

void se_frame_edge_row(uint8_t *row, int width, const se_frame *f) {
    int x;
    for (x = 0; x < width; x++) {
        row[x * 3 + 0] = (uint8_t)((f->bg[0] * 4 + f->fg[0]) / 5);
        row[x * 3 + 1] = (uint8_t)((f->bg[1] * 4 + f->fg[1]) / 5);
        row[x * 3 + 2] = (uint8_t)((f->bg[2] * 4 + f->fg[2]) / 5);
    }
}

void se_frame_layout(se_frame *f) {
    int s = f->scale < 1 ? 1 : (f->scale > 4 ? 4 : f->scale);
    int pad = 5 * s;
    int label_s = s > 1 ? s / 2 : 1;
    int value_s = s > 1 ? s - 1 : 1;
    int model_s = s > 1 ? s - 1 : 1;
    int brand_top = pad;
    int model_top = brand_top + 7 * s + 3 * s;
    int values_top = pad + 7 * label_s + 2 * s;
    int a = model_top + 7 * model_s;
    int b = values_top + 7 * value_s;

    f->scale = s;
    f->top = pad;
    f->bottom = (a > b ? a : b) + pad;
}

void se_frame_top_row(uint8_t *row, int width, const se_frame *f, int y) {
    (void)y;
    se_frame_fill_row(row, width, f);
}

void se_frame_bottom_row(uint8_t *row, int width, const se_frame *f, int y) {
    int s = f->scale < 1 ? 1 : f->scale;
    int pad = 5 * s;
    int label_s = s > 1 ? s / 2 : 1;
    int value_s = s > 1 ? s - 1 : 1;
    int model_s = s > 1 ? s - 1 : 1;
    int brand_top = pad;
    int model_top = brand_top + 7 * s + 3 * s;
    int labels_top = pad;
    int values_top = labels_top + 7 * label_s + 2 * s;
    int cw[3], total = 0, gap = 5 * s, x, i;

    se_frame_fill_row(row, width, f);

    glyph_row(row, width, y, brand_top, s, f->brand, pad, f->fg);
    glyph_row(row, width, y, model_top, model_s, f->model, pad, f->dim);

    /* three columns, right aligned, labels centred over their values */
    for (i = 0; i < 3; i++) {
        int wl = se_frame_text_width(f->label[i], label_s);
        int wv = se_frame_text_width(f->value[i], value_s);
        cw[i] = wl > wv ? wl : wv;
        total += cw[i];
    }
    total += 2 * gap;
    x = width - pad - total;
    if (x < pad) x = pad;

    for (i = 0; i < 3; i++) {
        int wl = se_frame_text_width(f->label[i], label_s);
        int wv = se_frame_text_width(f->value[i], value_s);
        glyph_row(row, width, y, labels_top, label_s, f->label[i],
                  x + (cw[i] - wl) / 2, f->dim);
        glyph_row(row, width, y, values_top, value_s, f->value[i],
                  x + (cw[i] - wv) / 2, f->fg);
        x += cw[i] + gap;
    }
}
