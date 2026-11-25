/**
 * Film stock table - one source of truth for every module.
 *
 * A film stock is described by what the emulsion actually does, then mapped
 * onto whatever levers a given output path has:
 *
 *   picture style   base style + contrast + sharpness + saturation + colour
 *                   tone + Kelvin + WB Shift (Blue/Amber & Green/Magenta).
 *                   This writes into Canon's User Defined slots with zero lag.
 *
 *   LiveView        the same numbers drive a per-pixel transform, which can do
 *                   far more (split toning, highlight desaturation, grain) in
 *                   real time without interrupting shooting.
 */

#ifndef FILM_TABLE_H
#define FILM_TABLE_H

#include <stdint.h>

typedef struct
{
    const char * name;          /* Full descriptive name */
    const char * code;          /* Short code for canister graphic (P400, GOLD, 800T) */
    const char * brand;         /* Manufacturer (Kodak, Fujifilm, CineStill, Ilford, Foma, Lomography) */
    int base;                   /* Canon base style: 0x81 Standard, 0x82 Portrait, 0x83 Landscape,
                                 * 0x84 Neutral, 0x85 Faithful, 0x86 Monochrome */
    int iso;                    /* Nominal box speed ISO (50, 100, 200, 400, 800, 3200) */
    int raw_iso;                /* Canon raw ISO code (0x48=100, 0x50=200, 0x58=400, etc.) */
    int contrast;               /* -4 .. +4 hardware contrast */
    int sharpness;              /*  0 ..  7 hardware sharpness */
    int saturation;             /* -4 .. +4 hardware saturation */
    int tone;                   /* -4 .. +4 hardware colour tone */
    int kelvin;                 /* 4000 .. 6800 K colour temperature */
    int wbs_ba;                 /* -9 .. +9 Blue (-) / Amber (+) shift */
    int wbs_gm;                 /* -9 .. +9 Magenta (-) / Green (+) shift */
    int mono;                   /* 1 = black & white */
    const char * description;   /* Emulsion character description */
} film_t;

enum
{
    /* Kodak Color Negatives */
    FILM_PORTRA_160 = 0,
    FILM_PORTRA_400,
    FILM_PORTRA_800,
    FILM_EKTAR_100,
    FILM_GOLD_200,
    FILM_COLORPLUS_200,
    FILM_ULTRAMAX_400,

    /* Fujifilm Color Negatives */
    FILM_PRO_400H,
    FILM_SUPERIA_400,
    FILM_C200,

    /* Cinema & Specialty */
    FILM_CINESTILL_800T,
    FILM_CINESTILL_50D,
    FILM_LOMO_800,

    /* Color Slide / Reversal */
    FILM_VELVIA_50,
    FILM_PROVIA_100F,
    FILM_EKTACHROME_E100,
    FILM_KODACHROME_64,

    /* Black & White */
    FILM_TRI_X_400,
    FILM_HP5_PLUS,
    FILM_TMAX_100,
    FILM_TMAX_P3200,
    FILM_DELTA_100,
    FILM_PAN_F_50,
    FILM_ACROS_100,
    FILM_FOMAPAN_100,
    FILM_XP2_SUPER,

    /* Fujifilm film simulations that ship with a hand-authored .fs recipe.
     * These were on the card as ML/FILM/<name>.fs but had no table entry,
     * so the shelf could not reach them. */
    FILM_ASTIA_100F,
    FILM_CLASSIC_CHROME,
    FILM_CLASSIC_NEG,
    FILM_NOSTALGIC_NEG,
    FILM_REALA_ACE,
    FILM_ETERNA,
    FILM_BLEACH_BYPASS,
    FILM_OLYMPUS_NATURAL,
    FILM_PRO_NEG_HI,
    FILM_PRO_NEG_STD,

    FILM_COUNT
};

/* Push / Pull Development Chemistry Modes ("The Soup") */
enum
{
    FS_DEV_STANDARD = 0,    /* Normal box speed development */
    FS_DEV_PUSH_1,          /* Push +1 stop (+1 EV, ISO +8 codes, Contrast +1) */
    FS_DEV_PUSH_2,          /* Push +2 stops (+2 EV, ISO +16 codes, Contrast +2) */
    FS_DEV_PULL_1,          /* Pull -1 stop (-1 EV, ISO -8 codes, Contrast -1) */
    FS_DEV_COUNT
};

