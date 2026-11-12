/*
 * se_cam.c - the Soup Engine develop, running on the camera.
 *
 * Split from film_sim.c so that the engine has no Magic Lantern dependency
 * beyond the four file callbacks at the bottom of this file. Everything
 * above them is plain C and is the same code the host build runs, which is
 * why the camera output can be compared byte for byte against the desktop.
 */

#include <stdlib.h>

#include <dryos.h>
#include <fio-ml.h>
#include <mem.h>

#include "se_cam.h"
#include "frame.h"
#include "engine/se.h"
#include "engine/se_libc.h"
#include "engine/cr2.h"
#include "engine/look.h"
#include "engine/lut.h"
#include "engine/stock.h"
#include "engine/jpeg_dec.h"
#include "engine/jpeg_enc.h"
#include "engine/pipeline.h"

#ifdef FS_QEMU_TEST
/* Emulation-only breadcrumb: which step failed, when there is no console. */
const char *se_cam_last_step = "start";
#define SE_STEP(x) do { se_cam_last_step = (x); } while (0)
#else
#define SE_STEP(x) do { } while (0)
#endif

/* Measured on a 6D CR2: se_cr2_parse needs 128 KB, so 256 KB has margin.
 * A 4096x4096 frame at 12 bits with a big EXIF block still fits. */
#define SE_WINDOW  (256 * 1024)

/* 420 KB measured peak for look + decoder on 5472x3648, plus the encoder
 * strip (8 * 5472 * 3 = 131 KB) which now comes out of the same arena,
 * plus the stock file. 1 MB leaves room for a taller frame. */
/*
 * Decode + look arena.
 *
 * The engine's own accounting (engine/look.h) puts the camera's worst case at
 * 528 KB, of which the look gets 192 KB and the decoder the rest. This was a
 * whole megabyte, which made the single malloc below 1,376 KB with the window,
 * the output buffer and the recipe -- 2.6x what the work needs, and a single
 * contiguous block that size is exactly what fails on a body already holding
 * the raw buffer. 640 KB leaves the documented worst case plus slack.
 */
#define SE_ARENA   (384 * 1024)
#define SE_VIEW_ARENA (256 * 1024)
#define SE_VIEW_HDR   (24 * 1024)   /* whole APP1 (12 KB) plus the tables */

#define SE_STOCK_MAX (64 * 1024)

/* Decode scale handed to the JPEG decoder: 2 = half size. */
#define SE_DECODE_SCALE 2

/* Output is double-buffered and written in blocks, the way Soup Engine's
 * camera develop does it.  A write per row from the encoder stalls the card
 * layer; 16 KB blocks flush cleanly and cost nothing. */
#define SE_OUTHALF (16 * 1024)
#define SE_OUTBUF  (SE_OUTHALF * 2)

/*
 * Writes go out through a DMA staging buffer, for the same reason reads come in
 * through one.
 *
 * FIO_WriteFile() with an ordinary cacheable pointer calls sync_caches(), i.e.
 * flushes the whole data cache, before every call (src/fio-ml.c). Writing the
 * file 16 KB at a time therefore flushed the cache 256 times during a develop
 * -- and every flush threw away whatever the decoder and the look had cached,
 * which they then had to fetch again. A DMA buffer is already uncacheable, so
 * the flush is skipped, and staging in 64 KB cuts the card calls by four.
 */
#define SE_OUTDMA (64 * 1024)

#define SE_THUMB_W       160
#define SE_THUMB_H       120
#define SE_THUMB_MAXH    120
#define SE_THUMB_JPEGMAX 16384

/*
 * Thumbnail workspace.
 *
 * File scope, NOT a member of struct dev. struct dev is a *stack* local in
 * se_cam_develop(), and these buffers are about 100 KB: on the camera that
 * overflowed the task stack and the body restarted in the middle of the
 * develop ("Waiting for the card" forever, then a fresh boot in the log). On
 * the host there is 8 MB of stack, so the host test never noticed.
 */
struct thumbws {
    unsigned char app1[SE_JPEG_EXIF_RESERVE];
    unsigned char jpeg[SE_THUMB_JPEGMAX];
    unsigned char strip[SE_JPEG_MCU_ROWS * SE_THUMB_W * 3];
    unsigned char canvas[SE_THUMB_W * SE_THUMB_H * 3];
    se_jpeg enc;
};

struct dev {
    FILE     *in;         /* the CR2                                       */
    FILE     *out;        /* the JPEG                                      */
    uint32_t  base;       /* file offset of the preview JPEG               */
    se_jpeg  *enc;
    se_look_run *run;
    int       width;
    int       height;
    int       rows_done;
    int       crop_x0, crop_x1;   /* framing window, columns kept */
    int       crop_y0, crop_y1;   /* framing window, rows kept */
    int       tgt_w, tgt_h;       /* delivery size, 0 = keep full resolution */
    int       src_w, src_h;       /* the cropped window, the resampler input */
    int       last_oy;
    int       row_in;             /* rows offered by the look so far */
    long     *err;
    se_cam_progress_fn progress;
    se_cam_note_fn     note;
    void     *ctx;
    int       last_pct;
    int       bar_pct;
    int       short_reads;         /* highest percentage shown: the bar never goes back */

    /* Exif thumbnail: sampled while the picture streams past, so the file can
     * carry an IFD1 without a second decode. The workspace is the file-scope
     * thumb_ws below -- never put it here, see the note on struct thumbws. */
    unsigned char *thumb;     /* points into thumb_ws.canvas */
    int       tw, th;
    int       th_step;        /* source rows per thumbnail row */
    int       th_y0;          /* top of the picture inside the 160x120 canvas */
    int       th_x0;          /* left edge of the picture inside the canvas */

    /* where the input file position is, so forward reads skip the seek */
    int32_t   rd_pos;

    /* block-buffered output */
    unsigned char *obuf[2];
    int       ocur;
    size_t    oused;
    int       ofail;
};

/* One develop at a time, guarded by the caller's fs_develop_active flag. */
static struct thumbws thumb_ws;

/* ------------------------------------------------------------- callbacks */

/*
 * FIO_ReadFile() asserts "count <= 8192" whenever the destination is an
 * ordinary (cacheable) buffer -- see src/fio-ml.c. The engine asks for 256 KB
 * windows and whatever chunk the JPEG decoder wants, so on a camera whose
 * cache hacks are off every develop died with an ML ASSERT and a half-written
 * .TMP. Reading in 8 KB pieces costs nothing and works in both configurations.
 */
#define SE_READ_CHUNK 8192
static int      se_read(FILE *f, void *dst, size_t want);

void se_cam_lut_path(const char *p);
static const se_lut3 *lut_get(void);
static uint32_t se_now_ms(void);
static uint32_t t_io;                  /* time spent waiting for the card */

/*
 * FIO_ReadFile() refuses more than 8 KB when the destination is ordinary
 * cacheable memory (src/fio-ml.c asserts, and asserts are fatal). A DMA buffer
 * -- fio_malloc -- has no such limit, so reading through one lets a develop ask
 * for 64 KB at a time instead of 8: about 500 card calls for a 4 MB preview
 * become about 64, and each one stops paying for the firmware's internal
 * allocate-and-copy dance.
 */
#define SE_BOUNCE (64 * 1024)
static void *se_bounce;

static int se_bounce_alloc(void)
{
#ifdef SE_HOSTED
    return -1;                    /* no DMA pool on the host: use se_read */
#else
    if (!se_bounce) se_bounce = fio_malloc(SE_BOUNCE);
    return se_bounce ? 0 : -1;
#endif
}

static void se_bounce_free(void)
{
#ifndef SE_HOSTED
    if (se_bounce) { free(se_bounce); se_bounce = NULL; }
#endif
}

static int se_read_io(FILE *f, void *dst, size_t want)
{
    unsigned char *d = (unsigned char *)dst;
    size_t done = 0;

    if (!se_bounce && se_bounce_alloc() != 0) return se_read(f, dst, want);
    while (done < want) {
        size_t c = want - done;
        int got;
        if (c > SE_BOUNCE) c = SE_BOUNCE;
        got = FIO_ReadFile(f, se_bounce, c);
        if (got <= 0) break;
        memcpy(d + done, se_bounce, (size_t)got);
        done += (size_t)got;
    }
    return (int)done;
}

static int se_read(FILE *f, void *dst, size_t want)
{
    unsigned char *p = (unsigned char *)dst;
    size_t done = 0;
    while (done < want) {
        size_t c = want - done;
        if (c > SE_READ_CHUNK) c = SE_READ_CHUNK;
        int got = FIO_ReadFile(f, p + done, c);
        if (got <= 0) break;
        done += (size_t)got;
        /* A short return is not EOF on the camera; keep going until the
         * request is satisfied or the file really ends (got == 0). */
    }
    return (int)done;
}

static int cr2_read(void *ctx, uint32_t off, void *dst, uint32_t n)
{
    struct dev *d = (struct dev *)ctx;
    unsigned char *p = (unsigned char *)dst;

    /*
     * The decoder only ever reads forwards, in 8 KB steps, so seek only when
     * the file is not already where the request starts. That removes about
     * five hundred seeks per develop; FIO_ReadFile cannot be asked for more
     * than 8 KB at a time (src/fio-ml.c asserts on cacheable destinations),
     * so the seeks were the only thing left to remove.
     */
    if (d->rd_pos != (int32_t)(d->base + off)) {
        if (FIO_SeekSkipFile(d->in, (int64_t)d->base + off, SEEK_SET) < 0)
            return -1;
    }

    /*
     * A short read is not a failure on the camera: FIO can return less than
     * asked for on a large request, and the decoder treats any short return as
     * fatal, so a single one failed the whole develop for no reason. Read on
     * until the request is satisfied, and log the first one.
     */
    {
        uint32_t io0 = se_now_ms();
        int got = se_read_io(d->in, p, n);
        t_io += se_now_ms() - io0;
        d->rd_pos = (int32_t)(d->base + off) + got;
        if ((uint32_t)got < n) {
            if (!d->short_reads)
                se_log("develop: short read %d of %d bytes", got, (int)n);
            d->short_reads++;
            return -1;
        }
    }
    return 0;
}

/* --- output staging ------------------------------------------------------ */
/*
 * Small p buffer in, big DMA buffer out. The copy is 64 KB of memcpy per card
 * call and buys the skipped cache flush and a quarter of the calls.
 */
static void    *se_outdma;
static size_t   se_outdma_len;
static uint32_t se_wr_calls, se_wr_bytes, se_wr_ms;

static int se_out_start(void)
{
#ifdef SE_HOSTED
    return -1;                       /* no DMA pool on the host */
#else
    if (!se_outdma) se_outdma = fio_malloc(SE_OUTDMA);
    se_outdma_len = 0;
    return se_outdma ? 0 : -1;
#endif
}

static void se_out_stop(void)
{
#ifndef SE_HOSTED
    if (se_outdma) { free(se_outdma); se_outdma = NULL; }
#endif
    se_outdma_len = 0;
}

static int se_out_put(FILE *f, const void *p, size_t n)
{
    uint32_t t0 = se_now_ms();
    int rc = (FIO_WriteFile(f, p, (int)n) == (int)n) ? 0 : -1;
    se_wr_calls++;
    se_wr_bytes += (uint32_t)n;
    se_wr_ms += se_now_ms() - t0;
    return rc;
}

static int se_out_write(FILE *f, const void *p, size_t n)
{
    const unsigned char *s = (const unsigned char *)p;

    if (!se_outdma)
        return se_out_put(f, p, n);
    while (n) {
        size_t room = SE_OUTDMA - se_outdma_len;
        size_t c = n < room ? n : room;
        memcpy((unsigned char *)se_outdma + se_outdma_len, s, c);
        se_outdma_len += c;
        s += c;
        n -= c;
        if (se_outdma_len == SE_OUTDMA) {
            size_t w = se_outdma_len;
            se_outdma_len = 0;
            if (se_out_put(f, se_outdma, w) != 0)
                return -1;
        }
    }
    return 0;
}

/* Must run before anything seeks in the file -- the thumbnail patch does. */
static int se_out_drain(FILE *f)
{
    size_t n = se_outdma_len;

    if (!se_outdma || !n)
        return 0;
    se_outdma_len = 0;
    return se_out_put(f, se_outdma, n);
}

static int out_flush(struct dev *d)
{
    if (d->oused) {
        if (se_out_write(d->out, d->obuf[d->ocur], d->oused) != 0)
            d->ofail = 1;
        d->oused = 0;
    }
    d->ocur ^= 1;
    return d->ofail ? -1 : 0;
}

static int jpeg_out(void *ctx, const unsigned char *data, size_t len)
{
    struct dev *d = (struct dev *)ctx;

    while (len) {
        size_t room = SE_OUTHALF - d->oused;
        size_t n = len < room ? len : room;
        memcpy(d->obuf[d->ocur] + d->oused, data, n);
        d->oused += n;
        data += n;
        len -= n;
        if (d->oused == SE_OUTHALF && out_flush(d) != 0) return -1;
    }
    return 0;
}

/*
 * The engine reports its own progress: "decode" while the CR2 is parsed and
 * "jpeg" per MCU row while the preview is decoded. Nothing was listening, so
 * the bar sat at 10% ("Decoding") for the whole decode -- and decode is by far
 * the longest step on the camera, which made a working develop look hung.
 */
/* Decode and grading stream at the same time (rows are emitted as the decoder
 * produces them), so the two mapped ranges can interleave. Never let the bar
 * move backwards. */
