/*
 * A Fuji recipe as this camera can hold it.
 *
 * The field set and the ranges are Fuji's, taken from their X-T4 manual index
 * for the IMAGE QUALITY SETTING menu -- not invented. A recipe on a Fuji body
 * is not a screen of its own: it is a bundle of those individual menu items,
 * saved in a C1..C7 slot. That is what this is: the bundle, saved as a .fs,
 * because a .fs is this module's C-slot.
 *
 * Three fields a real recipe carries cannot be honoured here and are NOT
 * offered, rather than offered as knobs that do nothing:
 *
 *   DYNAMIC RANGE   raises ISO and pulls highlights before digitising; on an
 *                   8-bit JPEG the headroom is already gone
 *   CLARITY         needs a graded-luma ring; designed, measured, reverted
 *   HIGH ISO NR     Canon's NR lives in the picture style; not driven yet
 */
#ifndef FS_FUJI_H
#define FS_FUJI_H

#define FS_FUJI_NAME 16

typedef struct
{
    char name[FS_FUJI_NAME];   /* up to 8.3 on the card: keep it short      */

    /* Fuji's I.Q. menu, its ranges. 0 means "no change" in every case. */
    int highlight;    /* -2 .. +4  TONE CURVE: + compresses the top end     */
    int shadow;       /* -2 .. +4  TONE CURVE: + deepens the bottom end     */
    int color;        /* -4 .. +4  SAT                                      */
    int chrome;       /*  0 ..  2  COLOR CHROME EFFECT   off/weak/strong    */
    int fx_blue;      /*  0 ..  2  COLOR CHROME FX BLUE  off/weak/strong    */
    int grain;        /*  0 ..  2  GRAIN EFFECT          off/weak/strong    */
    int grain_size;   /*  0 ..  1  SIZE                  small/large        */

    /*
     * White balance is NOT part of the .fs: it is the camera's own setting,
     * and this module already expresses it as an offset on the stock's
     * balance. Kept here so one page can show the whole recipe.
     */
    int base_sim;     /* index into the base simulation list, 0 = none      */
    int wb_r;         /* -9 .. +9  Fuji Red axis                            */
    int wb_b;         /* -9 .. +9  Fuji Blue axis                           */
} fs_fuji_t;

void fs_fuji_defaults(fs_fuji_t *f);

/* Format as a .fs recipe. Returns bytes written, or -1 if it did not fit. */
int  fs_fuji_write(const fs_fuji_t *f, const char *base_text, char *buf, int cap);

#endif