static const film_t film_table[FILM_COUNT] =
{
    /* Name                 Code     Brand        Base  ISO  RawISO  C   S  Sat Ton  Kelvin  BA  GM Mono Description
     *
     * The saturation slider has to point the same way as the stock's .fs
     * recipe, or the camera and the develop produce opposite pictures of the
     * same film. Portra had +1 against a recipe of SAT 86 and came out more
     * saturated than neutral -- the reverse of pastel. Gold had -1 against SAT
     * 110, so Kodak Gold rendered as a muted neutral. Both now follow the
     * recipe: unit LUT steps, roughly one slider stop per 10% of SAT. */
    { "Portra 160",        "P160",  "Kodak",     0x84, 160, 0x4d,   0,  1,  0,  0,  4900,  +3, +1, 0, "PASTEL SKIN, FINE GRAIN, GENTLE CONTRAST" },
    { "Portra 400",        "P400",  "Kodak",     0x84, 400, 0x58,   0,  1, -1,  0,  5200,  +2, +1, 0, "WARM SKIN, MUTED GREENS, SOFT HIGHLIGHTS" },
    { "Portra 800",        "P800",  "Kodak",     0x84, 800, 0x60,  +1,  1, +2,  0,  5200,  +2, +1, 0, "WARMER PORTRA FOR LOW LIGHT, OPEN SHADOWS" },
    { "Ektar 100",         "EK100", "Kodak",     0x81, 100, 0x48,   0,  3, +1,  0,  6000,  +1, -1, 0, "VIVID REDS, DEEP SKIES, HARD CONTRAST" },
    { "Gold 200",          "GOLD",  "Kodak",     0x84, 200, 0x50,   0,  2, +1,  0,  5800,  +2,  0, 0, "WARM, YELLOW GLOW, NOSTALGIC MIDS" },
    { "ColorPlus 200",     "CP200", "Kodak",     0x84, 200, 0x50,   0,  2,  0,  0,  6000,  +2, -1, 0, "NINETIES FAMILY ALBUM, WARM AND GENTLE" },
    { "UltraMax 400",      "UMAX",  "Kodak",     0x84, 400, 0x58,  +2,  3, +2,  0,  5200,  +2, +1, 0, "NATURAL COLOUR, WARM HINT, SNAPPY" },
    { "Pro 400H",          "400H",  "Fujifilm",  0x84, 400, 0x58,  +1,  1, +1,  0,  4900,  +1, +1, 0, "PASTEL AND AIRY, MINT GREENS" },
    { "Superia 400",       "SUP400","Fujifilm",  0x84, 400, 0x58,   0,  2, +1,  0,  5200,  +2, +2, 0, "GREENS UP FRONT, MUTED REDS, SOFT" },
    { "C200",              "C200",  "Fujifilm",  0x84, 200, 0x50,  +1,  0,  0,  0,  5200,  +1, +1, 0, "NEUTRAL WARM, KIND TO SKIN, GREEN LEAN" },
    { "CineStill 800T",    "800T",  "CineStill", 0x84, 800, 0x60,  -1,  0, +2,  0,  4000,  -1, +3, 0, "CYAN-GREEN NIGHT LIGHTS, TUNGSTEN" },
    { "CineStill 50D",     "50D",   "CineStill", 0x82,  50, 0x48,   0,  3, -2,  0,  6800,  -1, +2, 0, "CLEAN RESTRAINED DAYLIGHT, VERY FINE" },
    { "Color 800",         "LOMO",  "Lomography",0x81, 800, 0x60,  +1,  3, +2,  0,  5800,  +1,  0, 0, "VIBRANT, RED LEAN, SATURATED" },
    { "Velvia 50",         "VEL50", "Fujifilm",  0x83,  50, 0x48,   0,  4, +2,  0,  5200,   0,  0, 0, "EXTREME SATURATION, DENSE BLUE-GREENS" },
    { "Provia 100F",       "PRO100","Fujifilm",  0x84, 100, 0x48,  +2,  2, +2,  0,  5200,  -2, -1, 0, "CLEAN COLOUR, SLIGHTLY COOL, REAL BLACKS" },
    { "Ektachrome E100",   "E100",  "Kodak",     0x84, 100, 0x48,  +2,  3, +1,  0,  5200,   0,  0, 0, "NEUTRAL, CLEAN WHITES, CRISP" },
    { "Kodachrome 64",     "KCHR64","Kodak",     0x84,  64, 0x48,  +1,  4, +1,  0,  5200,  +2, +1, 0, "DEEP REDS, GOLDEN YELLOWS, DARK BLUES" },
    { "Tri-X 400",         "TRI-X", "Kodak",     0x86, 400, 0x58,  +3,  4,  0,  0,  5500,   0,  0, 1, "BLACK SHADOWS, HARD SHOULDER, DENSE" },
    { "HP5 Plus",          "HP5",   "Ilford",    0x86, 400, 0x58,   0,  2,  0,  0,  5500,   0,  0, 1, "RICH TONES, MODERATE CONTRAST, FORGIVING" },
    { "T-Max 100",         "TMX100","Kodak",     0x86, 100, 0x48,  +2,  5, +1,  0,  5500,   0,  0, 1, "FINE AND SHARP, MODERN MONO" },
    { "T-Max P3200",       "TMZ",   "Kodak",     0x86,3200, 0x70,  +3,  5,  0,  0,  5500,   0,  0, 1, "NIGHTS AND CONCERTS, DENSE ATMOSPHERE" },
    { "Delta 100",         "DLT100","Ilford",    0x86, 100, 0x48,  +1,  3,  0,  0,  5500,   0,  0, 1, "VERY FINE GRAIN, CRISP, HIGH CONTRAST" },
    { "Pan F Plus 50",     "PANF",  "Ilford",    0x86,  50, 0x48,  +1,  3,  0,  0,  5500,   0,  0, 1, "FINE AND CLEAN, DEEP BLACKS" },
    { "Acros 100",         "ACROS", "Fujifilm",  0x86, 100, 0x48,  +2,  3,  0,  0,  5500,   0,  0, 1, "DARK SKIES, CRYSTAL HIGHLIGHTS" },
    { "Fomapan 100",       "FOMA",  "Foma",      0x86, 100, 0x48,  +3,  3,  0,  0,  5500,   0,  0, 1, "OLD SCHOOL, STRONG CONTRAST, RUSTIC" },
    { "XP2 Super",         "XP2",   "Ilford",    0x86, 400, 0x58,  +1,  2,  0,  0,  5500,   0,  0, 1, "FINE AND SHARP, BRIGHT HIGHLIGHTS" },

    /* The seven below have real recipes in ML/FILM/. Their hardware
     * numbers only have to agree with the recipe's intent, because the .fs is
     * what does the grading -- the capture path just has to not fight it. */
    { "Astia 100F",        "ASTIA", "Fujifilm",  0x84, 100, 0x48,  +1,  2,  0,  0,  5200,   0, -1, 0, "SOFT CONTRAST, CALM SKIN, CLEAN SLIDE BLACKS" },
    { "Classic Chrome",    "CCHR",  "Fujifilm",  0x84, 200, 0x50,  +1,  2, -2, -1,  5200,  +1, +1, 0, "OLIVE GREENS, CYAN BLUES, DENSE MUTED MIDS" },
    { "Classic Neg",       "CNEG",  "Fujifilm",  0x84, 400, 0x58,  +1,  2, +1,  0,  5000,  +2, +1, 0, "TEAL SHADOWS, AMBER SKIN, OLIVE FOLIAGE" },
    { "Nostalgic Neg",     "NNEG",  "Fujifilm",  0x84, 400, 0x58,   0,  2, +1, +1,  4700,  +3, +1, 0, "AMBER LIGHT, DENSE VIOLET BLUES, OPEN SHADOWS" },
    { "Reala Ace",         "RACE",  "Fujifilm",  0x84, 100, 0x48,  +2,  3, +1,  0,  5200,   0,  0, 0, "TRUE COLOUR, HARD BLACKS, SNAPPY SHOULDER" },
    { "Eterna",            "ETRN",  "Fujifilm",  0x84, 400, 0x58,  -2,  1, -2,  0,  5200,  -1, +2, 0, "FLAT CINEMA NEGATIVE, GREEN-CYAN SHADOWS" },
    { "Bleach Bypass",     "BLPAS", "Process",   0x84, 400, 0x58,  +3,  4, -3,  0,  5500,   0,  0, 0, "SILVER RETAINED, CRUSHED BLACKS, NEAR GREY" },
    /* An authored look, not an emulsion: the colour is in films/OLYNAT.fs, and
     * the numbers here are the picture style it is paired with -- Neutral base,
     * no contrast or saturation push, because the recipe does that work and a
     * style that also pushed would double it. */
    { "Olympus Natural",   "OLYNAT", "Authored", 0x84, 200, 0x50,   0,  2,  0,  0,  5500,   0,  0, 0, "CLEAN COOL WHITES, HONEST GREENS, GENTLE S" },
    { "PRO Neg. Hi",       "PNHI",   "Fujifilm", 0x84, 160, 0x4d,  +1,  2,  0,  0,  5200,   0,  0, 0, "SOFT PORTRAIT SKIN, CRISP STUDIO CONTRAST" },
    { "PRO Neg. Std",      "PNSTD",  "Fujifilm", 0x84, 160, 0x4d,  -1,  2, -1,  0,  5200,   0,  0, 0, "SOFT GRADATION, FLAT STUDIO PORTRAIT" }
};

