/*
 * fsg.h - the Film Sim screen layer.
 *
 * Everything that draws is here, behind two primitives, so the same code that
 * runs on the camera can be rendered on a desktop and looked at. The module
 * only supplies state; it never touches bmp_* for these two screens.
 *
 * Colours are 0xRRGGBB. Text always sits on a known solid background because
 * the camera's font renderer has no transparency.
 */
#ifndef FSG_H_INCLUDED
#define FSG_H_INCLUDED

#define FSG_W 720
#define FSG_H 480

/* Palette. Every one of these maps to an exact entry of the camera's 256-colour
 * overlay palette (gui/fsg_ml.c); the desktop preview uses the RGB values here.
 * Nothing outside this list may be drawn: the camera cannot mix colours. */
#define FSG_BLACK      0x000000
#define FSG_BG_DEEP    0x050508
#define FSG_BG         0x0B0B0F
#define FSG_BG2        0x121218
#define FSG_BG_RED     0x140608
#define FSG_PANEL      0x16161D
#define FSG_PANEL_HI   0x232330
#define FSG_PANEL_DARK 0x0E0E13
#define FSG_LINE       0x3A3A46
#define FSG_LINE_DIM   0x24242E
#define FSG_PERF       0x2A2A33
#define FSG_PERF_HI    0x3C3C48
#define FSG_AMBER      0xFFC24A
#define FSG_AMBER_DIM  0x8A6A22
#define FSG_RED        0xC8102E
#define FSG_RED_DARK   0x8C1330
#define FSG_RED_DEEP   0x3A000C
#define FSG_RED_MID    0x2A0A0E
#define FSG_RED_HI     0xE2547A
#define FSG_GREEN      0x54D07A
#define FSG_GREEN_DARK 0x10240F
#define FSG_WHITE      0xF4F2EC
#define FSG_GREY       0x8C8C99
#define FSG_GREY_DIM   0x5A5A66
#define FSG_BUBBLE     0x6E0417
#define FSG_BUBBLE_HI  0xE85A80
/*
 * Reserved palette entries.
 *
 * ML defines entries 0..79 of the overlay palette (nine hues, the rest a grey
 * ramp) and keeps 255 for ClearScreen, so a Kodak yellow canister and a Fuji
 * green one cannot both be drawn from the fixed entries -- that is why the old
 * tone ladder looked identical for every stock. 80..254 are free: fsg_pal()
 * reprograms them and fsg_pal_reset() puts the originals back when the screen
 * closes. FSG_SLOT(n) is a colour like any other, so the ordinary fill/text
 * calls draw with it.
 *
 * The two users of those entries are exclusive, because whoever programs them
 * last wins: fifteen stock colours for the drawn cassette (FSG_SLOT), or the
 * canister photograph's own palette (FSG_PIX, up to 175 colours). The drawer
 * never asks for both at once.
 */
#define FSG_PAL_BASE   80
#define FSG_PAL_N      175             /* 80..254; 255 is ClearScreen */
#define FSG_NSLOT      16
#define FSG_SLOT(n)    (0x01000000 | ((n) & 0xFF))
#define FSG_PIX(i)     (0x01000000 | ((i) & 0xFF))
#define FSG_IS_SLOT(c) (((c) & 0xFF000000) == 0x01000000)

/* Slot meaning for the canister drawing. */
#define FSG_SL_BORDER   0   /* canister outline, near black            */
#define FSG_SL_BODY_HI  1   /* lit edge of the canister body           */
#define FSG_SL_BODY     2   /* body                             */
#define FSG_SL_BODY_LO  3   /* shaded edge of the canister body        */
#define FSG_SL_CAP_HI   4   /* spool cap, lit                          */
#define FSG_SL_CAP_LO   5   /* spool cap, shaded                       */
#define FSG_SL_PAPER    6   /* label paper                             */
#define FSG_SL_INK      7   /* ink on the label                        */
#define FSG_SL_ACCENT   8   /* brand stripe                            */
#define FSG_SL_TONGUE   9   /* film tongue leaving the cassette        */
#define FSG_SL_SHADOW   10  /* response window: shadows                */
#define FSG_SL_MID      11  /* response window: midtones               */
#define FSG_SL_LIGHT    12  /* response window: highlights             */
#define FSG_SL_SPROCKET 13  /* perforation hole                        */
#define FSG_SL_GLOW     14  /* highlight on the tongue                 */
#define FSG_SL_EXTRA    15  /* spare, kept dark                        */

