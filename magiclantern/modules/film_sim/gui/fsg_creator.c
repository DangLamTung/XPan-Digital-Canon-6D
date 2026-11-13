/*
 * The 12-colour creator, drawn.
 *
 * Only fsg_fill() and fsg_text(): no lines, no circles, no alpha. So the
 * wheel is a dodecagon radar rasterised via scanline polygon fills, and the
 * guide ray is a run of small squares. Every colour is either a fixed constant
 * from fsg.h or a palette slot registered via fsg_pal(), so the camera's 8-bit
 * overlay and the desktop preview produce the exact same screen.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fsg.h"
#include "fsg_creator.h"
#include "fs_num.h"

/* ---- colours ---------------------------------------------------------- */

/* radial steps per wedge: one palette entry each, see register_palette */
#define CREATOR_BANDS   4

static const char *const wedge_name[FS_CREATOR_WEDGES] = {
    "RED", "ORANGE", "YELLOW", "LIME", "GREEN", "SPRING",
    "CYAN", "AZURE", "BLUE", "VIOLET", "MAGENTA", "ROSE",
};

static const char *const wedge_short[FS_CREATOR_WEDGES] = {
    "R", "OR", "Y", "LM", "G", "SP",
    "CY", "AZ", "B", "VI", "MG", "RS",
};

const char *fs_creator_wedge_name(int i)
{
    return wedge_name[((i % FS_CREATOR_WEDGES) + FS_CREATOR_WEDGES) % FS_CREATOR_WEDGES];
}

/*
 * Nominal colour of a wedge, as a hue sweep at a fixed S and V so the twelve
 * read as one family. Integer only: the camera has no libm in this path.
 *
 * These are for the BEADS and SECTORS. They are not what the stock does to a
 * colour -- the engine decides that -- they only say which wedge a number
 * belongs to.
 */
int fs_creator_wedge_rgb(int i)
{
    /* hue in sixths of a turn, 0..3599; wedge i sits at i * 300 */
    int h = ((i % FS_CREATOR_WEDGES) + FS_CREATOR_WEDGES) % FS_CREATOR_WEDGES;
    int seg = (h * 300) / 600;          /* which 60-degree sector */
    int f   = (h * 300) % 600;          /* 0..599 within it       */
    int up  = f * 255 / 600;
    /* S = 0.72, V = 1.0: vivid enough to tell apart, dull enough not to
     * look like a warning light on a camera screen. */
    int v   = 235;
    int lo  = v - (v * 72) / 100;
    int r, g, b;

    switch (seg) {
    case 0:  r = v;  g = lo + (v - lo) * up / 255; b = lo; break;
    case 1:  r = v - (v - lo) * up / 255; g = v;   b = lo; break;
    case 2:  r = lo; g = v;   b = lo + (v - lo) * up / 255; break;
    case 3:  r = lo; g = v - (v - lo) * up / 255; b = v;   break;
    case 4:  r = lo + (v - lo) * up / 255; g = lo; b = v;  break;
    default: r = v;  g = lo;  b = v - (v - lo) * up / 255; break;
    }
    return (r << 16) | (g << 8) | b;
}

/*
 * The wheel is filled, so one colour per wedge is not enough: each wedge is
 * drawn as four concentric bands and the bands need their own entries. The
 * overlay palette has 175 (FSG_PAL_N, entries 80..254) and only the first
 * sixteen are ever borrowed by another screen, so 48 is safe -- nothing else
 * references slots 16..47.
 */
static const int wedge_shade[CREATOR_BANDS] = { 68, 122, 184, 255 };

void fs_creator_register_palette(void)
{
    int pal[FS_CREATOR_WEDGES * CREATOR_BANDS], i, b;
    for (i = 0; i < FS_CREATOR_WEDGES; i++) {
        int rgb = fs_creator_wedge_rgb(i);
        for (b = 0; b < CREATOR_BANDS; b++) {
            int k = wedge_shade[b], r = (rgb >> 16) & 255;
            int g = (rgb >> 8) & 255, bl = rgb & 255;
            pal[i * CREATOR_BANDS + b] =
                ((r * k / 255) << 16) | ((g * k / 255) << 8) | (bl * k / 255);
        }
    }
    fsg_pal(pal, FS_CREATOR_WEDGES * CREATOR_BANDS);
}

static const int sin30[12] = { 0, 500, 866, 1000, 866, 500,
                               0, -500, -866, -1000, -866, -500 };

/* ---- small drawing helpers over fills -------------------------------- */

