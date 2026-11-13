/**
 * Probe of the shot-time YUV frame - the buffer Canon's own JPEG encoder is
 * about to read.
 *
 * Why this exists
 * ---------------
 * Developing a film frame after the shot means reading the 5472x3648 preview
 * out of the CR2, Huffman-decoding it, grading it and encoding it again: about
 * 20 s of the camera's CPU per photo, and two JPEG generations.
 *
 * Florin's Film Roll avoids all of that. Disassembling
 * tools/florin_filmroll_1.1/FilmRoll-6D-beta11/ML/modules/filmroll.mo shows it
 * patching two firmware entry points with a hook of its own:
 *
 *      patch_hook_function(0xFF1640E8 or 0xFF16E930, 0xE92D41FC, yuv_hook)
 *
 * and that hook grades a frame of width 5760 and height 3840 (or 3648) --
 * the sensor-sized YUV buffer, at the RAM ranges 0x40000000/0x44000000 or
 * 0x58000000/0x60000000 depending on the JPEG quality it was told about --
 * before Canon's encoder runs. The look lands in the JPEG the camera writes.
 *
 * Before doing that here, the buffer has to be identified on this camera: which
 * argument carries it, in what order the planes are, what the stride is. Guessing
 * wrongly writes garbage into a frame buffer mid-capture. So this file only
 * observes -- it records the registers of the hooked function and copies one
 * band of the candidate buffer into RAM, and film_sim writes that to the card
 * from its own task afterwards. A read inside a shot hook is safe; a file write
 * is not, and this never does one.
 *
 * What is written
 * ---------------
 * ML/FILMSIM/YUV.BIN: a header (magic, patched address, the 14 saved registers,
 * the source pointer and length) followed by the raw band. Offsets inside that
 * band are what the grading code will need.
 *
 * Safety
 * ------
 * patch_hook_function() is given the instruction word it expects to find, and
 * ML refuses to patch if the address does not hold it, so a body or firmware
 * where these addresses mean something else is left untouched. The hook itself
 * only reads: no allocation, no FIO, no delays -- it runs inside Canon's
 * capture path.
 */

#include <dryos.h>
#include <fio-ml.h>
#include <patch.h>
#include <exmem.h>
#include <lens.h>          /* lens_info.raw_picstyle: the style the camera is ON */
#include <stdio.h>
#include <string.h>

#include "fs_core.h"

#include "fs_yuvhook.h"
#include "fs_yuvlook.h"

static uint32_t fs_hook_ms = 0;   /* when the hook last ran, for save timing */

/*
 * Writing this frame through the uncached alias costs ~8 s, not the 817 ms the
 * frame reported for a 5472x3648 grade: halving film.sim.yuvrows halved the
 * wall-clock wait (8 s -> 4 s) while the reported time halved with it (817 ->
 * 408 ms), so the figure was real but the clock inside this context under-counts
 * by an order of magnitude. 76 MiB in 8 s is ~9.5 MB/s -- the signature of
 * uncached stores, where every word is its own bus transaction.
 *
 * Canon hands us 0x4c394000, the uncached view. The same RAM is reachable at
 * 0x0c394000 (CACHEABLE() in mem_defs.h: clear bit 30), so the pass runs through
 * the cache and then the touched lines are written back once, before Canon's DMA
 * reads the frame. If the write-back is ever wrong the look simply will not
 * reach the JPEG, which the per-shot yuvcap/diag lines already make visible.
 */
/* Set by every grade, not latched: this value is printed in the per-shot
 * "yuvcap:" line, so it has to describe the most recent pass. Latched, one
 * shot that used the alias made every later line claim the same. */
static int fs_dcache_used = 0;

/*
 * Aspect bars, burned into the frame Canon is about to encode.
 *
 * The camera has no stills aspect setting (nothing in src/property.h nor in the
 * 6D platform headers), so the only way a shot carries a ratio is to put black
 * in the pixels. That is one memset per bar -- 6 MB for 16:9 at full size --
 * through the cacheable alias, before the D-cache write-back hands the frame
 * over. The preview draws the same bars with the same function.
 */
static int fs_bars_mode = 0;
static int fs_bars_burned = 0;

void fs_yuvhook_bars(int mode) { fs_bars_mode = mode; }
int fs_yuvhook_bars_burned(void) { return fs_bars_burned; }

/* Header burned into the shot: the band goes in the bottom bar, so it covers
 * black pixels and never the picture. rgb_scratch is the caller's: nothing is
 * allocated inside the capture path. */
static se_frame fs_hdr;
static unsigned char *fs_hdr_scratch;
static int fs_hdr_burned;

void fs_yuvhook_header(const se_frame *f, unsigned char *scratch) {
    if (f && scratch) {
        fs_hdr = *f;
        fs_hdr_scratch = scratch;
    } else {
        memset(&fs_hdr, 0, sizeof fs_hdr);
        fs_hdr_scratch = NULL;
    }
}
int fs_yuvhook_header_burned(void) { return fs_hdr_burned; }

/* RGB row -> UYVY row (Y, Cb, Cr, BT.601 studio swing). */
static void fs_hdr_row_uyvy(unsigned char *dst, const unsigned char *rgb, int w) {
    int x;
    for (x = 0; x + 1 < w; x += 2) {
        const unsigned char *p0 = rgb + (size_t)x * 3;
        const unsigned char *p1 = rgb + (size_t)(x + 1) * 3;
        int y0 = (77 * p0[0] + 150 * p0[1] + 29 * p0[2] + 128) >> 8;
        int y1 = (77 * p1[0] + 150 * p1[1] + 29 * p1[2] + 128) >> 8;
        int r = (p0[0] + p1[0]) / 2;
        int g = (p0[1] + p1[1]) / 2;
        int b = (p0[2] + p1[2]) / 2;
        /* DIGIC stores U and V as signed bytes with neutral 0 (fs_core.h);
         * writing 128 here is what shifted every colour in the band. */
        int u = ((-43 * r - 85 * g + 128 * b + 128) >> 8);
        int v = ((128 * r - 107 * g - 21 * b + 128) >> 8);
        if (u < -128) u = -128; else if (u > 127) u = 127;
        if (v < -128) v = -128; else if (v > 127) v = 127;
        dst[x * 2 + 0] = (unsigned char)(signed char)u;
        dst[x * 2 + 1] = (unsigned char)(y0 < 0 ? 0 : (y0 > 255 ? 255 : y0));
        dst[x * 2 + 2] = (unsigned char)(signed char)v;
        dst[x * 2 + 3] = (unsigned char)(y1 < 0 ? 0 : (y1 > 255 ? 255 : y1));
    }
}