/*
 * The stock picture.
 *
 * The shelf shows the manufacturer's own canister, not a drawing: a hand-drawn
 * cassette is a guess about a film, a photograph of the box is what the film
 * actually is. The artwork is read from ML/data/FILMCAN.DAT -- the users' own
 * copy, shipped with the FilmRoll module -- and never bundled here.
 *
 *   offset 0   "FCAN"
 *   offset 4   u16 count, u16 ncolors, u16 w, u16 h, u16 thumb_w, u16 thumb_h
 *   offset 16  ncolors * 3 palette bytes, RGB
 *              count * w*h     image indices, one canister each
 *              count * tw*th   thumbnail indices
 *
 * Total size is 16 + 3*ncolors + count*(w*h + tw*th) bytes, which is what the
 * loader checks before it trusts a single offset. Pixel values index the file's
 * own palette, so drawing is a blit and no colour maths runs on the camera --
 * the palette itself goes into overlay entries 80.. via fsg_pal().
 */
/* The shelf's mount, which is also the artwork's own size: the loader refuses a
 * record that would not fit, so the drawer never has to scale or clip one. */
#define FSG_CAN_MAXW 170
#define FSG_CAN_MAXH 240
/* Ours first: it holds every stock (tools/make_can_art.py + merge_cans.py write
 * it to the card). The manufacturer artwork the user already has is the
 * fallback, so a card without our merged file behaves exactly as before. */
#define FSG_CAN_PATH  "ML/FILMSIM/CANS.DAT"
#define FSG_CAN_PATH2 "ML/data/FILMCAN.DAT"

typedef struct {
  int w, h;
  const unsigned char *px;   /* w * h indices into the borrowed palette */
} fsg_img_t;

/* The picture the shelf will show; px == NULL draws the cassette instead.
 * The buffer must stay valid while the screen is open. */
void fsg_img_set(const unsigned char *px, int w, int h);

enum { FSG_T_SMALL = 0, FSG_T_MED = 1, FSG_T_LARGE = 2 };

/* ---- primitives (implemented twice: camera and host) ------------------- */
void fsg_fill(int x, int y, int w, int h, int rgb);
void fsg_text(int x, int y, int size, int fg, int bg, const char *fmt, ...);

/* Program the borrowed palette entries FSG_PAL_BASE+0 .. +n-1 with rgb[] (0xRRGGBB).
 * Safe to call every frame: the shadow copies are saved once, and re-writing is
 * what keeps the entries alive when ML's bmp_on() restores all 256 entries from
 * its own shadows. fsg_fill/fsg_text with FSG_SLOT(i) or FSG_PIX(i) then draw in
 * rgb[i]; the host build keeps the same table so the preview matches. */
void fsg_pal(const int *rgb, int n);

/*
 * The module's own palette entries for the screen chrome.
 *
 * The borrowed range (FSG_PAL_BASE..) carries the stock's colours or the
 * canister photograph, and both are transient. The chrome -- black, the three
 * background steps and the panel greys, i.e. everything the eye reads as "the
 * screen" -- gets its own seven entries at the top of the range, programmed
 * from the design's own RGB at full opacity. That is the point: ML's palette
 * on this body comes back with opacity 0x3f (the shelf probe reads
 * pal[9]=0x3f4054d2), so a picture behind the screen shows through it. Writing
 * our own entries is the only way to ask for opaque.
 */
#define FSG_CHROME_BASE 240
#define FSG_CHROME_N    7
void fsg_chrome_init(void);   /* program them; call when a screen is taken */
void fsg_chrome_reset(void);  /* and hand them back on release */
int  fsg_chrome_entry(int i); /* palette entry for chrome slot i, for the log */
/* Put the borrowed entries back the way they were found. */
void fsg_pal_reset(void);

/* ---- canister artwork (gui/fsg_dat.c) ---------------------------------
 *
 * fsg_can_load() validates the file once; fsg_can_pick() reads one record and
 * makes it the shelf picture. Both are no-ops that report failure when the file
 * is absent, so a card without the artwork draws the cassette instead. */
int fsg_can_load(const char *path);
void fsg_can_unload(void);
int fsg_can_count(void);
const char *fsg_can_using(void);   /* path the records came from */
int fsg_can_pick(int index);
/* Read every 40x56 thumbnail into heap memory, for the film strip. Returns 0
 * when the file has no thumbnails or the memory is not there. */
int fsg_can_thumbs(void);
const unsigned char *fsg_can_thumb(int index, int *w, int *h);
void fsg_can_hide(void);      /* keep the file, drop the current record */
int fsg_can_ready(void);      /* a record is loaded and is the current picture */
void fsg_can_reprogram(void); /* re-program its palette (call once per frame) */