static int isqrt_i(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

static void line_fill(int x0, int y0, int x1, int y1, int c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        fsg_fill(x0, y0, 1, 1, c);
        if (x0 == x1 && y0 == y1) break;
        {
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

/* sin/cos for a tenth of a degree, from the 30-degree table, in 0.001 units.
 * Index 0 is +x, so angle 900 is straight up. */
static void sincos10(int ang, int *s10, int *c10)
{
    int idx, f, a, b, id2;
    ang = ((ang % 3600) + 3600) % 3600;
    idx = (ang / 300) % 12;
    f = ang % 300;
    a = sin30[idx]; b = sin30[(idx + 1) % 12];
    *s10 = a + (b - a) * f / 300;
    id2 = (idx + 3) % 12;
    a = sin30[id2]; b = sin30[(id2 + 1) % 12];
    *c10 = a + (b - a) * f / 300;

    if (f) {
        int mag2 = *s10 * *s10 + *c10 * *c10;
        if (mag2 > 0) {
            int mag = isqrt_i(mag2);
            if (mag > 0) {
                *s10 = *s10 * 1000 / mag;
                *c10 = *c10 * 1000 / mag;
            }
        }
    }
}

static void dodec_point(int cx, int cy, int r, int ang10, int *ox, int *oy)
{
    int s10, c10;
    sincos10(ang10, &s10, &c10);
    *ox = cx + (r * c10) / 1000;
    *oy = cy - (r * s10) / 1000;
}

static void circle_fill(int cx, int cy, int r, int colour)
{
    int dy;
    for (dy = -r; dy <= r; dy++) {
        int half = isqrt_i(r * r - dy * dy);
        fsg_fill(cx - half, cy + dy, half * 2 + 1, 1, colour);
    }
}

static void disc(int cx, int cy, int r, int colour)
{
    circle_fill(cx, cy, r, colour);
}

static void rect_outline(int x, int y, int w, int h, int c)
{
    fsg_fill(x, y, w, 1, c);
    fsg_fill(x, y + h - 1, w, 1, c);
    fsg_fill(x, y, 1, h, c);
    fsg_fill(x + w - 1, y, 1, h, c);
}

static void corners(int x, int y, int w, int h, int rgb, int len)
{
    fsg_fill(x, y, len, 2, rgb);
    fsg_fill(x, y, 2, len, rgb);
    fsg_fill(x + w - len, y, len, 2, rgb);
    fsg_fill(x + w - 2, y, 2, len, rgb);
    fsg_fill(x, y + h - 2, len, 2, rgb);
    fsg_fill(x, y + h - len, 2, len, rgb);
    fsg_fill(x + w - len, y + h - 2, len, 2, rgb);
    fsg_fill(x + w - 2, y + h - len, 2, len, rgb);
}

static void panel_card(int x, int y, int w, int h)
{
    fsg_fill(x + 3, y + 3, w, h, FSG_BLACK);
    fsg_fill(x, y, w, h, FSG_PANEL);
    rect_outline(x, y, w, h, FSG_LINE);
    corners(x, y, w, h, FSG_AMBER_DIM, 14);
}

/* the ring's geometry, in one place: the static screen and the spin redraw
 * must agree or a bead would be erased where it was not drawn */
#define CREATOR_PANEL_X 388   /* the wheel's box ends here */
#define CREATOR_CX      194
#define CREATOR_CY      252
#define CREATOR_R_OUT   152
#define CREATOR_R_IN    54
#define CREATOR_DISC_BG FSG_PANEL_DARK

/* Bipolar bar: fills outward from the neutral midpoint (`mid_val`), so an
 * untouched control reads as a clean zero tick rather than a half-full bar. */
static void bar_bipolar(int x, int y, int w, int h, int value, int lo, int hi,
                        int mid_val, int fill, int active)
{
    int span = hi - lo;
    int inner = w - 4;
    int pos = (span > 0) ? ((value - lo) * inner) / span : 0;
    int mid = (span > 0) ? ((mid_val - lo) * inner) / span : inner / 2;
    int x0, x1;

    if (pos < 0) pos = 0;
    if (pos > inner) pos = inner;
    if (mid < 0) mid = 0;
    if (mid > inner) mid = inner;

    fsg_fill(x, y, w, h, FSG_BG_DEEP);
    rect_outline(x, y, w, h, active ? FSG_AMBER : FSG_LINE_DIM);

    /* quarter ticks */
    fsg_fill(x + 2 + inner / 4, y + h - 4, 1, 3, FSG_LINE_DIM);
    fsg_fill(x + 2 + (inner * 3) / 4, y + h - 4, 1, 3, FSG_LINE_DIM);

    if (pos > mid) {
        x0 = x + 2 + mid;
        x1 = x + 2 + pos;
        fsg_fill(x0, y + 2, x1 - x0, h - 4, fill);
    } else if (pos < mid) {
        x0 = x + 2 + pos;
        x1 = x + 2 + mid;
        fsg_fill(x0, y + 2, x1 - x0, h - 4, fill);
    }

    /* neutral centre tick */
    fsg_fill(x + 2 + mid, y + 1, 1, h - 2, active ? FSG_WHITE : FSG_LINE);
    /* thumb marker at current value */
    fsg_fill(x + 1 + pos, y + 1, 3, h - 2, active ? FSG_WHITE : FSG_GREY);
}

/* ---- header / footer -------------------------------------------------- */

static void header(const char *title, const char *stock, const char *right)
{
    fsg_fill(0, 0, FSG_W, 44, FSG_RED_DEEP);
    fsg_fill(0, 42, FSG_W, 2, FSG_RED);
    fsg_fill(0, 0, 6, 44, FSG_AMBER);
    fsg_text(18, 7, FSG_T_MED, FSG_AMBER, FSG_RED_DEEP, "%s", title);
    if (stock && stock[0])
        fsg_text(18, 27, FSG_T_SMALL, FSG_GREY, FSG_RED_DEEP, "%s", stock);
    if (right && right[0]) {
        int rx = FSG_W - 18 - (int)strlen(right) * 11;
        fsg_text(rx, 13, FSG_T_MED, FSG_WHITE, FSG_RED_DEEP, "%s", right);
    }
}

static void footer(const char *keys_l, const char *keys_r)
{
    fsg_fill(0, FSG_H - 36, FSG_W, 36, FSG_BG_DEEP);
    fsg_fill(0, FSG_H - 36, FSG_W, 1, FSG_LINE);
    if (keys_l)
        fsg_text(16, FSG_H - 24, FSG_T_SMALL, FSG_GREY, FSG_BG_DEEP, "%s", keys_l);
    if (keys_r) {
        int rx = FSG_W - 16 - (int)strlen(keys_r) * 8;
        fsg_text(rx, FSG_H - 24, FSG_T_SMALL, FSG_AMBER, FSG_BG_DEEP, "%s", keys_r);
    }
}

const char *const fs_creator_tabs[3] = { "Saturation", "Luminance", "Hue" };
const int fs_creator_tab_field[3] = { 0, 2, 1 };

/*
 * Filled sectors: the radar is a colour wheel, not twelve dots. A scanline
 * fill is one fsg_fill per row per polygon, which is the cheapest way to put
 * ~39k coloured pixels on the panel without a per-pixel call.
 */
#define CREATOR_SLOT(i, b) FSG_PIX((i) * CREATOR_BANDS + (b))

static void poly_fill(const int *xs, const int *ys, int n, int colour)
{
    int i, j, y, ymin = ys[0], ymax = ys[0];

    for (i = 1; i < n; i++) {
        if (ys[i] < ymin) ymin = ys[i];
        if (ys[i] > ymax) ymax = ys[i];
    }
    for (y = ymin; y <= ymax; y++) {
        int hit[8], nh = 0;
        for (i = 0, j = n - 1; i < n; j = i++) {
            if ((ys[i] > y) != (ys[j] > y)) {
                int dy = ys[i] - ys[j];
                if (nh < 8)
                    hit[nh++] = xs[j] +
                        (int)((long long)(y - ys[j]) * (xs[i] - xs[j]) / dy);
            }
        }
        for (i = 0; i < nh; i++)
            for (j = i + 1; j < nh; j++)
                if (hit[j] < hit[i]) { int t = hit[i]; hit[i] = hit[j]; hit[j] = t; }
        for (i = 0; i + 1 < nh; i += 2)
            fsg_fill(hit[i], y, hit[i + 1] - hit[i] + 1, 1, colour);
    }
}

/* one radial band of one wedge, as a quadrilateral */
static void sector_band(int cx, int cy, int r0, int r1, int a1, int a2, int colour)
{
    int xs[4], ys[4];
    dodec_point(cx, cy, r0, a1, &xs[0], &ys[0]);
    dodec_point(cx, cy, r1, a1, &xs[1], &ys[1]);
    dodec_point(cx, cy, r1, a2, &xs[2], &ys[2]);
    dodec_point(cx, cy, r0, a2, &xs[3], &ys[3]);
    poly_fill(xs, ys, 4, colour);
}

/* ---- the wheel -------------------------------------------------------- */

#define CREATOR_R_MID   90
#define CREATOR_R_SPAN  32  /* +-32 px over the full value range */
#define CREATOR_R_MAX   126    /* the coloured perimeter */

static int wedge_radius(const fs_creator_t *c, int i, int tab)
{
    int v    = (tab == 0) ? c->sat[i] : (tab == 1 ? c->rot[i] : c->lum[i]);
    int mid  = (tab == 1) ? 0 : 100;
    int span = (tab == 1) ? 180 : 100;
    int d = v - mid;
    if (d > span) d = span;
    if (d < -span) d = -span;
    return CREATOR_R_MID + (d * CREATOR_R_SPAN) / span;
}

/* angle of wedge i, in tenths of a degree, with index 0 at the top */
static int wedge_angle(int i, int offset_q16)
{
    int off10 = (int)(((long long)offset_q16 * 300) / 65536);
    return ((i * 300 - off10 + 900) % 3600 + 3600) % 3600;
}

void fsg_draw_creator_wheel(const fs_creator_t *c, int offset_q16)
{
    const int cx = CREATOR_CX, cy = CREATOR_CY;
    int i, px[FS_CREATOR_WEDGES], py[FS_CREATOR_WEDGES];
    char num[12];

    /* clear the wheel's box only: the right-hand panel is untouched and stays
     * in the idle buffer, which is what makes a spin frame cheap */
    fsg_fill(0, FSG_CREATOR_RING_Y0, CREATOR_PANEL_X,
             FSG_CREATOR_RING_Y1 - FSG_CREATOR_RING_Y0, FSG_BG);

    /* Outer machined bezel ring + dark dial plate */
    disc(cx, cy, CREATOR_R_OUT + 2, FSG_LINE_DIM);
    disc(cx, cy, CREATOR_R_OUT, CREATOR_DISC_BG);

    /* 12 wedges x 4 concentric bands from the hub out to the perimeter */
    {
        int b, r0 = CREATOR_R_IN + 2, r1 = CREATOR_R_MAX;
        int step = (r1 - r0) / CREATOR_BANDS;
        for (b = 0; b < CREATOR_BANDS; b++) {
            int lo = r0 + b * step, hi = (b == CREATOR_BANDS - 1) ? r1 : lo + step;
            for (i = 0; i < FS_CREATOR_WEDGES; i++) {
                int a = wedge_angle(i, offset_q16);
                sector_band(cx, cy, lo, hi, a - 150, a + 150, CREATOR_SLOT(i, b));
            }
        }
    }

    /* 12 radial facet dividers between wedges + neutral R_MID reference ring */
    for (i = 0; i < FS_CREATOR_WEDGES; i++) {
        int x0, y0, x1, y1, mx0, my0, mx1, my1;
        int a = wedge_angle(i, offset_q16);
        dodec_point(cx, cy, CREATOR_R_IN + 2, a - 150, &x0, &y0);
        dodec_point(cx, cy, CREATOR_R_MAX, a - 150, &x1, &y1);
        line_fill(x0, y0, x1, y1, FSG_BG_DEEP);

        /* Neutral 100% / 0-deg reference dodecagon chord */
        dodec_point(cx, cy, CREATOR_R_MID, a - 150, &mx0, &my0);
        dodec_point(cx, cy, CREATOR_R_MID, a + 150, &mx1, &my1);
        line_fill(mx0, my0, mx1, my1, FSG_BG_DEEP);
    }

    /* Colored outer perimeter along the 12 facets */
    for (i = 0; i < FS_CREATOR_WEDGES; i++) {
        int x1, y1, x2, y2, a = wedge_angle(i, offset_q16);
        dodec_point(cx, cy, CREATOR_R_MAX, a - 150, &x1, &y1);
        dodec_point(cx, cy, CREATOR_R_MAX, a + 150, &x2, &y2);
        line_fill(x1, y1, x2, y2, CREATOR_SLOT(i, CREATOR_BANDS - 1));
        line_fill(x1, y1 + 1, x2, y2 + 1, CREATOR_SLOT(i, CREATOR_BANDS - 1));
    }

    /* Inner hub disc + crisp bezel ring */
    disc(cx, cy, CREATOR_R_IN + 1, FSG_LINE);
    disc(cx, cy, CREATOR_R_IN - 1, FSG_BG_DEEP);

    /* Compute 12 radar vertices */
    for (i = 0; i < FS_CREATOR_WEDGES; i++)
        dodec_point(cx, cy, wedge_radius(c, i, c->field),
                    wedge_angle(i, offset_q16), &px[i], &py[i]);

    /* Olympus-style radial dotted guide ray along the selected wedge */
    {
        int r, a = wedge_angle(c->sel, offset_q16);
        int r_sel = wedge_radius(c, c->sel, c->field);
        for (r = CREATOR_R_IN + 7; r <= CREATOR_R_MAX - 5; r += 8) {
            int dx, dy;
            if (r >= r_sel - 8 && r <= r_sel + 8) continue;
            dodec_point(cx, cy, r, a, &dx, &dy);
            fsg_fill(dx - 1, dy - 1, 3, 3,
                     r < r_sel ? FSG_AMBER : FSG_WHITE);
        }
    }

    /* Value polygon contour (shadow stroke + bright white contour) */
    for (i = 0; i < FS_CREATOR_WEDGES; i++) {
        int j = (i + 1) % FS_CREATOR_WEDGES;
        line_fill(px[i], py[i] + 1, px[j], py[j] + 1, FSG_BG_DEEP);
        line_fill(px[i], py[i], px[j], py[j], FSG_WHITE);
    }

    /* 12 Colored Vertex Beads */
    for (i = 0; i < FS_CREATOR_WEDGES; i++) {
        if (i == c->sel) {
            circle_fill(px[i], py[i], 9, FSG_AMBER);
            circle_fill(px[i], py[i], 6, FSG_BG_DEEP);
            circle_fill(px[i], py[i], 4, CREATOR_SLOT(i, CREATOR_BANDS - 1));
        } else {
            circle_fill(px[i], py[i], 6, FSG_BG_DEEP);
            circle_fill(px[i], py[i], 5, CREATOR_SLOT(i, CREATOR_BANDS - 1));
            circle_fill(px[i], py[i], 2, FSG_BG_DEEP);
        }
    }

    /* 12 Short Hue Labels outside the perimeter */
    for (i = 0; i < FS_CREATOR_WEDGES; i++) {
        int x, y;
        const char *tag = wedge_short[i];
        int tw = (int)strlen(tag) * 7;
        dodec_point(cx, cy, CREATOR_R_MAX + 15, wedge_angle(i, offset_q16), &x, &y);
        fsg_text(x - tw / 2, y - 6, FSG_T_SMALL,
                 i == c->sel ? FSG_AMBER : FSG_GREY, CREATOR_DISC_BG, "%s", tag);
    }

    /* Fixed 12-o'clock index pointer perched on the outer bezel rim,
     * well clear of both the rim labels and boosted vertices */
    {
        int y_rim = cy - CREATOR_R_OUT - 6;
        fsg_fill(cx - 6, y_rim, 13, 2, FSG_AMBER);
        fsg_fill(cx - 4, y_rim + 2, 9, 2, FSG_AMBER);
        fsg_fill(cx - 2, y_rim + 4, 5, 2, FSG_AMBER);
    }

    /* Hub readout: color swatch bar + centered wedge name + centered value */
    fsg_fill(cx - 16, cy - 32, 32, 4, CREATOR_SLOT(c->sel, CREATOR_BANDS - 1));
    {
        const char *wname = fs_creator_wedge_name(c->sel);
        int nw = (int)strlen(wname) * 11;
        fsg_text(cx - nw / 2, cy - 20, FSG_T_MED, FSG_WHITE, FSG_BG_DEEP, "%s", wname);
    }
    {
        int v = (c->field == 0) ? c->sat[c->sel]
              : (c->field == 1) ? c->rot[c->sel] : c->lum[c->sel];
        int vw;
        snprintf(num, sizeof num, (c->field == 1) ? "%+d" : "%d", v);
        vw = (int)strlen(num) * 16;
        fsg_text(cx - vw / 2, cy + 4, FSG_T_LARGE, FSG_AMBER, FSG_BG_DEEP, "%s", num);
    }
}

void fsg_draw_creator_beads(const fs_creator_t *c, int offset_q16,
                            fs_creator_spin_t *sp)
{
    (void)sp;
    fsg_draw_creator_wheel(c, offset_q16);
}

void fsg_draw_fx(const fs_creator_t *c)
{
    static const char *const label[5] = {
        "GRAIN", "GRAIN SIZE", "HALATION", "COLOR CHROME", "FX BLUE"
    };
    static const int lo[5] = { 0, 0, 0, 0, 0 };
    static const int hi[5] = { 100, 100, 100, 2, 2 };
    char right[24], value[32];
    int i;

    fsg_fill(0, 0, FSG_W, FSG_H, FSG_BG);
    snprintf(right, sizeof right, "%s", c->saved ? "SAVED" : "UNSAVED");
    header("COLOR CREATOR", c->stock, right);

    fsg_text(24, 76, FSG_T_LARGE, FSG_AMBER, FSG_BG, "FX");
    fsg_text(90, 84, FSG_T_SMALL, FSG_GREY, FSG_BG,
             "what this look asks for (nothing is forced)");

    for (i = 0; i < 5; i++) {
        int v = (i == 0) ? c->grain : (i == 1) ? c->grain_size
              : (i == 2) ? c->halation : (i == 3) ? c->chrome : c->fx_blue;
        int y = 120 + i * 52;
        fsg_text(24, y, FSG_T_SMALL, i == c->field ? FSG_AMBER : FSG_GREY,
                 FSG_BG, "%s", label[i]);
        if (i >= 3)
            snprintf(value, sizeof value, "%s", v == 0 ? "OFF" : (v == 1 ? "WEAK" : "STRONG"));
        else
            snprintf(value, sizeof value, "%d", v);
        fsg_text(600, y, FSG_T_SMALL, FSG_WHITE, FSG_BG, "%s", value);
        bar_bipolar(24, y + 18, 640, 16, v, lo[i], hi[i], lo[i],
                    i == c->field ? FSG_AMBER : FSG_RED_MID, i == c->field);
    }
    if (c->save_err) {
        fsg_text(24, 386, FSG_T_SMALL, FSG_RED_MID, FSG_BG,
                 "%s", "SAVE FAILED - see FILMSIM.LOG");
    } else if (c->saved) {
        char line[48];
        snprintf(line, sizeof line, "SAVED ML/FILM/%s.fs",
                 c->saved_name ? c->saved_name : "WHEEL");
        fsg_text(24, 386, FSG_T_SMALL, FSG_GREEN, FSG_BG, "%s", line);
    } else {
        char line[48];
        snprintf(line, sizeof line, "PLAY writes %s.fs",
                 c->slot_name ? c->slot_name : "WHEEL");
        fsg_text(24, 386, FSG_T_SMALL, FSG_GREY, FSG_BG, "%s", line);
    }
    footer("UP/DOWN value    SET field    PLAY save",
           "Q: wheel/tone/fx    MENU back");
}


/* ---- the slot screen ------------------------------------------------- */

/*
 * Four wheels, listed with what each one holds. UP/DOWN picks, SET loads.
 * A slot that has never been saved says EMPTY and SET on it starts a blank
 * wheel rather than overwriting the one you are on -- "start a second look"
 * has to be one keypress from "I am looking at the first".
 */
void fsg_draw_slots(const fs_creator_t *c)
{
    char right[24];
    int i;

    fsg_fill(0, 0, FSG_W, FSG_H, FSG_BG);
    snprintf(right, sizeof right, "SLOT %d / %d", c->slot_cur + 1,
             FS_CREATOR_SLOTS);
    header("COLOR CREATOR", c->stock, right);

    fsg_text(24, 76, FSG_T_LARGE, FSG_AMBER, FSG_BG, "RECIPE SLOTS");
    fsg_text(24, 112, FSG_T_SMALL, FSG_GREY, FSG_BG,
             "Each slot is its own wheel. SET loads the one under the bar.");

    for (i = 0; i < FS_CREATOR_SLOTS; i++) {
        int col = i / 5, row = i % 5;
        int y = 140 + row * 52;
        int x0 = 16 + col * 352;
        int here = (i == c->slot_pick);
        const char *label = c->slot_label[i];
        char file[24];

        snprintf(file, sizeof file, "ML/FILM/%s.fs",
                 c->slot_file[i] ? c->slot_file[i] : "WHEEL");
        if (here)
            fsg_fill(x0, y - 6, 336, 44, FSG_PANEL);
        fsg_text(x0 + 8, y, FSG_T_SMALL, here ? FSG_AMBER : FSG_GREY,
                 here ? FSG_PANEL : FSG_BG, "%s%s", here ? "> " : "  ", file);
        fsg_text(x0 + 150, y, FSG_T_SMALL,
                 label ? FSG_WHITE : FSG_GREY_DIM,
                 here ? FSG_PANEL : FSG_BG, "%s",
                 label ? label : "EMPTY");
        if (i == c->slot_cur)
            fsg_text(x0 + 288, y, FSG_T_SMALL, FSG_GREEN,
                     here ? FSG_PANEL : FSG_BG, "%s", "LOADED");
    }

    if (c->save_err)
        fsg_text(24, 386, FSG_T_SMALL, FSG_RED_MID, FSG_BG, "%s",
                 "SAVE FAILED - see FILMSIM.LOG");
    else
        fsg_text(24, 386, FSG_T_SMALL, FSG_GREY, FSG_BG,
                 "%s", "PLAY saves the wheel you have open into this slot");

    footer("UP/DOWN slot    SET load    PLAY save",
           "Q: wheel/tone/fx/slots    MENU back");
}

void fsg_draw_creator(const fs_creator_t *c)
{
    char right[32];
    char value[32];
    int t;

    fs_creator_register_palette();

    fsg_fill(0, 0, FSG_W, FSG_H, FSG_BG);
    { char a[4], b[4];
      fs_pad_num(a, sizeof a, c->sel + 1, 2);
      fs_pad_num(b, sizeof b, FS_CREATOR_WEDGES, 2);
      snprintf(right, sizeof right, "WEDGE %s / %s", a, b); }
    header("COLOR CREATOR", c->stock, right);

    /* Three series tabs above the wheel */
    {
        int tx = 18;
        for (t = 0; t < 3; t++) {
            int w = 114;
            int on = (c->field == fs_creator_tab_field[t]);
            fsg_fill(tx, 52, w, 26, on ? FSG_RED_MID : FSG_PANEL_DARK);
            rect_outline(tx, 52, w, 26, on ? FSG_AMBER : FSG_LINE_DIM);
            fsg_fill(tx, 52, w, 3, on ? FSG_AMBER : FSG_LINE_DIM);
            fsg_text(tx + 12, 59, FSG_T_SMALL, on ? FSG_AMBER : FSG_GREY,
                     on ? FSG_RED_MID : FSG_PANEL_DARK, "%s", fs_creator_tabs[t]);
            tx += w + 6;
        }
    }

    /* Dodecagon radar wheel on the left */
    fsg_draw_creator_wheel(c, c->sel * 65536);

    /* ---- Right-hand inspector card ---- */
    {
        const int px = 392, py = 52, pw = 310, ph = 376;
        const int ix = px + 18, iw = pw - 36;
        int y = py + 14;
        static const char *const label[3] = {
            "SATURATION", "LUMINANCE", "HUE ROTATION"
        };
        static const int lo[3]  = { 0,   0,   -180 };
        static const int hi[3]  = { 200, 200,  180 };
        static const int mid[3] = { 100, 100,    0 };

        panel_card(px, py, pw, ph);

        /* Selected wedge swatch + title + degree badge */
        fsg_fill(ix, y + 2, 24, 24, CREATOR_SLOT(c->sel, CREATOR_BANDS - 1));
        rect_outline(ix, y + 2, 24, 24, FSG_WHITE);
        fsg_text(ix + 34, y + 1, FSG_T_LARGE, FSG_AMBER, FSG_PANEL, "%s",
                 fs_creator_wedge_name(c->sel));
        snprintf(value, sizeof value, "%d DEG", c->sel * 30);
        fsg_text(px + pw - 18 - (int)strlen(value) * 8, y + 8,
                 FSG_T_SMALL, FSG_GREY, FSG_PANEL, "%s", value);

        y += 36;
        fsg_fill(ix, y, iw, 1, FSG_LINE_DIM);
        y += 14;

        /* Three bipolar sliders in the exact same order as the top tabs */
        for (t = 0; t < 3; t++) {
            int fld = fs_creator_tab_field[t];
            int on  = (c->field == fld);
            int v   = (fld == 0) ? c->sat[c->sel]
                    : (fld == 1) ? c->rot[c->sel] : c->lum[c->sel];

            fsg_text(ix, y, FSG_T_SMALL, on ? FSG_AMBER : FSG_GREY,
                     FSG_PANEL, "%s", label[t]);
            snprintf(value, sizeof value, (fld == 1) ? "%+d DEG" : "%d%%", v);
            fsg_text(ix + iw - (int)strlen(value) * 8, y, FSG_T_SMALL,
                     on ? FSG_WHITE : FSG_GREY, FSG_PANEL, "%s", value);

            bar_bipolar(ix, y + 18, iw, 16, v, lo[t], hi[t], mid[t],
                        on ? FSG_AMBER : FSG_RED_MID, on);
            y += 56;
        }

        fsg_fill(ix, y, iw, 1, FSG_LINE_DIM);
        y += 12;

        /* Wedges edited counter + target slot */
        {
            int moved = 0, i;
            for (i = 0; i < FS_CREATOR_WEDGES; i++)
                if (c->sat[i] != 100 || c->rot[i] != 0 || c->lum[i] != 100)
                    moved++;
            fsg_text(ix, y, FSG_T_SMALL, FSG_GREY, FSG_PANEL, "WEDGES EDITED");
            snprintf(value, sizeof value, "%d / %d", moved, FS_CREATOR_WEDGES);
            fsg_text(ix + iw - (int)strlen(value) * 8, y, FSG_T_SMALL,
                     moved ? FSG_WHITE : FSG_GREEN, FSG_PANEL, "%s", value);
            y += 22;

            fsg_text(ix, y, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL, "RECIPE SLOT");
            snprintf(value, sizeof value, "ML/FILM/%s.fs",
                     c->slot_name ? c->slot_name : "WHEEL");
            fsg_text(ix + iw - (int)strlen(value) * 8, y, FSG_T_SMALL,
                     FSG_WHITE, FSG_PANEL, "%s", value);
            y += 22;

            /* The slot decides the file; the name is what the shelf calls it.
             * Both are on screen because getting this wrong is what made a
             * saved look impossible to find again. */
            fsg_text(ix, y, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL, "LOOK NAME");
            snprintf(value, sizeof value, "%s",
                     c->look_name ? c->look_name : "WHEEL");
            fsg_text(ix + iw - (int)strlen(value) * 8, y, FSG_T_SMALL,
                     FSG_GREEN, FSG_PANEL, "%s", value);
            y += 26;
        }

        /* Save status badge */
        if (c->save_err) {
            fsg_fill(ix, y, iw, 26, FSG_RED_DEEP);
            rect_outline(ix, y, iw, 26, FSG_RED_MID);
            fsg_text(ix + 10, y + 7, FSG_T_SMALL, FSG_WHITE, FSG_RED_DEEP,
                     "%s", "SAVE FAILED - see FILMSIM.LOG");
        } else if (c->saved) {
            char line[48];
            snprintf(line, sizeof line, "SAVED ML/FILM/%s.fs",
                     c->saved_name ? c->saved_name : "WHEEL");
            fsg_fill(ix, y, iw, 26, FSG_GREEN_DARK);
            rect_outline(ix, y, iw, 26, FSG_GREEN);
            fsg_text(ix + 10, y + 7, FSG_T_SMALL, FSG_GREEN, FSG_GREEN_DARK,
                     "%s", line);
        } else if (c->dirty) {
            fsg_fill(ix, y, iw, 26, FSG_RED_DEEP);
            rect_outline(ix, y, iw, 26, FSG_AMBER);
            fsg_text(ix + 10, y + 7, FSG_T_SMALL, FSG_AMBER, FSG_RED_DEEP,
                     "UNSAVED  -  PRESS PLAY TO SAVE");
        } else {
            fsg_fill(ix, y, iw, 26, FSG_BG_DEEP);
            rect_outline(ix, y, iw, 26, FSG_LINE_DIM);
            fsg_text(ix + 10, y + 7, FSG_T_SMALL, FSG_GREY_DIM, FSG_BG_DEEP,
                     "NEUTRAL PROFILE  (PLAY SAVES)");
        }
    }

    footer("DIAL wedge   UP/DOWN value   SET series   PLAY save",
           "Q tone   MENU back");
}

/* ---- the tone screen -------------------------------------------------- */

/* out level for an in level, the same arithmetic the tone screen draws */
static int tone_out(const fs_creator_t *c, int in)
{
    int t = in * 100 / 255;
    int out = in + (c->shadow * (100 - t) * (100 - t)) / 10000
                - (c->highlight * t * t)              / 10000;
    if (out < 0) out = 0;
    if (out > 255) out = 255;
    return out;
}

void fsg_draw_tone(const fs_creator_t *c)
{
    const int sx = 18, sy = 54, sw = 396, sh = 374;
    const int gx = sx + 24, gy = sy + 42, gw = sw - 48, gh = sh - 66;
    const int px = 428, py = 54, pw = 274, ph = 374;
    const int ix = px + 18, iw = pw - 36;
    char value[32];
    int i, x;

    fsg_fill(0, 0, FSG_W, FSG_H, FSG_BG);
    header("HIGHLIGHT & SHADOW", c->stock, "TONE CURVE");

    /* Left card: Oscilloscope Tone Curve Graph */
    panel_card(sx, sy, sw, sh);
    fsg_text(gx, sy + 14, FSG_T_MED, FSG_AMBER, FSG_PANEL, "RESPONSE CURVE");
    fsg_text(gx + gw - 104, sy + 16, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL,
             "5-PT SPLINE");

    fsg_fill(gx, gy, gw, gh, FSG_BG_DEEP);
    for (i = 1; i < 4; i++) {
        fsg_fill(gx + gw * i / 4, gy, 1, gh, FSG_LINE_DIM);
        fsg_fill(gx, gy + gh * i / 4, gw, 1, FSG_LINE_DIM);
    }
    rect_outline(gx, gy, gw, gh, FSG_LINE);

    /* Diagonal linear reference */
    for (x = 0; x < gw; x += 5)
        fsg_fill(gx + x, gy + gh - 1 - (x * (gh - 2)) / gw, 2, 1, FSG_LINE);

    /* Active tone curve (shadow stroke + 3px amber curve) */
    for (x = 0; x < gw; x++) {
        int in  = x * 255 / gw;
        int out = tone_out(c, in);
        int cy_pt = gy + gh - 2 - (out * (gh - 4)) / 255;
        fsg_fill(gx + x, cy_pt + 2, 1, 2, FSG_BLACK);
        fsg_fill(gx + x, cy_pt, 1, 3, FSG_AMBER);
    }

    /* Shadow and Highlight control knot handles on the curve */
    {
        int xs = gx + gw / 5;
        int ys = gy + gh - 2 - (tone_out(c, 51) * (gh - 4)) / 255;
        int xh = gx + (gw * 4) / 5;
        int yh = gy + gh - 2 - (tone_out(c, 204) * (gh - 4)) / 255;

        circle_fill(xs, ys, c->field == 0 ? 7 : 5,
                    c->field == 0 ? FSG_AMBER : FSG_GREY);
        circle_fill(xs, ys, 3, c->field == 0 ? FSG_WHITE : FSG_BG_DEEP);

        circle_fill(xh, yh, c->field == 1 ? 7 : 5,
                    c->field == 1 ? FSG_AMBER : FSG_GREY);
        circle_fill(xh, yh, 3, c->field == 1 ? FSG_WHITE : FSG_BG_DEEP);
    }

    /* Axis labels along bottom of scope (never crossed by the curve) */
    fsg_text(gx, gy + gh + 6, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL,
             "SHADOWS (0)");
    fsg_text(gx + gw - 110, gy + gh + 6, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL,
             "HIGHLIGHTS (255)");

    /* Right card: Shadow & Highlight Controls */
    panel_card(px, py, pw, ph);
    {
        int y = py + 16;
        fsg_text(ix, y, FSG_T_MED, FSG_WHITE, FSG_PANEL, "TONE CONTROLS");
        y += 28;
        fsg_fill(ix, y, iw, 1, FSG_LINE_DIM);
        y += 16;

        /* Shadow block */
        fsg_text(ix, y, FSG_T_SMALL, c->field == 0 ? FSG_AMBER : FSG_GREY,
                 FSG_PANEL, "SHADOW (TOE)");
        snprintf(value, sizeof value, "%+d", c->shadow);
        fsg_text(ix, y + 18, FSG_T_LARGE,
                 c->field == 0 ? FSG_AMBER : FSG_GREY_DIM, FSG_PANEL, "%s", value);
        bar_bipolar(ix, y + 50, iw, 16, c->shadow, -60, 60, 0,
                    c->field == 0 ? FSG_AMBER : FSG_RED_MID, c->field == 0);
        y += 84;

        /* Highlight block */
        fsg_text(ix, y, FSG_T_SMALL, c->field == 1 ? FSG_AMBER : FSG_GREY,
                 FSG_PANEL, "HIGHLIGHT (SHOULDER)");
        snprintf(value, sizeof value, "%+d", c->highlight);
        fsg_text(ix, y + 18, FSG_T_LARGE,
                 c->field == 1 ? FSG_AMBER : FSG_GREY_DIM, FSG_PANEL, "%s", value);
        bar_bipolar(ix, y + 50, iw, 16, c->highlight, -60, 60, 0,
                    c->field == 1 ? FSG_AMBER : FSG_RED_MID, c->field == 1);
        y += 82;

        fsg_fill(ix, y, iw, 1, FSG_LINE_DIM);
        y += 14;

        /* Output level endpoints */
        fsg_text(ix, y, FSG_T_SMALL, FSG_GREY, FSG_PANEL, "BLACK POINT");
        snprintf(value, sizeof value, "0 -> %d", tone_out(c, 0));
        fsg_text(ix + iw - (int)strlen(value) * 8, y, FSG_T_SMALL,
                 FSG_WHITE, FSG_PANEL, "%s", value);
        y += 22;

        fsg_text(ix, y, FSG_T_SMALL, FSG_GREY, FSG_PANEL, "WHITE POINT");
        snprintf(value, sizeof value, "255 -> %d", tone_out(c, 255));
        fsg_text(ix + iw - (int)strlen(value) * 8, y, FSG_T_SMALL,
                 FSG_WHITE, FSG_PANEL, "%s", value);
        y += 28;

        if (c->save_err) {
            snprintf(value, sizeof value, "%s", "SAVE FAILED - FILMSIM.LOG");
            fsg_fill(ix, y, iw, 26, FSG_RED_DEEP);
            rect_outline(ix, y, iw, 26, FSG_RED_MID);
            fsg_text(ix + 10, y + 7, FSG_T_SMALL, FSG_WHITE, FSG_RED_DEEP,
                     "%s", value);
        } else if (c->saved) {
            snprintf(value, sizeof value, "SAVED %s.fs",
                     c->saved_name ? c->saved_name : "WHEEL");
            fsg_fill(ix, y, iw, 26, FSG_GREEN_DARK);
            rect_outline(ix, y, iw, 26, FSG_GREEN);
            fsg_text(ix + 10, y + 7, FSG_T_SMALL, FSG_GREEN, FSG_GREEN_DARK,
                     "%s", value);
        } else if (c->dirty) {
            fsg_fill(ix, y, iw, 26, FSG_RED_DEEP);
            rect_outline(ix, y, iw, 26, FSG_AMBER);
            fsg_text(ix + 10, y + 7, FSG_T_SMALL, FSG_AMBER, FSG_RED_DEEP,
                     "UNSAVED  (PLAY SAVES)");
        } else {
            fsg_fill(ix, y, iw, 26, FSG_BG_DEEP);
            rect_outline(ix, y, iw, 26, FSG_LINE_DIM);
            fsg_text(ix + 10, y + 7, FSG_T_SMALL, FSG_GREY_DIM, FSG_BG_DEEP,
                     "LINEAR CURVE");
        }
    }

    footer("DIAL/UP/DOWN value   SET shadow/highlight   PLAY save",
           "Q wheel   MENU back");
}

/* ---- keys ------------------------------------------------------------- */

void fs_creator_defaults(fs_creator_t *c, const char *stock)
{
    int i;
    memset(c, 0, sizeof *c);
    for (i = 0; i < FS_CREATOR_WEDGES; i++) {
        c->sat[i] = 100;
        c->rot[i] = 0;
        c->lum[i] = 100;
    }
    c->screen = 0;
    c->slot_name = "WHEEL";
    c->look_name = "WHEEL";
    {
        static const char *files[FS_CREATOR_SLOTS] = {
            "WHEEL", "WHEEL2", "WHEEL3", "WHEEL4", "WHEEL5",
            "WHEEL6", "WHEEL7", "WHEEL8", "WHEEL9", "WHEEL10"
        };
        int k;
        for (k = 0; k < FS_CREATOR_SLOTS; k++) {
            c->slot_file[k] = files[k];
            c->slot_label[k] = NULL;
        }
    }
    c->slot_cur = 0;
    c->slot_pick = 0;
    c->stock = stock;
}

/*
 * Takes the module's own action codes (see fs_action_of_key in film_sim.c),
 * not raw key codes, so the same handler works for the preview and the body.
 */
enum { A_LEFT = 1, A_RIGHT, A_UP, A_DOWN, A_SET, A_Q };

void fs_creator_step(fs_creator_t *c, int act)
{
    switch (act) {
    case A_LEFT:
    case A_RIGHT:
        if (c->screen == 3) {
            int d = (act == A_RIGHT) ? 1 : (FS_CREATOR_SLOTS - 1);
            c->slot_pick = (c->slot_pick + d) % FS_CREATOR_SLOTS;
            break;
        }
        if (c->screen == 0) {
            int d = (act == A_RIGHT) ? 1 : (FS_CREATOR_WEDGES - 1);
            c->sel = (c->sel + d) % FS_CREATOR_WEDGES;
            break;
        }
        /* On the tone screen there is no wedge to select, so turning a dial
         * steps the active tone handle directly. */
        act = (act == A_RIGHT) ? A_UP : A_DOWN;
        /* fall through */
    case A_UP:
    case A_DOWN:
        if (c->screen == 3) {
            int step = (act == A_UP) ? 1 : -1;
            c->slot_pick = (c->slot_pick + step + FS_CREATOR_SLOTS) %
                           FS_CREATOR_SLOTS;
            break;
        }
        if (c->screen == 0) {
            int step = (act == A_UP) ? 1 : -1;
            if (c->field == 0) {
                c->sat[c->sel] += step * 10;
                if (c->sat[c->sel] < 0) c->sat[c->sel] = 0;
                if (c->sat[c->sel] > 200) c->sat[c->sel] = 200;
            } else if (c->field == 1) {
                c->rot[c->sel] += step * 15;
                if (c->rot[c->sel] < -180) c->rot[c->sel] = -180;
                if (c->rot[c->sel] > 180) c->rot[c->sel] = 180;
            } else {
                c->lum[c->sel] += step * 10;
                if (c->lum[c->sel] < 0) c->lum[c->sel] = 0;
                if (c->lum[c->sel] > 200) c->lum[c->sel] = 200;
            }
        } else if (c->screen == 2) {
            int step = (act == A_UP) ? 1 : -1;
            int *v = (c->field == 0) ? &c->grain
                   : (c->field == 1) ? &c->grain_size
                   : (c->field == 2) ? &c->halation
                   : (c->field == 3) ? &c->chrome : &c->fx_blue;
            int hi = (c->field >= 3) ? 2 : 100;
            *v += step * (c->field >= 3 ? 1 : 2);
            if (*v < 0) *v = 0;
            if (*v > hi) *v = hi;
        } else {
            if (c->field == 0) {
                c->shadow += (act == A_UP) ? 2 : -2;
                if (c->shadow < -60) c->shadow = -60;
                if (c->shadow > 60) c->shadow = 60;
            } else {
                c->highlight += (act == A_UP) ? 2 : -2;
                if (c->highlight < -60) c->highlight = -60;
                if (c->highlight > 60) c->highlight = 60;
            }
        }
        c->dirty = 1;
        c->saved = 0;
        break;
    case A_SET:
        if (c->screen == 3) {
            /* No file I/O from a key handler: raise the flag, the module's
             * loop does the read and the parse. */
            if (c->slot_pick != c->slot_cur)
                c->slot_request = 1;
            break;
        }
        if (c->screen == 0) {
            int t;
            for (t = 0; t < 3; t++)
                if (fs_creator_tab_field[t] == c->field) break;
            c->field = fs_creator_tab_field[(t + 1) % 3];
        } else if (c->screen == 2) {
            c->field = (c->field + 1) % 5;
        } else {
            c->field = (c->field + 1) % 2;
        }
        break;
    case A_Q:
        c->screen = (c->screen + 1) % 4;
        c->field = 0;
        break;
    default:
        break;
    }
}

/* ---- writing a recipe ------------------------------------------------ */

static int append(char *buf, int cap, int n, const char *fmt, ...)
{
    va_list ap;
    int wrote;
    if (n >= cap) return -1;
    va_start(ap, fmt);
    wrote = vsnprintf(buf + n, (size_t)(cap - n), fmt, ap);
    va_end(ap);
    if (wrote < 0 || n + wrote >= cap) return -1;
    return n + wrote;
}

int fs_creator_write(const fs_creator_t *c, char *buf, int cap, const char *name)
{
    static const int curve_in[5] = { 0, 64, 128, 192, 255 };
    int n = 0, i;
    char vec[160];
    int k;

#define PUT(...) do { n = append(buf, cap, n, __VA_ARGS__); if (n < 0) return -1; } while (0)

    PUT("VERSION   3\n");
    PUT("NAME      %s\n", name && name[0] ? name : "CUSTOM");
    PUT("DESC      MADE IN COLOR CREATOR\n");
    PUT("WB        AS_SHOT\n");

    /* tone: five points, the ends at the extremes */
    n = append(buf, cap, n, "CURVE     ");
    if (n < 0) return -1;
    for (k = 0; k < 5; k++)
        PUT("%s%d,%d", k ? " " : "", curve_in[k], tone_out(c, curve_in[k]));
    PUT("\n");
    /* the per-channel curves are identity: all the colour is in HUE_* */
    PUT("CURVE_R   0,0 128,128 255,255\n");
    PUT("CURVE_G   0,0 128,128 255,255\n");
    PUT("CURVE_B   0,0 128,128 255,255\n");

    /* the twelve wedges, as twelve values -- not six, so a bead every 30
     * degrees keeps its own knot */
    for (k = 0; k < 3; k++) {
        const char *key = (k == 0) ? "HUE_SAT" : (k == 1 ? "HUE_ROT" : "HUE_LUM");
        int len = 0;
        for (i = 0; i < FS_CREATOR_WEDGES; i++) {
            int v = (k == 0) ? c->sat[i] : (k == 1 ? c->rot[i] : c->lum[i]);
            len += snprintf(vec + len, sizeof vec - (size_t)len, "%s%d",
                            i ? "," : "", v);
        }
        /* A plain "%s %s", not a left-justified width: ML's vsnprintf has no
         * '-' flag and writes it literally, so the file came out as
         * "-9s 80,100,...". The parser
         * does not know a key called "-9s", so it dropped all three hue lines
         * and the saved look kept only its curve. HUE_SAT, HUE_ROT and
         * HUE_LUM are all seven characters, which is what the padding was
         * for. */
        PUT("%s %s\n", key, vec);
    }

    /* Nothing forced: what the FX screen says, and only that. GRAIN 8 and
     * HALATION 45 226 16 used to be written into every look, so a clean
     * portrait came back with CineStill's red halation around every highlight. */
    PUT("GRAIN     %d\n", c->grain);
    PUT("GRAIN_SIZE %d\n", c->grain_size);
    if (c->halation > 0)
        PUT("HALATION  %d 226 16\n", c->halation);
    else
        PUT("HALATION  0 0 0\n");
    if (c->chrome)
        PUT("COLOR_CHROME %s\n", c->chrome > 1 ? "STRONG" : "WEAK");
    if (c->fx_blue)
        PUT("FX_BLUE   %s\n", c->fx_blue > 1 ? "STRONG" : "WEAK");
#undef PUT
    return n;
}
