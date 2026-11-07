/*
 * fsg_ml.c - fsg primitives on the camera.
 *
 * ML's overlay is a 256-entry palette, so the screen chrome is drawn from the
 * fixed constants in fsg.h, each mapped here to an exact palette entry.
 * Anything unmapped falls back to white: a visible mistake is better than a
 * silently wrong shade. The one exception is FSG_SLOT(n) / FSG_PIX(n), which is
 * not a constant at all -- it is one of the entries fsg_pal() reprograms with
 * the current film's own colours or the canister's own palette.
 */
#include <stdarg.h>
#include <stdio.h>

#include <bmp.h>
#include <dryos.h>
#include <fio-ml.h>
#include <gui-common.h>
#include <mem.h>

#include "fsg.h"

/*
 * The borrowed palette entries.
 *
 * ML defines entries 0..79 and keeps 255 for ClearScreen, so 80..254 are free.
 * Each is reprogrammed as YUV (opacity/Y/U/V, the layout
 * alter_bitmap_palette_entry() uses) through the same two ENGIO registers, and
 * the originals are written back by fsg_pal_reset(). If the panel had been off
 * the entry is simply not written, and anything drawn with it keeps the colour
 * that entry already had -- wrong shade, never a blank screen.
 */
static uint32_t fsg_pal_orig[FSG_PAL_N];
static int fsg_pal_saved;

static void fsg_pal_write(int entry, int rgb, uint32_t alpha)
{
  int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
  int y = (77 * r + 150 * g + 29 * b) >> 8;
  int u = ((-43 * r - 85 * g + 128 * b) >> 8) & 255;
  int v = ((128 * r - 107 * g - 21 * b) >> 8) & 255;
  /* Keep the opacity an entry already had, so the overlay keeps whatever
   * transparency ML relies on -- but an entry nothing ever wrote reads back as
   * opacity 0, which is fully transparent, and preserving that would draw an
   * invisible picture. Those become opaque. */
  uint32_t a = alpha & 0xFF000000;
  if (!a) a = 0xFF000000;
  uint32_t e = a | ((uint32_t)y << 16) | ((uint32_t)u << 8) | (uint32_t)v;
  EngDrvOut(LCD_Palette[3 * entry], e);
  EngDrvOut(LCD_Palette[3 * entry + 0x300], e);
  /* LCD_Palette[3*e+2] is Canon's RAM shadow of the entry, and
   * palette_disable(0) -- what bmp_on() runs -- copies every shadow back to
   * the hardware. Leaving it alone means one bmp_off()/bmp_on() pair anywhere
   * in ML silently undoes this write. */
  LCD_Palette[3 * entry + 2] = (int)e;
}

/* Chrome slots, in the order ml_color() returns them. */
static const int fsg_chrome_rgb[FSG_CHROME_N] = {
  FSG_BLACK, FSG_BG_DEEP, FSG_BG, FSG_BG2,
  FSG_PANEL, FSG_PANEL_HI, FSG_PANEL_DARK
};

int fsg_chrome_entry(int i)
{
  return (i >= 0 && i < FSG_CHROME_N) ? FSG_CHROME_BASE + i : -1;
}

/*
 * Program the chrome entries, and ask for full opacity rather than inheriting
 * it. fsg_pal_write() deliberately keeps whatever opacity an entry already had
 * -- that is right for the borrowed range, where ML may rely on transparency --
 * but here the whole point is to stop the picture under the screen from showing
 * through, so 0xFF is written directly.
 */
static uint32_t fsg_chrome_orig[FSG_CHROME_N];
static int fsg_chrome_saved;

