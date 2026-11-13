/*
 * The 12-colour creator: state and drawing.
 *
 * Twelve wedges, 30 degrees apart, matching the engine's SE_HUE_SECTORS --
 * not a coincidence, that is why the engine was widened. A wheel with a bead
 * every 30 degrees and only six knots would move two neighbours by half as
 * much as the bead being turned.
 *
 * Drawing goes through fsg_fill()/fsg_text() only, like the shelf, so the
 * same code paints the camera's 8-bit palette and the desktop preview.
 */
#ifndef FSG_CREATOR_H
#define FSG_CREATOR_H

#define FS_CREATOR_WEDGES 12
#define FS_CREATOR_SLOTS  10

typedef struct
{
    /*
     * Per wedge, in the units the .fs file uses: saturation and luminance as
     * percent (100 = unchanged), rotation in degrees. -5..+5 would be too
     * coarse to author with; these are the same ranges a recipe has.
     */
    int sat[FS_CREATOR_WEDGES];
    int rot[FS_CREATOR_WEDGES];
    int lum[FS_CREATOR_WEDGES];

    int sel;        /* which wedge the dials act on, 0..11               */
    int field;      /* 0 = saturation, 1 = rotation, 2 = luminance       */
    int screen;     /* 0 wheel, 1 tone, 2 FX, 3 slots                    */
    int dirty;      /* something changed since the last save             */

    /* Tone screen. Signed, in recipe units. */
    int shadow;
    int highlight;

    /* FX screen: what a look asks for instead of being handed it. The writer
     * used to force GRAIN 8 / HALATION 45 226 16 on every look. */
    int grain;        /* 0..100  */
    int grain_size;   /* 0..100  */
    int halation;     /* 0..100  */
    int chrome;       /* 0 off, 1 weak, 2 strong */
    int fx_blue;      /* 0 off, 1 weak, 2 strong */

    /*
     * Save feedback lives on this screen. It cannot be a NotifyBox: the
     * creator repaints the whole frame from its own loop, so a toast drawn by
     * ML is erased before anyone sees it -- which is why "nothing shows the
     * profile is saved". `saved` is cleared by the next value change.
     */
    int         saved;
    /*
     * Non-zero when the last save failed. It has to live on this struct and be
     * drawn by this screen: the save task used to report with NotifyBox(), and
     * the creator's own repaint erased it before anyone saw it -- which is why
     * a failed save looked like a save that did nothing.
     */
    int         save_err;
    const char *slot_name;   /* the file PLAY writes, e.g. "WHEEL2"     */
    const char *look_name;   /* the name the shelf will show for it     */

    /*
     * Four wheels, not one.
     *
     * Typing a name with UP/DOWN on this panel is the reason nobody had more
     * than one: the NAME editor exists, but spelling "WOLFPACK" one letter at
     * a time is not a workflow. The slot screen lists the four files, says
     * what is in each, and loads the one you pick -- so a second wheel is one
     * keypress away and its name is only a label.
     */
    int slot_cur;                    /* which slot is loaded             */
    int slot_pick;                   /* highlighted on the slot screen   */
    int slot_request;                /* module: load slot_pick, please   */
    const char *slot_file[FS_CREATOR_SLOTS];   /* WHEEL .. WHEEL4      */
    const char *slot_label[FS_CREATOR_SLOTS];  /* saved NAME, or NULL  */
    const char *saved_name;  /* what was just written, once it exists   */

    const char *stock;   /* base stock name, shown in the header          */
} fs_creator_t;

void fs_creator_defaults(fs_creator_t *c, const char *stock);
void fs_creator_step(fs_creator_t *c, int key);   /* returns via .dirty */
void fsg_draw_creator(const fs_creator_t *c);

/*
 * The rotating ring.
 *
 * `offset_q16` is the wheel's position in 2^16 units of ONE wedge, so the
 * selected bead sits under a fixed pointer at the top and the rest of the
 * ring turns past it -- which is what a physical dial does, and what the
 * screen did not: it used to jump a whole 30 degrees per press.
 *
 * Only the beads move, so only the beads are redrawn: the previous centres
 * in `sp` are erased with the disc colour and the new ones drawn. The disc
 * behind them is flat, so that erase is exact rather than approximate, and a
 * frame costs ~12 small fills instead of the 301-fill disc.
 */
typedef struct { int x[FS_CREATOR_WEDGES], y[FS_CREATOR_WEDGES]; int valid; } fs_creator_spin_t;

/* The rows the ring occupies, so a spin frame copies back only those. */
#define FSG_CREATOR_RING_Y0 92
#define FSG_CREATOR_RING_Y1 408

void fsg_draw_creator_beads(const fs_creator_t *c, int offset_q16,
                            fs_creator_spin_t *sp);

/* The whole wheel at a given rotation: the frame the spin repaints. */
void fsg_draw_creator_wheel(const fs_creator_t *c, int offset_q16);

/*
 * Tabs, as on the reference UI: Saturation, Luminance, Hue. The panel order is
 * the panel's, not the struct's -- the struct is sat, rot, lum -- so tab_field
 * is the only correct way to map both ways.
 */
extern const char *const fs_creator_tabs[3];
extern const int fs_creator_tab_field[3];
void fsg_draw_tone(const fs_creator_t *c);
void fsg_draw_fx(const fs_creator_t *c);
void fsg_draw_slots(const fs_creator_t *c);

/*
 * Format the creator's state as a .fs recipe. Returns the length written, or
 * -1 if it did not fit. The tone screen becomes a CURVE, which is how every
 * recipe in films/ expresses tone -- there is no BLACK or SHOULDER key.
 */
int fs_creator_write(const fs_creator_t *c, char *buf, int cap,
                     const char *name);

/*
 * Wedge 0..11 -> its name, and its nominal colour.
 *
 * The colour is a PALETTE ENTRY, not an RGB value: the panel has 175 indices
 * and ml_color() sends anything it does not recognise to pure white, so the
 * first version drew twelve white squares on the camera while the desktop
 * preview showed twelve colours. fs_creator_register_palette() must be called
 * before drawing (fsg_draw_creator does it for you).
 */
const char *fs_creator_wedge_name(int i);
int         fs_creator_wedge_rgb(int i);
void        fs_creator_register_palette(void);

#endif