/* Draw the band into the bottom bar of a frame that already has bars. */
static void fs_hdr_burn(unsigned char *gp, int width, int rows, int stride,
                        int num, int den) {
    int box_h = (width * num) / den;
    int top, bar_h, band_h, y;

    if (box_h <= 0 || box_h >= rows) return;
    top = (rows - box_h) / 2;
    bar_h = rows - (top + box_h);
    if (bar_h <= 0) return;

    /* band height for this width, then the placement the develop uses */
    if (fs_hdr.inner_h <= 0) {
        fs_hdr.inner_h = width / 14;
        if (fs_hdr.inner_h < 120) fs_hdr.inner_h = 120;
        if (fs_hdr.inner_h > 400) fs_hdr.inner_h = 400;
    }
    se_frame_layout(&fs_hdr);
    band_h = fs_hdr.bottom;
    if (band_h <= 0 || band_h > bar_h) return;

    /* The bars become paper, not black: the shot then looks like the framed
     * print the develop writes, and the band sits on the same paper. */
    for (y = 0; y < top; y++) {
        unsigned char *row = gp + (size_t)y * stride;
        for (int x = 0; x + 1 < width; x += 2) {
            row[x * 2 + 0] = 0;      /* U = neutral, signed */
            row[x * 2 + 1] = 255;
            row[x * 2 + 2] = 0;      /* V = neutral */
            row[x * 2 + 3] = 255;
        }
    }
    for (y = top + box_h + band_h; y < rows; y++) {
        unsigned char *row = gp + (size_t)y * stride;
        for (int x = 0; x + 1 < width; x += 2) {
            row[x * 2 + 0] = 0;      /* U = neutral, signed */
            row[x * 2 + 1] = 255;
            row[x * 2 + 2] = 0;      /* V = neutral */
            row[x * 2 + 3] = 255;
        }
    }

    /* The band goes right under the picture, not at the bottom of the frame:
     * the pad below it stays paper. */
    for (y = 0; y < band_h; y++) {
        unsigned char *row = gp + (size_t)(top + box_h + y) * stride;
        se_frame_fill_row(fs_hdr_scratch, width, &fs_hdr);
        if (y == 0)
            se_frame_edge_row(fs_hdr_scratch, width, &fs_hdr);
        else
            se_frame_bottom_row(fs_hdr_scratch, width, &fs_hdr, y);
        fs_hdr_row_uyvy(row, fs_hdr_scratch, width);
    }
    fs_hdr_burned++;
}

