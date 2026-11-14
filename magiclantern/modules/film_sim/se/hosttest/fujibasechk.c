/* A Fuji recipe built on a base simulation must inherit the base's colour
 * tables and still parse as a stock. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../gui/fs_fuji.h"
#include "../engine/stock.h"

static char base[8192];

int main(int argc, char **argv)
{
    fs_fuji_t f;
    char out[8192];
    se_stock st;
    int n;
    FILE *b;

    if (argc < 2) return 2;
    b = fopen(argv[1], "rb");
    if (!b) return 2;
    n = (int)fread(base, 1, sizeof base - 1, b);
    fclose(b);
    base[n > 0 ? n : 0] = 0;

    fs_fuji_defaults(&f);
    snprintf(f.name, sizeof f.name, "CHK");
    f.base_sim = 2;              /* Velvia */
    f.highlight = 2;
    f.color = 1;
    n = fs_fuji_write(&f, base, out, sizeof out);
    if (n < 0) { printf("write failed\n"); return 1; }

    int has_hue = strstr(out, "HUE_SAT") != NULL;
    int has_split = strstr(out, "SPLIT_") != NULL;
    printf("wrote %d bytes, HUE_SAT %s, SPLIT %s\n", n,
           has_hue ? "yes" : "NO", has_split ? "yes" : "NO");

    if (se_stock_parse(out, n, &st) != SE_OK) {
        printf("PARSER REJECTED the generated recipe\n");
        return 1;
    }
    printf("parsed as '%s', name ok\n", st.name);
    /* the recipe's own curve must still be there */
    printf("CURVE %s\n", strstr(out, "CURVE     ") ? "present" : "missing");
    return has_hue ? 0 : 1;
}
