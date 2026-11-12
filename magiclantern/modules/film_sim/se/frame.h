/*
 * Film-frame compositor: the rebate around a developed photo.
 *
 * Real film carries its identity in the rebate: the stock name and edge code
 * along one edge, the frame number along the other. A print from a lab gets a
 * border for the same reason -- it tells you what you are looking at. This
 * draws the same thing into the developed JPEG: a band top and bottom in the
 * stock's own colour, with the stock name and the camera line.
 *
 * No camera dependencies, no allocation: one row at a time, which is what the
 * develop path already writes. That also makes it testable on the host.
 */
#ifndef SE_FRAME_H
#define SE_FRAME_H

#include <stdint.h>

/* Longest line the bands accept, including the terminator. */
#define SE_FRAME_LINE 48

typedef struct {
    int          on;          /* 0 = draw nothing */
    int          top;         /* band height in rows, top */
    int          bottom;      /* band height in rows, bottom */
    int          scale;       /* glyph scale: 1..4 */
    uint8_t      bg[3];       /* band colour (stock brand) */
    uint8_t      fg[3];       /* text colour */
    char         line1[SE_FRAME_LINE];  /* top band, usually the stock */
    char         line2[SE_FRAME_LINE];  /* bottom band, the camera line */
} se_frame;

/* Rows a frame of this config adds to the image (0 when off). */
int se_frame_extra_rows(const se_frame *f);

/* Fill one band row with the band colour. */
/* Fill one row with the band colour blended half-way to the text colour: the
 * keyline between a band and the picture, the way a print border is ruled. */
void se_frame_edge_row(uint8_t *row, int width, const se_frame *f);

/* Draw `text` starting at column `x0` instead of the band's left padding. */
void se_frame_text_row_at(uint8_t *row, int width, const se_frame *f,
                          int y, int ytop, const char *text, int x0);
void se_frame_fill_row(uint8_t *row, int width, const se_frame *f);

/*
 * Draw one band row's worth of `text`.
 *
 * `y` is the row's index inside the band (0 = first row of the band), `ytop`
 * the first row of the text, both in band coordinates. Rows outside the glyph
 * box are left alone, so the caller can loop over every band row.
 */
void se_frame_text_row(uint8_t *row, int width, const se_frame *f,
                       int y, int ytop, const char *text);

/* Where the text starts vertically inside a band of height `band`. */
int se_frame_text_top(int band, int scale);

/* Pixel width of `text` at `scale`, including inter-character spacing. */
int se_frame_text_width(const char *text, int scale);

/* Pick a text colour that reads on `bg`. */
void se_frame_contrast(const uint8_t bg[3], uint8_t fg[3]);

#endif
