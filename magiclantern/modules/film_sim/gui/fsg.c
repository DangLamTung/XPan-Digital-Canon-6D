/*
 * fsg.c - drawing for the Film Sim shelf and darkroom screens.
 *
 * Only fsg_fill() and fsg_text() touch the panel, so this file is compiled
 * unchanged for the camera (gui/fsg_ml.c) and for the desktop preview
 * (gui/fsg_host.c). That is deliberate: what you see rendered
 * on a Mac is the same code that draws on the 6D, pixel for pixel.
 */
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#include "fsg.h"
#include "fs_num.h"

/* ---- small helpers ----------------------------------------------------- */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int absi(int v) { return v < 0 ? -v : v; }

/* The camera's overlay is a 256-entry palette, not RGB, so the background is
 * four flat bands rather than a gradient: every colour in this file has to be
 * one of the constants in fsg.h with an exact camera mapping. */
static void vgrad(int x, int y, int w, int h, int top, int bot)
{
  (void)top; (void)bot;
  int h1 = h / 4, h2 = h / 2;
  fsg_fill(x, y, w, h1, FSG_BG2);
  fsg_fill(x, y + h1, w, h2 - h1, FSG_BG);
  fsg_fill(x, y + h2, w, h - h2, FSG_BG_DEEP);
}

static void rect_outline(int x, int y, int w, int h, int rgb)
{
  fsg_fill(x, y, w, 1, rgb);
  fsg_fill(x, y + h - 1, w, 1, rgb);
  fsg_fill(x, y, 1, h, rgb);
  fsg_fill(x + w - 1, y, 1, h, rgb);
}

/* Four corner marks instead of rounded corners: 3 px is enough to read as a
 * deliberate frame and costs four fills. */
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

static void meter_bar(int x, int y, int w, int h, int pct, int max_pct, int fg)
{
  int fill_w;
  pct = clampi(pct, 0, max_pct);
  fill_w = max_pct > 0 ? ((w - 4) * pct) / max_pct : 0;
  fsg_fill(x, y, w, h, FSG_BG_DEEP);
  rect_outline(x, y, w, h, FSG_LINE_DIM);
  if (fill_w > 0)
    fsg_fill(x + 2, y + 2, fill_w, h - 4, fg);
}

int fsg_wrap(const char *text, char out[][48], int max_lines, int width)
{
  int line = 0, len = 0;
  if (!text || width < 8) return 0;
  out[0][0] = 0;
  for (const char *p = text; *p && line < max_lines; ) {
    while (*p == ' ') p++;
    const char *w = p;
    while (*p && *p != ' ') p++;
    int wl = (int)(p - w);
    if (wl > 47) wl = 47;
    if (len + (len ? 1 : 0) + wl > width) {
      if (len == 0) { /* single long word: hard break */
        for (int i = 0; i < wl && i < width; i++) out[line][i] = w[i];
        out[line][width] = 0;
        line++;
        if (line >= max_lines) break;
        len = 0;
        out[line][0] = 0;
        continue;
      }
      line++;
      if (line >= max_lines) break;
      len = 0;
    }
    if (len) out[line][len++] = ' ';
    for (int i = 0; i < wl && len < 47; i++) out[line][len++] = w[i];
    out[line][len] = 0;
  }
  return line + 1;
}

/* ---- shared chrome ----------------------------------------------------- */

static void top_bar(const char *title, const char *badge, const char *right,
                    unsigned tick)
{
  fsg_fill(0, 0, FSG_W, 44, FSG_RED_DEEP);
  fsg_fill(0, 0, 5, 44, FSG_AMBER);
  fsg_fill(0, 44, FSG_W, 2, FSG_RED);
  /* Smooth glowing highlight along the 2px bottom rule so it never collides
   * with the text background in rows 0..43. */
  {
    int sweep = (int)((tick / 4) % (unsigned)(FSG_W + 160)) - 160;
    int sx0 = clampi(sweep, 0, FSG_W);
    int sx1 = clampi(sweep + 140, 0, FSG_W);
    int cx0 = clampi(sweep + 40, 0, FSG_W);
    int cx1 = clampi(sweep + 100, 0, FSG_W);
    if (sx1 > sx0)
      fsg_fill(sx0, 44, sx1 - sx0, 2, FSG_RED_HI);
    if (cx1 > cx0)
      fsg_fill(cx0, 44, cx1 - cx0, 2, FSG_AMBER);
  }
  fsg_text(18, 7, FSG_T_MED, FSG_AMBER, FSG_RED_DEEP, "%s", title);
  if (badge && badge[0])
    fsg_text(18, 28, FSG_T_SMALL, FSG_GREY, FSG_RED_DEEP, "%s", badge);
  if (right && right[0]) {
    int rx = FSG_W - 20 - (int)strlen(right) * 11;
    if (rx < 440) rx = 440;
    fsg_text(rx, 13, FSG_T_MED, FSG_WHITE, FSG_RED_DEEP, "%s", right);
  }
}

static void hint_row(const char *l, const char *r)
{
  fsg_fill(0, 448, FSG_W, 32, FSG_BG_DEEP);
  fsg_fill(0, 448, FSG_W, 1, FSG_LINE_DIM);
  if (l) fsg_text(20, 456, FSG_T_SMALL, FSG_GREY, FSG_BG_DEEP, "%s", l);
  if (r) {
    int rx = FSG_W - 20 - (int)strlen(r) * 8;
    if (rx < 420) rx = 420;
    fsg_text(rx, 456, FSG_T_SMALL, FSG_AMBER, FSG_BG_DEEP, "%s", r);
  }
}

