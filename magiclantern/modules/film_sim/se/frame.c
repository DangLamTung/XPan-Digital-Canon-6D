/*
 * Film-frame compositor: the print border around a developed photo.
 *
 * Layout, following a bordered print:
 *
 *   +--------------------------------------+
 *   |            the photograph            |
 *   +--------------------------------------+   keyline
 *   | Canon            f      SS     ISO   |   bottom band
 *   | EOS 6D          2.8   1/200    400   |
 *   +--------------------------------------+
 *
 * The wordmark and the type are rasterised bitmaps rather than a pixel font:
 * a 5x7 glyph grid reads as a camera overlay, not as print. Both tables are
 * generated once and baked in -- see docs/film-frame.md for the command.
 *
 * No camera dependencies and no allocation: one row at a time.
 */
#include "frame.h"
#include <string.h>

/* "Canon" wordmark, 115x26, 1 bit per pixel, row-major, MSB left */
#define LOGO_W 115
#define LOGO_H 26
static const unsigned char g_logo[] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,31,243,0,0,0,0,0,0,0,0,0,0,0,0,
    0,255,255,0,0,0,0,0,0,0,0,0,0,0,0,
    1,248,127,0,0,0,0,0,0,0,0,0,0,0,0,
    3,224,31,0,0,0,0,0,0,0,0,0,0,0,0,
    7,224,15,0,0,0,0,0,0,0,0,0,0,0,0,
    15,192,7,0,0,0,0,0,0,0,0,0,0,0,0,
    31,192,7,0,0,0,0,0,0,0,0,0,0,0,0,
    31,128,3,1,254,1,254,63,0,15,240,15,241,248,0,
    31,128,3,7,143,128,254,127,128,60,124,7,243,252,0,
    63,128,0,15,143,192,127,143,192,120,62,3,252,126,0,
    63,128,0,31,135,192,127,7,192,248,31,3,248,62,0,
    63,128,0,31,135,192,126,7,193,248,31,131,240,62,0,
    63,128,0,15,7,192,126,7,225,240,31,131,240,63,0,
    63,128,0,0,31,192,126,7,227,240,31,131,240,63,0,
    63,128,0,0,255,192,126,7,227,240,31,131,240,63,0,
    31,128,0,7,247,192,126,7,227,240,31,131,240,63,0,
    31,128,0,15,199,192,126,7,227,240,31,131,240,63,0,
    31,192,1,31,135,192,126,7,227,240,31,131,240,63,0,
    15,192,3,191,7,192,126,7,225,240,31,131,240,63,0,
    15,224,3,63,7,192,126,7,225,248,31,3,240,63,0,
    7,240,14,63,7,192,126,7,224,248,31,3,240,63,0,
    3,248,28,31,143,224,126,7,224,120,62,3,240,63,0,
    0,255,248,31,247,248,127,15,224,60,124,3,248,127,0,
    0,31,192,7,195,241,255,159,248,15,224,15,252,255,192,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/* Arial 12 px, baseline-aligned, proportional: width + advance per glyph */