/* Aspect framing for the written file. The numbering is the MENU's, the same
 * one fs_core_aspect_ratio() uses for the LiveView bars, because the whole
 * point is that the two agree:
 *   0 3:2 full (native)  1 1:1   2 4:5   3 9:16   4 16:9   5 65:24 XPan
 *   6 2.39:1 cinemascope
 * (This comment said "1 16:9, 2 XPan", which never matched the switch below
 * in aspect_target().) Set once, read when the encoder is sized and when rows
 * are emitted. */
static int se_aspect_mode = 0;
void se_cam_set_aspect(int mode) { se_aspect_mode = mode; }

/* Letterbox a frame that is wider than 3:2 into a 3:2 frame with black bars.
 * A 65:24 or 16:9 file is a legal JPEG, but the body's playback gives it the
 * same treatment as a normal photo and the result is not what was framed; a
 * 3:2 file with bars looks like a photo with bars, which is what a contact
 * sheet of an XPan roll looks like anyway. */
static int se_pad_32 = 0;
void se_cam_set_pad32(int on) { se_pad_32 = on ? 1 : 0; }

/* The frame drawn around developed files. Static: one develop runs at a time
 * and the caller sets it immediately before the call. */
static se_frame g_frame;
void se_cam_set_frame(const se_frame *f) {
    if (f) g_frame = *f;
    else memset(&g_frame, 0, sizeof g_frame);
}

/*
 * Write one frame band: `rows` rows of the band colour, the text inside it,
 * and a keyline on the side that meets the picture (`edge_first` for the
 * bottom band, last row for the top one). Returns 0, or -1 if the arena or
 * the encoder refuses.
 */
static int write_band(se_jpeg *enc, se_arena *ar, int width,
                      const se_frame *f, const char *text,
                      int rows, int edge_first) {
    unsigned char *row = SE_NEW(ar, unsigned char, (size_t)width * 3);
    int y, ytop;
    if (!row || rows <= 0) return -1;
    ytop = se_frame_text_top(rows, f->scale);
    for (y = 0; y < rows; y++) {
        int edge = edge_first ? (y == 0) : (y == rows - 1);
        if (edge) {
            se_frame_edge_row(row, width, f);
        } else {
            se_frame_fill_row(row, width, f);
            se_frame_text_row(row, width, f, y, ytop,
                              edge_first ? f->line2 : f->line1);
        }
        if (se_jpeg_write_rows(enc, row, 1) != SE_OK) return -1;
    }
    return 0;
}

/* Strength of the look, 0..100. The menu has offered this slider all along and
 * only the LiveView path honoured it; a developed file was always 100%. */
static int se_strength = 100;

/* Widest row the cube is applied to in one go. The develop's own output width,
 * and a static buffer rather than 32 KB of a task's stack. */
#define SE_LUT_ROW_MAX 5472
static uint16_t lut_rowbuf[SE_LUT_ROW_MAX * 3];
void se_cam_set_strength(int s) { se_strength = (s < 0) ? 0 : (s > 100 ? 100 : s); }

/*
 * Saturation boost applied on top of whatever the stock asks for, 100 = as
 * written.
 *
 * The stocks' own SAT line is not the whole story: it is applied after the
 * per-hue fade, and a real landscape sits at a mean chroma of ~18, so most of
 * a photograph was reaching the hue stage at partial strength. The fade knee is
 * fixed (see SE_HUE_FULL), but that only recovered a level or two -- if a scene
 * is hazy, or the camera's own JPEG was muted before we ever saw it, only a
 * multiplier moves it. This one number scales all stocks at once so the look
 * can be tuned to taste without editing fourteen recipe files.
 */
static int se_grainsize = -1;
void se_cam_set_grainsize(int v) { se_grainsize = (v < -1) ? -1 : (v > 100 ? 100 : v); }

static int se_satboost = 100;
void se_cam_set_satboost(int s) { se_satboost = (s < 20) ? 20 : (s > 200 ? 200 : s); }

/* Every path that builds a look must set the three session options, or the
 * sliders apply to some develops and not others: the raw and grade-jpeg
 * paths were missing all three. SE_DECODE_SCALE-1 sites cannot use this
 * because their scale differs, so it takes the scale. */
static void apply_look_options(se_look *lk)
{
    se_look_set_strength(lk, se_strength);
    se_look_set_satboost(lk, se_satboost);
    se_look_set_grain_size(lk, se_grainsize);
}


/*
 * Phase profile of the develop.
 *
 * On the camera 32 s went missing between "Decoding 10%" and "Done", and the
 * only way to know whether that was the decoder, the look or the encoder is to
 * have the camera say so. Millisecond resolution is enough for a 30 s run, so
 * this uses get_ms_clock() there and gettimeofday() on the host, where the same
 * numbers come out in microseconds.
 */
#ifdef SE_HOSTED
#include <sys/time.h>
static uint32_t se_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint32_t)tv.tv_sec * 1000u + (uint32_t)tv.tv_usec / 1000u;
}
#else
#include <dryos.h>
static uint32_t se_now_ms(void) { return get_ms_clock(); }
#endif

static uint32_t t_look, t_enc, t_dec, t_gui, t_rows;

static void report_bar(struct dev *d, const char *stage, int pct)
{
    uint32_t t0;
    if (!d->progress || !stage) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct <= d->bar_pct && pct < 100) return;
    d->bar_pct = pct;
    /* The progress callback is the camera's screen: drawing it is charged to
     * t_gui, not to whatever phase happened to be running. Those seconds were
     * invisible before -- they fell between the look and the encoder, in the
     * one stretch of the develop nothing was timing. */
    t0 = se_now_ms();
    d->progress(d->ctx, stage, pct);
    t_gui += se_now_ms() - t0;
}

/*
 * Framing window for the written file, centred.
 *
 * Modes are the delivery shapes people actually need: 1:1 and 4:5 for feeds,
 * 9:16 for stories, 16:9 for video, 65:24 for XPan. A shape taller than the
 * sensor's 3:2 has to lose width rather than height, so the window can crop
 * either axis.
 */
/*
 * The pixel size each mode has to end up at. These are the sizes the platforms
 * themselves accept and downscale to; anything larger gets re-compressed and
 * loses detail on upload, anything smaller stays soft.
 */
static void aspect_target(int mode, int *tw, int *th)
{
    *tw = 0; *th = 0;
    switch (mode) {
    case 1: *tw = 1080; *th = 1080; break;   /* 1:1  feed     */
    case 2: *tw = 1080; *th = 1350; break;   /* 4:5  portrait */
    case 3: *tw = 1080; *th = 1920; break;   /* 9:16 story    */
    case 4: *tw = 1920; *th = 1080; break;   /* 16:9 video    */
    /* Neither XPan nor cinemascope is a platform shape, so there is nothing to
     * downscale for: the frame keeps its own resolution and is only cropped
     * (2736 x 1010 for XPan on a 6D, 2736 x 1144 for 2.39:1). */
    case 5: break;                           /* 65:24 XPan: native */
    case 6: break;                           /* 2.39:1 cinemascope: native */
    default: break;                          /* 0 keeps the raw resolution */
    }
}

void se_cam_aspect_window(int w, int h, int *x0, int *x1, int *y0, int *y1)
{
    *x0 = 0; *x1 = w; *y0 = 0; *y1 = h;
    if (se_aspect_mode <= 0 || w <= 0 || h <= 0) return;

    int num, den;                       /* target height/width */
    switch (se_aspect_mode) {
    case 1: num = 1;  den = 1;  break;  /* 1:1    */
    case 2: num = 5;  den = 4;  break;  /* 4:5    */
    case 3: num = 16; den = 9;  break;  /* 9:16   */
    case 4: num = 9;  den = 16; break;  /* 16:9   */
    case 5: num = 24; den = 65; break;  /* 65:24  */
    case 6: num = 100; den = 239; break; /* 2.39:1 cinemascope */
    default: return;
    }

    long boxh = (long)w * num / den;
    if (boxh <= h) {
        if (boxh < 16) return;
        *y0 = (int)((h - boxh) / 2);
        *y1 = *y0 + (int)boxh;
    } else {
        long boxw = (long)h * den / num;
        if (boxw < 16) return;
        *x0 = (int)((w - boxw) / 2);
        *x1 = *x0 + (int)boxw;
    }
}

static void engine_progress(void *ctx, const char *stage, int percent)
{
    struct dev *d = (struct dev *)ctx;
    if (!d->progress || !stage) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (!strcmp(stage, "jpeg"))
        report_bar(d, "Decoding", 10 + percent * 60 / 100);   /* 10..70 */
    else if (!strcmp(stage, "decode"))
        report_bar(d, "Reading", 1 + percent * 9 / 100);      /* 1..10  */
}


/*
 * Streaming area-average resampler.
 *
 * The look hands over one full-width row at a time, so a plain bilinear resize
 * would need the whole frame. Averaging boxes instead: each source row lands in
 * one output row, columns are averaged over their source span, and an output
 * row is written when the mapping moves on. Memory is one output row.
 */
#define RST_MAXW 1920
static unsigned char rst_row[RST_MAXW * 3];
static uint32_t rst_acc[RST_MAXW * 3];
static int rst_wsum;

/*
 * The column map, built once per develop instead of once per output row.
 *
 * rst_write() used to recompute `ox * sw / tw` and `(ox+1) * sw / tw` for every
 * column of every row -- four divisions per output pixel, plus three more for
 * the average. On this ARMv5 each one is a call to __aeabi_idiv, and a develop
 * with 1,010 output rows of 1,824 pixels was making eleven million of them:
 * the log shows that time as the sink being 33 s while the look (12 s) and the
 * encoder (8 s) only account for 20 s of it.
 *
 * The map depends only on the two widths, so it is built once. The average
 * divides by `span * rows`, where the span is the same for every column unless
 * the width is being reduced by a fractional ratio; in that common case one
 * exact reciprocal per row replaces 3 divisions per pixel.
 */
static uint16_t rst_xs[RST_MAXW], rst_xe[RST_MAXW];
static int      rst_map_sw, rst_map_tw, rst_span_uniform;

static void rst_build_map(int sw, int tw)
{
    int ox, span = -1;

    if (rst_map_sw == sw && rst_map_tw == tw) return;
    rst_map_sw = sw;
    rst_map_tw = tw;
    rst_span_uniform = 1;
    for (ox = 0; ox < tw && ox < RST_MAXW; ox++) {
        int xs = (int)((long)ox * sw / tw);
        int xe = (int)((long)(ox + 1) * sw / tw);
        if (xe <= xs) xe = xs + 1;
        rst_xs[ox] = (uint16_t)xs;
        rst_xe[ox] = (uint16_t)xe;
        if (span < 0) span = xe - xs;
        else if (xe - xs != span) rst_span_uniform = 0;
    }
}

/*
 * Exact division by an invariant divisor, without the library call.
 *
 * q0 = (acc * R) >> 24 with R = floor(2^24 / n) under-estimates by less than
 * one whenever acc <= 255 * n, so a single compare and increment finishes it,
 * and acc * R <= 255 * 2^24 still fits in 32 bits. Checked exhaustively
 * against the division for every n up to 4096 and every acc up to 255 * n --
 * 2.1 billion pairs, no mismatch, at most one correction step -- because this
 * has to produce the same byte the division produced, not a close one.
 */
static inline unsigned rst_div(uint32_t acc, uint32_t n, uint32_t r)
{
    uint32_t q = (acc * r) >> 24;

    while ((q + 1) * n <= acc) q++;
    return (unsigned)q;
}


static int rst_write(struct dev *d)
{
    int tw = d->tgt_w;
    uint32_t n, r = 0;
    int ox;

    if (rst_wsum <= 0) return 0;
    rst_build_map(d->src_w, tw);

    n = (uint32_t)(rst_xe[0] - rst_xs[0]) * (uint32_t)rst_wsum;
    if (!n) n = 1;
    if (rst_span_uniform && n <= 4096)
        r = (1u << 24) / n;          /* exact per the note on rst_div */

    for (ox = 0; ox < tw && ox < RST_MAXW; ox++) {
        int c;
        uint32_t ncol = n;
        if (!rst_span_uniform) {
            ncol = (uint32_t)(rst_xe[ox] - rst_xs[ox]) * (uint32_t)rst_wsum;
            if (!ncol) ncol = 1;
        }
        for (c = 0; c < 3; c++) {
            uint32_t acc = rst_acc[ox * 3 + c];
            rst_row[ox * 3 + c] = (unsigned char)
                (r ? rst_div(acc, ncol, r) : (acc / ncol));
        }
    }
    if (se_jpeg_write_rows(d->enc, rst_row, 1) != SE_OK) { *d->err = 1; return -1; }
    d->rows_done++;
    if (d->progress && d->height > 0) {
        int pct = 70 + d->rows_done * 30 / d->height;
        if (pct != d->last_pct) {
            d->last_pct = pct;
            report_bar(d, "Grading", pct);
        }
    }
    return 0;
}

/*
 * Write output rows until `target` of them exist.
 *
 * Mapping source row sy to output row sy*tgt_h/src_h gives the row it belongs
 * to, but one flush per change only ever writes as many rows as the source has.
 * When the target is taller than the window (9:16 on a 3:2 frame is 1824 rows
 * into 1920) that leaves the file short: the SOF says 1920 rows, the scan holds
 * 1824, and a decoder that validates the scan calls it "premature end of data".
 * Writing every row up to the mapped one fills the gap with the rows that were
 * meant to be repeated anyway.
 */
