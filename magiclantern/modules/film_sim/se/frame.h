/*
 * Film-frame compositor: the print border around a developed photo.
 *
 * The layout is the one a photo app uses for a bordered print: a plain margin
 * all round, and a wider bottom band carrying the camera on the left and the
 * exposure on the right.
 *
 *   +--------------------------------------+
 *   |                                      |   margin
 *   |            the photograph            |
 *   |                                      |
 *   +--------------------------------------+   keyline
 *   | CANON            f      SS     ISO   |   bottom band
 *   | EOS 6D          2.8   1/200    400   |
 *   +--------------------------------------+
 *
 * No camera dependencies and no allocation: one row at a time, which is what
 * the develop path already writes. That also makes it testable on the host.
 */
#ifndef SE_FRAME_H
#define SE_FRAME_H

#include <stdint.h>

#define SE_FRAME_LINE 24

typedef struct {
    int          on;
    int          scale;        /* bitmap scale for wordmark and text: 1..4 */
    int          top;          /* margin above the picture, rows */
    int          bottom;       /* band below the picture, rows */
    uint8_t      bg[3];        /* paper colour */
    uint8_t      fg[3];        /* main text */
    uint8_t      dim[3];       /* model line and column labels */
    uint8_t      brand_colour[3];  /* wordmark; a brand colour, or fg */
    char         model[SE_FRAME_LINE];   /* "EOS 6D" */
    char         stock[SE_FRAME_LINE];   /* the stock's name, under its mark */
    char         lens[SE_FRAME_LINE];    /* the lens, under the camera model */
    int          inner_h;                /* band height; 0 = 150 */
    const uint8_t *sprite;    /* stock canister, RGB, sprite_w x sprite_h */
    int          sprite_w, sprite_h, sprite_scale;
    const uint8_t *brand;     /* brand mark, BRAND_SZ x BRAND_SZ, 8-bit coverage */
    int          brand_i;     /* index into the baked brand table, -1 = none */
    uint8_t      brand_fg[3]; /* the mark's own colour */
    char         label[3][8];            /* "f", "SS", "ISO" */
    char         value[3][12];           /* "2.8", "1/200", "400" */
} se_frame;

/* Fill top/bottom from scale. Call after setting scale. */
void se_frame_layout(se_frame *f);

/* Pixel width of `text` at `scale`, including inter-character spacing. */
int se_frame_text_width(const char *text, int scale);

void se_frame_fill_row(uint8_t *row, int width, const se_frame *f);
void se_frame_edge_row(uint8_t *row, int width, const se_frame *f);

/* One row of the top margin, or of the bottom band, in band coordinates. */
void se_frame_top_row(uint8_t *row, int width, const se_frame *f, int y);
void se_frame_bottom_row(uint8_t *row, int width, const se_frame *f, int y);

/* Format the three exposure values for the band, no libc float needed. */
void se_frame_fmt_iso(char *out, int cap, int iso);
void se_frame_fmt_ap(char *out, int cap, int ap_x10);   /* 28 -> "2.8" */
void se_frame_fmt_ss(char *out, int cap, int denom);    /* 200 -> "1/200" */

/* Pick a text colour that reads on `bg`. */
void se_frame_contrast(const uint8_t bg[3], uint8_t fg[3]);

#endif