#define FONT_H 14
static const char g_charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .:/-+";
static const unsigned char g_font_w[] = {
    8,6,7,7,6,6,8,7,1,5,7,5,8,7,8,7,9,7,6,7,7,8,11,8,8,7,6,4,6,5,6,5,6,6,6,5,1,1,1,3,4,5,
};
static const unsigned char g_font_adv[] = {
    10,8,9,9,8,8,10,9,3,7,9,7,10,9,10,9,11,9,8,9,9,10,13,10,10,9,8,6,8,7,8,7,8,8,8,7,4,3,3,5,6,7,
};
static const unsigned short g_font[][FONT_H] = {
    {0,0,24,24,60,36,36,126,66,67,193,0,0,0},  /* A w8 */
    {0,0,31,49,33,17,63,33,33,33,31,0,0,0},  /* B w6 */
    {0,0,60,99,65,1,1,1,65,99,62,0,0,0},  /* C w7 */
    {0,0,31,33,97,65,65,65,97,33,31,0,0,0},  /* D w7 */
    {0,0,63,1,1,1,63,1,1,1,63,0,0,0},  /* E w6 */
    {0,0,63,1,1,1,31,1,1,1,1,0,0,0},  /* F w6 */
    {0,0,60,98,65,1,241,193,193,66,60,0,0,0},  /* G w8 */
    {0,0,97,97,97,97,127,97,97,97,97,0,0,0},  /* H w7 */
    {0,0,1,1,1,1,1,1,1,1,1,0,0,0},  /* I w1 */
    {0,0,16,16,16,16,16,16,17,27,14,0,0,0},  /* J w5 */
    {0,0,33,17,25,13,15,27,17,49,97,0,0,0},  /* K w7 */
    {0,0,1,1,1,1,1,1,1,1,31,0,0,0},  /* L w5 */
    {0,0,195,195,199,165,165,173,153,153,153,0,0,0},  /* M w8 */
    {0,0,65,67,69,69,73,73,113,97,97,0,0,0},  /* N w7 */
    {0,0,60,99,65,193,129,193,65,99,60,0,0,0},  /* O w8 */
    {0,0,63,33,97,33,63,1,1,1,1,0,0,0},  /* P w7 */
    {0,0,120,198,130,386,387,386,162,230,440,0,0,0},  /* Q w9 */
    {0,0,63,97,65,97,63,17,49,97,65,0,0,0},  /* R w7 */
    {0,0,30,51,33,3,30,32,33,35,30,0,0,0},  /* S w6 */
    {0,0,127,8,8,8,8,8,8,8,8,0,0,0},  /* T w7 */
    {0,0,97,97,97,97,97,97,97,35,30,0,0,0},  /* U w7 */
    {0,0,195,66,66,102,36,36,60,24,24,0,0,0},  /* V w8 */
    {0,0,1121,1123,1650,594,722,670,908,396,268,0,0,0},  /* W w11 */
    {0,0,66,102,36,24,24,60,36,102,195,0,0,0},  /* X w8 */
    {0,0,195,66,36,60,24,24,24,24,24,0,0,0},  /* Y w8 */
    {0,0,126,96,48,16,8,12,6,2,127,0,0,0},  /* Z w7 */
    {0,0,28,50,34,35,35,35,34,50,28,0,0,0},  /* 0 w6 */
    {0,0,12,14,15,12,12,12,12,12,12,0,0,0},  /* 1 w4 */
    {0,0,30,50,32,32,16,8,4,2,63,0,0,0},  /* 2 w6 */
    {0,0,14,25,16,8,12,16,16,25,15,0,0,0},  /* 3 w5 */
    {0,0,16,24,28,20,18,19,63,16,16,0,0,0},  /* 4 w6 */
    {0,0,31,1,1,15,25,16,16,25,15,0,0,0},  /* 5 w5 */
    {0,0,28,50,2,31,51,35,34,50,28,0,0,0},  /* 6 w6 */
    {0,0,63,48,16,8,8,12,4,4,4,0,0,0},  /* 7 w6 */
    {0,0,28,50,34,50,28,50,35,50,28,0,0,0},  /* 8 w6 */
    {0,0,14,25,17,17,25,22,16,25,15,0,0,0},  /* 9 w5 */
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0},  /*   w1 */
    {0,0,0,0,0,0,0,0,0,0,1,0,0,0},  /* . w1 */
    {0,0,0,0,1,0,0,0,0,0,1,0,0,0},  /* : w1 */
    {0,0,4,4,4,2,2,2,2,1,1,0,0,0},  /* / w3 */
    {0,0,0,0,0,0,0,15,0,0,0,0,0,0},  /* - w4 */
    {0,0,0,0,4,4,31,4,4,0,0,0,0,0},  /* + w5 */
};

#define FONT_N ((int)(sizeof g_font / sizeof g_font[0]))

static int glyph_index(char c) {
    int i;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (i = 0; g_charset[i]; i++)
        if (g_charset[i] == c) return i;
    return -1;
}