/* The palette-index blitter (below, with the picture code) is also what draws
 * the strip's thumbnails, so it is declared here. */
static void blit(int x, int y, const unsigned char *px, int w, int h);
static void blit_scaled(int x, int y, const unsigned char *px,
                        int sw, int sh, int dw, int dh);

/* ---- the film strip (M-Film style perspective carousel) ---------------- */

#define STRIP_Y     FSG_STRIP_Y
#define STRIP_H     FSG_STRIP_H
#define FRAME_PITCH 108

static void perf_row(int y, int phase)
{
  int m = phase % 24;
  if (m < 0) m += 24;
  for (int x = -24 + m; x < FSG_W + 24; x += 24) {
    if (x < 2 || x + 13 > FSG_W - 2) continue;
    fsg_fill(x, y, 13, 7, FSG_BG_DEEP);
    fsg_fill(x + 1, y + 1, 11, 5, FSG_PERF);
    fsg_fill(x + 1, y + 6, 11, 1, FSG_PERF_HI);
  }
}

/* Pick a mapped accent colour for the mini-canister icon inside the strip
 * (safe even when borrowed palette entries 80..254 hold FILMCAN.DAT). */
static int strip_can_accent(const char *lbl, int idx)
{
  if (!lbl) return FSG_AMBER;
  if (!strncmp(lbl, "Velvia", 6) || !strncmp(lbl, "Superia", 7) ||
      !strncmp(lbl, "Pro ", 4)   || !strncmp(lbl, "C200", 4) ||
      !strncmp(lbl, "Provia", 6) || !strncmp(lbl, "Reala", 5))
    return FSG_GREEN;
  if (!strncmp(lbl, "CineStill", 9) || !strncmp(lbl, "Ektar", 5) ||
      !strncmp(lbl, "Redscale", 8)  || !strncmp(lbl, "Foma", 4))
    return FSG_RED;
  if (!strncmp(lbl, "HP5", 3)   || !strncmp(lbl, "Tri-X", 5) ||
      !strncmp(lbl, "Delta", 5) || !strncmp(lbl, "Acros", 5) ||
      !strncmp(lbl, "T-Max", 5) || !strncmp(lbl, "Pan F", 5))
    return FSG_GREY;
  return (idx & 1) ? FSG_AMBER : FSG_RED_HI;
}

static void mini_can_icon(int x, int y, int h, int accent, int active)
{
  int cw = 16;
  int ch = clampi(h - 14, 24, 36);
  int cy = y + (h - ch) / 2;

  /* Drop shadow + spool cap + cylinder body */
  fsg_fill(x + 2, cy + 2, cw, ch, FSG_BLACK);
  fsg_fill(x + 4, cy - 3, cw - 8, 3, active ? FSG_WHITE : FSG_GREY);
  fsg_fill(x, cy, cw, 3, active ? FSG_GREY : FSG_GREY_DIM);
  fsg_fill(x, cy + 3, cw, ch - 5, FSG_BG_DEEP);
  /* Label wrap + highlight edge */
  fsg_fill(x + 2, cy + 6, cw - 4, ch - 11, accent);
  fsg_fill(x + 2, cy + 6, 2, ch - 11, active ? FSG_WHITE : FSG_GREY);
  fsg_fill(x + 5, cy + 10, cw - 7, ch - 18, active ? FSG_WHITE : FSG_GREY);
  /* Bottom rim */
  fsg_fill(x, cy + ch - 2, cw, 2, FSG_GREY_DIM);
}