static int rst_flush_to(struct dev *d, int target)
{
    if (rst_wsum <= 0) return 0;
    while (d->rows_done < target && d->rows_done < d->tgt_h) {
        if (rst_write(d) != 0) return -1;
    }
    return 0;
}

static int rst_feed(struct dev *d, const unsigned char *row, int sy)
{
    int tw = d->tgt_w, sw = d->src_w;
    int oy = (int)((long)sy * d->tgt_h / (d->src_h ? d->src_h : 1));

    if (d->last_oy >= 0 && oy != d->last_oy) {
        if (rst_flush_to(d, oy) != 0) return -1;
        memset(rst_acc, 0, sizeof(rst_acc[0]) * (size_t)tw * 3);
        rst_wsum = 0;
    }
    d->last_oy = oy;

    for (int ox = 0; ox < tw && ox < RST_MAXW; ox++) {
        int xs = (int)((long)ox * sw / tw);
        int xe = (int)((long)(ox + 1) * sw / tw);
        if (xe <= xs) xe = xs + 1;
        unsigned int r = 0, g = 0, b = 0;
        for (int x = xs; x < xe; x++) {
            r += row[x * 3 + 0];
            g += row[x * 3 + 1];
            b += row[x * 3 + 2];
        }
        rst_acc[ox * 3 + 0] += r;
        rst_acc[ox * 3 + 1] += g;
        rst_acc[ox * 3 + 2] += b;
    }
    rst_wsum++;
    return 0;
}



/*
 * One thumbnail line, box-averaged across the source columns.
 *
 * The thumbnail exists because Canon's playback indexes a JPEG by its Exif
 * IFD1: without one the developed frame is written to the card and never
 * listed on the camera. It is taken while the picture streams, so it costs a
 * pass over the sampled rows only -- one row in th_step.
 */
static void thumb_line(struct dev *d, const unsigned char *row, int ty)
{
    int step_x = d->src_w / d->tw;
    unsigned char *out;
    int x;
    if (step_x < 1) step_x = 1;
    out = d->thumb +
          ((size_t)(ty + d->th_y0) * SE_THUMB_W + (size_t)d->th_x0) * 3;
    for (x = 0; x < d->tw; x++) {
        const unsigned char *p = row + (size_t)x * step_x * 3;
        int sum0 = 0, sum1 = 0, sum2 = 0, i;
        for (i = 0; i < step_x; i++) {
            sum0 += p[i * 3 + 0];
            sum1 += p[i * 3 + 1];
            sum2 += p[i * 3 + 2];
        }
        out[x * 3 + 0] = (unsigned char)(sum0 / step_x);
        out[x * 3 + 1] = (unsigned char)(sum1 / step_x);
        out[x * 3 + 2] = (unsigned char)(sum2 / step_x);
    }
}

static int look_emit(void *ctx, const unsigned char *row, int width)
{
    struct dev *d = (struct dev *)ctx;
    (void)width;
    /* Rows outside the framing window are simply never written, so the output
     * file is cropped rather than letterboxed. */
    int y = d->row_in++;
    if (y < d->crop_y0 || y >= d->crop_y1) return 0;
    if (d->thumb) {
        int oy = y - d->crop_y0;
        if (d->th_step > 0 && d->th > 0 && (oy % d->th_step) == 0) {
            int ty = oy / d->th_step;
            if (ty < d->th) thumb_line(d, row + (size_t)d->crop_x0 * 3, ty);
        }
    }
    /*
     * A local clock, not the shared PH0/PHI pair.
     *
     * Those use one file-scope `t_ph0`, and this function is called from inside
     * se_look_rows, which decode_rows() is timing with the same variable. The
     * inner PH0() reset it, so the outer measurement only ever accumulated the
     * tail after the last row -- which is why the log showed "look 11 s" and
     * "encode 8 s" for a sink that totalled 32 s, and why a diagnosis was built
     * on numbers that could not add up.
     */
    {
        uint32_t wr0 = se_now_ms();
        if (d->tgt_w > 0) {
            int rc = rst_feed(d, row + d->crop_x0 * 3, y - d->crop_y0);
            t_enc += se_now_ms() - wr0;
            t_rows++;   /* counted here too, or the log says "rows 0" for a crop */
            return rc;
        }
        if (se_jpeg_write_rows(d->enc, row + d->crop_x0 * 3, 1) != SE_OK) {
            *d->err = 1;
            return -1;
        }
        t_enc += se_now_ms() - wr0;
    }
    t_rows++;
    d->rows_done++;
    if (d->progress && d->height > 0) {
        int pct = 70 + d->rows_done * 30 / d->height;
        if (pct != d->last_pct) {
            d->last_pct = pct;
            report_bar(d, "Grading", pct);
        }
    }
    return 0;
}

/*
 * Apply the external cube to rows the decoder has just produced.
 *
 * The rows are sRGB-encoded 8-bit, which is exactly the domain a .cube exported
 * from Lightroom or Resolve expects, so the only conversion is 8 -> 16 bit and
 * back. Done here rather than in the look because the cube models the camera
 * profile the look is meant to sit on top of, not the other way round.
 */
static int lut_apply_rows(const uint8_t *rgb, int nrows, int width)
{
    const se_lut3 *cube = lut_get();
    uint16_t *buf = lut_rowbuf;
    int r, x;

    if (!cube) return 0;
    if (width > SE_LUT_ROW_MAX) return -1;

    for (r = 0; r < nrows; r++) {
        const uint8_t *in = rgb + (size_t)r * width * 3u;
        uint8_t *out = (uint8_t *)in;   /* in place: the decoder's buffer is ours */
        for (x = 0; x < width * 3; x++)
            buf[x] = (uint16_t)(in[x] * 257u);   /* 0..255 -> 0..65535 exactly */
        se_lut3_apply_row(cube, buf, width);
        for (x = 0; x < width * 3; x++)
            out[x] = (uint8_t)((buf[x] + 128u) / 257u);
    }
    return 0;
}

static int decode_rows(void *ctx, const unsigned char *rgb, int nrows, int width)
{
    struct dev *d = (struct dev *)ctx;
    (void)width;
    {
        uint32_t lk0 = se_now_ms();
        int rc;
        if (lut_apply_rows(rgb, nrows, width) != 0) return -1;
        rc = se_look_rows(d->run, rgb, nrows, look_emit, d) == SE_OK ? 0 : -1;
        t_look += se_now_ms() - lk0;
        return rc;
    }
}

/*
 * An external 3D LUT for the develop path.
 *
 * A .cube is ASCII and 786 KB at 32^3, which this camera cannot hold; the packed
 * form written by tools/cube2bin.py is 29 KB at 17^3 and 216 KB at 33^3. The
 * file is read straight into the buffer se_lut3_apply_row() indexes, so there is
 * one allocation and no second copy. Peak memory is the cube alone.
 *
 * Applied to the decoded rows before the look, in every develop path, so a
 * camera-profile cube sees the sRGB input it was made for. That was NOT true
 * until the full-RAW and mRAW paths were given the cube: they passed NULL and
 * ran only the look, so a cube-carrying stock developed one way through the
 * LJPEG path and another through the sensor path.
 */
static char     lut_path[64];
static se_lut3  lut_cube;
static void    *lut_hold;      /* owns lut_cube.data */
static int      lut_state;     /* 0 untried, 1 ready, -1 failed */

void se_cam_lut_path(const char *p)
{
    snprintf(lut_path, sizeof lut_path, "%s", p ? p : "");
    if (lut_hold) { free(lut_hold); lut_hold = NULL; }
    memset(&lut_cube, 0, sizeof lut_cube);
    lut_state = 0;
}

static const se_lut3 *lut_get(void)
{
    unsigned char h[8];
    uint32_t size, need;
    FILE *f;
    int n;

    if (lut_state) return lut_state > 0 ? &lut_cube : NULL;
    if (!lut_path[0]) { lut_state = -1; return NULL; }

    f = FIO_OpenFile(lut_path, O_RDONLY);
    if (!f) {
        se_log("lut: %s not on the card, developing without it", lut_path);
        lut_state = -1;
        return NULL;
    }
    if ((n = se_read(f, h, sizeof h)) != (int)sizeof h ||
        h[0] != 'F' || h[1] != 'L' || h[2] != 'U' || h[3] != 'T') {
        FIO_CloseFile(f);
        se_log("lut: %s is not an FLUT cube, developing without it", lut_path);
        lut_state = -1;
        return NULL;
    }
    size = (uint32_t)(h[4] | (h[5] << 8));
    need = (uint32_t)se_lut3_bin_bytes((int)size);
    if (!need || need != 8u + size * size * size * 6u) {
        FIO_CloseFile(f);
        se_log("lut: %s declares size %d, unusable", lut_path, (int)size);
        lut_state = -1;
        return NULL;
    }
    lut_hold = malloc(need - 8u);
    if (!lut_hold) {
        FIO_CloseFile(f);
        se_log("lut: no memory for a %d byte cube, developing without it",
               (int)(need - 8u));
        lut_state = -1;
        return NULL;
    }
    /* little-endian u16 triples, which is the layout se_lut3.data already has
     * on this body; the header is skipped rather than parsed again. */
    if (se_read(f, lut_hold, (int)(need - 8u)) != (int)(need - 8u)) {
        FIO_CloseFile(f);
        free(lut_hold); lut_hold = NULL;
        se_log("lut: %s is truncated, developing without it", lut_path);
        lut_state = -1;
        return NULL;
    }
    FIO_CloseFile(f);

    lut_cube.size = (int)size;
    lut_cube.data = (uint16_t *)lut_hold;
    lut_state = 1;
    se_log("lut: %s loaded, %d^3 (%d bytes)", lut_path, (int)size,
           (int)(need - 8u));
    return &lut_cube;
}

static void engine_log(void *ctx, const char *msg)
{
    struct dev *d = (struct dev *)ctx;
    if (d->note) d->note(d->ctx, msg);
}

/* -------------------------------------------------------------- develop */

#ifdef FS_QEMU_TEST
extern const char *se_cam_last_step;
#endif

static se_err read_whole(const char *path, void *buf, size_t cap, size_t *got)
{
    FILE *f = FIO_OpenFile(path, O_RDONLY | O_SYNC);
    if (!f) return SE_E_IO;
    int n = se_read(f, buf, cap);
    FIO_CloseFile(f);
    if (n <= 0) return SE_E_IO;
    *got = (size_t)n;
    return SE_OK;
}


/* ---------------------------------------------------------- Exif thumbnail */

/*
 * Where the APP1 payload starts: SOI (2) + APP1 marker (2) + length (2).
 *
 * This was 24 while the encoder also wrote a JFIF APP0 in front of the Exif
 * segment. APP0 is gone -- DCF wants APP1 immediately after SOI -- so the
 * payload moved to 6, and patching 24 would have written the thumbnail header
 * eighteen bytes into the Exif directory and made the file unreadable.
 */
#define SE_APP1_PAYLOAD_AT 6

struct thsink { unsigned char *p; int len, cap; };
static int thumb_sink(void *ctx, const uint8_t *data, size_t len)
{
    struct thsink *t = (struct thsink *)ctx;
    if (t->len + (int)len > t->cap) return -1;
    memcpy(t->p + t->len, data, len);
    t->len += (int)len;
    return 0;
}

/*
 * Re-write the reserved APP1 segment with one that also carries an IFD1
 * thumbnail. Returns 0 on success. Failure is not fatal: the file is complete
 * and valid without it, it just will not be indexed by the camera.
 */
static int se_attach_thumb(struct dev *d, const se_jpeg_exif *ex,
                           int out_w, int out_h)
{
    /* Static, not arena: after a 16-row band buffer the arena is tight, and a
     * silent -1 here meant the frame was written without its Exif thumbnail --
     * which is the one thing that makes the camera list it at all. Only one
     * develop runs at a time, which is the same rule the look already follows. */
    struct thumbws *w = &thumb_ws;
    unsigned char *app1 = w->app1;
    struct thsink t;
    int n, pad;

    if (!d->thumb || !d->enc || d->tw <= 0 || d->th <= 0)
        return -1;

    t.p = w->jpeg; t.len = 0; t.cap = SE_THUMB_JPEGMAX;
    if (se_jpeg_begin(&w->enc, SE_THUMB_W, SE_THUMB_H, 70, w->strip, NULL,
                      thumb_sink, &t) != SE_OK)
        return -1;
    if (se_jpeg_write_rows(&w->enc, d->thumb, SE_THUMB_H) != SE_OK) return -1;
    if (se_jpeg_end(&w->enc) != SE_OK) return -1;
    if (t.len <= 0) return -1;

    n = se_jpeg_exif_thumb(app1, SE_JPEG_EXIF_RESERVE, ex, out_w, out_h,
                           w->jpeg, t.len, SE_THUMB_W, SE_THUMB_H);
    if (n <= 0 || n > SE_JPEG_EXIF_RESERVE) {
        se_log("thumbnail: does not fit (%d bytes, payload %d)", n, t.len);
        return -1;
    }

    /* The segment length is fixed, so the tail must be zero-filled. */
    for (pad = n; pad < SE_JPEG_EXIF_RESERVE; pad++) app1[pad] = 0;

    /*
     * Only patch when the encoder really reserved the segment. It does that only
     * if the header fits its own buffer; if it did not, offset 24 lands in the
     * quantization tables and this write would corrupt the picture instead of
     * adding a thumbnail. The encoder records the fact -- reading it back is not
     * possible, the output file is open for writing only.
     */
    if (!d->enc->exif_reserved) {
        se_log("thumbnail: encoder reserved no APP1, left the file alone");
        return -1;
    }

    if (FIO_SeekSkipFile(d->out, SE_APP1_PAYLOAD_AT, SEEK_SET) < 0) return -1;
    if (FIO_WriteFile(d->out, app1, SE_JPEG_EXIF_RESERVE) != SE_JPEG_EXIF_RESERVE)
        return -1;
    se_log("thumbnail: %dx%d in a %dx%d frame, %d bytes into IFD1",
           d->tw, d->th, SE_THUMB_W, SE_THUMB_H, t.len);
    return 0;
}