int se_frame_text_width(const char *text, int scale) {
    int i, w = 0;
    if (!text || scale < 1) return 0;
    for (i = 0; text[i]; i++) {
        int gi = glyph_index(text[i]);
        w += gi >= 0 ? g_font_adv[gi] : 6;
    }
    return w * scale;
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

/* One scaled text row, `ytop` being the top of the glyph box within the band. */
static void text_row(uint8_t *row, int width, int y, int ytop, int scale,
                     const char *text, int x0, const uint8_t fg[3]) {
    int gy, x, i, sx, sy;
    if (!text || y < ytop) return;
    gy = (y - ytop) / scale;
    if (gy < 0 || gy >= FONT_H) return;
    x = x0;
    for (i = 0; text[i]; i++) {
        int gi = glyph_index(text[i]);
        int gw;
        if (gi < 0) { x += 6 * scale; continue; }
        gw = g_font_w[gi];
        for (sx = 0; sx < gw; sx++) {
            if (!((g_font[gi][gy] >> sx) & 1)) continue;
            for (sy = 0; sy < scale; sy++) {
                int px = x + sx * scale + sy;
                if (px < 0 || px >= width) continue;
                row[px * 3 + 0] = fg[0];
                row[px * 3 + 1] = fg[1];
                row[px * 3 + 2] = fg[2];
            }
        }
        x += g_font_adv[gi] * scale;
    }
}

/* The wordmark, same scaling rule. */
static void logo_row(uint8_t *row, int width, int y, int ytop, int scale,
                     int x0, const uint8_t fg[3]) {
    int gy, gx, sx, sy;
    if (y < ytop) return;
    gy = (y - ytop) / scale;
    if (gy < 0 || gy >= LOGO_H) return;
    for (gx = 0; gx < LOGO_W; gx++) {
        if (!((g_logo[gy * ((LOGO_W + 7) / 8) + (gx >> 3)] >> (7 - (gx & 7))) & 1))
            continue;
        for (sx = 0; sx < scale; sx++) {
            for (sy = 0; sy < scale; sy++) {
                int px = x0 + gx * scale + sx;
                if (px < 0 || px >= width) continue;
                row[px * 3 + 0] = fg[0];
                row[px * 3 + 1] = fg[1];
                row[px * 3 + 2] = fg[2];
            }
        }
    }
}

void se_frame_layout(se_frame *f) {
    int s = f->scale < 1 ? 1 : (f->scale > 4 ? 4 : f->scale);
    int pad = 8 * s;
    int model_top = pad + LOGO_H * s + 4 * s;
    f->scale = s;
    f->top = pad;
    f->bottom = model_top + FONT_H * s + pad;
}

void se_frame_top_row(uint8_t *row, int width, const se_frame *f, int y) {
    (void)y;
    se_frame_fill_row(row, width, f);
}

void se_frame_bottom_row(uint8_t *row, int width, const se_frame *f, int y) {
    int s = f->scale < 1 ? 1 : f->scale;
    int pad = 8 * s;
    int logo_top = pad;
    int model_top = logo_top + LOGO_H * s + 4 * s;
    int label_top = pad;
    int value_top = label_top + FONT_H * s + 3 * s;
    int cw[3], total = 0, gap = 8 * s, x, i;

    se_frame_fill_row(row, width, f);

    logo_row(row, width, y, logo_top, s, pad, f->brand_colour);
    text_row(row, width, y, model_top, s, f->model, pad, f->dim);

    for (i = 0; i < 3; i++) {
        int wl = se_frame_text_width(f->label[i], s);
        int wv = se_frame_text_width(f->value[i], s);
        cw[i] = wl > wv ? wl : wv;
        total += cw[i];
    }
    total += 2 * gap;
    x = width - pad - total;
    if (x < pad) x = pad;
    for (i = 0; i < 3; i++) {
        int wl = se_frame_text_width(f->label[i], s);
        int wv = se_frame_text_width(f->value[i], s);
        text_row(row, width, y, label_top, s, f->label[i], x + (cw[i] - wl) / 2, f->dim);
        text_row(row, width, y, value_top, s, f->value[i], x + (cw[i] - wv) / 2, f->fg);
        x += cw[i] + gap;
    }
}