static inline int film_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

typedef struct
{
    int base;       /* Canon base style id (0x81, 0x82, 0x83, 0x84, 0x86) */
    int contrast;   /* -4 .. +4 */
    int sharpness;  /*  0 ..  7 */
    int saturation; /* -4 .. +4, or -1 for N/A */
    int tone;       /* -4 .. +4, or -1 for N/A */
    int kelvin;     /* 4000 .. 6800 K */
    int wbs_ba;     /* -9 .. +9 (Amber + / Blue -) */
    int wbs_gm;     /* -9 .. +9 (Green + / Magenta -) */
    int raw_iso;    /* Canon raw ISO code */
} film_style_t;

#define FILM_NA (-1)

/* Convert stock index + dev mode into exact hardware parameters */
static inline void film_to_style_ex(int idx, int dev_mode, film_style_t * out)
{
    if (idx < 0 || idx >= FILM_COUNT) idx = 0;
    const film_t * f = &film_table[idx];

    out->base = f->base;
    out->sharpness  = f->sharpness;
    out->kelvin     = f->kelvin;
    out->wbs_ba     = f->wbs_ba;
    out->wbs_gm     = f->wbs_gm;

    /* Push / Pull chemistry modifiers */
    int c_boost = 0;
    int iso_offset = 0;
    switch (dev_mode)
    {
        case FS_DEV_PUSH_1:
            c_boost = 1;
            iso_offset = 8;     /* +1 EV */
            break;
        case FS_DEV_PUSH_2:
            c_boost = 2;
            iso_offset = 16;    /* +2 EV */
            break;
        case FS_DEV_PULL_1:
            c_boost = -1;
            iso_offset = -8;    /* -1 EV */
            break;
        default:
            break;
    }

    out->contrast = film_clamp(f->contrast + c_boost, -4, 4);
    out->raw_iso  = film_clamp(f->raw_iso + iso_offset, 0x48, 0x78);

    if (f->mono)
    {
        out->saturation = FILM_NA;
        out->tone       = FILM_NA;
    }
    else
    {
        out->saturation = film_clamp(f->saturation, -4, 4);
        out->tone       = film_clamp(f->tone, -4, 4);
    }
}

static inline void film_to_style(int idx, film_style_t * out)
{
    film_to_style_ex(idx, FS_DEV_STANDARD, out);
}

#endif /* FILM_TABLE_H */