se_err se_cam_develop(const char *cr2_path, const char *stock_path,
                      const char *out_path,
                      se_cam_progress_fn progress, se_cam_note_fn note,
                      void *ctx)
{
    struct dev d;
    se_err    e = SE_OK;
    long      failed = 0;
    unsigned char *mem = NULL, *win = NULL, *abuf = NULL;
    char     *stocktext = NULL;
    size_t    got = 0;
    se_cr2_info info;
    se_jdec_info ji;
    se_stock  st;
    se_look   lk;
    se_arena  arena;
    uint32_t  poff = 0, plen = 0;
    int       pw = 0, ph = 0, ow = 0, oh = 0;
    size_t    lookcost;
    int       out_w = 0, out_h = 0;
    int       pad_h = 0, pad_top = 0;
    int       frm_top = 0, frm_bot = 0;
    se_jpeg_exif ex;
    static se_jpeg enc;      /* ~5 KB, keep it off this task's stack */

    memset(&d, 0, sizeof d);
    memset(&info, 0, sizeof info);
    memset(&ji, 0, sizeof ji);
    d.err = &failed;
    d.progress = progress;
    d.note = note;
    d.ctx = ctx;
    d.last_pct = -1;
    d.rd_pos = -1;

    /* Installed here, not just before decode. The stock is parsed well before
     * that point, and its warnings ("unknown white balance", "no NAME") were
     * being dropped on the floor -- so a recipe could fail to load and leave no
     * trace anywhere. */
    se_set_log(engine_log, &d);
    se_set_progress(engine_progress, &d);

    if (progress) progress(ctx, "Opening", 0);

    mem = malloc(SE_WINDOW + SE_ARENA + SE_OUTBUF + SE_STOCK_MAX);
    se_log("develop: arena request %d bytes -> 0x%x",
           (int)(SE_WINDOW + SE_ARENA + SE_OUTBUF + SE_STOCK_MAX),
           (unsigned)(uintptr_t)mem);
    if (!mem) return SE_E_NOMEM;
    /*
     * How fast is this memory?
     *
     * Every stage of a develop measures 50-200x slower here than the same code
     * on a desktop: the decoder's block loop, the look, the encoder, all of it.
     * A single copy says whether that is the arithmetic or the buffers, and it
     * costs 3 ms to ask. Desktop DRAM copies at several GB/s; a DIGIC does not,
     * but if this comes back at tens of MB/s the memory itself is the problem
     * and no algorithm change will help.
     */
    {
        /*
         * Three sizes, because one number cannot tell a slow bus from a small
         * cache: 16 KB fits in L1, 64 KB likely in L2, 256 KB does not. If
         * only the 256 KB copy is slow, the fix is a smaller working set
         * (process in bands) rather than fewer operations; if all three are
         * slow, the buffers themselves are the wrong kind of memory.
         */
        /*
         * Only sizes that fit: the window is 256 KB and the arena 384 KB, so
         * anything above 256 KB would run off the allocation. The previous
         * version asked for 1 MB and 16 MB here, which never ran, and before
         * that aimed at the end of the buffer and wrote 32 KB past it.
         */
        static const int kb[3] = { 16, 64, 256 };
        char *dst = (char *)mem;
        char *src = (char *)mem + SE_WINDOW;
        for (int i = 0; i < 3; i++) {
            size_t n = (size_t)kb[i] * 1024;
            uint32_t tm;
            if (n > (size_t)SE_WINDOW || n > (size_t)SE_ARENA) break;
            tm = se_now_ms();
            memcpy(dst, src, n);
            tm = se_now_ms() - tm;
            se_log("develop: memtest %d KB copy %d ms (%d MB/s)", kb[i], (int)tm,
                   tm ? (int)((uint32_t)n / tm / 1000u) : 0);
        }
    }

    win   = mem;
    abuf  = mem + SE_WINDOW;
    d.obuf[0] = mem + SE_WINDOW + SE_ARENA;
    d.obuf[1] = d.obuf[0] + SE_OUTHALF;
    stocktext = (char *)(mem + SE_WINDOW + SE_ARENA + SE_OUTBUF);
    se_wr_calls = se_wr_bytes = se_wr_ms = 0;
    se_out_start();

    /* --- the stock ----------------------------------------------------- */
    SE_STEP("stock-read");
    e = read_whole(stock_path, stocktext, SE_STOCK_MAX, &got);
    if (e != SE_OK) goto done;
    SE_STEP("stock-parse");
    e = se_stock_parse(stocktext, got, &st);
    if (e != SE_OK) goto done;

    /* Record what was actually loaded. Without this line a develop that fell
     * back to a generated recipe looks identical in the log to one that used a
     * real stock, which is exactly the case worth being able to tell apart. */
#ifdef SE_HOSTED
    t_look = t_enc = t_rows = t_io = t_dec = t_gui = 0;
    se_je_n_blocks = se_je_n_flat = se_je_n_coef = 0;
#else
    t_look = t_enc = t_rows = t_io = t_dec = t_gui = 0;
    se_je_n_blocks = se_je_n_flat = se_je_n_coef = 0;   /* per image, not per boot: a batch of four
                                    * used to report four develops' worth */
#endif
    se_log("develop: recipe %s (sat=%d grain=%d) from %s",
           st.name, (int)(st.sat + 0.5f), (int)(st.grain + 0.5f), stock_path);

    /* --- the CR2 ------------------------------------------------------- */
    SE_STEP("cr2-open");
    d.in = FIO_OpenFile(cr2_path, O_RDONLY | O_SYNC);
    if (!d.in) { e = SE_E_IO; goto done; }

    {
        SE_STEP("window-read");
        int n = se_read(d.in, win, SE_WINDOW);
        if (n <= 0) { e = SE_E_IO; goto done; }
        got = (size_t)n;

        SE_STEP("cr2-parse");
        e = se_cr2_parse(win, got, &info);
        if (e != SE_OK) goto done;
        SE_STEP("find-preview");
        e = se_cr2_find_preview(win, got, &poff, &plen, &pw, &ph);
        if (e != SE_OK) goto done;
        if (poff >= got) { e = SE_E_TRUNCATED; goto done; }

        /* the preview header lives inside the window we already read */
        if (se_jpeg_dec_info(win + poff, got - poff, &ji) != SE_OK) {
            e = SE_E_UNSUPPORTED;
            goto done;
        }
    }

    if (progress) progress(ctx, "Stock", 5);

    /*
     * Half size by default.  The preview is 5472x3648 on a 6D; grading all of
     * it costs four times as much for a frame that is only going to be shared.
     * The scale is given to the decoder so it does a 4x4 inverse DCT instead
     * of 8x8 and converts a quarter of the colour.
     */
    se_jpeg_scaled_size(ji.width, ji.height, SE_DECODE_SCALE, &ow, &oh);

    /* --- the look ------------------------------------------------------ */
    SE_STEP("look-compile");
    se_look_from_stock(&st, SE_DECODE_SCALE, &lk);
    se_look_set_strength(&lk, se_strength);
    se_look_set_satboost(&lk, se_satboost);
    se_look_set_grain_size(&lk, se_grainsize);
    lookcost = se_look_fit(&lk, ow, SE_LOOK_BUDGET);
    (void)lookcost;

    se_arena_init(&arena, abuf, SE_ARENA);
    if (se_look_begin(&d.run, &lk, ow, &arena) != SE_OK) {
        e = SE_E_NOMEM;
        goto done;
    }

    d.base = poff;
    d.width = ow;
    d.height = oh;
    se_cam_aspect_window(ow, oh, &d.crop_x0, &d.crop_x1, &d.crop_y0, &d.crop_y1);
    aspect_target(se_aspect_mode, &d.tgt_w, &d.tgt_h);
    if (d.tgt_w > RST_MAXW || d.tgt_h <= 0) { d.tgt_w = 0; d.tgt_h = 0; }
    d.src_w = d.crop_x1 - d.crop_x0;
    d.src_h = d.crop_y1 - d.crop_y0;
    /* Thumbnail geometry: 160 px wide, the framing's own aspect. */
    {
        /* Fit inside 160x120 preserving aspect, then centre -- letterboxed, the
         * way every camera writes its DCF thumbnail. A 160x59 strip for an XPan
         * frame was accepted by PIL and ignored by the camera. */
        int tw = SE_THUMB_W, th = SE_THUMB_H;
        if (d.src_w * SE_THUMB_H > d.src_h * SE_THUMB_W) {
            th = d.src_h * SE_THUMB_W / (d.src_w ? d.src_w : 1);
            if (th < 1) th = 1;
            if (th > SE_THUMB_H) th = SE_THUMB_H;
        } else {
            tw = d.src_w * SE_THUMB_H / (d.src_h ? d.src_h : 1);
            if (tw < 1) tw = 1;
            if (tw > SE_THUMB_W) tw = SE_THUMB_W;
        }
        d.tw = tw;
        d.th = th;
        d.th_y0 = (SE_THUMB_H - th) / 2;
        d.th_x0 = (SE_THUMB_W - tw) / 2;
        d.th_step = d.src_h / th;
        if (d.th_step < 1) d.th_step = 1;
        /* Not the arena: the decoder draws from the same one, and 57 KB more of
         * it turned a working develop into "corrupt data". */
        d.thumb = thumb_ws.canvas;
        memset(d.thumb, 0, (size_t)SE_THUMB_W * SE_THUMB_H * 3);
    }
    d.last_oy = -1;
    rst_wsum = 0;
    if (d.tgt_w > 0)
        memset(rst_acc, 0, sizeof(rst_acc[0]) * (size_t)d.tgt_w * 3);
    d.row_in = 0;
    d.height = d.tgt_w ? d.tgt_h : (d.crop_y1 - d.crop_y0);

    {
        /* Encoder strip: 8 rows, from the same arena, so a taller frame
         * costs arena rather than a separate fixed buffer. */
        unsigned char *strip = SE_NEW(&arena, unsigned char, (size_t)SE_JPEG_MCU_ROWS * ow * 3);
        if (!strip) { e = SE_E_NOMEM; goto done; }

        memset(&ex, 0, sizeof ex);
        ex.make = info.make[0] ? info.make : "Canon";
        ex.model = info.model[0] ? info.model : "Canon EOS";
        ex.software = st.name;
        ex.datetime = info.datetime[0] ? info.datetime : NULL;
        ex.exposure_time = info.exposure_time;
        ex.aperture = info.aperture;
        ex.iso = info.iso;
        ex.exposure_program = info.exposure_program;
        ex.metering_mode = info.metering_mode;
        ex.flash = info.flash;

        SE_STEP("out-create");
        d.out = FIO_CreateFile(out_path);
        if (!d.out) { e = SE_E_IO; goto done; }

        d.enc = &enc;
        SE_STEP("jpeg-begin");
        out_w = d.tgt_w ? d.tgt_w : (d.crop_x1 - d.crop_x0);
        out_h = d.tgt_w ? d.tgt_h : (d.crop_y1 - d.crop_y0);

        /* Pad to 3:2 when the framing is wider than that (16:9, 65:24). */
        if (se_pad_32) {
            int want_h = (out_w * 2) / 3;
            if (want_h > out_h)
                pad_h = want_h - out_h;   /* extra rows, not the total */
        }
        /*
         * Film frame: bands above and below the picture. The reduced-size
         * develop (thumbnail / preview, d.tgt_w set) skips them -- a frame
         * belongs on the file that is kept, not on a 400 px preview.
         */
        if (g_frame.on && !d.tgt_w) {
            frm_top = g_frame.top > 0 ? g_frame.top : 0;
            frm_bot = g_frame.bottom > 0 ? g_frame.bottom : 0;
        }
        if (se_jpeg_begin(&enc, out_w, out_h + pad_h + frm_top + frm_bot, d.tgt_w ? 92 : 88, strip,
                          &ex, jpeg_out, &d) != SE_OK) {
            e = SE_E_IO;
            goto done;
        }
        if (frm_top > 0 &&
            write_band(&enc, &arena, out_w, &g_frame, g_frame.line1, frm_top, 0) != 0) {
            e = SE_E_IO;
            goto done;
        }
        if (pad_h > 0) {
            /* Black bars above and below: written as ordinary rows, so the
             * file is one image, not an image with a trick in it. */
            unsigned char *bar = SE_NEW(&arena, unsigned char, (size_t)out_w * 3);
            if (!bar) { e = SE_E_NOMEM; goto done; }
            memset(bar, 0, (size_t)out_w * 3);
            pad_top = pad_h / 2;
            for (int i = 0; i < pad_top; i++)
                if (se_jpeg_write_rows(&enc, bar, 1) != SE_OK) {
                    e = SE_E_IO;
                    goto done;
                }
            se_log("develop: padded to 3:2, %dx%d with %d rows of bar each side",
                   out_w, out_h + pad_h, pad_top);
        }
    }

    if (progress) progress(ctx, "Decoding", 10);

    {
        /* Static, not on the stack: this is live across the whole decode and
         * the develop task's stack is already carrying the decoder's 8.7 KB
         * se_jpeg_dec_info plus its frames. Only one develop runs at a time. */
        static unsigned char hdr[8192];
        size_t hdrlen = (size_t)plen < sizeof hdr ? (size_t)plen : sizeof hdr;
        if (hdrlen > got - poff) hdrlen = got - poff;
        memcpy(hdr, win + poff, hdrlen);

        SE_STEP("decode");
        {
            uint32_t d0 = se_now_ms();
            se_err drc = se_jpeg_decode_stream_scaled(hdr, hdrlen, plen, cr2_read,
                                                      &d, SE_DECODE_SCALE, &arena,
                                                      decode_rows, &d, &ji);
            t_dec = se_now_ms() - d0;   /* includes look + encode + screen */
            if (drc != SE_OK) {
                e = SE_E_CORRUPT;
                goto done;
            }
        }
    }
    se_look_end(d.run, look_emit, &d);
    /* the last accumulated output row is still in the accumulator, and any
     * output rows the mapping skipped are repeats of it */
    if (d.tgt_w > 0 && rst_wsum > 0) {
        if (rst_flush_to(&d, d.tgt_h) != 0) failed = 1;
        rst_wsum = 0;
    }
    if (pad_h > pad_top) {
        unsigned char *bar = SE_NEW(&arena, unsigned char, (size_t)out_w * 3);
        if (bar) {
            uint32_t b0 = se_now_ms();
            memset(bar, 0, (size_t)out_w * 3);
            for (int i = pad_top; i < pad_h; i++)
                if (se_jpeg_write_rows(&enc, bar, 1) != SE_OK) { failed = 1; break; }
            t_enc += se_now_ms() - b0;   /* the bars are encoded like any row */
        } else {
            failed = 1;
        }
    }
    if (frm_bot > 0 && !failed) {
        if (write_band(&enc, &arena, out_w, &g_frame, g_frame.line2, frm_bot, 1) != 0)
            failed = 1;
    }
    se_jpeg_end(&enc);
    d.enc = &enc;             /* still valid: the patch only needs the FILE* */
    if (out_flush(&d) != 0 || se_out_drain(d.out) != 0) e = SE_E_IO;
    if (e == SE_OK && d.thumb && d.out)
        se_attach_thumb(&d, &ex, out_w, out_h + pad_h + frm_top + frm_bot);
    d.enc = NULL;

    if (failed) e = SE_E_IO;
    if (progress) progress(ctx, "Done", 100);

done:
    /* decode = the whole engine call: decoder, look, encoder and the screen
     * drawing that happens between its row callbacks. The parts are broken out
     * so the next optimisation is chosen by measurement, not by guesswork. */
    se_log("develop: decode %d ms (io %d, look %d, enc %d, screen %d), "
           "%d card writes %d KB in %d ms, rows %d",
           (int)t_dec, (int)t_io, (int)t_look, (int)t_enc, (int)t_gui,
           (int)se_wr_calls, (int)(se_wr_bytes >> 10), (int)se_wr_ms, (int)t_rows);
    se_log("develop: decoder split: blocks %d ms, colour %d ms, sink %d ms",
           (int)se_jd_t_blocks, (int)se_jd_t_color, (int)se_jd_t_sink);
    /* Per-stage detail. Counts where there are too many calls to clock (623,000
     * blocks per frame), times where there are few enough (one per row). */
    se_log("develop: look split: grade %d ms, halo mask %d ms, halo add %d ms, "
           "grain %d ms, rows %d",
           (int)se_lk_t_grade, (int)se_lk_t_mask, (int)se_lk_t_glow,
           (int)se_lk_t_grain, (int)se_lk_n_rows);
    se_log("develop: dec blocks %d (flat %d) coefs %d | enc blocks %d (flat %d) "
           "coefs %d",
           (int)se_jd_n_blocks, (int)se_jd_n_flat, (int)se_jd_n_coef,
           (int)se_je_n_blocks, (int)se_je_n_flat, (int)se_je_n_coef);
    se_set_log(NULL, NULL);
    se_set_progress(NULL, NULL);
    if (d.enc) { se_jpeg_end(&enc); d.enc = NULL; }
    if (d.out) { FIO_CloseFile(d.out); d.out = NULL; }
    if (d.in)  { FIO_CloseFile(d.in);  d.in = NULL; }
    se_bounce_free();
    se_out_stop();
    if (e != SE_OK && out_path) FIO_RemoveFile(out_path);
    free(mem);
    return e;
}

