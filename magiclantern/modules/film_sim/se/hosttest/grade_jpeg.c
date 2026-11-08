/* Host tool: run the camera's develop colour path on a standalone JPEG.
 *
 *   ./grade_jpeg photo.jpg stock.fs out.jpg [lut.bin]
 *
 * The optional LUT is the packed form cube2bin.py writes, applied the way the
 * develop applies it: to the decoded rows, before the look. That is what makes
 * a .fs and its exported .cube comparable on one frame -- fs2cube writes the
 * cube from a recipe, this checks the two land in the same place.
 */
#include <stdio.h>
#include <stdlib.h>
#include "se/se_cam.h"
#include "../engine/jpeg_dec.h"

static void note(void *ctx, const char *msg) { (void)ctx; fprintf(stderr, "  engine: %s\n", msg); }
static void progress(void *ctx, const char *stage, int pct) { (void)ctx; (void)stage; (void)pct; }

/*
 * Read the file back and say what is in it.
 *
 * The look ran, the encoder ran, and the write succeeded -- and the output can
 * still be a frame of grey, because look_emit() keeps rows inside a framing
 * window that this path left zeroed. That was a real bug and nothing noticed
 * it; a percentage of neutral pixels makes it impossible to miss.
 */
struct stats { long n, neutral, sum[3]; };

static int stat_rows(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    struct stats *st = (struct stats *)ctx;
    int x, y;
    for (y = 0; y < nrows; y++)
        for (x = 0; x < width; x++) {
            const uint8_t *p = rgb + ((size_t)y * width + x) * 3;
            st->n++;
            st->sum[0] += p[0]; st->sum[1] += p[1]; st->sum[2] += p[2];
            if (p[0] == p[1] && p[1] == p[2]) st->neutral++;
        }
    return 0;
}

static void report(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf, *arena;
    long len;
    se_arena a;
    se_jdec_info ji;
    struct stats st;
    if (!f) return;
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)len);
    arena = (unsigned char *)malloc(32u << 20);
    if (!buf || !arena) { fclose(f); free(buf); free(arena); return; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(buf); free(arena); return; }
    fclose(f);
    st.n = st.neutral = 0;
    st.sum[0] = st.sum[1] = st.sum[2] = 0;
    se_arena_init(&a, arena, 32u << 20);
    if (se_jpeg_decode(buf, (size_t)len, &a, stat_rows, &st, &ji) == SE_OK && st.n) {
        printf("out: %dx%d, neutral %.1f%%, mean %ld,%ld,%ld\n",
               ji.width, ji.height,
               100.0 * (double)st.neutral / (double)st.n,
               st.sum[0] / st.n, st.sum[1] / st.n, st.sum[2] / st.n);
    }
    free(buf);
    free(arena);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s photo.jpg stock.fs out.jpg [lut.bin] [strength] [satboost]\n",
                argv[0]);
        return 2;
    }
    /* Before the develop opens the file: the loader caches on this path. */
    se_cam_lut_path((argc > 4 && argv[4][0] && argv[4][0] != '-') ? argv[4] : NULL);
    if (argc > 5) se_cam_set_strength(atoi(argv[5]));
    if (argc > 6) se_cam_set_satboost(atoi(argv[6]));
    se_err e = se_cam_grade_jpeg(argv[1], argv[2], argv[3], progress, note, NULL);
    printf("%s: %s\n", argv[1], se_strerror(e));
    if (e == SE_OK) report(argv[3]);
    return e == SE_OK ? 0 : 1;
}