static void film_strip(const fsg_shelf_t *s)
{
  int n = s->count;
  if (n <= 0) return;
  if (n > FSG_MAX_LABELS) n = FSG_MAX_LABELS;

  /* Self-clearing 94-row strip band (rows 352..445) */
  fsg_fill(0, STRIP_Y, FSG_W, STRIP_H, FSG_BG_DEEP);
  fsg_fill(0, STRIP_Y + 2, FSG_W, STRIP_H - 4, FSG_BG2);
  fsg_fill(0, STRIP_Y, FSG_W, 2, FSG_LINE);
  fsg_fill(0, STRIP_Y + STRIP_H - 2, FSG_W, 2, FSG_LINE);

  /* Sprocket tracks move in lockstep with s->scroll */
  int phase = -(s->scroll * FRAME_PITCH) / 256;
  perf_row(STRIP_Y + 4, phase);
  perf_row(STRIP_Y + STRIP_H - 11, phase);

  /* Center gate spotlight backdrop */
  const int cx = FSG_W / 2;
  fsg_fill(cx - 58, STRIP_Y + 12, 116, STRIP_H - 24, FSG_PANEL);
  fsg_fill(cx - 58, STRIP_Y + 12, 1, STRIP_H - 24, FSG_LINE_DIM);
  fsg_fill(cx + 57, STRIP_Y + 12, 1, STRIP_H - 24, FSG_LINE_DIM);

  int scr = s->scroll; /* 1/256 frame units */

  for (int i = 0; i < n; i++) {
    /* Continuous signed distance from the center gate in 1/256 stock units */
    int d = i * 256 - scr;
    int ad = absi(d);

    /* M-Film pose_of() non-linear carousel spread:
     * wider spacing near the gate (112 px), tighter on the wings (94 px) */
    int dx;
    if (ad <= 256) {
      dx = (d * 112) / 256;
    } else {
      int sign = d < 0 ? -1 : 1;
      dx = sign * (112 + ((ad - 256) * 94) / 256);
    }

    /* Continuous 2-stage perspective scale & elevation (M-Film pose_of) */
    int fw, fh;
    if (ad <= 256) {
      int p1 = 256 - ad;                  /* 256 at gate -> 0 at 1 stock */
      fw = 90 + (p1 * 16) / 256;          /* 90 px -> 106 px */
      fh = 56 + (p1 * 12) / 256;          /* 56 px ->  68 px */
    } else {
      int p2 = clampi(512 - ad, 0, 256);  /* 256 at 1 stock -> 0 at 2+ stocks */
      fw = 78 + (p2 * 12) / 256;          /* 78 px ->  90 px */
      fh = 46 + (p2 * 10) / 256;          /* 46 px ->  56 px */
    }
    int x  = cx + dx - fw / 2;
    int y  = STRIP_Y + (STRIP_H - fh) / 2;

    if (x + fw < 0 || x > FSG_W) continue;

    int in_gate = (ad < 96);
    int near_gate = (ad < 320);
    int body = in_gate ? FSG_PANEL_HI : (near_gate ? FSG_PANEL : FSG_PANEL_DARK);
    int border = in_gate ? FSG_AMBER : (near_gate ? FSG_LINE : FSG_LINE_DIM);

    /* Contact drop shadow + frame card */
    fsg_fill(x + 2, y + 3, fw, fh, FSG_BLACK);
    fsg_fill(x, y, fw, fh, body);
    rect_outline(x, y, fw, fh, border);
    if (in_gate) {
      /* Lit top bevel + floor reflection bar under the center gate */
      fsg_fill(x + 1, y + 1, fw - 2, 2, FSG_AMBER);
      fsg_fill(x + 8, y + fh + 2, fw - 16, 2, FSG_AMBER_DIM);
    }

    int tw = 0, th = 0;
    int ai = s->art[i];
    const unsigned char *tp =
        (fsg_can_ready() && ai >= 0) ? fsg_can_thumb(ai, &tw, &th) : NULL;

    if (tp && tw > 0 && th > 0) {
      int dh = clampi(fh - 8, 28, th);
      int dw = (tw * dh) / th;
      if (dh == th && dw == tw)
        blit(x + (fw - tw) / 2, y + (fh - th) / 2, tp, tw, th);
      else
        blit_scaled(x + (fw - dw) / 2, y + (fh - dh) / 2, tp, tw, th, dw, dh);
      char num[8];
      fs_pad_num(num, sizeof num, i + 1, 2);
      fsg_text(x + 5, y + fh - 15, FSG_T_SMALL,
               in_gate ? FSG_AMBER : FSG_GREY_DIM, body, "%s", num);
    } else {
      const char *lbl = s->labels[i] ? s->labels[i] : "";
      int accent = strip_can_accent(lbl, i);
      mini_can_icon(x + 5, y + 1, fh, accent, in_gate);

      char line1[10], line2[8];
      {
        const char *sp = strchr(lbl, ' ');
        int l1 = sp ? (int)(sp - lbl) : (int)strlen(lbl);
        if (l1 > 8) l1 = 8;
        memcpy(line1, lbl, (size_t)l1);
        line1[l1] = 0;
        if (sp && sp[1]) {
          snprintf(line2, sizeof line2, "%s", sp + 1);
        } else {
          line2[0] = 0;
        }
      }
      fsg_text(x + 24, y + 6, FSG_T_SMALL,
               in_gate ? FSG_AMBER : (near_gate ? FSG_WHITE : FSG_GREY),
               body, "%s", line1);
      if (line2[0] && fh >= 50)
        fsg_text(x + 24, y + 20, FSG_T_SMALL,
                 in_gate ? FSG_WHITE : FSG_GREY_DIM, body, "%s", line2);

      char num[8];
      fs_pad_num(num, sizeof num, i + 1, 2);
      fsg_text(x + fw - 20, y + fh - 15, FSG_T_SMALL,
               in_gate ? FSG_AMBER : FSG_GREY_DIM, body, "%s", num);
    }
  }

  /* Fixed cinema gate markers above and below the center aperture */
  fsg_fill(cx - 10, STRIP_Y + 2, 20, 3, FSG_AMBER);
  fsg_fill(cx - 10, STRIP_Y + STRIP_H - 5, 20, 3, FSG_AMBER);
}

/* ---- the film's own colours -------------------------------------------- */

static int fs_rgb(int r, int g, int b) {
  r = r < 0 ? 0 : r > 255 ? 255 : r;
  g = g < 0 ? 0 : g > 255 ? 255 : g;
  b = b < 0 ? 0 : b > 255 ? 255 : b;
  return (r << 16) | (g << 8) | b;
}

static int fs_luma(int c) {
  return (77 * ((c >> 16) & 255) + 150 * ((c >> 8) & 255) + 29 * (c & 255)) >> 8;
}

static int fs_bright(int c, int pct) {
  return fs_rgb(((c >> 16) & 255) * pct / 100, ((c >> 8) & 255) * pct / 100,
                (c & 255) * pct / 100);
}

static int fs_cast(int c, int dr, int dg, int db) {
  return fs_rgb(((c >> 16) & 255) + dr, ((c >> 8) & 255) + dg,
                (c & 255) + db);
}