/* ===================================================================== */
/* Level 3: develop the sensor data, not the embedded JPEG.              */
/*                                                                       */
/* The camera cannot hold a decoded 6D frame (5472 x 3648 uint16 is 40   */
/* MB), so the lossless-JPEG raw is decoded once to a spool file on the  */
/* card and then streamed back a band at a time. The heavy lifting is    */
/* the engine's own pipeline: black subtract + white balance on the CFA  */
/* grid, Malvar-He-Cutler demosaic (or Bayer-quad binning at scale 2),   */
/* camera->sRGB matrix, linear->sRGB encode. Rows arrive as 8-bit RGB    */
/* through the pipeline's row sink, which is where the film look and the */
/* JPEG encoder take over -- the same look and encoder Level 2 uses.     */
/* ===================================================================== */

struct rawspool {
    FILE     *f;
    uint32_t  width;      /* raw_width, samples per row                */
    uint32_t  written;    /* bytes written so far, for sequential fast path */
};

static int spool_sink(void *ctx, int row, int col0, int n, const uint16_t *s)
{
    struct rawspool *sp = (struct rawspool *)ctx;
    uint32_t off = ((uint32_t)row * sp->width + (uint32_t)col0) * 2u;
    uint32_t bytes = (uint32_t)n * 2u;

    if (!sp->f) return -1;
    if (off != sp->written) {
        if (FIO_SeekSkipFile(sp->f, (int64_t)off, SEEK_SET) < 0) return -1;
    }
    if (FIO_WriteFile(sp->f, s, (int)bytes) != (int)bytes) return -1;
    sp->written = off + bytes;
    return 0;
}

static int spool_read_rows(void *ctx, int y, int n, uint16_t *dst)
{
    struct rawspool *sp = (struct rawspool *)ctx;
    uint32_t bytes = (uint32_t)n * sp->width * 2u;

    if (FIO_SeekSkipFile(sp->f, (int64_t)((uint32_t)y * sp->width * 2u),
                         SEEK_SET) < 0)
        return -1;
    return se_read(sp->f, dst, bytes) == (int)bytes ? 0 : -1;
}

/* Rows from the pipeline, straight into the look and then the encoder. */
static int raw_row_sink(void *ctx, const uint8_t *rgb8, int nrows, int width)
{
    struct dev *d = (struct dev *)ctx;
    (void)width;
    if (se_look_rows(d->run, rgb8, nrows, look_emit, d) != SE_OK) return -1;
    return 0;
}

/* ===================================================================== */
/* Level 3 on a body that writes mRAW                                      */
/*                                                                         */
/* The frames this camera writes are mRAW: 15-bit YCbCr at half the        */
/* sensor's width and height, already demosaiced by DIGIC. There is no CFA */
/* grid to interpolate and no black level to find, so the develop is:      */
/* decode the subsampled stream, turn luma and chroma into camera RGB,     */
/* apply the same white balance, matrix and encode the full-raw path uses, */
/* then hand the row to the look.                                          */
/*                                                                         */
/* The coding is band-major -- four column bands of 684 pixels, each its   */
/* own block of rows -- so one decoded row is a quarter of an image row.   */
/* The spool puts the bands back together: written in the order they       */
/* arrive, which is sequential within a band, and read back four rows at a */
/* time so the look and the encoder always see whole rows.                 */
/* ===================================================================== */

#define MRAW_BANDS_MAX 8

struct mraw_spool {
    FILE    *f;
    uint32_t band_bytes;    /* bytes in one band row                     */
    uint32_t written;       /* where the file position already is        */
    int      band_w;
    int      height;
    int      rows;          /* band rows spooled so far                  */
    int      rows_total;
    struct dev *d;          /* for the progress bar, may be NULL         */
};

static int mraw_spool_sink(void *ctx, int row, int col0, int n,
                           const uint16_t *rgb)
{
    struct mraw_spool *sp = (struct mraw_spool *)ctx;
    int      band = col0 / sp->band_w;
    uint32_t off  = ((uint32_t)band * (uint32_t)sp->height + (uint32_t)row)
                    * sp->band_bytes;
    uint32_t bytes = (uint32_t)n * 3u * 2u;

    if (!sp->f) return -1;
    if (off != sp->written) {
        if (se_out_drain(sp->f) != 0) return -1;
        if (FIO_SeekSkipFile(sp->f, (int64_t)off, SEEK_SET) < 0) return -1;
    }
    /* Through the DMA staging buffer: a row is 4 KB and there are 7,296 of
     * them, and FIO_WriteFile flushes the cache on every call taken straight
     * from ordinary memory. */
    sp->written = off + bytes;
    if (se_out_write(sp->f, rgb, bytes) != 0) return -1;
    /* This is the longest single step of a raw develop and it used to report
     * only "Unpacking 3%" for minutes, which is indistinguishable from a hang.
     * One line per 256 band rows is 28 lines for a whole frame. */
    sp->rows++;
    if ((sp->rows & 255) == 0) {
        se_log("mrawdevelop: unpack %d/%d band rows", sp->rows, sp->rows_total);
        /* The bar has 3..24% to spend here; "Developing" picks up at 25. */
        if (sp->d && sp->d->progress && sp->rows_total > 0)
            sp->d->progress(sp->d->ctx, "Unpacking",
                            3 + 21 * sp->rows / sp->rows_total);
    }
    return 0;
}

static int mraw_read_at(FILE *f, uint32_t off, void *dst, uint32_t n)
{
    if (FIO_SeekSkipFile(f, (int64_t)off, SEEK_SET) < 0) return -1;
    /* se_read_io goes through the DMA bounce buffer, which is what FIO wants
     * for a large read into ordinary memory. */
    return se_read_io(f, dst, (size_t)n) == (int)n ? 0 : -1;
}

/*
 * Decode the mRAW frame, then grade it. `d` is the caller's context: the CR2
 * is already open on it and the stock is already parsed. Returns with d->in
 * untouched -- the caller closes it.
 */
