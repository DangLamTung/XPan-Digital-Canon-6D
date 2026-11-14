/*
 * fsg_dat.c - the canister artwork reader.
 *
 * FILMCAN.DAT holds fifteen 35 mm film canisters as indexed images plus the
 * palette they were made from. It is the artwork that ships with the FilmRoll
 * module; this file only reads the user's own copy off the card, it does not
 * carry a pixel of it.
 *
 * The format is described entirely by its own header, which is what makes it
 * safe to parse: the loader recomputes the expected file size from the header
 * and refuses the file unless it matches exactly, so a truncated copy or a BMP
 * with the same name can never be walked off the end of the buffer.
 *
 * Compiled for the camera and for the desktop preview alike; the only
 * difference is fsg_file_*() (FIO vs stdio).
 */
#include <string.h>

#include "fsg.h"

static unsigned char can_px[FSG_CAN_MAXW * FSG_CAN_MAXH];
static int can_rgb[FSG_PAL_N];

static unsigned char *can_thumbs;  /* count * tw*th bytes, or NULL */
static const char *can_src;   /* the path the records came from */
static void *can_fp;      /* the open file, kept for the record reads */
static int can_n;         /* records in the file */
static int can_ncol;      /* palette entries */
static int can_w, can_h;  /* image geometry */
static int can_tw, can_th;/* thumbnail geometry */
static int can_off;       /* first image record */
static int can_cur = -1;  /* record the picture currently shows */

static int le16(const unsigned char *p) { return p[0] | (p[1] << 8); }

void fsg_can_unload(void)
{
  if (can_fp) fsg_file_close(can_fp);
  if (can_thumbs) fsg_free(can_thumbs);
  can_thumbs = NULL;
  can_fp = NULL;
  can_n = can_ncol = can_w = can_h = can_tw = can_th = can_off = 0;
  can_cur = -1;
  fsg_img_set(NULL, 0, 0);
}

int fsg_can_load(const char *path)
{
  unsigned char h[16];
  unsigned char pal[FSG_PAL_N * 3];
  int size = 0, need;

  fsg_can_unload();
  if (!path || !path[0]) return 0;

  can_fp = fsg_file_open(path, &size);
  if (!can_fp) return 0;

  if (size < 16 || fsg_file_read(can_fp, h, 16) != 16 ||
      memcmp(h, "FCAN", 4)) {
    fsg_can_unload();
    return 0;
  }

  can_n    = le16(h + 4);
  can_ncol = le16(h + 6);
  can_w    = le16(h + 8);
  can_h    = le16(h + 10);
  can_tw   = le16(h + 12);
  can_th   = le16(h + 14);

  /* Every field is bounded before it is used in arithmetic: the header is the
   * only thing standing between a wrong file and an overrun. */
  if (can_n <= 0 || can_n > 64 || can_ncol <= 0 || can_ncol > FSG_PAL_N ||
      can_w <= 0 || can_w > FSG_CAN_MAXW || can_h <= 0 ||
      can_h > FSG_CAN_MAXH || can_tw < 0 || can_tw > FSG_CAN_MAXW ||
      can_th < 0 || can_th > FSG_CAN_MAXH) {
    fsg_can_unload();
    return 0;
  }

  need = 16 + 3 * can_ncol + can_n * (can_w * can_h + can_tw * can_th);
  if (size != need) {          /* exact: a short file has no usable record */
    fsg_can_unload();
    return 0;
  }

  if (fsg_file_read(can_fp, pal, can_ncol * 3) != can_ncol * 3) {
    fsg_can_unload();
    return 0;
  }
  for (int i = 0; i < can_ncol; i++)
    can_rgb[i] = (pal[3 * i] << 16) | (pal[3 * i + 1] << 8) | pal[3 * i + 2];

  can_off = 16 + 3 * can_ncol;
  can_src = path;
  return 1;
}

int fsg_can_count(void) { return can_n; }

/* Which file the records came from, for the log. */
const char *fsg_can_using(void) { return can_src ? can_src : "?"; }

/* The thumbnails sit in one block after all the images. Read the whole block
 * once into heap memory, so the strip can draw any of them without touching the
 * card again; a card that cannot spare 15 * 40 * 56 bytes simply keeps the film
 * names it used to show. */
int fsg_can_thumbs(void)
{
  int per, total;

  if (can_thumbs) return 1;
  if (!can_fp || can_tw <= 0 || can_th <= 0) return 0;

  per = can_tw * can_th;
  total = can_n * per;
  can_thumbs = (unsigned char *)fsg_alloc(total);
  if (!can_thumbs) return 0;

  if (!fsg_file_seek(can_fp, can_off + can_n * can_w * can_h) ||
      fsg_file_read(can_fp, can_thumbs, total) != total) {
    fsg_free(can_thumbs);
    can_thumbs = NULL;
    return 0;
  }
  return 1;
}

const unsigned char *fsg_can_thumb(int index, int *w, int *h)
{
  if (!can_thumbs || index < 0 || index >= can_n) return NULL;
  if (w) *w = can_tw;
  if (h) *h = can_th;
  return can_thumbs + (size_t)index * can_tw * can_th;
}

int fsg_can_pick(int index)
{
  int off;

  if (!can_fp || index < 0 || index >= can_n) return 0;

  if (index != can_cur) {
    off = can_off + index * can_w * can_h;
    if (!fsg_file_seek(can_fp, off) ||
        fsg_file_read(can_fp, can_px, can_w * can_h) != can_w * can_h) {
      /* can_px is now half written: stop showing whatever is in it */
      fsg_can_hide();
      can_cur = -1;
      return 0;
    }
    can_cur = index;
  }
  fsg_img_set(can_px, can_w, can_h);
  return 1;
}

/* Give the picture back to the drawer, keeping the file open: the caller is
 * showing a stock this file has no record for, and it must stop programming the
 * canister palette or the drawn cassette would use the wrong colours. */
void fsg_can_hide(void)
{
  can_cur = -1;
  fsg_img_set(NULL, 0, 0);
}

int fsg_can_ready(void) { return can_cur >= 0; }

void fsg_can_reprogram(void)
{
  if (can_cur >= 0) fsg_pal(can_rgb, can_ncol);
}