static int fs_saturate(int c, int pct) {
  int l = fs_luma(c);
  int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
  return fs_rgb(l + (r - l) * pct / 100, l + (g - l) * pct / 100,
                l + (b - l) * pct / 100);
}

static int fs_brand_colour(const char *brand) {
  if (!brand) return 0x8C8C99;
  if (!strncmp(brand, "Kodak", 5))       return 0xF0B32C;   /* yellow box */
  if (!strncmp(brand, "Fuji", 4))        return 0x1E8B4F;   /* green box  */
  if (!strncmp(brand, "Ilford", 6))      return 0xDCD6C6;   /* cream box  */
  if (!strncmp(brand, "CineStill", 9))   return 0xC0392B;   /* red can    */
  if (!strncmp(brand, "Lomography", 10)) return 0xE4572E;   /* orange     */
  if (!strncmp(brand, "Foma", 4) ||
      !strncmp(brand, "FOMA", 4))        return 0x2B6CB0;   /* blue       */
  if (!strncmp(brand, "Agfa", 4))        return 0xD64527;   /* agfa red   */
  if (!strncmp(brand, "Process", 7))     return 0x2AA198;   /* teal       */
  return 0x8C8C99;
}

void fsg_stock_palette(const fsg_film_t *f, int out[FSG_NSLOT]) {
  int brand = fs_brand_colour(f->brand);
  int speed = clampi(f->iso, 50, 3200);
  int kelvin = clampi(f->kelvin, 4000, 6800);

  int body_l = 100 - (speed - 50) * 45 / 3150;      /* 100 % at 50 ASA */
  int warm = (5200 - kelvin) * 100 / 1300;          /* + warm, - cool  */
  int gm = clampi(f->wbs_gm, -9, 9) * 12;

  int body = fs_bright(f->mono ? 0x9A9A9A : brand, body_l);
  body = fs_cast(body, warm / 6, gm / 7, -warm / 6);
  if (!f->mono)
    body = fs_saturate(body, 100 + clampi(f->saturation, -3, 3) * 14);

  int paper = fs_cast(0xF2EDE0, warm / 12, gm / 14, -warm / 12);
  int accent = f->mono ? fs_saturate(brand, 60) : fs_saturate(brand, 120);

  out[FSG_SL_BORDER]   = fs_bright(body, 20);
  out[FSG_SL_BODY_HI]  = fs_bright(body, 138);
  out[FSG_SL_BODY]     = body;
  out[FSG_SL_BODY_LO]  = fs_bright(body, 58);
  out[FSG_SL_CAP_HI]   = fs_cast(0xD9D9DD, warm / 10, gm / 12, -warm / 10);
  out[FSG_SL_CAP_LO]   = 0x8E8E96;
  out[FSG_SL_PAPER]    = paper;
  out[FSG_SL_INK]      = 0x14141A;
  out[FSG_SL_ACCENT]   = accent;
  out[FSG_SL_TONGUE]   = fs_bright(body, 42);
  out[FSG_SL_SHADOW]   = f->mono ? 0x18181A : fs_bright(fs_cast(body, -warm / 10, gm / 10, warm / 10), 52);
  out[FSG_SL_MID]      = f->mono ? 0x6E6E70 : body;
  out[FSG_SL_LIGHT]    = f->mono ? 0xD8D8DA : fs_saturate(fs_cast(paper, warm / 8, -gm / 10, -warm / 8), 70);
  out[FSG_SL_SPROCKET] = fs_bright(body, 28);
  out[FSG_SL_GLOW]     = fs_bright(fs_cast(paper, warm / 6, 0, -warm / 6), 96);
  out[FSG_SL_EXTRA]    = 0x101016;
}

/* ---- the stock picture ------------------------------------------------- */

static fsg_img_t cur_img;

void fsg_img_set(const unsigned char *px, int w, int h)
{
  cur_img.px = px;
  cur_img.w = px ? w : 0;
  cur_img.h = px ? h : 0;
}

static void blit(int x, int y, const unsigned char *px, int w, int h)
{
  if (!px || w <= 0 || h <= 0) return;
  for (int yy = 0; yy < h; yy++) {
    const unsigned char *row = px + yy * w;
    int sx = 0;
    while (sx < w) {
      int v = row[sx], run = 1;
      while (sx + run < w && row[sx + run] == v) run++;
      if (v != 255) fsg_fill(x + sx, y + yy, run, 1, FSG_PIX(v));
      sx += run;
    }
  }
}