static se_err se_cam_develop_mraw(struct dev *d, const se_cr2_info *info,
                                  const se_stock *st, const char *out_path,
                                  void *mem, se_arena *ar)
{
    struct mraw_spool sp;
    se_cr2_info mi;
    se_color    col;
    se_look     lk;
    se_look_run *run = NULL;
    se_jpeg     enc;
    se_jpeg_exif ex;
    se_lut1    *encode;
    const se_lut3 *cube = lut_get();   /* NULL when the stock carries none   */
    uint16_t   *band  = NULL;  /* `bands` whole rows, being assembled       */
    uint16_t   *rd    = NULL;  /* one band block, straight off the spool    */
    uint16_t   *row16 = NULL;  /* camera RGB, then sRGB, for one row        */
    uint8_t    *row8  = NULL;  /* one finished 8-bit row                    */
    unsigned char *strip;
    char        spool_path[128];
    int         W, H, band_w, bands, k_count, y, b, g, x;
    int         gq[3], clip[3];
    int         ok = 0;
    se_err      e = SE_OK;

    if (info->slice_width <= 0 || (info->raw_width % (info->slice_width / 2))) {
        se_log("mrawdevelop: no usable band geometry");
        return SE_E_UNSUPPORTED;
    }
    band_w = info->slice_width / 2;
    W      = info->raw_width;
    H      = info->raw_height;
    bands  = W / band_w;
    if (bands < 1 || bands > MRAW_BANDS_MAX || (H % bands) != 0)
        return SE_E_UNSUPPORTED;
    k_count = H / bands;

    se_log("mrawdevelop: %dx%d, %d bands of %d px, %d rows per group",
           W, H, bands, band_w, k_count);

    /* --- decode to a spool -------------------------------------------- */
    if (d->progress) d->progress(d->ctx, "Unpacking", 3);
    snprintf(spool_path, sizeof spool_path, "%s.RAW", out_path);
    FIO_RemoveFile(spool_path);
    memset(&sp, 0, sizeof sp);
    sp.f          = FIO_CreateFile(spool_path);
    sp.band_w     = band_w;
    sp.height     = H;
    sp.band_bytes = (uint32_t)band_w * 3u * 2u;
    sp.rows_total = bands * H;
    sp.d          = d;
    if (!sp.f) { e = SE_E_IO; goto done; }

    if (FIO_SeekSkipFile(d->in, 0, SEEK_SET) < 0) { e = SE_E_IO; goto done; }
    d->rd_pos = -1;
    e = se_cr2_decode_mraw(NULL, 0, cr2_read, d, info, mraw_spool_sink, &sp, ar);
    if (se_out_drain(sp.f) != 0 && e == SE_OK) e = SE_E_IO;
    FIO_CloseFile(sp.f);
    sp.f = NULL;
    if (e != SE_OK) goto done;
    se_log("mrawdevelop: spooled %d KB of camera RGB",
           (int)(((uint32_t)bands * (uint32_t)H * sp.band_bytes) >> 10));
    /* The decoder's scratch (Huffman tables, a row of samples) is dead now and
     * the look needs the arena. */
    se_arena_reset(ar);

    /* --- the colour the full-raw path would have used ------------------ */
    mi = *info;
    /* mRAW samples are 15-bit and already black-subtracted: there is no masked
     * region to measure and Canon stores no white level for the subsampled IFD. */
    mi.white          = 0x3fff;
    mi.black          = 0;
    mi.black_measured = 0;
    e = se_color_setup(&col, &mi, NULL, 0.0f);
    if (e != SE_OK) goto done;
    /*
     * No white balance: the subsampled raw is already balanced.
     *
     * That is not an assumption, it is what the numbers in a frame say. Taking
     * the camera's own as-shot multipliers (2020/1024/1733 on the test frames)
     * and solving for the gains that put the mean of a frame on the grey axis
     * gives 1.03 / 1.00 / 0.96 -- unity. Applying the as-shot multipliers on
     * top turns the whole picture magenta, which is exactly what happened
     * before this was measured: 122/104/118 against the camera's own
     * 106/111/120, and a purple lake.
     *
     * So the only scaling here is 15-bit coded samples to the 16-bit working
     * range, and every channel clips at the same value. The white balance
     * control therefore does not reach raw develop on this body, which is said
     * out loud rather than silently ignored.
     */
    {
        float g = 65535.0f / 16383.0f;
        int   c;
        for (c = 0; c < 3; c++) {
            gq[c]   = (int)(g * (float)SE_QONE + 0.5f);
            clip[c] = 65535;
        }
        se_log("mrawdevelop: white balance is Canon's (as-shot %d/%d/%d); "
               "no second application", (int)info->wb[0], (int)info->wb[1],
               (int)info->wb[2]);
    }

    encode = SE_NEW(ar, se_lut1, 1);
    if (!encode) { e = SE_E_NOMEM; goto done; }
    se_lut1_srgb_encode(encode);

    band  = (uint16_t *)malloc((size_t)bands * W * 3 * sizeof(uint16_t));
    rd    = (uint16_t *)malloc((size_t)bands * band_w * 3 * sizeof(uint16_t));
    row16 = (uint16_t *)malloc((size_t)W * 3 * sizeof(uint16_t));
    row8  = (uint8_t  *)malloc((size_t)W * 3);
    se_log("mrawdevelop: row buffers %d KB (%d bands of %d px)",
           (int)(((size_t)bands * W * 3 * 2 + (size_t)bands * band_w * 3 * 2 +
                  (size_t)W * 3 * 2 + (size_t)W * 3) >> 10), bands, band_w);
    if (!band || !rd || !row16 || !row8) { e = SE_E_NOMEM; goto done; }

    /* --- look + encoder ----------------------------------------------- */
    se_look_from_stock(st, 2, &lk);
    se_look_set_strength(&lk, se_strength);
    se_look_set_satboost(&lk, se_satboost);
    se_look_set_grain_size(&lk, se_grainsize);
    /* The framing window, or look_emit drops every row: it keeps
     * crop_y0..crop_y1 and the struct starts zeroed. */
    se_cam_aspect_window(W, H, &d->crop_x0, &d->crop_x1, &d->crop_y0, &d->crop_y1);
    d->src_w = d->crop_x1 - d->crop_x0;
    d->src_h = d->crop_y1 - d->crop_y0;
    d->width = W;
    d->height = H;
    d->rows_done = 0;
    d->last_pct = -1;
    se_look_fit(&lk, W, SE_LOOK_BUDGET);
    if (se_look_begin(&run, &lk, W, ar) != SE_OK) { e = SE_E_NOMEM; goto done; }
    d->run = run;

    d->out = FIO_CreateFile(out_path);
    if (!d->out) { e = SE_E_IO; goto done; }
    strip = SE_NEW(ar, unsigned char, (size_t)SE_JPEG_MCU_ROWS * W * 3);
    if (!strip) { e = SE_E_NOMEM; goto done; }

    ex.make = info->make[0] ? info->make : "Canon";
    ex.model = info->model[0] ? info->model : "Canon EOS";
    ex.software = st->name;
    ex.datetime = info->datetime[0] ? info->datetime : NULL;
    ex.exposure_time = info->exposure_time;
    ex.aperture = info->aperture;
    ex.iso = info->iso;
    ex.exposure_program = info->exposure_program;
    ex.metering_mode = info->metering_mode;
    ex.flash = info->flash;
    /* The framing window, not the full mRAW frame -- look_emit() emits only
     * d->crop_y0..d->crop_y1 rows from `row + crop_x0 * 3`. */
    if (se_jpeg_begin(&enc, d->crop_x1 - d->crop_x0,
                      d->crop_y1 - d->crop_y0, 88, strip, &ex, jpeg_out,
                      d) != SE_OK) {
        e = SE_E_IO;
        goto done;
    }
    d->enc = &enc;
    if (d->progress) d->progress(d->ctx, "Developing", 25);

    sp.f = FIO_OpenFile(spool_path, O_RDONLY);
    if (!sp.f) { e = SE_E_IO; goto done; }

    for (y = 0; y < k_count; y++) {
        int y0 = y * bands;

        /* One band block at a time: `bands` consecutive rows of one band. */
        for (b = 0; b < bands; b++) {
            uint32_t off = ((uint32_t)b * (uint32_t)H + (uint32_t)y0)
                           * sp.band_bytes;
            if (mraw_read_at(sp.f, off, rd,
                             (uint32_t)bands * sp.band_bytes) != 0) {
                e = SE_E_IO;
                goto enc_done;
            }
            for (g = 0; g < bands; g++) {
                const uint16_t *src = rd + (uint32_t)g * band_w * 3u;
                uint16_t *dst = band + (uint32_t)g * W * 3u
                                + (uint32_t)b * band_w * 3u;
                for (x = 0; x < band_w * 3; x++) dst[x] = src[x];
            }
        }

        /* White balance, then the path's own matrix, encode and dither. */
        for (g = 0; g < bands; g++) {
            const uint16_t *src = band + (size_t)g * W * 3u;
            for (x = 0; x < W; x++) {
                int c;
                for (c = 0; c < 3; c++) {
                    int64_t w = ((int64_t)src[x * 3 + c] * gq[c]
                                 + (SE_QONE / 2)) >> SE_Q;
                    if (w > clip[c]) w = clip[c];
                    row16[x * 3 + c] = (uint16_t)w;
                }
            }
            se_develop_encode_row(&col, encode, row16, W);
            /* same order as pipeline.c: cube between encode and dither */
            if (cube) se_lut3_apply_row(cube, row16, W);
            se_develop_dither_row(row16, row8, W, y0 + g, 1);
            if (se_look_rows(run, row8, 1, look_emit, d) != SE_OK) {
                e = SE_E_IO;
                goto enc_done;
            }
        }
    }

enc_done:
    se_look_end(run, look_emit, d);
    d->run = NULL;
    se_jpeg_end(&enc);
    d->enc = NULL;
    if (e == SE_OK && (out_flush(d) != 0 || se_out_drain(d->out) != 0)) e = SE_E_IO;
    if (e == SE_OK && (d->ofail || (d->err && *d->err))) e = SE_E_IO;
    if (e == SE_OK) ok = 1;
    if (e == SE_OK && d->progress) d->progress(d->ctx, "Done", 100);

done:
    if (d->enc) { se_jpeg_end(&enc); d->enc = NULL; }
    if (run) { se_look_end(run, look_emit, d); d->run = NULL; }
    if (d->out) { FIO_CloseFile(d->out); d->out = NULL; }
    if (sp.f) { FIO_CloseFile(sp.f); sp.f = NULL; }
    se_bounce_free();
    se_out_stop();
    FIO_RemoveFile(spool_path);
    if (!ok && out_path) FIO_RemoveFile(out_path);
    free(band);
    free(row8);
    /* rd and row16 were allocated above and never released: ~32 KB of system
     * heap leaked on every mRAW develop. */
    free(row16);
    free(rd);
    (void)mem;
    return e;
}

se_err se_cam_develop_raw(const char *cr2_path, const char *stock_path,
                          const char *out_path, int scale,
                          se_cam_progress_fn progress, se_cam_note_fn note,
                          void *ctx)
{
    struct dev      d;
    struct rawspool sp;
    se_err          e = SE_OK;
    se_cr2_info     info;
    se_stock        st;
    se_look         lk;
    se_look_run    *run = NULL;
    se_jpeg         enc;
    se_jpeg_exif    ex;
    se_dev_opts     opts;
    se_raw_src      src;
    se_arena        ar, ar2;
    unsigned char  *mem = NULL;
    char           *stocktext = NULL;
    char            spool_path[128];
    size_t          got = 0;
    uint32_t        sz = 0;
    static unsigned char hdr[256 * 1024];
    size_t          need;
    size_t          arena_size;
    int             ow = 0, oh = 0, ok = 0;
    long            failed = 0;

    if (!cr2_path || !out_path) return SE_E_CORRUPT;
    if (scale != 1 && scale != 2 && scale != 4) scale = 2;

    /* Empty until there is a spool: the failure path below removes it, and a
     * stray uninitialised path there would delete a file that is not ours. */
    spool_path[0] = 0;
    memset(&d, 0, sizeof d);
    memset(&sp, 0, sizeof sp);
    d.err = &failed;
    d.progress = progress;
    d.note = note;
    d.ctx = ctx;
    d.last_pct = -1;
    d.rd_pos = -1;
    se_set_log(engine_log, &d);
    se_set_progress(engine_progress, &d);

    if (progress) progress(ctx, "Opening", 0);

    /* The header first: it decides how much scratch the pipeline needs. */
    /* --- the CR2 header ------------------------------------------------ */
    d.in = FIO_OpenFile(cr2_path, O_RDONLY | O_SYNC);
    if (!d.in) { e = SE_E_IO; goto done; }
    {
        int n = se_read(d.in, hdr, sizeof hdr);
        if (n <= 0) { e = SE_E_IO; goto done; }
        e = se_cr2_parse(hdr, (size_t)n, &info);
        if (e != SE_OK) goto done;
    }
    se_log("rawdevelop: raw %dx%d, active %dx%d, cfa %d, black %d",
           info.raw_width, info.raw_height, info.active_width,
           info.active_height, info.cfa_phase, info.black);

    /*
     * Size the arena from what the pipeline says it needs, not from a constant.
     *
     * SE_ARENA is 384 KB. se_develop() allocates band + rgb + rgb8 + strip +
     * the curve table (+ the raw staging buffer and a jpeg context) from this
     * arena, and on a 6D full frame at half size that is roughly 600-1000 KB:
     * with the fixed 384 KB it returned SE_E_NOMEM every time, which is why a
     * level-3 develop on this body never produced a file. The real number
     * depends on the frame, so the caller asks instead of assuming.
     */
    need = se_develop_scratch_size(&info);
    {
        arena_size = need > (size_t)SE_ARENA ? need : (size_t)SE_ARENA;
        mem = malloc(arena_size + SE_OUTBUF + SE_STOCK_MAX + (64 * 1024));
        if (!mem) {
            se_log("rawdevelop: no memory: need %d bytes of scratch for this "
                   "frame, malloc refused",
                   (int)need);
            e = SE_E_NOMEM;
            goto done;
        }
        d.obuf[0] = (unsigned char *)mem + arena_size;
        d.obuf[1] = d.obuf[0] + SE_OUTHALF;
        stocktext = (char *)(mem + arena_size + SE_OUTBUF);
        se_arena_init(&ar, mem, arena_size);
        se_log("rawdevelop: scale %d, scratch need %d, arena %d bytes",
               scale, (int)need, (int)arena_size);
    }
    se_out_start();

    /* --- the stock ----------------------------------------------------- */
    e = read_whole(stock_path, stocktext, SE_STOCK_MAX, &got);
    if (e != SE_OK) goto done;
    e = se_stock_parse(stocktext, got, &st);
    if (e != SE_OK) goto done;
    se_log("rawdevelop: recipe %s (sat=%d grain=%d) from %s", st.name,
           (int)(st.sat + 0.5f), (int)(st.grain + 0.5f), stock_path);

    /* mRAW is not a mosaic: no black level, no CFA, no demosaic. */
    if (se_cr2_is_mraw(cr2_read, &d, &info)) {
        e = se_cam_develop_mraw(&d, &info, &st, out_path, mem, &ar);
        if (e == SE_OK && (failed || d.ofail)) e = SE_E_IO;
        ok = (e == SE_OK);
        goto done;
    }

    /* --- spool the decoded raw ---------------------------------------- */
    if (progress) progress(ctx, "Unpacking", 3);
    snprintf(spool_path, sizeof spool_path, "%s.RAW", out_path);
    FIO_RemoveFile(spool_path);
    sp.f = FIO_CreateFile(spool_path);
    if (!sp.f) { e = SE_E_IO; goto done; }
    sp.width = (uint32_t)info.raw_width;

    d.base = 0;
    if (FIO_SeekSkipFile(d.in, 0, SEEK_SET) < 0) { e = SE_E_IO; goto done; }
    d.rd_pos = -1;
    e = se_cr2_decode_stream(cr2_read, &d, &info, spool_sink, &sp, &ar);
    FIO_CloseFile(sp.f);
    sp.f = NULL;
    if (e != SE_OK) goto done;
    se_log("rawdevelop: spooled %d bytes", (int)sp.written);

    if (progress) progress(ctx, "Developing", 25);

    /* --- look + encoder ------------------------------------------------ */
    sp.f = FIO_OpenFile(spool_path, O_RDONLY);
    if (!sp.f) { e = SE_E_IO; goto done; }

    se_look_from_stock(&st, scale, &lk);
    apply_look_options(&lk);
    se_develop_out_size(&info, scale, &ow, &oh);
    /*
     * The framing window. Without this the struct stays zeroed, crop_y1 is 0,
     * and look_emit() drops every row: a full-RAW develop wrote an empty file.
     * The mRAW branch has always done this (see the call above).
     */
    se_cam_aspect_window(ow, oh, &d.crop_x0, &d.crop_x1, &d.crop_y0, &d.crop_y1);
    d.width = ow;
    d.height = oh;
    d.rows_done = 0;
    d.last_pct = -1;
    se_look_fit(&lk, ow, SE_LOOK_BUDGET);
    /* Same memory as `ar`: the decode arena is done with by the time the
     * pipeline runs, and that was already true before the size became a
     * variable. */
    se_arena_init(&ar2, mem, arena_size);
    if (se_look_begin(&run, &lk, ow, &ar2) != SE_OK) { e = SE_E_NOMEM; goto done; }
    d.run = run;

    d.out = FIO_CreateFile(out_path);
    if (!d.out) { e = SE_E_IO; goto done; }

    unsigned char *strip = SE_NEW(&ar2, unsigned char, (size_t)SE_JPEG_MCU_ROWS * ow * 3);
        if (!strip) { e = SE_E_NOMEM; goto done; }
    ex.make = info.make[0] ? info.make : "Canon";
    ex.model = info.model[0] ? info.model : "Canon EOS";
    ex.software = st.name;
    ex.datetime = info.datetime[0] ? info.datetime : NULL;
    ex.exposure_time = info.exposure_time;
    ex.aperture = info.aperture;
    ex.iso = info.iso;
    ex.exposure_program = info.exposure_program;
    ex.metering_mode = info.metering_mode;
    ex.flash = info.flash;
    /* The framing window, not the full frame: look_emit() emits only
     * crop_y0..crop_y1 rows and passes `row + crop_x0 * 3`, so an encoder
     * told ow x oh reads ow pixels from that offset (past the end of the row
     * buffer) and its SOF0 promises rows it never receives. se_cam_develop()
     * has always passed the crop size; this path and the mRAW one did not. */
    if (se_jpeg_begin(&enc, d.crop_x1 - d.crop_x0, d.crop_y1 - d.crop_y0, 88,
                      strip, &ex, jpeg_out, &d) != SE_OK) {
        e = SE_E_IO;
        goto done;
    }
    d.enc = &enc;

    se_dev_opts_default(&opts);
    opts.scale = scale;
    opts.jpeg_quality = 88;
    opts.dither = 1;
    opts.rows = raw_row_sink;    /* our look + our encoder own the output */
    opts.rows_ctx = &d;
    opts.grain_amount = 0.0f;    /* the look already grains it          */

    memset(&src, 0, sizeof src);
    src.mem = NULL;
    src.read_rows = spool_read_rows;
    src.ctx = &sp;

    /*
     * `lut_get()`, not NULL. This path passed NULL, so a stock carrying a
     * cube developed WITHOUT it while the LJPEG path and se_cam_grade_jpeg
     * both applied it -- the same stock came out two different ways
     * depending on which develop ran. The cube sits between the encode and
     * the look here, which is where pipeline.c applies it and what the
     * sRGB domain a .cube is exported for expects.
     */
    e = se_develop(&info, &src, lut_get(), &opts, &ar2, NULL, NULL);
    if (e != SE_OK) goto done;
    se_log("develop_raw: cube %s", lut_get() ? "applied" : "none for this stock");

    se_look_end(run, look_emit, &d);
    d.run = NULL;
    se_jpeg_end(&enc);
    d.enc = NULL;
    if (out_flush(&d) != 0 || se_out_drain(d.out) != 0) { e = SE_E_IO; goto done; }
    if (d.ofail) { e = SE_E_IO; goto done; }
    ok = 1;
    if (progress) progress(ctx, "Done", 100);
    se_log("rawdevelop: wrote %dx%d, %d bytes",
           ow, oh, (int)0);

done:
    se_log("rawdevelop: profile look %d ms, encode %d ms, screen %d ms, "
           "%d writes %d KB in %d ms, rows %d",
           (int)t_look, (int)t_enc, (int)t_gui,
           (int)se_wr_calls, (int)(se_wr_bytes >> 10), (int)se_wr_ms, (int)t_rows);
    se_set_log(NULL, NULL);
    se_set_progress(NULL, NULL);
    if (d.enc) { se_jpeg_end(&enc); d.enc = NULL; }
    if (d.out) { FIO_CloseFile(d.out); d.out = NULL; }
    if (sp.f) { FIO_CloseFile(sp.f); sp.f = NULL; }
    if (d.in) { FIO_CloseFile(d.in); d.in = NULL; }
    se_bounce_free();
    se_out_stop();
    if (spool_path[0]) FIO_RemoveFile(spool_path);
    if (!ok && out_path) FIO_RemoveFile(out_path);
    free(mem);
    (void)sz; (void)need; (void)oh;
    if (failed && e == SE_OK) e = SE_E_IO;
    return e;
}