/* File access, implemented per backend: FIO on the camera, stdio on the host.
 * read() must fill the whole request or report a short count. */
void *fsg_alloc(int n);       /* heap block for the thumbnails, or NULL */
void  fsg_free(void *p);
void *fsg_file_open(const char *path, int *size);
int fsg_file_read(void *h, void *dst, int n);
int fsg_file_seek(void *h, int off);
void fsg_file_close(void *h);

/* ---- model ------------------------------------------------------------ */
#define FSG_MAX_LABELS 48

/* What the drawer needs to know about a stock to give it a look of its own. */
typedef struct {
  const char *brand;   /* "Kodak", "Fujifilm", "Ilford" ... */
  int iso;             /* box speed: 50 ASA is a light body, 3200 a dark one */
  int kelvin;          /* recipe white balance: warm / cool cast */
  int wbs_gm;          /* green / magenta shift, -9..+9 */
  int saturation;      /* -3..+3 */
  int mono;            /* 1 = black and white, no colour at all */
} fsg_film_t;

typedef struct {
  const char *name;
  const char *brand;   /* "Kodak" */
  const char *kind;    /* "negative" / "slide" / "b&w" */
  const char *desc;    /* sentence(s); the drawer wraps them */
  const char *soup;    /* "Standard C-41" / "Push +1" */
  const char *code;    /* "P400", printed on the canister label */
  int iso;
  int kelvin;          /* recipe white balance, shown as "WB 5200K" */
  int wbs_gm;          /* green / magenta shift, shown as "G/M +1" */
  int saturation;      /* -3..+3, shown as "SAT +1" */
  int palette[FSG_NSLOT];   /* the film's own colours, see fsg_pal() */
} fsg_stock_t;

typedef struct {
  int grain;           /* 0..100, shown on the right of the panel */
  int strength;        /* 0..100 */
  const char *title;   /* "FILM SHELF" / "DARKROOM" */
  const char *photo;   /* frame the darkroom will develop, or NULL */
  const char *hint_l;  /* footer, left */
  const char *hint_r;  /* footer, right */
  const char *badge;   /* "LEVEL 2 - STYLE + GRADE" */
  const char *counter; /* "12 / 33" */
  fsg_stock_t stock;
  const char *labels[FSG_MAX_LABELS];
  /* Which FILMCAN.DAT record belongs to labels[i], or -1 when the artwork has
   * no canister for that film. The strip cannot use i directly: the file's
   * order is the manufacturer's, not the card's stock list. */
  signed char art[FSG_MAX_LABELS];
  int count;
  int sel;
  /* Animation. `scroll` is the strip position in 1/256 frame units and is
   * eased by the caller; `tick` is milliseconds for pulses and perforations. */
  int scroll;
  unsigned tick;
  int key_flash;       /* 0..255, fades after a keypress */
} fsg_shelf_t;

typedef struct {
  const char *photo;
  const char *stock;
  const char *brand;
  const char *stage;
  const char *soup;
  const char *result;  /* NULL while running */
  int iso;
  int pct;
  int index;           /* 1-based, 0 when a single develop */
  int total;
  int ok, skipped, failed, done;
  unsigned tick;
} fsg_dev_t;

/* Fill `out` with the 16 colours the stock's cassette and response windows
 * are drawn in. Lives here, not in the module, so the desktop preview derives
 * exactly the same palette the camera will program into the panel. */
void fsg_stock_palette(const fsg_film_t *f, int out[FSG_NSLOT]);

void fsg_draw_shelf(const fsg_shelf_t *s);
void fsg_draw_develop(const fsg_dev_t *d);

/*
 * The two bands of the shelf that move on their own: the header sheen and the
 * film strip. Each paints its own background, so redrawing one leaves every
 * other row of the panel untouched -- which is what lets an animation frame
 * copy 94 rows instead of the whole 720x480 screen.
 *
 * fsg_draw_shelf() is still the only thing that draws the rest.
 */
void fsg_draw_shelf_header(const fsg_shelf_t *s);
void fsg_draw_shelf_strip(const fsg_shelf_t *s);

#define FSG_HEADER_H 46   /* rows 0..45: the red bar and its sweep    */
#define FSG_STRIP_Y  352  /* the strip band, self-clearing             */
#define FSG_STRIP_H  94

/* The word wrap the drawer applies to descriptions. Returns line count. */
int fsg_wrap(const char *text, char out[][48], int max_lines, int width);

#endif
