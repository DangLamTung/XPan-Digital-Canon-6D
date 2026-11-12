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
    int          scale;        /* glyph scale for the brand and values: 1..4 */
    int          top;          /* margin above the picture, rows */
    int          bottom;       /* band below the picture, rows */
    uint8_t      bg[3];        /* paper colour */
    uint8_t      fg[3];        /* main text */
    uint8_t      dim[3];       /* model line and column labels */
    char         brand[SE_FRAME_LINE];   /* "CANON" */
    char         model[SE_FRAME_LINE];   /* "EOS 6D" */
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

/* Pick a text colour that reads on `bg`. */
void se_frame_contrast(const uint8_t bg[3], uint8_t fg[3]);

#endif