void fsg_chrome_init(void)
{
  if (!display_is_on() || !bmp_is_on())
    return;
  if (!fsg_chrome_saved) {
    for (int i = 0; i < FSG_CHROME_N; i++)
      fsg_chrome_orig[i] = (uint32_t)LCD_Palette[3 * (FSG_CHROME_BASE + i) + 2];
    fsg_chrome_saved = 1;
  }
  for (int i = 0; i < FSG_CHROME_N; i++) {
    int rgb = fsg_chrome_rgb[i];
    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    int y = (77 * r + 150 * g + 29 * b) >> 8;
    int u = ((-43 * r - 85 * g + 128 * b) >> 8) & 255;
    int v = ((128 * r - 107 * g - 21 * b) >> 8) & 255;
    uint32_t e = 0xFF000000u | ((uint32_t)y << 16) | ((uint32_t)u << 8) |
                 (uint32_t)v;
    EngDrvOut(LCD_Palette[3 * (FSG_CHROME_BASE + i)], e);
    EngDrvOut(LCD_Palette[3 * (FSG_CHROME_BASE + i) + 0x300], e);
    LCD_Palette[3 * (FSG_CHROME_BASE + i) + 2] = (int)e;
  }
}

/* Hand the chrome entries back. These are indices Canon's own GUI can also
 * draw with, so they must not be left opaque and recoloured after the screen
 * closes. */
void fsg_chrome_reset(void)
{
  if (!fsg_chrome_saved)
    return;
  fsg_chrome_saved = 0;
  if (!display_is_on() || !bmp_is_on())
    return;
  for (int i = 0; i < FSG_CHROME_N; i++) {
    uint32_t entry = fsg_chrome_orig[i];
    EngDrvOut(LCD_Palette[3 * (FSG_CHROME_BASE + i)], entry);
    EngDrvOut(LCD_Palette[3 * (FSG_CHROME_BASE + i) + 0x300], entry);
    LCD_Palette[3 * (FSG_CHROME_BASE + i) + 2] = (int)entry;
  }
}

void fsg_pal(const int *rgb, int n)
{
  if (!rgb || !display_is_on() || !bmp_is_on())
    return;
  /* Keep clear of the chrome entries at the top of the range. A canister
   * record carries 40 colours, so 160 is more than the artwork ever asks for. */
  if (n > FSG_CHROME_BASE - FSG_PAL_BASE)
    n = FSG_CHROME_BASE - FSG_PAL_BASE;
  if (!fsg_pal_saved) {
    /* Only the borrowed range: the chrome entries above it are ours, and
     * saving them here would mean handing our own values back as "original". */
    for (int i = 0; i < FSG_CHROME_BASE - FSG_PAL_BASE; i++)
      fsg_pal_orig[i] = LCD_Palette[3 * (FSG_PAL_BASE + i) + 2];
    fsg_pal_saved = 1;
  }
  for (int i = 0; i < n; i++)
    fsg_pal_write(FSG_PAL_BASE + i, rgb[i], fsg_pal_orig[i]);
}

void fsg_pal_reset(void)
{
  if (!fsg_pal_saved)
    return;
  fsg_pal_saved = 0;
  if (!display_is_on() || !bmp_is_on())
    return;
  for (int i = 0; i < FSG_CHROME_BASE - FSG_PAL_BASE; i++) {
    uint32_t entry = fsg_pal_orig[i];
    EngDrvOut(LCD_Palette[3 * (FSG_PAL_BASE + i)], entry);
    EngDrvOut(LCD_Palette[3 * (FSG_PAL_BASE + i) + 0x300], entry);
    LCD_Palette[3 * (FSG_PAL_BASE + i) + 2] = (int)entry;
  }
}

