#ifndef _fs_yuvlook_h_
#define _fs_yuvlook_h_

/*
 * The shot-time look, baked from a .fs recipe into three tables and applied to
 * the UYVY frame Canon's encoder is about to read. See fs_yuvlook.c for what
 * this can and cannot express (grain and halation stay in the develop).
 */

/* 0 = baked, ready to apply. `strength` is 0..100, the same slider the
 * develop honours. */
int  fs_yuvlook_bake(const char *stock_path, int strength);
/* Same bake, with the stock's own 3D cube applied before the look -- the
 * order Develop uses. NULL lut_path is exactly fs_yuvlook_bake(). */
void fs_yuvlook_set_satboost(int s);
int  fs_yuvlook_bake_lut(const char *stock_path, int strength,
                         const char *lut_path);

/* How much of the look the shot path applies: 0 high, 1 normal, 2 fast.
 * See the comment on the definition for what each one drops. */
void fs_yuvlook_set_quality(int q);
int  fs_yuvlook_quality(void);

int  fs_yuvlook_ready(void);
const char *fs_yuvlook_name(void);

/* The baked tables, for host tools that plot them. se_yuvtbl lives in
 * engine/look.h, which a caller includes first when it wants the contents. */
#include "se/engine/look.h"
const se_yuvtbl *fs_yuvlook_table(void);

/* The same look as a joint (y,u,v) lattice -- the form the LiveView filter
 * applies. NULL until a bake succeeds. */
const se_yuv3 *fs_yuvlook_uv3(void);

/* Millisecs spent, 0 when nothing was done. */
/* Exact path: the real engine per pixel on the shot buffer. Slower than the
 * baked tables, and free of their 256-entry quantisation. */
int  fs_yuvlook_engine_ready(void);
/* LiveView form: same engine, colour only (no halation, no grain). */
int  fs_yuvlook_engine_lv_ready(void);
uint32_t fs_yuvlook_apply_engine_lv(unsigned char *buf, int width, int height, int stride);
uint32_t fs_yuvlook_apply_engine(unsigned char *buf, int width, int height, int stride);

uint32_t fs_yuvlook_apply(unsigned char *buf, int width, int height, int stride);

#endif