static void blit_scaled(int x, int y, const unsigned char *px,
                        int sw, int sh, int dw, int dh)
{
  if (!px || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
  for (int yy = 0; yy < dh; yy++) {
    const unsigned char *row = px + (yy * sh / dh) * sw;
    /*
     * (sx * sw) / dw was two software divisions per pixel of a film strip,
     * seven strips a frame. Same mapping as a fixed-point step: no divide in
     * the loop, and the source index still advances by one per source pixel.
     */
    const unsigned int step = ((unsigned)sw << 16) / (unsigned)dw;
    int sx = 0;
    while (sx < dw) {
      int v = row[((unsigned)sx * step) >> 16], run = 1;
      while (sx + run < dw && row[((unsigned)(sx + run) * step) >> 16] == v) run++;
      if (v != 255) fsg_fill(x + sx, y + yy, run, 1, FSG_PIX(v));
      sx += run;
    }
  }
}

static void draw_picture(int x, int y)
{
  blit(x, y, cur_img.px, cur_img.w, cur_img.h);
}

/* ---- 3D cylinder-shaded 35 mm cassette fallback ------------------------ */

static void slot_box(int x, int y, int w, int h, int slot)
{
  fsg_fill(x, y, w, h, FSG_SLOT(slot));
}

/*
 * Full-scale cylinder-shaded 35 mm cassette in the film's own 16 palette slots.
 * Used whenever a stock has no bitmap in FILMCAN.DAT (e.g. imported LUT stocks
 * or custom user recipes), so the mount always displays a rich 3D canister.
 */
static void cassette_image(const fsg_shelf_t *s, int x, int y)
{
  const int cw = 110;     /* canister cylinder width */
  const int cap = 14;     /* top crimped rim + spool */
  const int bh = 166;     /* cylinder body height    */
  const int by = y + cap;

  /* Floor contact shadow + subtle floor reflection */
  fsg_fill(x + 6, y + 8, cw + 4, cap + bh, FSG_BLACK);
  fsg_fill(x + 12, by + bh + 6, cw - 24, 3, FSG_SLOT(FSG_SL_BODY_LO));
  fsg_fill(x + 24, by + bh + 10, cw - 48, 2, FSG_SLOT(FSG_SL_BORDER));

  /* Spool spindle neck at the top */
  slot_box(x + 32, y - 8, 46, 9, FSG_SL_CAP_LO);
  slot_box(x + 36, y - 8, 12, 9, FSG_SL_CAP_HI);
  fsg_fill(x + 42, y - 6, 26, 4, FSG_BLACK);

  /* Top crimped metal cap with cylindrical specular highlight */
  slot_box(x - 2, y, cw + 4, cap, FSG_SL_CAP_LO);
  slot_box(x + 8, y, 24, cap - 3, FSG_SL_CAP_HI);
  slot_box(x - 2, y + cap - 3, cw + 4, 3, FSG_SL_BORDER);

  /* Cylindrical body with 5-band shading (border -> hi -> body -> lo -> border) */
  slot_box(x, by, cw, bh, FSG_SL_BODY);
  slot_box(x, by, 4, bh, FSG_SL_BORDER);
  slot_box(x + 4, by, 8, bh, FSG_SL_BODY_LO);
  slot_box(x + 12, by, 18, bh, FSG_SL_BODY_HI);
  slot_box(x + cw - 22, by, 16, bh, FSG_SL_BODY_LO);
  slot_box(x + cw - 6, by, 6, bh, FSG_SL_BORDER);

  /*DX barcode silver/black conductive patches on left side of cylinder */
  for (int i = 0; i < 6; i++) {
    int dy = by + 18 + i * 20;
    slot_box(x + 5, dy, 12, 16, (i & 1) ? FSG_SL_CAP_HI : FSG_SL_BORDER);
  }

  /* Printed wrap-around paper label */
  {
    int lx = x + 22, ly = by + 14, lw = cw - 32, lh = bh - 30;
    slot_box(lx, ly, lw, lh, FSG_SL_PAPER);
    /* Specular highlight strip on the paper wrap */
    slot_box(lx, ly, 8, lh, FSG_SL_GLOW);
    /* Brand colour header band */
    slot_box(lx, ly, lw, 24, FSG_SL_ACCENT);
    fsg_text(lx + 6, ly + 5, FSG_T_SMALL, FSG_SLOT(FSG_SL_PAPER),
             FSG_SLOT(FSG_SL_ACCENT), "%s",
             s->stock.brand ? s->stock.brand : "FILM");

    /* Emulsion code + large ISO speed + format badge */
    fsg_text(lx + 6, ly + 32, FSG_T_MED, FSG_SLOT(FSG_SL_INK),
             FSG_SLOT(FSG_SL_PAPER), "%s",
             s->stock.code ? s->stock.code : "");
    fsg_text(lx + 6, ly + 58, FSG_T_LARGE, FSG_SLOT(FSG_SL_INK),
             FSG_SLOT(FSG_SL_PAPER), "%d", s->stock.iso);
    slot_box(lx + 6, ly + 94, lw - 12, 2, FSG_SL_ACCENT);
    fsg_text(lx + 6, ly + 102, FSG_T_SMALL, FSG_SLOT(FSG_SL_INK),
             FSG_SLOT(FSG_SL_PAPER), "35mm 36EXP");
  }

  /* Bottom crimped metal rim */
  slot_box(x - 2, by + bh - 6, cw + 4, 6, FSG_SL_CAP_LO);
  slot_box(x + 8, by + bh - 6, 24, 4, FSG_SL_CAP_HI);
  slot_box(x - 2, by + bh - 1, cw + 4, 1, FSG_SL_BORDER);

  /* Film leader tongue emerging from the light trap on the right */
  {
    int tx = x + cw - 2, ty = by + bh - 56, tw = 34, th = 36;
    fsg_fill(tx + 3, ty + 3, tw, th, FSG_BLACK);
    slot_box(tx, ty, tw, th, FSG_SL_TONGUE);
    slot_box(tx, ty + 6, tw - 4, 3, FSG_SL_GLOW);
    for (int i = 0; i < 3; i++) {
      slot_box(tx + 4 + i * 10, ty + 2, 6, 4, FSG_SL_SPROCKET);
      slot_box(tx + 4 + i * 10, ty + th - 6, 6, 4, FSG_SL_SPROCKET);
    }
  }
}

#define PICT_W FSG_CAN_MAXW
#define PICT_H FSG_CAN_MAXH

static void shelf_picture(const fsg_shelf_t *s, int x, int y)
{
  const int w = PICT_W, h = PICT_H;

  fsg_fill(x + 4, y + 4, w, h, FSG_BLACK);
  fsg_fill(x, y, w, h, FSG_BG_DEEP);
  /* Subtle vertical stage backdrop behind the canister */
  fsg_fill(x + 18, y + 6, w - 36, h - 12, FSG_PANEL_DARK);

  if (cur_img.px) {
    draw_picture(x + (w - cur_img.w) / 2, y + (h - cur_img.h) / 2);
  } else {
    cassette_image(s, x + 16, y + 28);
  }

  rect_outline(x, y, w, h, FSG_LINE);
  corners(x, y, w, h, FSG_AMBER, 14);
}

/* ---- shelf ------------------------------------------------------------- */

void fsg_draw_shelf(const fsg_shelf_t *s)
{
  vgrad(0, 0, FSG_W, FSG_H, 0, 0);
  top_bar(s->title && s->title[0] ? s->title : "FILM SHELF", s->badge,
          s->counter, s->tick);

  /* Main content card (rows 54..341) */
  const int px = 20, py = 54, pw = 680, ph = 288;
  fsg_fill(px + 3, py + 3, pw, ph, FSG_BLACK);
  fsg_fill(px, py, pw, ph, FSG_PANEL);
  rect_outline(px, py, pw, ph, FSG_LINE);
  corners(px, py, pw, ph, FSG_AMBER_DIM, 16);

  /* Column dividers: Left (Identity & Look), Middle (Telemetry), Right (Canister) */
  const int div1 = 336, div2 = 502;
  fsg_fill(div1, py + 14, 1, ph - 28, FSG_LINE_DIM);
  fsg_fill(div2, py + 14, 1, ph - 28, FSG_LINE_DIM);

  /* ---- Left Column: Emulsion Identity, Description & Response Strip ---- */
  {
    const int lx = px + 18, lw = div1 - lx - 14;
    fsg_text(lx, py + 12, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL, "EMULSION");

    /* ISO pill in top-right of left column */
    {
      char iso_buf[16];
      snprintf(iso_buf, sizeof iso_buf, "ISO %d", s->stock.iso);
      int bw = (int)strlen(iso_buf) * 8 + 14;
      int bx = div1 - 14 - bw;
      fsg_fill(bx, py + 10, bw, 18, FSG_BG_DEEP);
      rect_outline(bx, py + 10, bw, 18, FSG_AMBER_DIM);
      fsg_text(bx + 7, py + 13, FSG_T_SMALL, FSG_AMBER, FSG_BG_DEEP, "%s", iso_buf);
    }

    fsg_text(lx, py + 30, FSG_T_LARGE, FSG_AMBER, FSG_PANEL, "%s",
             s->stock.name ? s->stock.name : "-");
    fsg_text(lx, py + 62, FSG_T_SMALL, FSG_WHITE, FSG_PANEL, "%s  -  %s",
             s->stock.brand ? s->stock.brand : "",
             s->stock.kind ? s->stock.kind : "");

    if (s->photo && s->photo[0]) {
      fsg_fill(lx, py + 82, lw, 20, FSG_RED_DEEP);
      rect_outline(lx, py + 82, lw, 20, FSG_AMBER_DIM);
      fsg_text(lx + 8, py + 86, FSG_T_SMALL, FSG_AMBER, FSG_RED_DEEP,
               "FRAME: %s", s->photo);
    } else {
      fsg_text(lx, py + 84, FSG_T_SMALL, FSG_GREY, FSG_PANEL,
               "FORMAT: 135 FILM   PROCESS: %s",
               s->stock.soup ? s->stock.soup : "C-41");
    }

    fsg_fill(lx, py + 108, lw, 1, FSG_LINE_DIM);
    fsg_text(lx, py + 116, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL, "CHARACTER & RESPONSE");
    {
      char lines[4][48];
      int l = fsg_wrap(s->stock.desc ? s->stock.desc : "", lines, 4, lw / 8);
      for (int i = 0; i < l; i++)
        fsg_text(lx, py + 134 + i * 19, FSG_T_SMALL, FSG_WHITE, FSG_PANEL,
                 "%s", lines[i]);
    }

    /* Bottom of Left Column: 6-Step Tone / Chromatic Latitude Ladder */
    {
      int ty = py + 216;
      int sw = (lw - 15) / 6;
      fsg_text(lx, ty, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL,
               "TONE LATITUDE (SHADOW -> HIGHLIGHT)");
      ty += 16;
      if (!cur_img.px) {
        /* 16-slot stock palette is active: show the stock's actual tonal swatches */
        static const int slots[6] = {
          FSG_SL_BORDER, FSG_SL_SHADOW, FSG_SL_TONGUE,
          FSG_SL_MID,    FSG_SL_LIGHT,  FSG_SL_PAPER
        };
        for (int i = 0; i < 6; i++) {
          int sx = lx + i * (sw + 3);
          slot_box(sx, ty, sw, 26, slots[i]);
          rect_outline(sx, ty, sw, 26, FSG_LINE);
        }
      } else {
        /* Canister photograph owns slots 80..254: draw exposure step wedge */
        static const int greys[6] = {
          FSG_BG_DEEP, FSG_PANEL_DARK, FSG_LINE_DIM,
          FSG_GREY_DIM, FSG_GREY, FSG_WHITE
        };
        for (int i = 0; i < 6; i++) {
          int sx = lx + i * (sw + 3);
          fsg_fill(sx, ty, sw, 26, greys[i]);
          rect_outline(sx, ty, sw, 26, i == 3 ? FSG_AMBER : FSG_LINE);
        }
      }
      fsg_text(lx, ty + 30, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL,
               "-2 EV                    0 EV                   +2 EV");
    }
  }

  /* ---- Middle Column: Grain & Strength Meters + Recipe Telemetry ------- */
  {
    const int mx = div1 + 14, mw = div2 - mx - 14;
    int y = py + 12;

    fsg_text(mx, y, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL, "GRAIN");
    fsg_text(mx + mw - 36, y, FSG_T_SMALL, FSG_WHITE, FSG_PANEL, "%d%%", s->grain);
    meter_bar(mx, y + 16, mw, 12, s->grain, 40, FSG_AMBER);
    y += 40;

    fsg_text(mx, y, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL, "STRENGTH");
    fsg_text(mx + mw - 42, y, FSG_T_SMALL, FSG_WHITE, FSG_PANEL, "%d%%", s->strength);
    meter_bar(mx, y + 16, mw, 12, s->strength, 100, FSG_GREEN);
    y += 42;

    fsg_text(mx, y, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL, "ROLL SLOT");
    {
      char a[4], b[4];
      fs_pad_num(a, sizeof a, s->sel + 1, 2);
      fs_pad_num(b, sizeof b, s->count, 2);
      fsg_text(mx, y + 16, FSG_T_MED, FSG_AMBER, FSG_PANEL, "%s / %s", a, b);
    }
    y += 44;

    /* Recessed Recipe Box */
    fsg_fill(mx, y, mw, 86, FSG_BG_DEEP);
    rect_outline(mx, y, mw, 86, FSG_LINE_DIM);
    fsg_text(mx + 8, y + 8, FSG_T_SMALL, FSG_AMBER, FSG_BG_DEEP, "RECIPE");
    fsg_text(mx + 8, y + 26, FSG_T_SMALL, FSG_WHITE, FSG_BG_DEEP, "WB   %dK",
             s->stock.kelvin);
    fsg_text(mx + 8, y + 44, FSG_T_SMALL, FSG_WHITE, FSG_BG_DEEP,
             "G/M  %+d   SAT %+d", s->stock.wbs_gm, s->stock.saturation);
    fsg_text(mx + 8, y + 64, FSG_T_SMALL, FSG_GREY, FSG_BG_DEEP, "%s",
             s->stock.soup ? s->stock.soup : "Standard C-41");
    y += 98;

    /* Active status pill */
    fsg_fill(mx, y, mw, 24, FSG_GREEN_DARK);
    rect_outline(mx, y, mw, 24, FSG_GREEN);
    fsg_text(mx + 10, y + 6, FSG_T_SMALL, FSG_GREEN, FSG_GREEN_DARK,
             s->photo && s->photo[0] ? "SET = DEVELOP" : "ACTIVE LOOK");
  }

  /* ---- Right Column: Canister Slide Mount ------------------------------ */
  {
    const int rx = div2 + 14;
    fsg_text(rx, py + 12, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL,
             "CANISTER MOUNT");
    shelf_picture(s, rx, py + 30);
  }

  film_strip(s);
  hint_row(s->hint_l ? s->hint_l
                     : "DIAL/ARROWS = film    Q = soup    SET = apply",
           s->hint_r ? s->hint_r : "MENU = exit");
}

/* ---- the two bands that move on their own ------------------------------ */

void fsg_draw_shelf_header(const fsg_shelf_t *s)
{
  top_bar(s->title && s->title[0] ? s->title : "FILM SHELF", s->badge,
          s->counter, s->tick);
}

void fsg_draw_shelf_strip(const fsg_shelf_t *s)
{
  film_strip(s);
}

/* ---- darkroom chemical bath screen ------------------------------------- */

void fsg_draw_develop(const fsg_dev_t *d)
{
  vgrad(0, 0, FSG_W, FSG_H, 0, 0);

  char right[24];
  if (d->total > 0) snprintf(right, sizeof right, "%d / %d", d->index, d->total);
  else snprintf(right, sizeof right, "SINGLE");
  top_bar("DARKROOM", d->result ? "FINISHED" : "DEVELOPING", right, d->tick);

  /* --- Left Card: Chemical Developing Tank ---------------------------- */
  const int tx = 20, ty = 56, tw = 304, th = 300;
  fsg_fill(tx + 3, ty + 3, tw, th, FSG_BLACK);
  fsg_fill(tx, ty, tw, th, FSG_PANEL_DARK);
  rect_outline(tx, ty, tw, th, FSG_LINE);
  corners(tx, ty, tw, th, FSG_AMBER_DIM, 14);

  int pct = clampi(d->pct, 0, 100);
  int inner_h = th - 16;
  int liquid = (inner_h * pct) / 100;
  int ly = ty + 8 + (inner_h - liquid);
  if (liquid > 0) {
    fsg_fill(tx + 8, ly, tw - 16, liquid, d->done ? FSG_GREEN_DARK : FSG_RED_DEEP);
    /* Meniscus surface line + lighter wave skin under it */
    fsg_fill(tx + 8, ly, tw - 16, 2, d->done ? FSG_GREEN : FSG_BUBBLE_HI);
    fsg_fill(tx + 8, ly + 2, tw - 16, 6, d->done ? FSG_GREEN_DARK : FSG_RED_MID);
    /* Rising agitation bubbles */
    for (int i = 0; i < 14; i++) {
      int bx = tx + 14 + ((i * 71 + 23) % (tw - 28));
      int span = liquid > 8 ? liquid - 8 : 1;
      int by = ly + 8 + (int)(((d->tick / 3 + (unsigned)(i * 41)) % (unsigned)span));
      if (by > ty + 8 + inner_h - 6) continue;
      int r = 2 + (i % 3);
      fsg_fill(bx, by, r, r, (i & 1) ? FSG_BUBBLE : FSG_BUBBLE_HI);
    }
  }
  /* Tank graduated cylinder tick marks every 10% */
  for (int i = 1; i < 10; i++) {
    int y = ty + 8 + (inner_h * i) / 10;
    fsg_fill(tx + tw - 16, y, (i == 5) ? 12 : 7, 1,
             (i == 5) ? FSG_LINE : FSG_LINE_DIM);
  }

  /* Digital percentage readout badge inside tank */
  {
    char buf[8];
    snprintf(buf, sizeof buf, "%d%%", pct);
    int bw = 118, bh = 46, bx = tx + tw - bw - 14, by = ty + 14;
    fsg_fill(bx, by, bw, bh, FSG_BLACK);
    rect_outline(bx, by, bw, bh, d->done ? FSG_GREEN : FSG_AMBER_DIM);
    fsg_text(bx + 14, by + 10, FSG_T_LARGE,
             d->done ? FSG_GREEN : FSG_AMBER, FSG_BLACK, "%s", buf);
  }

  /* --- Right Card: Frame & Stage Telemetry ---------------------------- */
  const int rx = 340, ry = 56, rw = 360, rh = 300;
  const int ix = rx + 18, iw = rw - 36;
  fsg_fill(rx + 3, ry + 3, rw, rh, FSG_BLACK);
  fsg_fill(rx, ry, rw, rh, FSG_PANEL);
  rect_outline(rx, ry, rw, rh, FSG_LINE);
  corners(rx, ry, rw, rh, FSG_AMBER_DIM, 14);

  fsg_text(ix, ry + 14, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL, "SOURCE RAW FRAME");
  fsg_text(ix, ry + 32, FSG_T_LARGE, FSG_WHITE, FSG_PANEL, "%s",
           d->photo ? d->photo : "-");

  fsg_fill(ix, ry + 68, iw, 1, FSG_LINE_DIM);

  fsg_text(ix, ry + 80, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL, "EMULSION & SOUP");
  fsg_text(ix, ry + 98, FSG_T_MED, FSG_AMBER, FSG_PANEL, "%s",
           d->stock ? d->stock : "-");
  fsg_text(ix, ry + 124, FSG_T_SMALL, FSG_GREY, FSG_PANEL, "%s  -  ISO %d  -  %s",
           d->brand ? d->brand : "", d->iso, d->soup ? d->soup : "");

  fsg_fill(ix, ry + 148, iw, 1, FSG_LINE_DIM);

  /* Stage + progress bar */
  fsg_text(ix, ry + 160, FSG_T_SMALL, FSG_GREY_DIM, FSG_PANEL, "PIPELINE STAGE");
  fsg_text(ix, ry + 178, FSG_T_MED, FSG_WHITE, FSG_PANEL, "%s",
           d->stage ? d->stage : "Starting");
  {
    int bx = ix, by = ry + 210, bw = iw, bh = 20;
    fsg_fill(bx, by, bw, bh, FSG_BG_DEEP);
    rect_outline(bx, by, bw, bh, FSG_LINE);
    int fw = (bw - 4) * pct / 100;
    if (fw > 0) {
      fsg_fill(bx + 2, by + 2, fw, bh - 4, d->done ? FSG_GREEN_DARK : FSG_RED);
      fsg_fill(bx + 2, by + 2, fw, 3, d->done ? FSG_GREEN : FSG_BUBBLE_HI);
    }
  }

  if (d->total > 0) {
    char buf[64];
    if (d->done)
      snprintf(buf, sizeof buf, "%d ok   %d skipped   %d failed", d->ok,
               d->skipped, d->failed);
    else
      snprintf(buf, sizeof buf, "Frame %d of %d in chemical bath", d->index, d->total);
    fsg_text(ix, ry + 244, FSG_T_SMALL, FSG_GREY, FSG_PANEL, "%s", buf);
  }

  /* Bottom Status Banner */
  if (d->result) {
    int good = d->failed == 0;
    int bg = good ? FSG_GREEN_DARK : FSG_RED_MID;
    int fg = good ? FSG_GREEN : FSG_RED_HI;
    fsg_fill(20, 372, 680, 58, bg);
    rect_outline(20, 372, 680, 58, good ? FSG_GREEN : FSG_RED);
    fsg_text(38, 388, FSG_T_LARGE, fg, bg, "%s", d->result);
  } else {
    fsg_fill(20, 372, 680, 58, FSG_PANEL);
    rect_outline(20, 372, 680, 58, FSG_LINE);
    fsg_text(38, 384, FSG_T_MED, FSG_AMBER, FSG_PANEL, "BATH IN PROGRESS");
    fsg_text(38, 408, FSG_T_SMALL, FSG_GREY, FSG_PANEL,
             "Do not switch the camera off until developing finishes.");
  }

  hint_row(d->done ? "DONE - press MENU" : "DEVELOPING - do not switch off",
           d->total > 1 ? "BATCH QUEUE" : "SINGLE FRAME");
}
