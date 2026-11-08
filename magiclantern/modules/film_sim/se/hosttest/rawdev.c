/* Host driver for the Level 3 raw develop: CR2 -> spool -> pipeline -> JPEG. */
#include <stdio.h>
#include <stdlib.h>
#include "se/se_cam.h"

static void prog(void *c, const char *s, int p) { (void)c; if (p % 25 == 0) printf("  %-10s %3d%%\n", s, p); }
static void note(void *c, const char *m) { (void)c; printf("  engine: %s\n", m); }

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s file.CR2 stock.fs out.jpg [scale] [aspect] [strength] [satboost]\n", argv[0]);
        return 2;
    }
    int scale = argc > 4 ? atoi(argv[4]) : 2;
    if (argc > 5) se_cam_set_aspect(atoi(argv[5]));
    if (argc > 6) se_cam_set_strength(atoi(argv[6]));
    if (argc > 7) se_cam_set_satboost(atoi(argv[7]));
    se_err e = se_cam_develop_raw(argv[1], argv[2], argv[3], scale, prog, note, NULL);
    printf("raw develop: %s\n", e == SE_OK ? "ok" : se_strerror(e));
    return e == SE_OK ? 0 : 1;
}
