/*
 * fujiwrite - does a Fuji recipe survive the engine's own parser?
 *
 * The page offers Fuji's fields; the engine understands .fs keys. This test
 * checks the mapping in both directions for the fields that map, and that the
 * ones that cannot map are absent rather than silently zeroed.
 *
 *   ./fujiwrite
 */
#include <stdio.h>
#include <string.h>

#include "../../gui/fs_fuji.h"

#include "../engine/se.h"
#include "../engine/stock.h"

int main(void)
{
    fs_fuji_t f;
    char buf[2048];
    se_stock st;
    int n, fail = 0;

    fs_fuji_defaults(&f);
    memcpy(f.name, "KODAK64", 8);
    f.highlight = 1;
    f.shadow = -1;
    f.color = 2;
    f.chrome = 2;      /* strong */
    f.fx_blue = 1;     /* weak   */
    f.grain = 1;
    f.grain_size = 0;

    /* base_text is the base simulation's own recipe, merged in when
     * f.base_sim is set. This test builds a recipe with base_sim = 0, so it
     * passes NULL: nothing to merge. */
    n = fs_fuji_write(&f, NULL, buf, sizeof buf);
    if (n < 0) { puts("FAIL: did not fit"); return 1; }
    printf("----\n%s----\n", buf);

    if (se_stock_parse(buf, (size_t)n, &st) != SE_OK) {
        puts("FAIL: the engine's parser rejected it");
        return 1;
    }

    if (strcmp(st.name, "KODAK64")) { puts("FAIL: NAME lost"); fail = 1; }

    /* colour: SAT is a percent, and the engine stores it as written */
    {
        int want = 100 + f.color * 12;
        if ((int)(st.sat + 0.5f) != want) {
            printf("FAIL: SAT wrote %d, engine read %d\n", want, (int)(st.sat + 0.5f));
            fail = 1;
        }
    }
    if (st.color_chrome != f.chrome) {
        printf("FAIL: COLOR_CHROME wrote %d, read %d\n", f.chrome, st.color_chrome);
        fail = 1;
    }
    if (st.fx_blue != f.fx_blue) {
        printf("FAIL: FX_BLUE wrote %d, read %d\n", f.fx_blue, st.fx_blue);
        fail = 1;
    }
    if (st.grain <= 0.0f) { puts("FAIL: GRAIN lost"); fail = 1; }

    /* the tone curve must actually bend, and in Fuji's direction:
     * highlight +1 compresses the TOP, shadow -1 leaves the bottom alone */
    {
        se_curve *c = &st.master;   /* CURVE */
        printf("curve: %d points, first (%d,%d), last (%d,%d)\n",
               c->n, c->x[0], c->y[0], c->x[c->n - 1], c->y[c->n - 1]);
        if (c->n < 2) { puts("FAIL: no curve"); fail = 1; }
        else {
            if (c->y[0] < 5) { puts("FAIL: shadow -1 should not crush black"); fail = 1; }
            if (c->y[c->n - 1] < 240) { puts("FAIL: white was pulled down"); fail = 1; }
        }
    }

    /* and the unmappable fields must not appear at all */
    if (strstr(buf, "CLARITY") || strstr(buf, "DYNAMIC")) {
        puts("FAIL: wrote a field this body cannot honour");
        fail = 1;
    }

    printf("%s\n", fail ? "FAILED" : "PASS: recipe round-trips, no phantom fields");
    return fail;
}
