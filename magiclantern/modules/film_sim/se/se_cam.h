/*
 * se_cam.h - run the Soup Engine develop on the camera.
 *
 * This is the piece that was missing: the engine could grade a CR2 on a
 * desktop but not on the body. It uses the route the engine documents as
 * the camera route -- take the full-size JPEG DIGIC already wrote into the
 * CR2 and grade that -- and it never holds the coded file in RAM. Only a
 * 256 KB header window is resident (measured: se_cr2_parse needs 128 KB on
 * a 6D file), the JPEG is read forwards through a seek-and-read callback in
 * 8 KB steps, and the encoder writes out as rows arrive.
 *
 * Peak RAM is about 560 KB: 420 KB of arena for the look and the decoder,
 * plus the 8-row encoder strip. Measured on a 5472x3648 6D frame.
 */
#ifndef SE_CAM_H_INCLUDED
#define SE_CAM_H_INCLUDED

#include "engine/se.h"
#include "frame.h"

/*
 * Framing applied to the file the develop writes, indexed as in
 * film_sim_aspect: 0 full 3:2, 1 1:1, 2 4:5, 3 9:16, 4 16:9, 5 65:24 XPan,
 * 6 2.39:1 cinemascope. The rows outside the window are never encoded, so the
 * result is a crop, not letterbox bars. Set before calling a develop.
 */
void se_cam_set_aspect(int mode);

/* Letterbox framings wider than 3:2 (16:9, 65:24, 2.39:1) into a 3:2 frame. */
void se_cam_set_pad32(int on);

/*
 * Film frame: bands top and bottom carrying the stock name and the camera
 * line, drawn by se/frame.c. NULL or frame->on == 0 draws nothing. Not
 * applied to the reduced-size develops (thumbnail/preview paths).
 */
void se_cam_set_frame(const se_frame *f);

/* How much of the stock to apply to a developed file, 0..100 (100 = as written). */
void se_cam_set_strength(int s);
/* Packed 3D cube (tools/cube2bin.py) applied to the decoded rows before the
 * look; NULL or "" turns it off. Loaded lazily on first use, once. */
void se_cam_lut_path(const char *p);

/* Saturation on top of the stock's own, 100 = as written, 200 = double. */
void se_cam_set_satboost(int s);
/* -1 keeps each stock's GRAIN_SIZE; 0..100 overrides it. */
void se_cam_set_grainsize(int v);

/* Called with a stage name and 0..100. May be NULL. */
typedef void (*se_cam_progress_fn)(void *ctx, const char *stage, int percent);
/* Called with one finished line. May be NULL. */
typedef void (*se_cam_note_fn)(void *ctx, const char *line);

/*
 * Develop `cr2_path` with the stock in `stock_path`, writing `out_path`.
 *
 * Returns SE_OK, or SE_E_* with se_strerror() describing why. Never leaves a
 * partial file behind: the output is removed if anything fails.
 */
se_err se_cam_develop(const char *cr2_path, const char *stock_path,
                      const char *out_path,
                      se_cam_progress_fn progress, se_cam_note_fn note,
                      void *ctx);

/*
 * Level 3: develop the sensor data instead of the embedded JPEG.
 *
 * Decodes the CR2's lossless-JPEG raw to a spool file beside the output (a 6D
 * frame is 40 MB decoded and cannot live in RAM), then streams it back through
 * the engine's raw pipeline: black + WB on the CFA grid, demosaic (scale 2 bins
 * 2x2 quads instead of interpolating, an order of magnitude less work), matrix
 * to sRGB. Rows come back as 8-bit RGB, where the film look and the JPEG
 * encoder take over. `scale` is 1 (full, Malvar-He-Cutler), 2 or 4.
 */
se_err se_cam_develop_raw(const char *cr2_path, const char *stock_path,
                          const char *out_path, int scale,
                          se_cam_progress_fn progress, se_cam_note_fn note,
                          void *ctx);

/*
 * Encode a UYVY422 frame straight to a JPEG file.
 *
 * This is the shot path's exit: the frame the camera built is already in RAM
 * and has already been graded in place, so there is nothing to decode and no
 * raw file to open. Rows are converted to RGB with the same full-range BT.601
 * the LiveView path uses, then handed to the same encoder the develop uses.
 *
 * `stride` is in bytes. Returns SE_OK, or SE_E_* with se_strerror().
 */
se_err se_cam_save_yuv(const void *yuv, int width, int rows, int stride,
                       const char *path, int quality, int step);

/* Dimensions of the file that last se_cam_save_yuv() wrote. Differs from the
 * arguments when step > 1 or when film.sim.aspect crops. */
/* The crop window film.sim.aspect selects, in whatever frame size it is given.
 * One implementation, so the preview guides and the developed file cannot
 * disagree about where the frame is. */
void se_cam_aspect_window(int w, int h, int *x0, int *x1, int *y0, int *y1);

int se_cam_save_last_w(void);
int se_cam_save_last_h(void);

/*
 * Copy the frame the hook just graded into memory the module owns, so the
 * encode that follows (seconds later) does not read a buffer Canon has reused
 * for its LiveView planes. Cropped and step-decimated exactly as the encoder
 * will read it; call it while still inside the hook.
 */
se_err se_cam_keep_yuv(const void *yuv, int width, int rows, int stride, int step);
int se_cam_kept_w(void);
int se_cam_kept_h(void);

/* Encode the kept copy. Returns SE_E_UNSUPPORTED when nothing was kept. */
se_err se_cam_save_kept(const char *path, int quality);

/*
 * Decode a JPEG on the card and stream its rows to `sink` (8-bit RGB).
 *
 * Added for the module's own viewer. The body's playback will not list a file
 * this module wrote -- it lists Mac-created copies of the same bytes and plays
 * them, so the file is fine and the listing is the problem -- which leaves the
 * module to show its own output. `scale` is 1 or 2 (2 = half size by a 4-point
 * IDCT). Returns the DELIVERED size through out_w and out_h.
 */
se_err se_cam_view_info(const char *jpg_path, int scale, int *out_w, int *out_h);

se_err se_cam_view(const char *jpg_path, int scale,
                   int (*sink)(void *ctx, const uint8_t *rgb, int nrows, int width),
                   void *ctx, int *out_w, int *out_h);

#ifdef SE_HOSTED
/* Host-only: run the develop colour path on a standalone JPEG. Lets the
 * recipes be evaluated on real photographs without a raw file. */
se_err se_cam_grade_jpeg(const char *jpg_path, const char *stock_path,
                         const char *out_path, se_cam_progress_fn progress,
                         se_cam_note_fn note, void *ctx);
#endif

#endif /* SE_CAM_H_INCLUDED */