/* ---------------------------------------------------------- jpeg viewer */

/*
 * The module's own picture viewer.
 *
 * Canon's playback does not list files this module creates: a byte-identical
 * copy of the module's output, written from a Mac, lists and plays, while the
 * module's own file never appears. That is a directory-listing problem, not a
 * JPEG problem, and it is not something a module can fix from outside the
 * firmware's own directory handling -- so the developed frame is shown here.
 */
struct view_io {
    FILE    *f;
    uint32_t base;
    int32_t  pos;      /* where the file position is, -1 = unknown */
};

static int view_read(void *ctx, uint32_t off, void *dst, uint32_t n)
{
    struct view_io *v = (struct view_io *)ctx;
    if (v->pos != (int32_t)(v->base + off) &&
        FIO_SeekSkipFile(v->f, (int64_t)v->base + off, SEEK_SET) < 0)
        return -1;
    int got = se_read_io(v->f, dst, n);
    v->pos = (int32_t)(v->base + off) + got;
    return (got == (int)n) ? 0 : -1;
}

se_err se_cam_view_info(const char *jpg_path, int scale, int *out_w, int *out_h)
{
    static unsigned char hdr[SE_VIEW_HDR];
    FILE *f;
    se_jdec_info ji;
    se_jdec_info got;
    uint32_t total = 0;
    size_t hdrlen;

    if (!jpg_path) return SE_E_BADFILE;
    if (scale < 1) scale = 1;
    if (scale > 2) scale = 2;

    f = FIO_OpenFile(jpg_path, O_RDONLY | O_SYNC);
    if (!f) return SE_E_IO;
    if (FIO_GetFileSize(jpg_path, &total) != 0 || total < 128) {
        FIO_CloseFile(f);
        return SE_E_BADFILE;
    }
    hdrlen = sizeof hdr < (size_t)total ? sizeof hdr : (size_t)total;
    if (se_read(f, hdr, hdrlen) != (int)hdrlen) {
        FIO_CloseFile(f);
        return SE_E_IO;
    }
    FIO_CloseFile(f);

    if (se_jpeg_dec_info(hdr, hdrlen, &ji) != SE_OK) return SE_E_CORRUPT;
    got.width = ji.width;
    got.height = ji.height;
    got.ncomp = ji.ncomp;
    if (scale > 1) se_jpeg_scaled_size(ji.width, ji.height, scale, &got.width, &got.height);
    if (out_w) *out_w = got.width;
    if (out_h) *out_h = got.height;
    return SE_OK;
}

se_err se_cam_view(const char *jpg_path, int scale,
                   int (*sink)(void *ctx, const unsigned char *rgb, int nrows,
                               int width),
                   void *ctx, int *out_w, int *out_h)
{
    /* Static, not on the stack: a develop-sized local is what overflowed the
     * task stack before (100 KB of thumbnail workspace in a 32 KB stack). */
    static unsigned char hdr[SE_VIEW_HDR];
    struct view_io v;
    se_arena arena;
    se_jdec_info ji;
    unsigned char *mem;
    uint32_t total = 0;
    size_t hdrlen;
    se_err e;

    if (!jpg_path || !sink) return SE_E_BADFILE;
    if (scale < 1) scale = 1;
    if (scale > 2) scale = 2;

    v.f = FIO_OpenFile(jpg_path, O_RDONLY | O_SYNC);
    if (!v.f) return SE_E_IO;
    v.base = 0;
    v.pos = -1;

    if (FIO_GetFileSize(jpg_path, &total) != 0 || total < 128) {
        FIO_CloseFile(v.f);
        return SE_E_BADFILE;
    }

    hdrlen = sizeof hdr < (size_t)total ? sizeof hdr : (size_t)total;
    if (se_read(v.f, hdr, hdrlen) != (int)hdrlen) {
        FIO_CloseFile(v.f);
        return SE_E_IO;
    }
    v.pos = (int32_t)hdrlen;      /* the header read left us here */

    /* Smaller than the develop's arena on purpose: nothing here holds a whole
     * picture, and this can run while the develop's own buffers are still hot. */
    mem = malloc(SE_VIEW_ARENA);
    if (!mem) { FIO_CloseFile(v.f); return SE_E_NOMEM; }
    se_arena_init(&arena, mem, SE_VIEW_ARENA);

    e = se_jpeg_decode_stream_scaled(hdr, hdrlen, total, view_read, &v, scale,
                                     &arena, sink, ctx, &ji);
    free(mem);
    FIO_CloseFile(v.f);
    se_bounce_free();
    se_out_stop();
    if (e != SE_OK) return e;
    if (out_w) *out_w = ji.width;
    if (out_h) *out_h = ji.height;
    return SE_OK;
}

#ifdef SE_HOSTED
/*
 * Same colour path, different source: grade a standalone JPEG instead of the
 * preview embedded in a CR2.
 *
 * This exists so the recipes can be evaluated on real photographs without a
 * raw file. Everything after the decode is the identical code the camera runs
 * -- same se_look_from_stock, same se_look_rows, same encoder and quality --
 * so what it produces is what Develop would write, not a re-implementation.
 */
se_err se_cam_grade_jpeg(const char *jpg_path, const char *stock_path,
                         const char *out_path, se_cam_progress_fn progress,
                         se_cam_note_fn note, void *ctx)
{
    struct dev d;
    se_err     e = SE_OK;
    long       failed = 0;
    unsigned char *mem = NULL, *abuf = NULL, *stext = NULL;
    size_t     got = 0, jlen = 0;
    se_jdec_info ji;
    se_stock   st;
    se_look    lk;
    se_arena   arena;
    se_jpeg    enc;
    se_jpeg_exif ex;
    int        ow = 0, oh = 0, gscale = SE_DECODE_SCALE;
    size_t     ga_sz = SE_ARENA * 4;

    memset(&d, 0, sizeof d);
    memset(&ji, 0, sizeof ji);
    d.err = &failed;
    d.progress = progress;
    d.note = note;
    d.ctx = ctx;
    d.last_pct = -1;
    d.rd_pos = -1;
    se_set_log(engine_log, &d);
    se_set_progress(engine_progress, &d);

    if (progress) progress(ctx, "Opening", 0);

    mem = malloc(ga_sz + SE_OUTBUF + SE_STOCK_MAX);
    if (!mem) { se_set_log(NULL, NULL); return SE_E_NOMEM; }
    abuf   = mem;
    d.obuf[0] = mem + ga_sz;
    d.obuf[1] = d.obuf[0] + SE_OUTHALF;
    stext  = mem + ga_sz + SE_OUTBUF;

    e = read_whole(stock_path, (char *)stext, SE_STOCK_MAX, &got);
    if (e != SE_OK) goto done;
    e = se_stock_parse((const char *)stext, got, &st);
    if (e != SE_OK) goto done;
    se_log("grade: recipe %s (sat=%d grain=%d) from %s",
           st.name, (int)(st.sat + 0.5f), (int)(st.grain + 0.5f), stock_path);

    /* The JPEG is read whole: unlike a 5D, the host has the room. */
    {
        FILE *f = fopen(jpg_path, "rb");
        if (!f) { e = SE_E_IO; goto done; }
        fseek(f, 0, SEEK_END);
        jlen = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        d.in = (FILE *)malloc(jlen);
        if (!d.in || fread(d.in, 1, jlen, f) != jlen) { fclose(f); e = SE_E_IO; goto done; }
        fclose(f);
    }

    e = se_jpeg_dec_info(d.in, jlen, &ji);
    if (e != SE_OK) goto done;
    gscale = (ji.width > 2736) ? SE_DECODE_SCALE : 1;
    se_jpeg_scaled_size(ji.width, ji.height, gscale, &ow, &oh);

    se_look_from_stock(&st, SE_DECODE_SCALE, &lk);
    apply_look_options(&lk);
    se_arena_init(&arena, abuf, ga_sz);
    if (se_look_begin(&d.run, &lk, ow, &arena) != SE_OK) {
        e = SE_E_NOMEM;
        goto done;
    }

    /*
     * The whole frame: this path grades a JPEG it is handed, so it crops
     * nothing. The window must still be set, because look_emit() keeps rows
     * in crop_y0..crop_y1 and the struct starts zeroed -- with crop_y1 = 0 it
     * dropped every row and wrote a file of the encoder's grey initialiser.
     */
    d.crop_x0 = 0;
    d.crop_x1 = ow;
    d.crop_y0 = 0;
    d.crop_y1 = oh;
    d.row_in = 0;
    d.width = ow;
    d.height = oh;
    d.src_w = ow;
    d.src_h = oh;
    memset(&ex, 0, sizeof ex);
    ex.make = "Canon";              /* DCF readers look for these before anything else */
    ex.model = "Canon EOS";
    ex.software = st.name;
    {
        /* Fit inside 160x120 preserving aspect, then centre -- letterboxed, the
         * way every camera writes its DCF thumbnail. A 160x59 strip for an XPan
         * frame was accepted by PIL and ignored by the camera. */
        int tw = SE_THUMB_W, th = SE_THUMB_H;
        if (d.src_w * SE_THUMB_H > d.src_h * SE_THUMB_W) {
            th = d.src_h * SE_THUMB_W / (d.src_w ? d.src_w : 1);
            if (th < 1) th = 1;
            if (th > SE_THUMB_H) th = SE_THUMB_H;
        } else {
            tw = d.src_w * SE_THUMB_H / (d.src_h ? d.src_h : 1);
            if (tw < 1) tw = 1;
            if (tw > SE_THUMB_W) tw = SE_THUMB_W;
        }
        d.tw = tw;
        d.th = th;
        d.th_y0 = (SE_THUMB_H - th) / 2;
        d.th_x0 = (SE_THUMB_W - tw) / 2;
        d.th_step = d.src_h / th;
        if (d.th_step < 1) d.th_step = 1;
        /* Not the arena: the decoder draws from the same one, and 57 KB more of
         * it turned a working develop into "corrupt data". */
        d.thumb = thumb_ws.canvas;
        memset(d.thumb, 0, (size_t)SE_THUMB_W * SE_THUMB_H * 3);
    }

    {
        unsigned char *strip = SE_NEW(&arena, unsigned char, (size_t)SE_JPEG_MCU_ROWS * ow * 3);
        if (!strip) { e = SE_E_NOMEM; goto done; }
        d.out = fopen(out_path, "wb");
        if (!d.out) { e = SE_E_IO; goto done; }
        d.enc = &enc;
        if (se_jpeg_begin(&enc, ow, oh, 88, strip, &ex, jpeg_out, &d) != SE_OK) {
            e = SE_E_IO;
            goto done;
        }
    }

    if (progress) progress(ctx, "Decoding", 10);
    /* Must match gscale: the look was built for ow x oh, so decoding
     * full size here feeds it rows twice as wide as it allocated. */
    if (se_jpeg_decode_scaled(d.in, jlen, gscale, &arena,
                              decode_rows, &d, &ji) != SE_OK) {
        e = SE_E_CORRUPT;
        goto done;
    }

    se_look_end(d.run, look_emit, &d);
    se_jpeg_end(&enc);
    d.enc = &enc;
    if (out_flush(&d) != 0 || se_out_drain(d.out) != 0) e = SE_E_IO;
    if (e == SE_OK && d.thumb && d.out)
        se_attach_thumb(&d, &ex, ow, oh);
    d.enc = NULL;
    if (failed) e = SE_E_IO;
    if (progress) progress(ctx, "Done", 100);

done:
    se_set_log(NULL, NULL);
    se_set_progress(NULL, NULL);
    if (d.enc) { se_jpeg_end(&enc); d.enc = NULL; }
    if (d.out) { fclose(d.out); d.out = NULL; }
    free(d.in);
    free(mem);
    return e;
}
#endif /* SE_HOSTED */