static uint8_t ml_color(int rgb)
{
  if (FSG_IS_SLOT(rgb))
    return (uint8_t)(FSG_PAL_BASE + (rgb & 0xFF));
  switch (rgb) {
    /* Chrome: our own entries, programmed opaque by fsg_chrome_init(). */
    case FSG_BLACK:      return FSG_CHROME_BASE + 0;
    case FSG_BG_DEEP:    return FSG_CHROME_BASE + 1;
    case FSG_BG:         return FSG_CHROME_BASE + 2;
    case FSG_BG2:        return FSG_CHROME_BASE + 3;
    case FSG_BG_RED:     return COLOR_DARK_RED;
    /*
     * The chrome is drawn brighter than the design's own RGB, because this
     * body composites the OSD layer at a fraction of full weight: the shelf's
     * own probe reads pal[9]=0x3f4054d2, opacity 0x3f in the high byte, and the
     * photograph underneath was visibly showing through the panels before that
     * buffer was blanked. A colour chosen at full weight arrives at about a
     * quarter of it, so the label greys were the first thing to stop being
     * readable on a 3 inch panel -- which is exactly what the camera reported:
     * text present but unreadable.
     *
     * The ordering of the design is kept (deep background < panel < line <
     * label < white); only the levels move up, so what survives the composite
     * is the contrast the layout was drawn for.
     */
    case FSG_PANEL:      return FSG_CHROME_BASE + 4;
    case FSG_PANEL_HI:   return FSG_CHROME_BASE + 5;
    case FSG_PANEL_DARK: return FSG_CHROME_BASE + 6;
    case FSG_LINE:       return COLOR_GRAY(68);
    case FSG_LINE_DIM:   return COLOR_GRAY(53);
    case FSG_PERF:       return COLOR_GRAY(29);
    case FSG_PERF_HI:    return COLOR_GRAY(48);
    case FSG_AMBER:      return COLOR_ORANGE;
    case FSG_AMBER_DIM:  return COLOR_ORANGE;
    case FSG_RED:        return COLOR_RED;
    case FSG_RED_DARK:   return COLOR_RED;
    case FSG_RED_DEEP:   return COLOR_DARK_RED;
    case FSG_RED_MID:    return COLOR_DARK_RED;
    case FSG_RED_HI:     return COLOR_RED;
    case FSG_GREEN:      return COLOR_GREEN2;
    case FSG_GREEN_DARK: return COLOR_GREEN1;
    case FSG_WHITE:      return COLOR_WHITE;
    case FSG_GREY:       return COLOR_GRAY(90);
    case FSG_GREY_DIM:   return COLOR_GRAY(78);
    case FSG_BUBBLE:     return COLOR_DARK_RED;
    case FSG_BUBBLE_HI:  return COLOR_RED;
    default:             return COLOR_WHITE;
  }
}

void fsg_fill(int x, int y, int w, int h, int rgb)
{
  if (w <= 0 || h <= 0) return;
  bmp_fill(ml_color(rgb), x, y, w, h);
}

void fsg_text(int x, int y, int size, int fg, int bg, const char *fmt, ...)
{
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);

  int font = size == FSG_T_LARGE ? FONT_MED_LARGE
             : size == FSG_T_MED ? FONT_MED
                                 : FONT_SMALL;
  bmp_printf(FONT(font, ml_color(fg), ml_color(bg)), x, y, "%s", buf);
}

/* Heap for the strip's thumbnails: taken when the shelf opens and handed back
 * when it closes, so the module's own .bss does not have to carry them. */
void *fsg_alloc(int n)
{
  return n > 0 ? __mem_malloc(n, MEM_TEMPORARY, __FILE__, __LINE__) : NULL;
}

void fsg_free(void *p)
{
  if (p) __mem_free(p);
}

/* ---- file access for the artwork ---------------------------------------
 *
 * FIO_ReadFile asserts when asked for more than one cache page (8192 bytes),
 * and a canister record is 40800, so reads are split here rather than at every
 * call site. A short return is not end of file on this camera, so the loop keeps
 * going until the request is satisfied or the driver really stops producing.
 */
#define FSG_READ_CHUNK 8192

void *fsg_file_open(const char *path, int *size)
{
  FILE *f;
  uint32_t sz = 0;

  if (size) *size = 0;
  if (!path || FIO_GetFileSize(path, &sz) != 0) return NULL;
  f = FIO_OpenFile(path, O_RDONLY);
  if (!f) return NULL;
  if (size) *size = (int)sz;
  return f;
}

int fsg_file_read(void *h, void *dst, int n)
{
  unsigned char *p = (unsigned char *)dst;
  int done = 0;

  if (!h || n <= 0) return 0;
  while (done < n) {
    int want = n - done > FSG_READ_CHUNK ? FSG_READ_CHUNK : n - done;
    int got = FIO_ReadFile((FILE *)h, p + done, want);
    if (got <= 0) break;
    done += got;
  }
  return done;
}

int fsg_file_seek(void *h, int off)
{
  return h && FIO_SeekSkipFile((FILE *)h, off, SEEK_SET) == off;
}

void fsg_file_close(void *h)
{
  if (h) FIO_CloseFile((FILE *)h);
}
