/*
 * Format a Fuji recipe as a .fs the engine already knows how to read.
 *
 * Every mapping here has a measured precedent in this module, so nothing new
 * is being invented at save time:
 *
 *   highlight/shadow -> CURVE        the same five points the creator's tone
 *                                    screen writes, and the same arithmetic
 *   color            -> SAT          -4..+4 -> 50..150 percent
 *   chrome           -> COLOR_CHROME 0/1/2, parsed by sc_level()
 *   fx_blue          -> FX_BLUE      ditto
 *   grain            -> GRAIN        weak 8 / strong 16 on the 0..100 scale
 *                     -> GRAIN_SIZE  small 6 / large 20
 *
 * WB is deliberately absent: it is a camera property, applied through the
 * stock-offset path, not part of a look file.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "fs_fuji.h"

void fs_fuji_defaults(fs_fuji_t *f)
{
    memset(f, 0, sizeof *f);
    memcpy(f->name, "RECIPE", 7);
    f->grain = 1;          /* Fuji's own defaults are Weak grain, Small */
    f->grain_size = 0;
}

/*
 * The five knots of the curve the recipe writes, moved the way Fuji moves
 * them: the ends stay at 0 and 255 (a recipe does not turn white into grey),
 * Shadow Tone moves the 1/4 knot and Highlight Tone the 3/4 knot.
 *
 * The old formula subtracted t*t and (100-t)^2 from the level itself, which
 * peaks exactly at the ends: Highlight +4 pulled 255 down to 215 and
 * Highlight -2 pushed it to 275 (clipped back to 255), so "harden" greyed the
 * whites and "soften" blew them out.
 */
static int knot_lo(const fs_fuji_t *f)
{
    /* Shadow + deepens: the quarter tone comes down. -4..+4 -> +-24 levels */
    int v = 64 - f->shadow * 6;
    return v < 0 ? 0 : (v > 128 ? 128 : v);
}

static int knot_hi(const fs_fuji_t *f)
{
    /* Highlight + hardens: the three-quarter tone goes up. */
    int v = 192 + f->highlight * 9;
    return v < 128 ? 128 : (v > 255 ? 255 : v);
}

static int tone_out(const fs_fuji_t *f, int in)
{
    int k0 = 0, k1 = knot_lo(f), k2 = 128, k3 = knot_hi(f), k4 = 255;
    if (in <= 0) return 0;
    if (in >= 255) return 255;
    if (in < 64)  return k0 + (k1 - k0) * in / 64;
    if (in < 128) return k1 + (k2 - k1) * (in - 64) / 64;
    if (in < 192) return k2 + (k3 - k2) * (in - 128) / 64;
    return k3 + (k4 - k3) * (in - 192) / 63;
}

static int append(char *buf, int cap, int n, const char *fmt, ...)
{
    va_list ap;
    int wrote;
    if (n < 0 || n >= cap) return -1;
    va_start(ap, fmt);
    wrote = vsnprintf(buf + n, (size_t)(cap - n), fmt, ap);
    va_end(ap);
    if (wrote < 0 || n + wrote >= cap) return -1;
    return n + wrote;
}

static const char *level_word(int v)
{
    return v == 2 ? "STRONG" : (v == 1 ? "WEAK" : "OFF");
}

/*
 * A Fuji recipe is a base film simulation plus adjustments. The base carries
 * the colour: per-channel curves, the hue tables, the split tone. Those lines
 * are copied from the base .fs; everything this page owns (TONE CURVE, SAT,
 * CHROME, FX BLUE, GRAIN) is written afterwards and wins.
 */
static void copy_base_tables(const char *base, char *buf, int cap, int *pos)
{
    static const char *want[] = {
        "CURVE_R ", "CURVE_G ", "CURVE_B ",
        "HUE_SAT", "HUE_ROT", "HUE_LUM",
        "SPLIT_SH", "SPLIT_MID", "SPLIT_HI",
    };
    const char *p = base;
    if (!base) return;
    while (*p && *pos < cap - 1) {
        const char *e = p;
        int len, k, keep = 0;
        while (*e && *e != '\n') e++;
        len = (int)(e - p);
        for (k = 0; k < (int)(sizeof want / sizeof want[0]); k++) {
            int wl = (int)strlen(want[k]);
            if (len >= wl && !strncmp(p, want[k], wl)) { keep = 1; break; }
        }
        if (keep && len > 0 && len + 2 <= cap - *pos) {   /* line + '\n' + NUL  */
            memcpy(buf + *pos, p, (size_t)len);
            *pos += len;
            buf[(*pos)++] = '\n';
        }
        p = *e ? e + 1 : e;
    }
}

int fs_fuji_write(const fs_fuji_t *f, const char *base_text, char *buf, int cap)
{
    static const int curve_in[5] = { 0, 64, 128, 192, 255 };
    int n = 0, i;

#define PUT(...) do { n = append(buf, cap, n, __VA_ARGS__); if (n < 0) return -1; } while (0)

    PUT("VERSION   3\n");
    /* one comment line carries the recipe back: the curve below is a sampled
     * form of these numbers, and reading them back from knots would round */
    PUT("# FUJI %d %d %d %d %d %d %d %d\n", f->highlight, f->shadow, f->color,
        f->chrome, f->fx_blue, f->grain, f->grain_size, f->base_sim);
    PUT("NAME      %s\n", f->name[0] ? f->name : "RECIPE");
    PUT("DESC      FUJI RECIPE (TONE CURVE, COLOR, CHROME, GRAIN)\n");
    PUT("WB        AS_SHOT\n");

    n = append(buf, cap, n, "CURVE     ");
    if (n < 0) return -1;
    for (i = 0; i < 5; i++)
        PUT("%s%d,%d", i ? " " : "", curve_in[i], tone_out(f, curve_in[i]));
    PUT("\n");
    if (base_text && base_text[0]) {
        copy_base_tables(base_text, buf, cap, &n);
        if (n < 0) return -1;
    } else {
        PUT("CURVE_R   0,0 128,128 255,255\n");
        PUT("CURVE_G   0,0 128,128 255,255\n");
        PUT("CURVE_B   0,0 128,128 255,255\n");
    }

    {
        /* The base carries the saturation the look depends on: Acros is
         * SAT 0, Classic Chrome 74, Velvia 118. The Color slider scales that
         * rather than replacing it, so a monochrome base stays monochrome. */
        int base_sat = 100;
        const char *sp = base_text ? strstr(base_text, "\nSAT ") : NULL;
        if (sp) base_sat = atoi(sp + 5);
        PUT("SAT       %d\n", (base_sat * (100 + f->color * 12) + 50) / 100);
    }
    PUT("COLOR_CHROME %s\n", level_word(f->chrome));
    PUT("FX_BLUE   %s\n", level_word(f->fx_blue));
    PUT("GRAIN     %d\n", f->grain == 2 ? 16 : (f->grain == 1 ? 8 : 0));
    PUT("GRAIN_SIZE %d\n", f->grain_size ? 20 : 6);

#undef PUT
    return n;
}
