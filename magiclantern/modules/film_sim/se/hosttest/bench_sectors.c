#include <stdint.h>
static volatile int sink = 0;
static int dummy_emit(void *ctx, const uint8_t *row, int w) { *(volatile int *)ctx += row[0] + row[w]; return 0; }
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "engine/stock.h"
#include "engine/look.h"

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

int main(void) {
    char text[16384];
    se_stock st;
    se_look lk;
    FILE *f = fopen("../../films/50D_24sec.fs", "rb");
    if (!f) { perror("50D_24sec.fs"); return 1; }
    size_t len = fread(text, 1, sizeof text - 1, f);
    text[len] = '\0';
    fclose(f);

    if (se_stock_parse(text, len, &st) != SE_OK) {
        printf("se_stock_parse failed\n");
        return 1;
    }

    se_look_from_stock(&st, 1, &lk);

    const int width = 1000;
    const int height = 1000;
    const int npixels = width * height;
    uint8_t *row = malloc(width * 3);
    for (int i = 0; i < width * 3; i++) row[i] = (uint8_t)(i * 17);

    se_look_run *run = NULL;
    static uint8_t arena_mem[SE_LOOK_BUDGET];
    se_arena ar;
    se_arena_init(&ar, arena_mem, sizeof arena_mem);
    se_look_begin(&run, &lk, width, &ar);

    // Warm up
    for (int y = 0; y < 10; y++) se_look_rows(run, row, 1, dummy_emit, &sink);

    // Benchmark 10 million pixels (10 frames of 1000x1000)
    int iterations = 10;
    double t0 = now_ms();
    for (int it = 0; it < iterations; it++) {
        for (int y = 0; y < height; y++) {
            se_look_rows(run, row, 1, dummy_emit, &sink);
        }
    }
    double t1 = now_ms();
    double total_ms = t1 - t0;
    double ns_per_px = (total_ms * 1000000.0) / ((double)npixels * iterations);

    printf("============================================================\n");
    printf("SECTOR BENCHMARK: %d Sectors\n", SE_HUE_SECTORS);
    printf("Graded %d million pixels in %.2f ms\n", (npixels * iterations) / 1000000, total_ms);
    printf("Speed: %.2f ns/pixel (%.2f Megapixels/sec)\n", ns_per_px, 1000.0 / ns_per_px);
    printf("============================================================\n");

    free(row);
    return 0;
}