/* ------------------------------------------------------------ YUV -> JPEG */

/*
 * The shot path's exit.
 *
 * The camera builds the frame in RAM and the hook grades it in place, so a
 * file can be produced without touching the CR2 at all: no decode, no spool,
 * no second copy of a 20 MP image. Rows go UYVY -> RGB with the full-range
 * BT.601 the LiveView path already uses, then through the same encoder the
 * develop uses.
 */
/*
 * The encoder hands out many small chunks.  Writing each one to the card with
 * its own FIO call is what made the first _F file take a minute for 55% of the
 * frame: the DCT is expensive, but a few hundred small writes on exFAT are
 * their own tax.  Buffer 64 KB and write that.
 */
#define YUVSAVE_OUTBUF (64 * 1024)

struct yuvsave_io { FILE *f; int err; size_t used; unsigned char *buf; };

static int yuvsave_flush(struct yuvsave_io *w)
{
    if (w->used && FIO_WriteFile(w->f, w->buf, (int)w->used) != (int)w->used) {
        w->err = 1;
        return -1;
    }
    w->used = 0;
    return 0;
}

static int yuvsave_sink(void *ctx, const uint8_t *data, size_t len)
{
    struct yuvsave_io *w = (struct yuvsave_io *)ctx;
    if (w->err)
        return -1;
    while (len) {
        size_t room = YUVSAVE_OUTBUF - w->used;
        size_t n = len < room ? len : room;
        memcpy(w->buf + w->used, data, n);
        w->used += n;
        data += n;
        len -= n;
        if (w->used == YUVSAVE_OUTBUF && yuvsave_flush(w) != 0)
            return -1;
    }
    return 0;
}

static int yuvsave_c8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* What the last se_cam_save_yuv() actually wrote, as opposed to what it was
 * handed: with step > 1 those differ, and the caller's log has to say which
 * file it produced or the size on the card looks like a truncation. */
static int yuvsave_last_w = 0, yuvsave_last_h = 0;
int se_cam_save_last_w(void) { return yuvsave_last_w; }
int se_cam_save_last_h(void) { return yuvsave_last_h; }

/*
 * Our own copy of the graded frame, taken inside the hook.
 *
 * Encoding 6-15 s after the shot reads a 40 MB buffer that Canon has already
 * handed back: LiveView resumes, the two 720x480 planes (691,200 bytes each)
 * get allocated inside it, and the rows that land there come out as noise.
 * Measured on IMG_9982_F.JPG: 64 output rows of pure noise ending at output row
 * 415 in five of six files, 1,400,832 bytes -- against 2 x 691,200 = 1,382,400
 * for two LV planes. Canon's own JPEG of the same frame is clean in exactly
 * those rows, so the damage happens after Canon encoded and before we read.
 *
 * The copy is taken in the hook, so it cannot be overwritten afterwards. It is
 * decimated the same way the encoder will read it, so it is small: 5472x2020 at
 * step 2 becomes 2736x1010x2 = 5.5 MB rather than 22 MB.
 */
static unsigned char *se_keep = NULL;
static size_t se_keep_len = 0;
static int se_keep_w = 0, se_keep_h = 0;

int se_cam_kept_w(void) { return se_keep_w; }
int se_cam_kept_h(void) { return se_keep_h; }

se_err se_cam_keep_yuv(const void *yuv, int width, int rows, int stride, int step)
{
    const unsigned char *src = (const unsigned char *)yuv;
    int x0 = 0, x1 = width, y0 = 0, y1 = rows, out_w, out_h, r, c;
    size_t need;

    se_keep_w = se_keep_h = 0;
    if (!yuv || width < 2 || rows < 1 || stride < width * 2 || step < 1)
        return SE_E_UNSUPPORTED;

    se_cam_aspect_window(width, rows, &x0, &x1, &y0, &y1);
    if (x1 - x0 < 2 || y1 - y0 < 1) { x0 = 0; x1 = width; y0 = 0; y1 = rows; }
    x0 &= ~1; x1 &= ~1;

    out_w = ((x1 - x0) / (2 * step)) * 2;
    out_h = (y1 - y0) / step;
    if (out_w < 2 || out_h < 1)
        return SE_E_UNSUPPORTED;

    need = (size_t)out_w * 2u * (size_t)out_h;
    if (need != se_keep_len) {
        free(se_keep);
        se_keep = (unsigned char *)malloc(need);
        se_keep_len = se_keep ? need : 0;
        if (!se_keep)
            return SE_E_NOMEM;
    }

    for (r = 0; r < out_h; r++) {
        const unsigned char *s = src + (size_t)(y0 + r * step) * (size_t)stride
                               + (size_t)x0 * 2;
        unsigned char *d = se_keep + (size_t)r * (size_t)out_w * 2;
        for (c = 0; c < out_w; c += 2) {
            memcpy(d, s, 4);
            s += 4 * step;
            d += 4;
        }
    }
    se_keep_w = out_w;
    se_keep_h = out_h;
    return SE_OK;
}

/* Encode the frame kept by se_cam_keep_yuv(); no crop is applied twice. */
static se_err yuvsave_core(const void *yuv, int width, int rows, int stride,
                           const char *path, int quality, int step, int do_crop)
{
    const unsigned char *src = (const unsigned char *)yuv;
    struct yuvsave_io w;
    se_jpeg enc;
    unsigned char *strip = NULL, *rgb = NULL;
    se_err e = SE_OK;
    int y = 0, started = 0;
    int x0 = 0, x1 = width, y0 = 0, y1 = rows, out_w;

    if (!yuv || width < 2 || rows < 1 || stride < width * 2 || !path)
        return SE_E_UNSUPPORTED;
    yuvsave_last_w = 0;
    yuvsave_last_h = 0;

    /*
     * Same crop the develop applies, from the same function, so a 16:9 or XPan
     * framing is framed the same way in a file this path writes as in one the
     * develop writes. Without this the shot path silently ignored
     * film.sim.aspect and wrote the whole sensor frame.
     */
    if (do_crop)
        se_cam_aspect_window(width, rows, &x0, &x1, &y0, &y1);
    if (x1 - x0 < 2 || y1 - y0 < 1) { x0 = 0; x1 = width; y0 = 0; y1 = rows; }
    x0 &= ~1; x1 &= ~1;

    /*
     * step > 1 keeps every step-th row and every step-th pixel pair, i.e. a
     * 20 MP frame becomes 5 MP.  Not decoration: encoding 5472x3648 took longer
     * than the camera stayed on (4.26 MB written, no EOI, 30 s and counting,
     * with LiveView still running).  Cost scales with pixels, so this is the
     * only lever that changes it by a factor rather than a few percent.
     */
    if (step < 1) step = 1;
    {
        int wsrc = x1 - x0;
        int hsrc = y1 - y0;
        out_w = (wsrc / (2 * step)) * 2;
        rows  = hsrc / step;
        if (out_w < 2 || rows < 1) { step = 1; out_w = wsrc; rows = hsrc; }
    }
    yuvsave_last_w = out_w;
    yuvsave_last_h = rows;

    strip = malloc((size_t)SE_JPEG_MCU_ROWS * (size_t)out_w * 3);
    rgb   = malloc((size_t)SE_JPEG_MCU_ROWS * (size_t)out_w * 3);
    w.buf = malloc(YUVSAVE_OUTBUF);
    w.used = 0;
    if (!strip || !rgb || !w.buf) { e = SE_E_NOMEM; goto done; }

    w.f = FIO_CreateFile(path);
    w.err = 0;
    if (!w.f) { e = SE_E_IO; goto done; }

    if (se_jpeg_begin(&enc, out_w, rows, quality, strip, NULL,
                      yuvsave_sink, &w) != SE_OK) { e = SE_E_IO; goto done; }
    started = 1;

    while (y < rows) {
        int n = rows - y, r, x;
        if (n > SE_JPEG_MCU_ROWS) n = SE_JPEG_MCU_ROWS;
        for (r = 0; r < n; r++) {
            const unsigned char *s = src + (size_t)(y0 + (y + r) * step) * (size_t)stride
                                   + (size_t)x0 * 2;
            unsigned char *o = rgb + (size_t)r * (size_t)out_w * 3;
            for (x = 0; x + 1 < out_w; x += 2) {
                int u  = (int)(signed char)s[0], yy0 = s[1];
                int v  = (int)(signed char)s[2], yy1 = s[3];
                int rv = (359 * v + 128) >> 8;
                int gu = ( 88 * u + 183 * v + 128) >> 8;
                int bu = (454 * u + 128) >> 8;
                o[0] = (unsigned char)yuvsave_c8(yy0 + rv);
                o[1] = (unsigned char)yuvsave_c8(yy0 - gu);
                o[2] = (unsigned char)yuvsave_c8(yy0 + bu);
                o[3] = (unsigned char)yuvsave_c8(yy1 + rv);
                o[4] = (unsigned char)yuvsave_c8(yy1 - gu);
                o[5] = (unsigned char)yuvsave_c8(yy1 + bu);
                s += 4 * step;
                o += 6;
            }
        }
        if (se_jpeg_write_rows(&enc, rgb, n) != SE_OK) { e = SE_E_IO; break; }
        y += n;
    }
    if (started)
        se_jpeg_end(&enc);
    if (yuvsave_flush(&w) != 0)
        w.err = 1;
    if (w.err)
        e = SE_E_IO;

done:
    if (w.f) {
        FIO_CloseFile(w.f);
        if (e != SE_OK)
            FIO_RemoveFile(path);
    } else if (e != SE_OK) {
        FIO_RemoveFile(path);
    }
    free(strip);
    free(rgb);
    free(w.buf);
    return e;
}

se_err se_cam_save_yuv(const void *yuv, int width, int rows, int stride,
                       const char *path, int quality, int step)
{
    return yuvsave_core(yuv, width, rows, stride, path, quality, step, 1);
}

/*
 * Encode the frame kept by se_cam_keep_yuv(). Geometry was already applied
 * when the copy was taken, so the crop and the step are 1 here -- applying
 * film.sim.aspect again would crop the crop.
 */
se_err se_cam_save_kept(const char *path, int quality)
{
    if (!se_keep || se_keep_w < 2 || se_keep_h < 1)
        return SE_E_UNSUPPORTED;
    return yuvsave_core(se_keep, se_keep_w, se_keep_h, se_keep_w * 2,
                        path, quality, 1, 0);
}