static void fs_dcache_clean(void *p, uint32_t len)
{
    uint32_t a = (uint32_t)p & ~31u;
    uint32_t end = (uint32_t)p + len;
    for (; a < end; a += 32)
        asm volatile("mcr p15, 0, %0, c7, c11, 1" :: "r"(a) : "memory");
    /* DSB: the write-backs must complete before Canon reads the frame. */
    asm volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

int fs_yuvhook_dcache_used(void) { return fs_dcache_used; }

/*
 * Grade a frame through the cached alias, and hand it back to Canon.
 *
 * Both branches of the hook must come through here. Only the bare-pointer
 * branch used to, so a site that hands a memSuite instead would have graded
 * 40 MB through the uncached view: ~9.5 MB/s, i.e. ~8 s inside the shot. It
 * never ran on this body (every log so far shows a bare r0), which is exactly
 * why the difference went unnoticed.
 */
static uint32_t fs_yuv_grade_cached(void *buf, int width, int rows, int stride,
                                    int frame_h)
{
    uint32_t ba = (uint32_t)buf;
    unsigned char *gp = (unsigned char *)buf;
    uint32_t ms;

    /* 0x40000000-0x7FFFFFFF is the DRAM window; inside it CACHEABLE() is the
     * same RAM with bit 30 cleared. Outside it (ROM, low RAM) the pointer is
     * left alone -- clearing bit 30 there would point at something else. */
    if (ba >= 0x40000000u && ba < 0x80000000u)
        gp = (unsigned char *)CACHEABLE(buf);
    fs_dcache_used = (gp != (unsigned char *)buf);
    ms = fs_yuvlook_apply(gp, width, rows, stride);

    if (fs_bars_mode > 0) {
        int num, den;
        /* Geometry from the whole frame, not from the rows this pass graded:
         * a partial grade (film.sim.yuvrows) would put the bars in the wrong
         * place. If the two differ the bars are skipped rather than guessed. */
        if (rows == frame_h && fs_core_aspect_ratio(fs_bars_mode, &num, &den)) {
            fs_core_aspect_bars(gp, stride, width, 0, rows, num, den);
            fs_bars_burned++;
            if (fs_hdr.on && fs_hdr_scratch)
                fs_hdr_burn(gp, width, rows, stride, num, den);
        }
    }

    if (gp != (unsigned char *)buf)
        fs_dcache_clean(gp, (uint32_t)rows * stride);
    return ms;
}

static int fs_hook_shots = 0;

/* Still YUV capture path entry points for Canon 6D 1.1.6 */
#define FS_YUV_ADDR_STILL 0xFF3442DCu   /* sdfExecuteYuvToJpegPath, push {r0-r12, lr} (0xE92D4FFF) */
#define FS_YUV_ADDR_EEKO  0xFF59A758u   /* EekoShootJpegPath, push {r4-r8, lr} (0xE92D41F0)       */
#define FS_YUV_ADDR_SSRAW 0xFF16E930u   /* sssCompleteSsrawToYuv, push {r2-r8, lr} (0xE92D41FC)   */

#define FS_YUV_STRIDE 10944
#define FS_YUV_SAMPLE 4096
#define FS_YUV_SAMPLES 4
#define FS_YUV_CALLS 2

#define FS_YUV_IDLE 0
#define FS_YUV_WANT 1
#define FS_YUV_READY 2

static struct {
    uint32_t magic;
    uint32_t pc;
    uint32_t ncalls;                                  /* hook calls seen      */
    uint32_t nsamples;
    uint32_t len;                                     /* bytes per sample     */
    uint32_t regs[FS_YUV_CALLS][6];                   /* r0..r5 per call      */
    uint32_t row[FS_YUV_SAMPLES];                     /* source row sampled   */
    uint32_t src[FS_YUV_SAMPLES];
    uint32_t full[14];                                /* r0..r12, lr, 1st call */
    uint32_t ctx_at;
    uint32_t ctx_words[16];
} fs_yuv_hdr;

static unsigned char fs_yuv_sample[FS_YUV_SAMPLES][FS_YUV_SAMPLE];
static volatile int fs_yuv_state = FS_YUV_IDLE;
static volatile int fs_yuv_calls;
/* Per site: which of the two hook points is actually being called decides
 * whether a picture can even be graded in time.  sssCompleteSsrawToYuv is the
 * mRAW/sRAW -> YUV conversion completing, so it is the ideal place; the other
 * is the YUV -> JPEG path being started.  A single combined counter cannot
 * tell the two apart and one shot with these can. */
static volatile int fs_yuv_n_still, fs_yuv_n_ssraw;
static volatile uint32_t fs_yuv_last_pc;
static volatile uint32_t fs_yuv_last_reg[4];
/* per site: last r0..r3 and why the bare-pointer path did or did not grade */
static volatile uint32_t fs_yuv_site_reg[2][4];
static volatile int fs_yuv_site_diag[2];   /* 0 n/a, 1 graded, 2 not a picture,
                                            * 3 no stride, 4 nothing safe to write */
static volatile int fs_yuv_g_still, fs_yuv_g_ssraw;
static int fs_yuv_grade_rows;         /* 0 = do not grade                    */
static int fs_yuv_need_neutral;       /* 1 = only grade a Neutral capture    */
static volatile int fs_yuv_style_refused;  /* not graded: style not Neutral  */
static volatile uint32_t fs_yuv_grade_ms;
static volatile int fs_yuv_graded;   /* how many frames actually graded */
/* What the last grading call actually did. Without this the log can only say
 * "graded N rows" from the CONFIG value, which is how an earlier attempt
 * reported 3648 rows and 17 ms while the picture never changed. */
static volatile uint32_t fs_yuv_last_buf;
static volatile uint32_t fs_yuv_last_size;
static volatile int fs_yuv_last_w, fs_yuv_last_h, fs_yuv_last_stride;
static volatile int fs_yuv_nobuf;    /* hook ran, no frame buffer found */
static volatile int fs_yuv_last_rows;   /* rows actually graded */
static volatile uint32_t fs_yuv_unknown; /* size that matched no known plane */
static int fs_yuv_patched[4];
static char fs_yuv_note[160] = "yuv probe: not installed";

/* A frame buffer lives in the DRAM windows DIGIC 5 maps above 0x40000000.
 * Anything else -- a small integer, a code address, a length -- is skipped. */
static inline int fs_yuv_is_buf(uint32_t v)
{
    return (v & 3) == 0 && v >= 0x40000000u && v < 0x80000000u;
}

/*
 * Extract genuine DRAM YUV frame buffer from capture registers.
 * On Canon 6D 1.1.6:
 * - sdfExecuteYuvToJpegPath / sssCompleteSsrawToYuv passes struct memSuite* in r1.
 * - EekoShootJpegPath passes context in r0 where [r0+20] is struct memSuite*.
 * Dereferencing via GetFirstChunkFromSuite + GetMemoryAddressOfMemoryChunk yields the true
 * 0x4xxxxxxx DRAM frame buffer.
 */
/* The four YUV422 plane sizes this body produces, exactly. A match on one of
 * these is the only reliable way to find the frame: the suite also carries
 * scratch and stream buffers whose sizes are round hex numbers -- the first
 * attempt took the FIRST chunk and got 0x9C0000 and 0x1100000 bytes, which are
 * not any image and made the geometry guess change between two shots. */
static int fs_yuv_plane_geometry(uint32_t size, int *width, int *height,
                                int *stride)
{
    static const struct { uint32_t bytes; int w, h; } g[] = {
        { 5472u * 2u * 3648u, 5472, 3648 },   /* Large  */
        { 3648u * 2u * 2432u, 3648, 2432 },   /* Medium */
        { 2736u * 2u * 1824u, 2736, 1824 },   /* S1     */
        { 1920u * 2u * 1280u, 1920, 1280 },   /* S2     */
    };
    for (unsigned i = 0; i < sizeof g / sizeof g[0]; i++) {
        if (size == g[i].bytes) {
            if (width)  *width  = g[i].w;
            if (height) *height = g[i].h;
            if (stride) *stride = g[i].w * 2;
            return 1;
        }
    }
    return 0;
}


/*
 * Which chunk holds the picture?
 *
 * Size alone does not answer it.  GetSizeOfMemoryChunk() returns the
 * ALLOCATION size, rounded to 64 KB, so the frame arrives as 0x9C0000 or
 * 0x1100000 bytes -- neither of which is a plane this body produces -- and the
 * suite carries several chunks in the same range.  Two shots at identical
 * settings reported two different "largest chunk" sizes, which is what
 * different steps of one job chain look like.
 *
 * So look at the bytes.  Measured on a real dumped plane
 * (ML/FILMSIM/LV_RAW.BIN, 720x480): mean |chroma| 12, mean |dy| between
 * neighbouring pixels 1, luma spread 186.  Zeroed memory scores 0/0/0 and
 * random or uninitialised memory 64/84/255.  A buffer that passes these three
 * is a picture, so grading it cannot be grading scratch -- and grading scratch
 * is exactly what has been happening.
 *
 * Geometry comes from the plane that fits with the least slack: an allocation
 * is the plane plus a small overhead, so 0x9C0000 resolves to 2736x1824
 * (9,980,928) and 0x1100000 to 3648x2432 (17,743,872).
 */
/*
 * Is a pointer inside a window we may read?
 *
 * The suite and chunk structs live in ordinary RAM; only the pixels are in the
 * DRAM windows.  A pointer outside all of these is not worth dereferencing.
 */
static int fs_yuv_ptr_ok(uint32_t p)
{
    if (!p || (p & 3u))
        return 0;
    return (p < 0x20000000u)                       /* RAM, cached alias     */
        || (p >= 0x40000000u && p < 0x80000000u)   /* RAM, uncached alias   */
        || (p >= 0xFC000000u);                     /* ROM / DryOS image     */
}

/* DryOS tags every suite and every chunk with a signature string.  Until this
 * existed the walk accepted whatever r1 happened to point at, so "chunks 3"
 * in the log meant three structs of unknown provenance -- and their sizes
 * (14,450,688 / 9,601,024 / 1,605,632) match no plane this body produces, the
 * middle one being 4% too small for 2736x1824. */
static int fs_yuv_sig_is(uint32_t obj, const char *want)
{
    const char *sig;
    if (!fs_yuv_ptr_ok(obj))
        return 0;
    sig = *(const char *const *)obj;             /* first word of the struct */
    if (!fs_yuv_ptr_ok((uint32_t)sig))
        return 0;
    return strcmp(sig, want) == 0;
}

#define fs_yuv_is_suite(p)  fs_yuv_sig_is((uint32_t)(p), "MemSuite")
#define fs_yuv_is_chunk(p)  fs_yuv_sig_is((uint32_t)(p), "MemChunk")

/*
 * Is this buffer a picture?  Three statistics over sixteen slices spread
 * across it -- chroma magnitude, the step between neighbouring luma samples,
 * and the luma spread -- with NO assumption about the row stride, because the
 * stride is what we are trying to find and a wrong one still leaves these
 * three intact (measured: the same dumped frame scores 12/1/186 at its own
 * stride and 13/2/185 at a deliberately wrong one).
 *
 * Calibrated on ML/FILMSIM/LV_RAW.BIN:
 *     picture  12 /  1 / 186
 *     zeros     0 /  0 /   0
 *     random   64 / 84 / 255
 *
 * The previous version gated this on chunk size matching a plane in a table,
 * and every chunk of the suite failed that test before a single pixel was
 * read -- which is what "sniff 0x0" in the log actually meant.  It never
 * looked.
 */
static void fs_yuv_stats(const void *p, uint32_t bytes,
                         int *mc_out, int *md_out, int *spread_out)
{
    const unsigned char *b = (const unsigned char *)p;
    long long sc = 0, sd = 0, n = 0;
    int lo = 255, hi = 0, i, x;

    for (i = 0; i < 16; i++) {
        uint32_t off = (uint32_t)((long long)i * (bytes - 1024) / 16);
        const unsigned char *q = b + off;
        for (x = 0; x < 256; x++) {
            int u  = (int)(signed char)q[0], y0 = q[1];
            int v  = (int)(signed char)q[2], y1 = q[3];
            sc += (u < 0 ? -u : u) + (v < 0 ? -v : v);
            sd += (y1 > y0 ? y1 - y0 : y0 - y1);
            if (y0 < lo) lo = y0;
            if (y0 > hi) hi = y0;
            if (y1 < lo) lo = y1;
            if (y1 > hi) hi = y1;
            q += 4;
            n += 2;
        }
    }
    if (mc_out)     *mc_out     = (int)(sc / n);
    if (md_out)     *md_out     = (int)(sd / (n / 2));
    if (spread_out) *spread_out = hi - lo;
}

/* Bytes of one full plane at this stride, so a write can be bounded by the
 * image the stride implies and never run past it. */
static uint32_t fs_yuv_plane_bytes(uint32_t stride)
{
    switch (stride) {
    case 10944u: return 5472u * 2u * 3648u;
    case 7296u:  return 3648u * 2u * 2432u;
    case 5472u:  return 2736u * 2u * 1824u;
    case 3840u:  return 1920u * 2u * 1280u;
    default:     return 0u;
    }
}

static int fs_yuv_picture(const void *p, uint32_t bytes)
{
    int mc, md, sp;
    if (!p || bytes < 65536u)
        return 0;
    fs_yuv_stats(p, bytes, &mc, &md, &sp);
    /* Deliberately wide: a dark frame, a flat wall and a grainy ISO 6400 shot
     * must all pass.  What must NOT pass is zeroed memory (0/0/0) and random
     * memory (64/84/255). */
    return mc <= 64 && md <= 60 && sp >= 16;
}

/*
 * Does the buffer actually reach this row?
 *
 * The bare-pointer path has no size: r0 is a DRAM address and nobody hands us
 * a length.  Writing a guessed number of rows into a pointer of unknown extent
 * is how memory gets smashed during a shot, so before writing N rows, look at
 * the first row of the last chunk we intend to touch.  Unallocated or foreign
 * memory reads as zeros or as noise; a real frame reads as picture.
 */
static int fs_yuv_reaches(const void *p, uint32_t stride, uint32_t row)
{
    const unsigned char *q = (const unsigned char *)p + (size_t)row * stride;
    int zeros = 0, i, md = 0;
    for (i = 0; i < 128; i++) {
        int y0 = q[1], y1 = q[3];
        int d = y1 - y0;
        if (!q[0] && !q[1] && !q[2] && !q[3]) zeros++;
        md += d < 0 ? -d : d;
        q += 4;
    }
    if (zeros > 100) return 0;      /* blank memory */
    if (md / 128 > 70) return 0;    /* noise, not a picture */
    return 1;
}

/* Rows we may write at this stride: the requested count, one plane at most,
 * and only as far as the buffer demonstrably goes. */
static int fs_yuv_safe_rows(const void *p, uint32_t stride, uint32_t plane_bytes,
                            int want)
{
    int rows = (int)(plane_bytes / stride), i;
    if (want > 0 && want < rows) rows = want;
    for (i = 0; i < 6 && rows > 0; i++) {
        if (fs_yuv_reaches(p, stride, (uint32_t)(rows - 1)))
            return rows;
        rows /= 2;
    }
    return 0;
}

/*
 * Row stride, from the picture itself.
 *
 * At the true stride two consecutive rows are neighbours in the scene and
 * differ by very little; at a wrong one they are unrelated.  Measured on the
 * dumped frame: 2.54 at its own 1440-byte stride against 37-61 for the wrong
 * ones, so the winner is not a close call.
 */
static uint32_t fs_yuv_detect_stride(const void *p, uint32_t bytes, uint32_t limit)
{
    /*
     * Ascending, and ties keep the first: a stride that is exactly twice the
     * real one also lines rows up (the row two below is still the same part of
     * the scene), so the smaller of two equal scores is the real one.
     */
    static const uint32_t cand[] = { 3840u, 5472u, 7296u, 10944u };
    const unsigned char *b = (const unsigned char *)p;
    uint32_t best_s = 0, best = 0xFFFFFFFFu;
    unsigned k;

    for (k = 0; k < sizeof cand / sizeof cand[0]; k++) {
        uint32_t st = cand[k];
        long long tot = 0, n = 0;
        int i, x;
        if (limit && st > limit) continue;
        if (bytes / st < 24u) continue;
        for (i = 0; i < 16; i++) {
            uint32_t r = (uint32_t)((long long)(i + 1) * (bytes / st - 2) / 17);
            const unsigned char *a = b + (size_t)r * st;
            const unsigned char *c = a + st;
            for (x = 0; x < 128; x++) {
                int d = (int)a[x * 4 + 1] - (int)c[x * 4 + 1];
                tot += (d < 0 ? -d : d);
                n++;
            }
        }
        if (n) {
            uint32_t sc = (uint32_t)(tot / n);
            if (sc < best) { best = sc; best_s = st; }
        }
    }
    /* a picture has neighbouring rows within a few levels; noise does not */
    return best <= 20u ? best_s : 0;
}

#define FS_CHUNKS_MAX 8
static volatile uint32_t fs_yuv_chunk_addr[FS_CHUNKS_MAX];
static volatile uint32_t fs_yuv_chunk_size[FS_CHUNKS_MAX];
static volatile int fs_yuv_chunk_n;
static volatile int fs_yuv_chunk_used;
static volatile int fs_yuv_sniff_mask;   /* one bit per chunk that is a picture */
static volatile int fs_yuv_suite_at;     /* which argument was the suite: 0 r1, 1 r0, 2 r3, -1 none */
static volatile int fs_yuv_bare_pic;     /* 1 = the frame was a bare DRAM pointer, no suite */

int fs_yuvhook_chunks(void) { return fs_yuv_chunk_n; }
uint32_t fs_yuvhook_chunk_size(int i)
{
    return (i >= 0 && i < FS_CHUNKS_MAX) ? fs_yuv_chunk_size[i] : 0;
}
int fs_yuvhook_chunk_used(void) { return fs_yuv_chunk_used; }
int fs_yuvhook_sniff_mask(void) { return fs_yuv_sniff_mask; }
int fs_yuvhook_suite_at(void) { return fs_yuv_suite_at; }
int fs_yuvhook_bare_pic(void) { return fs_yuv_bare_pic; }
/*
 * ML's own picture-quality word, logged so the frame geometry can be checked
 * against a deterministic source instead of only against the stride measured
 * from the pixels.  Decoded the way lens.c does it for DIGIC 5; both candidate
 * fields are logged because the branch that applies depends on the body.
 */
int fs_yuvhook_pic_quality(void)
{
    extern int pic_quality;
    return pic_quality;
}

uint32_t fs_yuvhook_site_reg(int site, int i)
{
    return (site >= 0 && site < 2 && i >= 0 && i < 4) ? fs_yuv_site_reg[site][i] : 0;
}
int fs_yuvhook_site_diag(int site)
{
    return (site >= 0 && site < 2) ? fs_yuv_site_diag[site] : 0;
}

static void *fs_yuv_extract_buf(uint32_t *regs, uint32_t pc, uint32_t *out_size)
{
    struct memSuite *suite = NULL;
    struct memChunk *c;
    void *best = NULL;
    uint32_t best_sz = 0, cand[3];
    unsigned k;
    int i = 0;

    if (out_size) *out_size = 0;
    fs_yuv_chunk_n = 0;
    fs_yuv_chunk_used = -1;
    fs_yuv_sniff_mask = 0;
    fs_yuv_suite_at = -1;      /* not 0: 0 means "r1 was the suite" */

    /*
     * Three arguments are plausible carriers of the frame: r1 at the YUV->JPEG
     * dispatcher, r0 (0x4c394000 in the last log, a DRAM pointer we had never
     * looked at), and r3.  Whichever one actually carries a DryOS MemSuite is
     * the one we walk -- and the signature says which, instead of assuming.
     */
    cand[0] = regs[1];
    cand[1] = regs[0];
    cand[2] = (pc == FS_YUV_ADDR_EEKO) ? 0u : regs[3];
    if (pc == FS_YUV_ADDR_EEKO && regs[0] >= 0x1000u && (regs[0] & 3u) == 0)
        cand[0] = ((const uint32_t *)regs[0])[5];

    for (k = 0; k < 3; k++) {
        if (fs_yuv_is_suite(cand[k])) {
            suite = (struct memSuite *)cand[k];
            fs_yuv_suite_at = (int)k;
            break;
        }
    }
    if (!suite) {
        /* no argument is a suite: fall back to a plain DRAM frame pointer */
        for (k = 0; k < 3; k++)
            if (fs_yuv_is_buf(cand[k]))
                return (void *)cand[k];
        return NULL;
    }

    c = GetFirstChunkFromSuite(suite);
    while (c && i < FS_CHUNKS_MAX) {
        if (!fs_yuv_is_chunk(c))
            break;
        {
            void *a = GetMemoryAddressOfMemoryChunk(c);
            uint32_t sz = (uint32_t)GetSizeOfMemoryChunk(c);
            fs_yuv_chunk_addr[i] = (uint32_t)a;
            fs_yuv_chunk_size[i] = sz;
            if (fs_yuv_is_buf((uint32_t)a)) {
                if (fs_yuv_picture(a, sz))
                    fs_yuv_sniff_mask |= 1 << i;
                if (sz > best_sz) {
                    best = a;
                    best_sz = sz;
                    fs_yuv_chunk_used = i;
                }
            }
        }
        i++;
        fs_yuv_chunk_n = i;
        c = GetNextMemoryChunk(suite, c);
    }

    if (best && out_size)
        *out_size = best_sz;
    return best;
}

static void fs_yuv_diag_set(uint32_t pc, int v)
{
    if (pc == FS_YUV_ADDR_STILL) fs_yuv_site_diag[0] = v;
    else if (pc == FS_YUV_ADDR_SSRAW) fs_yuv_site_diag[1] = v;
}

/*
 * Grade once per version of the buffer, not once per address.
 *
 * Two calls of the chain hand us the SAME address (measured: ssraw r1 =
 * 0x4c394000, jpeg r0 = 0x4c394000), and a guard that skips on address alone
 * therefore skips the last call of the chain.  If Canon rewrites the buffer
 * between our grade and that last call -- which is exactly what a
 * "complete ... to Yuv" callback followed by more work looks like -- the graded
 * bytes are gone before anything reads them, and the saved JPEG shows nothing.
 *
 * So the skip now compares CONTENT: a 512-byte sample taken after we wrote.
 * Same address and same sample = nobody touched it, skip.  Same address with a
 * different sample = it was rewritten, grade it again.
 */
static uint32_t fs_yuv_finger(const void *p, uint32_t stride, int rows)
{
    const unsigned char *b = (const unsigned char *)p;
    uint32_t h = 2166136261u;
    int i, x;

    if (!b || stride < 8u || rows < 2)
        return 0;
    for (i = 0; i < 32; i++) {
        uint32_t r = (uint32_t)((long long)i * (rows - 1) / 31);
        const unsigned char *q = b + (size_t)r * stride;
        for (x = 0; x < 16; x++)
            h = (h ^ q[x * 4 + 1]) * 16777619u;
    }
    return h;
}

static uint32_t fs_yuv_done_addr, fs_yuv_done_finger, fs_yuv_done_stride;
static uint32_t fs_yuv_done_ms;

static int fs_yuv_should_grade(void *buf, uint32_t stride, int rows)
{
    uint32_t now = get_ms_clock();
    uint32_t f = fs_yuv_finger(buf, stride, rows);

    if ((uint32_t)buf == fs_yuv_done_addr && stride == fs_yuv_done_stride &&
        (now - fs_yuv_done_ms) < 5000u && f && f == fs_yuv_done_finger)
        return 0;
    return 1;
}

static void fs_yuv_note_graded(void *buf, uint32_t stride, int rows)
{
    fs_yuv_done_addr   = (uint32_t)buf;
    fs_yuv_done_stride = stride;
    fs_yuv_done_finger = fs_yuv_finger(buf, stride, rows);
    fs_yuv_done_ms     = get_ms_clock();
}

static void fs_yuv_hook_body(uint32_t *regs, uint32_t *stack, uint32_t pc);

/*
 * Time the WHOLE hook, not just the look.
 *
 * fs_yuvlook_apply() measures 817 ms on a 5472x3648 frame, yet with the hook
 * installed a shot takes ~10 s to reach the card and with it uninstalled the
 * same shot takes under 1 s. That gap can only be something else running inside
 * this callback -- the buffer search, the signature walk, the fingerprint -- so
 * the total has to be measured where the total happens.
 */
static int fs_hook_total_ms = 0;
static int fs_hook_total_max = 0;

static void fs_yuv_hook(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    uint32_t t0 = get_ms_clock();
    fs_yuv_hook_body(regs, stack, pc);
    fs_hook_total_ms = (int)(get_ms_clock() - t0);
    if (fs_hook_total_ms > fs_hook_total_max)
        fs_hook_total_max = fs_hook_total_ms;
}

int fs_yuvhook_hook_total_ms(void) { return fs_hook_total_ms; }
int fs_yuvhook_hook_total_max(void) { return fs_hook_total_max; }

static void fs_yuv_hook_body(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    static const uint32_t rows[FS_YUV_SAMPLES] = { 0, 400, 800, 1200 };
    uint32_t size = 0;
    void *buf;
    int i;

    (void)stack;

    fs_yuv_calls++;
    fs_yuv_last_pc = pc;
    fs_yuv_last_reg[0] = regs[0];
    fs_yuv_last_reg[1] = regs[1];
    fs_yuv_last_reg[2] = regs[2];
    fs_yuv_last_reg[3] = regs[3];
    if (pc == FS_YUV_ADDR_STILL || pc == FS_YUV_ADDR_SSRAW) {
        int si = (pc == FS_YUV_ADDR_STILL) ? 0 : 1;
        fs_yuv_site_reg[si][0] = regs[0];
        fs_yuv_site_reg[si][1] = regs[1];
        fs_yuv_site_reg[si][2] = regs[2];
        fs_yuv_site_reg[si][3] = regs[3];
    }
    if (pc == FS_YUV_ADDR_STILL) fs_yuv_n_still++;
    else if (pc == FS_YUV_ADDR_SSRAW) fs_yuv_n_ssraw++;

    buf = fs_yuv_extract_buf(regs, pc, &size);

    /*
     * Grade first, on every call, independent of the dump.
     *
     * Every chunk of the suite is offered to the sniffer, not just the largest
     * one and not only one whose allocation size happens to equal a plane:
     * those two rules between them selected 10,223,616 and 17,825,792 bytes of
     * scratch, graded them, and the saved JPEG never moved a pixel.  A chunk
     * that sniffs as a picture is graded with the geometry the sniff found.
     */
    if (!buf)
        fs_yuv_nobuf++;
    /*
     * The style check comes first, before anything is written.
     *
     * fs_yuv_grade_rows says the module *wants* the look in the shot; it does
     * not say the camera cooperated. If the forced Neutral did not take -- the
     * property call failed, or a style was selected afterwards -- Canon's
     * encoder has already applied that style to these very bytes, and grading
     * them again stacks two looks. Refuse, and say so with a code the log
     * carries, rather than produce a milky JPEG that looks like a soft lens.
     */
    if (fs_yuv_grade_rows > 0 && fs_yuv_need_neutral &&
        lens_info.raw_picstyle != 0x84) {
        fs_yuv_style_refused++;
        fs_yuv_diag_set(pc, 7);
        goto bare_done;
    }
    if (fs_yuv_grade_rows > 0 && fs_yuvlook_ready()) {
        int ci;
        if (fs_yuv_chunk_n == 0 && buf && size) {
            /* a site that hands a plain buffer instead of a suite */
            fs_yuv_chunk_addr[0] = (uint32_t)buf;
            fs_yuv_chunk_size[0] = size;
            fs_yuv_chunk_n = 1;
        } else if (fs_yuv_chunk_n == 0 && buf) {
            /*
             * No argument is a memSuite, so the frame is a bare DRAM pointer
             * (r0 = 0x4c394000 in the last log).  Identify it by its contents,
             * measure the stride, and write only as far as the buffer
             * demonstrably reaches.
             */
            uint32_t win = 1024u * 1024u;
            uint32_t st, pb;
            int r;

            if (!fs_yuv_picture(buf, win)) {
                fs_yuv_diag_set(pc, 2);
                goto bare_done;
            }
            if ((st = fs_yuv_detect_stride(buf, win, win)) == 0) {
                fs_yuv_diag_set(pc, 3);
                goto bare_done;
            }
            /* Stamp the moment the frame reached the hook: the save task is
             * gone, so this is the only clock that can say how long the camera
             * then takes to put the file on the card. */
            fs_hook_ms = get_ms_clock();
            fs_hook_shots++;
            if ((pb = fs_yuv_plane_bytes(st)) == 0 ||
                (r = fs_yuv_safe_rows(buf, st, pb, fs_yuv_grade_rows)) <= 0) {
                fs_yuv_diag_set(pc, 4);
                goto bare_done;
            }
            if (fs_yuv_grade_rows <= 0) {
                /* not grading: still record where the frame is, so the save
                 * path can write it out and be compared with Canon's JPEG */
                fs_yuv_last_buf    = (uint32_t)buf;
                fs_yuv_last_size   = pb;
                fs_yuv_last_w      = (int)(st / 2);
                fs_yuv_last_h      = (int)(pb / st);
                fs_yuv_last_stride = (int)st;
                fs_yuv_last_rows   = r;
                fs_yuv_diag_set(pc, 6);
                goto bare_done;
            }
            if (!fs_yuv_should_grade(buf, st, r)) { fs_yuv_diag_set(pc, 5); goto bare_done; }
            fs_yuv_diag_set(pc, 1);
            {
                fs_yuv_last_buf    = (uint32_t)buf;
                fs_yuv_last_size   = pb;
                fs_yuv_last_w      = (int)(st / 2);
                fs_yuv_last_h      = (int)(pb / st);
                fs_yuv_last_stride = (int)st;
                fs_yuv_last_rows   = r;
                {
                    fs_yuv_grade_ms = fs_yuv_grade_cached(
                        buf, (int)(st / 2), r, (int)st, (int)(pb / st));
                }
                fs_yuv_graded++;
                if (pc == FS_YUV_ADDR_STILL) fs_yuv_g_still++;
                else if (pc == FS_YUV_ADDR_SSRAW) fs_yuv_g_ssraw++;
                fs_yuv_note_graded(buf, st, r);
                fs_yuv_bare_pic = 1;
            }
bare_done:
            fs_yuv_chunk_n = 0;
            goto graded_done;
        }
        for (ci = 0; ci < fs_yuv_chunk_n; ci++) {
            uint32_t a  = fs_yuv_chunk_addr[ci];
            uint32_t sz = fs_yuv_chunk_size[ci];
            uint32_t stride, pb;
            int r;

            if (!fs_yuv_is_buf(a)) continue;
            /* Never grade a chunk that is not a picture.  Without this the
             * hook writes 10-40 MB of "graded" bytes into scratch memory. */
            if (!fs_yuv_picture((const void *)a, sz)) {
                fs_yuv_unknown = sz;
                continue;
            }
            stride = fs_yuv_detect_stride((const void *)a, sz, 0);
            if (!stride) { fs_yuv_unknown = sz; continue; }
            pb = fs_yuv_plane_bytes(stride);
            if (!pb) { fs_yuv_unknown = sz; continue; }
            if (sz < pb) pb = sz;
            r = fs_yuv_safe_rows((const void *)a, stride, pb, fs_yuv_grade_rows);
            if (r <= 0) { fs_yuv_unknown = sz; continue; }
            if (!fs_yuv_should_grade((void *)a, stride, r)) continue;

            fs_yuv_last_buf    = a;
            fs_yuv_last_size   = sz;
            fs_yuv_last_w      = (int)(stride / 2);
            fs_yuv_last_h      = (int)(pb / stride);
            fs_yuv_last_stride = (int)stride;
            fs_yuv_last_rows   = r;

            fs_yuv_grade_ms = fs_yuv_grade_cached((void *)a, (int)(stride / 2),
                                                  r, (int)stride,
                                                  (int)(pb / stride));
            fs_yuv_graded++;
            if (pc == FS_YUV_ADDR_STILL) fs_yuv_g_still++;
            else if (pc == FS_YUV_ADDR_SSRAW) fs_yuv_g_ssraw++;
            fs_yuv_note_graded((void *)a, stride, r);
            fs_yuv_chunk_used = ci;
        }
graded_done:
        ;
    }

    if (fs_yuv_state != FS_YUV_WANT)
        return;

    /* Keep recording calls until the budget is spent, then hand over. */
    if (fs_yuv_hdr.ncalls < FS_YUV_CALLS) {
        for (i = 0; i < 6; i++)
            fs_yuv_hdr.regs[fs_yuv_hdr.ncalls][i] = regs[i];
        fs_yuv_hdr.ncalls++;
    }
    if (fs_yuv_hdr.ncalls < FS_YUV_CALLS)
        return;

    fs_yuv_state = FS_YUV_IDLE;
    fs_yuv_hdr.magic = 0x56595346;   /* "FSYV" */
    fs_yuv_hdr.pc    = pc;
    fs_yuv_hdr.len   = FS_YUV_SAMPLE;
    for (i = 0; i < 14; i++)
        fs_yuv_hdr.full[i] = regs[i];

    fs_yuv_hdr.ctx_at = regs[0] + 0x3000u;
    if (fs_yuv_is_buf(fs_yuv_hdr.ctx_at)) {
        for (i = 0; i < 16; i++)
            fs_yuv_hdr.ctx_words[i] = ((const uint32_t *)fs_yuv_hdr.ctx_at)[i];
    }

    if (!buf) {
        fs_yuv_hdr.nsamples = 0;
        fs_yuv_state = FS_YUV_READY;
        return;
    }
    {
        int width = 5472, height = 3648, stride = 10944;
        if (!fs_yuv_plane_geometry(size, &width, &height, &stride)) {
            width = 5472; height = 3648; stride = 10944;
        }
        for (i = 0; i < FS_YUV_SAMPLES; i++) {
            uint32_t at = (uint32_t)buf + rows[i] * stride;
            if (!fs_yuv_is_buf(at))
                break;
            fs_yuv_hdr.row[i] = rows[i];
            fs_yuv_hdr.src[i] = at;
            memcpy(fs_yuv_sample[i], (const void *)at, FS_YUV_SAMPLE);
            fs_yuv_hdr.nsamples = (uint32_t)i + 1;
        }
    }

    fs_yuv_state = FS_YUV_READY;     /* last: the task polls this */
}

/*
 * Is this the start of a function that saves registers and will return?
 *
 * Patching is only safe on a real entry point, and the word also says whether
 * this firmware's version of the function is shaped like the one in Florin's
 * build. ML's own old-value check caught the first attempt (0x8,
 * E_PATCH_OLD_VALUE_MISMATCH), which is exactly what that check is for -- but
 * it could not say what was actually there, so that is recorded now.
 */

static int fs_yuv_entry_ok(uint32_t w);   /* defined below */

/*
 * The still-JPEG path, identified in the 6D 1.1.6 ROM (ROM1.BIN at 0xFF000000,
 * confirmed against the live reads of both old sites).
 *
 * At 0xFF16CFB8 the firmware enters the function that logs
 *   sssExecuteMem1ToJpegPath(%d)Address=%#X
 * with `push {r4,r5,r6,r7,lr}` (0xE92D40F0, lr included -- patchable). Two
 * instructions later it loads r4 from the literal at 0xFF16CD4C, which holds
 * 0x0009F9D8, and prints [r4+0x88] as that Address. So the address handed to
 * the JPEG encoder for a still is a word in RAM at 0x0009FA60, and the job's
 * other fields sit next to it.
 *
 * This probe only READS. Nothing is graded and nothing is written until the
 * geometry is known from what the camera itself puts in those words.
 */
#define FS_MEM1_ADDR   0xFF16CFB8u
#define FS_MEM1_BASE   0x0009F9D8u
#define FS_MEM1_WORDS  40
#define FS_MEM1_SNAPS  4

static volatile uint32_t fs_mem1[FS_MEM1_SNAPS][FS_MEM1_WORDS];
static volatile uint32_t fs_mem1_regs[FS_MEM1_SNAPS][4];
static volatile int fs_mem1_calls = 0;
static volatile uint32_t fs_mem1_ret = 0;

static void fs_mem1_hook(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    (void)stack;
    (void)pc;
    /*
     * One shot calls this more than once (DCF resize, thumbnail), and the
     * address in the struct is not necessarily the same at every call -- so up
     * to four calls are kept rather than only the first. The task writes them
     * afterwards; nothing here opens a file.
     */
    fs_mem1_calls++;
    if (fs_mem1_calls <= FS_MEM1_SNAPS)
    {
        int s = fs_mem1_calls - 1;
        int i;
        fs_mem1_ret = regs[13];            /* r0..r12 then lr: lr is [13] */
        for (i = 0; i < 4; i++)
            fs_mem1_regs[s][i] = regs[i];
        for (i = 0; i < FS_MEM1_WORDS; i++)
            fs_mem1[s][i] = *(volatile uint32_t *)(FS_MEM1_BASE + 4u * (uint32_t)i);
    }
}

/*
 * sasEncodeJpeg -- the still-JPEG encoder itself.
 *
 * 0xFF16A8DC is its entry (push {r4,r5,r6,r7,r8,r9,sl,lr} = 0xE92D47F0, lr
 * included) and its first act is DryosDebugMsg("sasEncodeJpeg(...)"), so unlike
 * the two addresses inherited from Florin's build this one is confirmed to be
 * on the path of every still: the string is at 0xFF16A8D0 and the code that
 * uses it at 0xFF16A8F8.
 *
 * r0 is a pointer to the encode job (`mov r4, r0`, then [r0+0x10], [r0+0x14]),
 * so the words around it are where the source plane, the dimensions and the
 * quality live. The probe reads them and nothing else.
 *
 * The first version of this probe used 0xFF3FE990, the call made from
 * sssExecuteMem1ToJpegPath -- that one never fired on a still (MEM1.TXT was
 * never written while a JPEG was), so that whole path is not what this body
 * uses for a photograph.
 */
#define FS_ENC_ADDR   0xFF16A8DCu
#define FS_ENC_WORDS  40
#define FS_ENC_SNAPS  4

static volatile uint32_t fs_enc[FS_ENC_SNAPS][FS_ENC_WORDS];
static volatile uint32_t fs_enc_regs[FS_ENC_SNAPS][4];
static volatile int fs_enc_calls = 0;

static void fs_enc_hook(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    (void)stack;
    (void)pc;
    fs_enc_calls++;
    if (fs_enc_calls <= FS_ENC_SNAPS)
    {
        int s = fs_enc_calls - 1;
        uint32_t p = regs[0];
        int i;
        for (i = 0; i < 4; i++)
            fs_enc_regs[s][i] = regs[i];
        /* DRAM only, word aligned; a code address or a small integer is not a
         * parameter block and must not be dereferenced. */
        if (p >= 0x00010000u && p < 0x80000000u && (p & 3u) == 0)
            for (i = 0; i < FS_ENC_WORDS; i++)
                fs_enc[s][i] = *(volatile uint32_t *)(p + 4u * (uint32_t)i);
        else
            for (i = 0; i < FS_ENC_WORDS; i++)
                fs_enc[s][i] = 0xDEADBEEFu;
    }
}

int fs_yuvhook_enc_calls(void) { return fs_enc_calls; }

int fs_yuvhook_mem1_calls(void) { return fs_mem1_calls; }
int fs_yuvhook_mem1_ready(void) { return fs_mem1_calls > 0 || fs_enc_calls > 0; }

int fs_yuvhook_install_mem1(void)
{
    uint32_t w = *(volatile uint32_t *)FS_MEM1_ADDR;
    int e;

    fs_yuv_patched[2] = 0;
    if (!fs_yuv_entry_ok(w))
    {
        snprintf(fs_yuv_note, sizeof fs_yuv_note,
                 "mem1 probe: %08x is %08x, not a push-with-lr entry",
                 FS_MEM1_ADDR, w);
        return E_PATCH_UNKNOWN_ERROR;
    }
    e = patch_hook_function(FS_MEM1_ADDR, w, fs_mem1_hook, "fsmem1");
    if (!e)
    {
        fs_yuv_patched[2] = 1;
        fs_mem1_calls = 0;
    }
    {
        uint32_t we = *(volatile uint32_t *)FS_ENC_ADDR;
        fs_yuv_patched[3] = 0;
        if (fs_yuv_entry_ok(we) &&
            !patch_hook_function(FS_ENC_ADDR, we, fs_enc_hook, "fsenc"))
        {
            fs_yuv_patched[3] = 1;
            fs_enc_calls = 0;
        }
    }
    snprintf(fs_yuv_note, sizeof fs_yuv_note,
             "mem1 probe: patched %08x (%08x), call count %d",
             FS_MEM1_ADDR, w, fs_mem1_calls);
    return e;
}

int fs_yuvhook_mem1_write(const char *path)
{
    FILE *f;
    int i, s;

    if (fs_mem1_calls <= 0 && fs_enc_calls <= 0) return 0;
    f = FIO_CreateFile(path);
    if (!f) return 0;
    {
        char line[96];
        int n = snprintf(line, sizeof line,
                         "calls %d lr %08x base %08x\n",
                         fs_mem1_calls, fs_mem1_ret, FS_MEM1_BASE);
        FIO_WriteFile(f, line, n);
    }
    {
        char line[96];
        int n = snprintf(line, sizeof line, "encoder calls %d (r0 = parameter block)\n",
                         fs_enc_calls);
        FIO_WriteFile(f, line, n);
    }
    for (s = 0; s < FS_ENC_SNAPS && s < fs_enc_calls; s++)
    {
        char line[96];
        int n = snprintf(line, sizeof line,
                         "enc %d  r0=%08x r1=%08x r2=%08x r3=%08x\n",
                         s, fs_enc_regs[s][0], fs_enc_regs[s][1],
                         fs_enc_regs[s][2], fs_enc_regs[s][3]);
        FIO_WriteFile(f, line, n);
        for (i = 0; i < FS_ENC_WORDS; i++)
        {
            n = snprintf(line, sizeof line, "  +%03x  %08x\n", i * 4, fs_enc[s][i]);
            FIO_WriteFile(f, line, n);
        }
    }
    for (s = 0; s < FS_MEM1_SNAPS && s < fs_mem1_calls; s++)
    {
        char line[96];
        int n = snprintf(line, sizeof line,
                         "snap %d  r0=%08x r1=%08x r2=%08x r3=%08x\n",
                         s, fs_mem1_regs[s][0], fs_mem1_regs[s][1],
                         fs_mem1_regs[s][2], fs_mem1_regs[s][3]);
        FIO_WriteFile(f, line, n);
        for (i = 0; i < FS_MEM1_WORDS; i++)
        {
            n = snprintf(line, sizeof line, "  +%03x  %08x\n",
                         i * 4, fs_mem1[s][i]);
            FIO_WriteFile(f, line, n);
        }
    }
    FIO_CloseFile(f);
    return 1;
}

static int fs_yuv_entry_ok(uint32_t w)
{
    return (w & 0xFFFF0000u) == 0xE92D0000u   /* STMDB sp!, {..} */
        && (w & 0x00004000u);                 /* ... including lr   */
}

int fs_yuvhook_install(void)
{
    uint32_t wst = *(volatile uint32_t *)FS_YUV_ADDR_STILL;
    uint32_t wsr = *(volatile uint32_t *)FS_YUV_ADDR_SSRAW;
    int est = -1, esr = -1;

    fs_yuv_patched[0] = fs_yuv_patched[1] = 0;

    /*
     * Patch the main still-JPEG path: sdfExecuteYuvToJpegPath (0xFF3442DC).
     * This is the function Canon calls for still captures before encoding to JPEG.
     */
    if (fs_yuv_entry_ok(wst)) {
        est = patch_hook_function(FS_YUV_ADDR_STILL, wst, fs_yuv_hook, "fsyuvStill");
        if (!est)
            fs_yuv_patched[0] = 1;
    }

    /*
     * Also patch sRAW completion path: sssCompleteSsrawToYuv (0xFF16E930).
     */
    if (fs_yuv_entry_ok(wsr)) {
        esr = patch_hook_function(FS_YUV_ADDR_SSRAW, wsr, fs_yuv_hook, "fsyuvSsraw");
        if (!esr)
            fs_yuv_patched[1] = 1;
    }

    snprintf(fs_yuv_note, sizeof fs_yuv_note,
             "yuv hook: still(%08x)=%d ssraw(%08x)=%d",
             FS_YUV_ADDR_STILL, est, FS_YUV_ADDR_SSRAW, esr);

    if (!fs_yuv_patched[0] && !fs_yuv_patched[1])
        return est ? est : E_PATCH_UNKNOWN_ERROR;

    fs_yuv_hdr.ncalls = 0;
    fs_yuv_state = FS_YUV_WANT;
    return 0;
}

void fs_yuvhook_uninstall(void)
{
    if (fs_yuv_patched[0]) {
        unpatch_memory(FS_YUV_ADDR_STILL);
        fs_yuv_patched[0] = 0;
    }
    if (fs_yuv_patched[1]) {
        unpatch_memory(FS_YUV_ADDR_SSRAW);
        fs_yuv_patched[1] = 0;
    }
    if (fs_yuv_patched[2]) {
        unpatch_memory(FS_MEM1_ADDR);
        fs_yuv_patched[2] = 0;
    }
    if (fs_yuv_patched[3]) {
        unpatch_memory(FS_ENC_ADDR);
        fs_yuv_patched[3] = 0;
    }
    fs_yuv_state = FS_YUV_IDLE;
    snprintf(fs_yuv_note, sizeof fs_yuv_note, "yuv probe: removed");
}

void fs_yuvhook_grade(int rows) { fs_yuv_grade_rows = rows; }
/* What the hook was armed with. 0 means the grade is off, and every other
 * counter in the summary then stays 0 -- which reads exactly like "the hook
 * never ran". A shot that did not grade has to be able to say that. */
int  fs_yuvhook_grade_rows(void) { return fs_yuv_grade_rows; }
void fs_yuvhook_need_neutral(int on) { fs_yuv_need_neutral = on ? 1 : 0; }
int  fs_yuvhook_style_refused(void) { return fs_yuv_style_refused; }

/*
 * When set, the frame is copied into memory the module owns while we are still
 * inside the hook. The encode that writes the _F file runs 6-15 s later, by
 * which time Canon has given that 40 MB back and put its LiveView planes inside
 * it: measured, 64 rows of noise ending at output row 415, 1,400,832 bytes
 * against 2 x 691,200 for two LV planes, and Canon's own JPEG of the same frame
 * is clean there. 0 disables the copy.
 */
uint32_t fs_yuvhook_hook_ms(void) { return fs_hook_ms; }
int fs_yuvhook_hook_shots(void) { return fs_hook_shots; }
uint32_t fs_yuvhook_grade_ms(void) { return fs_yuv_grade_ms; }
int fs_yuvhook_graded(void) { return fs_yuv_graded; }
int fs_yuvhook_nobuf(void) { return fs_yuv_nobuf; }
int fs_yuvhook_last_rows(void) { return fs_yuv_last_rows; }
uint32_t fs_yuvhook_unknown(void) { return fs_yuv_unknown; }
uint32_t fs_yuvhook_last_buf(void) { return fs_yuv_last_buf; }
uint32_t fs_yuvhook_last_size(void) { return fs_yuv_last_size; }
int fs_yuvhook_last_w(void) { return fs_yuv_last_w; }
int fs_yuvhook_last_h(void) { return fs_yuv_last_h; }
int fs_yuvhook_last_stride(void) { return fs_yuv_last_stride; }

int fs_yuvhook_sites(void) { return fs_yuv_patched[0] + fs_yuv_patched[1] + fs_yuv_patched[2] + fs_yuv_patched[3]; }
int fs_yuvhook_calls(void) { return fs_yuv_calls; }
int fs_yuvhook_site_calls(int ssraw)
{
    return ssraw ? fs_yuv_n_ssraw : fs_yuv_n_still;
}
uint32_t fs_yuvhook_last_pc(void) { return fs_yuv_last_pc; }
uint32_t fs_yuvhook_last_reg(int i)
{
    return (i >= 0 && i < 4) ? fs_yuv_last_reg[i] : 0;
}
int fs_yuvhook_site_graded(int ssraw)
{
    return ssraw ? fs_yuv_g_ssraw : fs_yuv_g_still;
}
const char *fs_yuvhook_note(void) { return fs_yuv_note; }
int fs_yuvhook_ready(void) { return fs_yuv_state == FS_YUV_READY; }

int fs_yuvhook_write(const char *path)
{
    FILE *f;
    int i;

    if (fs_yuv_state != FS_YUV_READY)
        return 0;
    f = FIO_CreateFile(path);
    if (!f)
        return 0;
    FIO_WriteFile(f, &fs_yuv_hdr, sizeof fs_yuv_hdr);
    for (i = 0; i < (int)fs_yuv_hdr.nsamples; i++)
        FIO_WriteFile(f, fs_yuv_sample[i], FS_YUV_SAMPLE);
    FIO_CloseFile(f);

    fs_yuv_state = FS_YUV_IDLE;
    snprintf(fs_yuv_note, sizeof fs_yuv_note,
             "yuvprobe: pc %08x calls %d src %08x rows %d grade_ms %d",
             fs_yuv_hdr.pc, (int)fs_yuv_hdr.ncalls, fs_yuv_hdr.src[0],
             (int)fs_yuv_hdr.nsamples, (int)fs_yuv_grade_ms);
    return 1;
}
