/**
 * Film Simulation - real-time film look in LiveView.
 *
 * Hooks the display filter API (CBR_DISPLAY_FILTER, see module.h) and
 * rewrites the LiveView image in place.  The actual image processing lives
 * in fs_core.h, with no ML dependencies, so the host simulator in
 * tools/sim runs exactly the same code.
 *
 * This affects the LiveView preview only.  Recorded raw data (CR2/DNG, MLV)
 * is not modified, by design: use the matching LUT produced by
 * tools/filmsim_lut.py to apply the same look in post.
 *
 * Display filter buffers on DIGIC 4/5 (including 6D) are UYVY: one 32-bit
 * word holds U, Y0, V, Y1, with U/V stored as signed two's complement bytes
 * (neutral = 0).  See fs_core.h and imgconv.c.
 */

#include <arm-mcr.h>
#include <battery.h>
#include <beep.h>
#include <bmp.h>
#include <config.h>
#include <dryos.h>
#include <fio-ml.h>
#include <imgconv.h>
#include <lens.h>
#include <lvinfo.h>
#include <mem.h>
#include <menu.h>
#include <module.h>
#include <picstyle.h>
#include <powersave.h>
#include <shoot.h>          /* close_liveview(): stop the sensor, not just ignore it */
#include <property.h>
#include <vram.h>
#include <zebra.h>

#include "../film_table.h"
#include "film_xml.h"
#include "se/frame.h"
#include <propvalues.h>
#include "fs_core.h"
#include "fs_yuvhook.h"
#include "fs_yuvlook.h"
#include "gui/fsg.h"
#include "gui/fs_num.h"
#include "gui/fsg_creator.h"
#include "gui/fsg_wb.h"
#include "gui/fs_spring.h"
#include "gui/fs_fuji.h"
#include "se/se_cam.h"

#define FS_PATH_LEN 128

static inline int fs_ends_with(const char *str, const char *suffix) {
  if (!str || !suffix)
    return 0;
  size_t len_str = strlen(str);
  size_t len_suf = strlen(suffix);
  if (len_str < len_suf)
    return 0;
  return strcasecmp(str + len_str - len_suf, suffix) == 0;
}

static CONFIG_INT("film.sim.enable", film_sim_enable, 1);

static CONFIG_INT("film.sim.preset", film_sim_preset, FS_PRESET_FUJI_SUPERIA);
static CONFIG_INT("film.sim.stock", film_sim_stock, 1);
static CONFIG_INT("film.sim.strength", film_sim_strength, 100);

/* Saturation on top of every stock's own SAT line, 100 = as written. The
 * develop path had no such control at all; this one applies to all fourteen
 * recipes at once. */
static CONFIG_INT("film.sim.satboost", film_sim_satboost, 100);
static CONFIG_INT("film.sim.grain", film_sim_grain, 8);
/* -1 = leave each stock's own GRAIN_SIZE alone; 0..100 overrides it. */
static CONFIG_INT("film.sim.grainsize", film_sim_grainsize, -1);
/*
 * Apply an external 3D cube while developing.
 *
 * The file is ML/FILMSIM/LOOK.BIN, made from a .cube by tools/cube2bin.py: the
 * camera cannot hold a .cube's 786 KB of text, and 17^3 is 29 KB. It is applied
 * to the decoded rows just before the film look, which is the sRGB input those
 * cubes are exported for. With this on, set Strength to 0 if the cube is meant
 * to BE the look rather than sit under it.
 */
static CONFIG_INT("film.sim.lut", film_sim_lut, 0);

/*
 * The stock's own cube, or NULL. Only stocks that arrived with a
 * ML/FILMSIM/LUT/ entry have one; the module's built-in names point at
 * ML/DATA/<CODE>.CUB, which does not exist, so they keep the global LOOK.BIN.
 * One function, so the develop path and the LiveView bake cannot disagree
 * about which cube is in play.
 */
static const char *fs_own_lut_path(void)
{
  if (film_active_count <= 0) return NULL;
  {
    int idx = fs_clamp_i(film_sim_stock, 0, film_active_count - 1);
    const char *p = film_active_table[idx].lut;
    if (p && !strncmp(p, "ML/FILMSIM/LUT/", 15)) return p;
  }
  return NULL;
}


/* Grade LiveView through the joint (y,u,v) lattice instead of the
 * preset tables. Off by default: it is ~3.5x the lookups and the
 * camera's frame budget for that is unmeasured. */
static CONFIG_INT("film.sim.lv3d", film_sim_lv3d, 0);
/* Defined up here because fs_sync() decides from it whether the bars are
 * honest: only a develop-and-replace shot is actually cropped. */
static CONFIG_INT("film.sim.replace", film_sim_replace, 1);
/*
 * Fuji-style white balance shift: the same thing a Fuji recipe calls WB Shift
 * (Blue/Amber, Green/Magenta), pushed into the CAMERA's own setting rather
 * than into the look. That is the difference that matters -- it moves the
 * white point DIGIC uses, so it travels with the exposure and the picture
 * style, exactly as it does on a Fuji body.
 *
 * -9..+9, one unit is 5 mireks on the Kelvin axis. 0/0 is no shift.
 */
static CONFIG_INT("film.sim.wbs_ba", film_sim_wbs_ba, 0);
static CONFIG_INT("film.sim.wbs_gm", film_sim_wbs_gm, 0);
static CONFIG_INT("film.sim.fps", film_sim_fps, 30);
static CONFIG_INT("film.sim.user1", film_sim_user1,
                  1); /* inject into User 1 + select it */
/* On by default until the filter has been seen working on the panel: it costs
 * one line per 120 frames and it is the only proof the grade reaches the
 * LiveView buffer at all. Switch it off in Debug once that is settled. */
/* Artwork on/off. A card whose CANS.DAT/FILMCAN.DAT the loader dislikes must
 * still leave a working shelf, and "turn it off" is a faster answer than a
 * rebuild while the cause is unknown. */
static CONFIG_INT("film.sim.art", film_sim_art, 1);
/* 0 = compose in the idle buffer and commit with one copy. That is the Lua
 * double-buffer pattern and it is the only way to stop the panel showing a
 * half-drawn screen; drawing primitives straight into the buffer the LCD is
 * scanning out (the old default) tore on every animation frame. */
static CONFIG_INT("film.sim.direct", film_sim_direct, 0);
/* Paint the palette calibration strip when a screen opens. 1 = on for
 * this round of testing; set 0 once the palette question is answered. */
static CONFIG_INT("film.sim.calib", film_sim_calib, 0);
static CONFIG_INT("film.sim.dbglog", film_sim_dbglog,
                  1); /* periodic LiveView logging */
/* Framing for the preview and the written file:
 * 0 full 3:2, 1 1:1, 2 4:5, 3 9:16, 4 16:9, 5 XPan 65:24,
 * 6 Cinemascope 2.39:1. */
static CONFIG_INT("film.sim.aspect", film_sim_aspect, 0);
/*
 * Letterbox framings wider than 3:2 into a 3:2 frame.
 *
 * A 65:24 or 16:9 file is legal, but the body's playback treats it like any
 * other photo and the result is not the frame that was composed. A 3:2 file
 * with black bars is unambiguous -- it looks like a contact sheet of a wide
 * negative -- and it survives every viewer. Off by default only for people who
 * want the bare crop; on for everyone who wants the camera to show what was
 * framed.
 */
static CONFIG_INT("film.sim.pad", film_sim_pad, 1);
/* Film frame: a rebate band top and bottom with the stock and camera line.
 * Off by default -- it changes the file the camera writes, so it is opt-in. */
static CONFIG_INT("film.sim.frame", film_sim_frame, 0);
#define FS_ASPECT_MAX 6

/* Off by default: this patches two firmware entry points to look at the frame
 * Canon is about to encode. See fs_yuvhook.c -- it only reads, but it is an
 * experiment and no shot should depend on an experiment being right. */
static CONFIG_INT("film.sim.yuvprobe", film_sim_yuvprobe, 0);

/* Grade the frame inside the shot instead of developing it afterwards.
 * Default 3648 rows (full 5472x3648 still frame). */
static CONFIG_INT("film.sim.yuvrows", film_sim_yuvrows, 3648);

/*
 * How much of the look the in-shutter grade applies.
 *
 * One core, and the grade sits inside Canon's pipeline, so its cost is added to
 * every shot: measured on the body at 3648 rows in 103 ms of reported clock
 * (~1 s of wall time). Measured on a Mac over the same 5472x3648 frame:
 * 21.0 ms high, 16.7 normal, 4.1 fast. The body scales those the same way.
 *
 *   0 High    the look, per-hue cells included          (what it always did)
 *   1 Normal  drop the per-hue cells: global chroma gain and matrix stay
 *   2 Fast    tone curve only, chroma left exactly as Canon wrote it
 */
static CONFIG_INT("film.sim.gradeq", film_sim_gradeq, 0);
/*
 * Neutral base at capture, and the switch that arms the in-shutter grade.
 *
 * Two mechanisms can put the look into a JPEG and they must not both run:
 *
 *   off (default) -- the stock's sliders are written into the picture style,
 *                    so Canon applies the look while it builds the YUV.
 *   on            -- the style is forced to built-in Neutral (base 0x84) and
 *                    fs_yuvhook_grade() is armed, so the frame is graded
 *                    inside sssCompleteSsrawToYuv, after Canon's style has
 *                    already shaped it (see fs_yuvhook.c). Either this key or
 *                    film.sim.capture arms it, and never at level 1 -- the two
 *                    are aliases, see fs_grade_in_shutter().
 *
 * Hence the gate is on the style, not on the grade: grading a frame that a
 * stock style already graded gives two looks stacked. Measured with the gate
 * missing: IMG_9973_F.JPG sat 63.2 against 29.6 in the camera's own JPEG of the
 * same shot.
 *
 * The grade does reach the file: a black-and-white recipe in this mode gives a
 * JPEG with R = G = B over the whole 5472x3648 frame. Read it together with the
 * "yuvcap" line from the same run -- the capture style must be built-in Neutral
 * (0x84), otherwise Canon's own Monochrome style (0x86) could be the source. The
 * log lines to check are
 * "yuvgrade: in-shutter grade ON (capture style Neutral)", and
 * "yuvcap: cache alias used ..." says how it wrote the buffer.
 */
static CONFIG_INT("film.sim.neutralcap", film_sim_neutralcap, 0);

/* Sharpness written into the forced-Neutral base, 0..7 on Canon's scale, where
 * 3 is what the Standard style uses. Only applies while the capture style is
 * Neutral (see fs_grade_in_shutter); at 0 the file is as soft as the sensor. */
static CONFIG_INT("film.sim.sharp", film_sim_sharp, 3);

/* The CR2's embedded JPEG is rendered through the active picture style, and
 * Develop decodes that same JPEG. If the style already applies the film look,
 * the .fs applies it a second time -- measured 20.8/255 for Velvia and
 * 23.5/255 for CineStill 800T on real captures. Neutral leaves base, sliders,
 * Kelvin and WB shift at Canon defaults so the recipe is the only grading.
 * This is the key the "Neutral Capture" menu item writes; film.sim.neutralcap
 * is the cfg alias for the same thing. */
static CONFIG_INT("film.sim.capture", film_sim_capture, 0);
/*
 * How much of the pipeline the develop uses.
 *
 *   1  picture style only: the look is written into the style slots and the
 *      camera bakes it into the JPEG it shoots. Nothing to develop, no cost.
 *   2  style + our grade: the style does the base colour, the engine adds
 *      curves, hue axes, split tone, halation and grain on the preview JPEG.
 *   3  raw develop: the sensor data is decoded and graded from scratch -- no
 *      picture style involved at all. Both payloads this body writes are
 *      handled: a mosaic frame, and the mRAW the 6D actually stores.
 */
#define FS_LEVEL_STYLE 1
#define FS_LEVEL_GRADE 2
#define FS_LEVEL_RAW 3
static CONFIG_INT("film.sim.level", film_sim_level, FS_LEVEL_GRADE);

/*
 * Is the look applied inside the shot?
 *
 * One predicate, because four places have to agree and used to not: the style
 * is forced to built-in Neutral, the stock's sliders are not written into the
 * style, the PF2 blob is not injected, and the hook is armed. Any one of them
 * disagreeing grades the frame twice -- Canon's style and then the engine --
 * which measured 20.8/255 on Velvia and saturation 63.2 against the camera's
 * own 29.6 on IMG_9973_F.
 *
 * The reverse case matters just as much: at level 1 the style IS the look (no
 * develop runs), so this must be false there.
 *
 * Two keys ask for the same thing and either one is enough: film.sim.capture is
 * what the "Neutral Capture" menu item writes, film.sim.neutralcap is the cfg
 * key that older cards carry. Treating them as an OR means neither can be set
 * without the other half of the job following it -- a menu switch that silently
 * did nothing (capture=1, neutralcap=0 shot a flat Neutral JPEG with no look at
 * all) and a cfg key that graded on top of a stock style (neutralcap=1,
 * capture=0) were both reachable before this.
 */
/* film_sim_level comes from the cfg file, where nothing clamps it; the menu
 * does. A level of 0 or 7 used to arm the in-shutter grade and still let the
 * develop path treat the frame as "develop it", which grades twice. */
static int fs_level(void) {
  return fs_clamp_i(film_sim_level, FS_LEVEL_STYLE, FS_LEVEL_RAW);
}

static int fs_grade_in_shutter(void) {
  return (film_sim_capture || film_sim_neutralcap) &&
         fs_level() != FS_LEVEL_STYLE;
}

/*
 * Queue mode: develop when the camera is idle instead of when the shot is
 * taken.
 *
 * A develop takes the screen (enter_play_mode + UILOCK_SHUTTER), which is why
 * the shutter cannot fire for the twenty seconds it runs. In queue mode the
 * develop runs without touching the screen at all, so the camera stays ready:
 * frames are collected by the watcher and drained by a worker whenever one is
 * not already running. The cost is that the JPEG on the card is still the
 * camera's until the queue reaches it.
 */
static CONFIG_INT("film.sim.queue", film_sim_queue, 0);
#define FS_QUEUE_MAX 16
static char fs_queue[FS_QUEUE_MAX][64];
static volatile int fs_queue_n;
/* In-shot gradings counted when the last frame was accepted; a change is the
 * only per-frame evidence that the frame in hand was graded too. */
static int fs_auto_graded_seen;

static fs_lut_t fs_lut;
static unsigned int fs_signature = 0;
static unsigned int fs_signature2 = 0;
static int fs_signature_valid = 0;
static int fs_valid_frame = 0;
static unsigned int fs_rnd = 0x9E3779B9;
static int fs_prev_t = 0;

/*
 * Real-camera non-blocking debug logger (ML/LOGS/FILMSIM.LOG).
 *
 * Never perform SD card I/O inside CBR_DISPLAY_FILTER or MENU_UPDATE_FUNC.
 * Instead, fs_log() writes timestamped lines into a RAM ring buffer and
 * fs_slot_backup_task flushes them asynchronously to ML/LOGS/FILMSIM.LOG.
 */
#define FS_LOG_BUF_SIZE 8192
static char fs_log_buf[FS_LOG_BUF_SIZE];
static volatile uint32_t fs_log_head = 0;
static volatile uint32_t fs_log_tail = 0;
static volatile int fs_user1_dirty = 0;
static volatile int fs_user1_last_preset = -1;
static volatile int fs_yuvlook_dirty = 0;
static int fs_active_stock_path(char *stk, int cap, const film_xml_t **out_f);
static volatile int fs_slot1_avail = 0;
static volatile int fs_force_dump_log = 0;

/*
 * The register the LiveView panel scans out of.
 *
 * platform/6D.116/consts.h has this as YUV422_LV_BUFFER_DISPLAY_ADDR, but a
 * module does not get that header. Same address, same expression, guarded so a
 * build for another body logs 0 rather than a wild guess.
 */
/* No CONFIG_6D in a module's defines (checked: the module compile line has
 * only __ARM__, CONFIG_MAGICLANTERN, MODULE*), so this cannot be conditional.
 * Same reasoning as fs_yuvhook.c, which already carries 6D firmware addresses:
 * the module is built for one body. */
#define FS_LV_DISPLAY_ADDR (*(volatile uint32_t *)(0x7550C + 0xA4))

/* Canon's three LiveView planes, uncached and cached alias. Anything else in
 * the display register is one of the module's own buffers. */
static int fs_is_canon_plane(uint32_t a) {
  return a == 0x5F227800 || a == 0x5F637800 || a == 0x5EE17800 ||
         a == 0x1F227800 || a == 0x1F637800 || a == 0x1EE17800;
}

/* Live telemetry counters for on-camera debugging */
static volatile uint32_t fs_stat_frames = 0;
static volatile uint32_t fs_stat_skipped = 0;
static volatile uint32_t fs_stat_junk_keys = 0;
static volatile int fs_stat_last_ms = 0;
static volatile int fs_stat_max_ms = 0;
static volatile int fs_stat_copy_ms = 0;
/* Split of the LiveView pass: copying the source costs one thing, the per-pixel
 * look another. Without both numbers in the log, "LiveView is slow" gets
 * guessed at instead of measured. */
static volatile int fs_stat_grade_ms = 0;
/* One-frame capture for the LV dump: the filter fills these, the task writes
 * them. Nothing else may read a LiveView plane while Canon still owns it. */
static volatile int fs_dump_want = 0;
static unsigned char *fs_dump_raw = 0;
static unsigned char *fs_dump_graded = 0;
static size_t fs_dump_total = 0;
/* Private copy of part of Canon's LiveView plane, so a pass cannot read a
 * buffer that LiveView has already started to rewrite. Graded a band at a
 * time; ordinary cached memory, not fio_malloc -- see fs_process_frame. */
static void *fs_snap = 0;
static size_t fs_snap_size = 0;
static int fs_snap_failed = 0; /* say it once, not every frame */
static volatile uint32_t fs_stat_src = 0;
static volatile uint32_t fs_stat_dst = 0;
static volatile int fs_stat_pitch = 0;
static volatile int fs_stat_height = 0;
static volatile int fs_stat_row0 = 0;
static volatile int fs_stat_row1 = 0;
static volatile int fs_stat_inject_rc = -99;
static const char *fs_stat_slot1_src = "none";
static int fs_last_poll_state = -1;

static void fs_log(const char *fmt, ...) {
  char line[256];
  int pos = snprintf(line, sizeof(line), "[%7dms][%s] ", get_ms_clock(),
                     get_current_task_name());
  if (pos < 0)
    pos = 0;
  if (pos < (int)sizeof(line) - 2) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line + pos, sizeof(line) - pos - 1, fmt, ap);
    va_end(ap);
    if (n > 0)
      pos += n;
    if (pos >= (int)sizeof(line) - 1)
      pos = sizeof(line) - 2;
  }
  line[pos++] = '\n';
  line[pos] = 0;

  uint32_t old_irq = cli();
  uint32_t h = fs_log_head;
  for (int i = 0; i < pos; i++) {
    fs_log_buf[h & (FS_LOG_BUF_SIZE - 1)] = line[i];
    h++;
  }
  fs_log_head = h;
  sei(old_irq);
}

static volatile int fs_log_flushing = 0;


/*
 * Create a directory once, not on every call.
 *
 * FIO_CreateDirectory() checks is_dir() and returns early, so a repeat call
 * is *supposed* to be harmless -- but only while that check succeeds. When it
 * fails transiently (card busy, FIO cache, early boot) the raw create runs and
 * FAT gets a SECOND directory entry with the same name. This card ended up
 * with LOGS listed 17 times and FILMS three times, and fs_log_flush() was
 * calling FIO_CreateDirectory("ML/LOGS") on every single flush.
 *
 * The flag is only set on success, so a call that happens before the card is
 * mounted is retried rather than lost.
 */
static int fs_ensure_dir(const char *dir, int *done)
{
  if (*done) return 0;
  if (FIO_CreateDirectory(dir) == 0) *done = 1;
  return *done;
}

static void fs_log_flush(void) {
  uint32_t old_irq = cli();
  if (fs_log_flushing) {
    sei(old_irq);
    return;
  }
  fs_log_flushing = 1;
  sei(old_irq);

  uint32_t h = fs_log_head;
  uint32_t t = fs_log_tail;

  if (h != t) {
    if (h - t > FS_LOG_BUF_SIZE)
      t = h - FS_LOG_BUF_SIZE;

    {
      static int ok;
      fs_ensure_dir("ML/LOGS", &ok);
    }
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/FILMSIM.LOG");
    if (!f) {
      fs_log_tail = h;
    } else {
      char chunk[512];
      while (t < h) {
        uint32_t n = h - t;
        if (n > sizeof(chunk))
          n = sizeof(chunk);
        for (uint32_t i = 0; i < n; i++)
          chunk[i] = fs_log_buf[(t + i) & (FS_LOG_BUF_SIZE - 1)];
        FIO_WriteFile(f, chunk, n);
        t += n;
      }
      FIO_CloseFile(f);
      fs_log_tail = h;
    }
  }

  fs_log_flushing = 0;
}

/*
 * The engine's diagnostics, sent to FILMSIM.LOG.
 *
 * se_log() is set per operation, and the bake never set it: when a recipe
 * carried keys the parser did not recognise, it said so to nobody. That is how
 * "-9s" lines from the creator's own writer went unseen -- the look baked with
 * no hue work at all and the log was silent.
 */
static void fs_engine_log(void *ctx, const char *msg) {
  (void)ctx;
  fs_log("engine: %s", msg);
}

static int fs_bake_stock(const char *path, int strength, const char *lut) {
  int rc;
  se_set_log(fs_engine_log, NULL);
  rc = fs_yuvlook_bake_lut(path, strength, lut);
  se_set_log(NULL, NULL);
  if (rc != 0)
    fs_log("bake: '%s' would not load (%s)", path, lut ? "with LUT" : "no LUT");
  fs_log_flush();
  return rc;
}


/*
 * Picture style slot probe (read-only).
 *
 * Canon keeps a per-slot "PC flavor" description, reachable over PTP as
 * Get/SetPictureStylePCFlavorParam (0x9189/0x918A) and PictureStyleParam1..3
 * (0xD170..0xD172).  src/picstyle.c only ever reads the 50-char name out of
 * 0x04010001/3/5 (54-byte minimum length), so it is unknown whether the
 * actual style payload lives in the neighbouring properties.  Capture
 * whatever the camera pushes for that family; the answer then comes off the
 * card without writing anything to the camera.
 */
#define FS_PS_PROBE_COUNT 6

static const char *const fs_ps_prop_name[FS_PS_PROBE_COUNT] = {
    "0x04010001 flavor 1", "0x04010002",          "0x04010003 flavor 2",
    "0x04010004",          "0x04010005 flavor 3", "0x04010006",
};

/*
 * Offline render test.
 *
 * QEMU-EOS has no sensor: no frames ever reach YUV422_LV_BUFFER_*, so the
 * display filter has nothing to work on there.  This renders a synthetic
 * 720x480 UYVY scene through the very same fs_core code the LiveView filter
 * uses, on the camera's own CPU, and saves before/after BMPs plus a report
 * so the result can be checked without a body.
 *
 * Trigger: a flag file ML/SETTINGS/FILM.TST, optionally containing
 * "preset=N strength=N grain=N".  The file is removed after the run, so a
 * boot loop cannot re-trigger it.
 */

#define FS_TEST_W 720
#define FS_TEST_H 480
#define FS_TEST_PITCH (FS_TEST_W * 2)

extern unsigned int time(unsigned int *t);

static void fs_timestamp(int *yy, int *mo, int *dd, int *hh, int *mi, int *ss) {
  static const int month_days[12] = {31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31};
  long t = (long)time(0);
  if (t < 0)
    t = 0;

  long days = t / 86400;
  long rem = t % 86400;
  if (days < 0 || days > 200000)
    days = 0; /* unset RTC */

  *hh = (int)(rem / 3600);
  *mi = (int)((rem % 3600) / 60);
  *ss = (int)(rem % 60);

  int year = 1970, month = 1;
  while (1) {
    int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    int dim = month_days[month - 1] + ((month == 2 && leap) ? 1 : 0);
    if (days >= dim) {
      days -= dim;
      month++;
      if (month > 12) {
        month = 1;
        year++;
      }
    } else
      break;
  }
  *yy = year;
  *mo = month;
  *dd = (int)days + 1;
}

static void fs_scene_pixel(int x, int y, int *R, int *G, int *B) {
  static const unsigned char bars[8][3] = {
      {235, 235, 235}, {235, 235, 16}, {16, 235, 235}, {16, 235, 16},
      {235, 16, 235},  {235, 16, 16},  {16, 16, 235},  {16, 16, 16},
  };

  if (y < 160) {
    int i = x * 8 / FS_TEST_W;
    *R = bars[i][0];
    *G = bars[i][1];
    *B = bars[i][2];
  } else if (y < 320) {
    int v = x * 255 / (FS_TEST_W - 1);
    *R = *G = *B = v;
  } else if (y < 400) {
    /* skin tone | foliage green, with a soft vertical tone gradient */
    int t = (y - 320) * 255 / 80;
    if (x < FS_TEST_W / 2) {
      *R = 200 - t / 6;
      *G = 150 - t / 10;
      *B = 125 - t / 12;
    } else {
      *R = 60 + t / 20;
      *G = 120 - t / 12;
      *B = 55;
    }
  } else {
    /* saturated chroma sweep at constant luma */
    int t = (x - (FS_TEST_W / 2)) * 255 / (FS_TEST_W / 2);
    if (t < 0)
      t = 0;
    if (t > 255)
      t = 255;
    *R = 190 - t / 3;
    *G = 60 + t / 6;
    *B = 60 + t / 2;
  }
}

static void fs_fill_scene(uint32_t *buf) {
  for (int y = 0; y < FS_TEST_H; y++) {
    for (int x = 0; x < FS_TEST_W; x += 2) {
      int yy[2], uu[2], vv[2];

      for (int p = 0; p < 2; p++) {
        int R, G, B;
        fs_scene_pixel(x + p, y, &R, &G, &B);
        yy[p] = (77 * R + 150 * G + 29 * B) >> 8;
        /* signed chroma, neutral 0 (matches the 6D LiveView buffers) */
        uu[p] = fs_clamp_i((-43 * R - 85 * G + 128 * B) >> 8, -128, 127);
        vv[p] = fs_clamp_i((128 * R - 107 * G - 21 * B) >> 8, -128, 127);
      }

      buf[(unsigned int)y * (FS_TEST_PITCH / 4) + x / 2] =
          fs_pack_px((uu[0] + uu[1]) / 2, yy[0], (vv[0] + vv[1]) / 2, yy[1]);
    }
  }
}

static int fs_scene_stats(const uint32_t *buf, int *mean_y, int *mean_c,
                          int *min_y, int *max_y) {
  unsigned int words = (unsigned int)FS_TEST_PITCH / 4 * FS_TEST_H;
  long sy = 0, sc = 0;
  int lo = 255, hi = 0;

  for (unsigned int i = 0; i < words; i++) {
    unsigned int px = buf[i];
    int ya = (px >> 8) & 0xFF;
    int yb = (px >> 24) & 0xFF;
    int du = fs_chroma_signed(px);
    int dv = fs_chroma_signed(px >> 16);

    sy += ya + yb;
    sc += (du < 0 ? -du : du) + (dv < 0 ? -dv : dv);
    if (ya < lo)
      lo = ya;
    if (yb < lo)
      lo = yb;
    if (ya > hi)
      hi = ya;
    if (yb > hi)
      hi = yb;
  }

  *mean_y = (int)(sy / (words * 2));
  *mean_c = (int)(sc / (words * 2));
  *min_y = lo;
  *max_y = hi;
  return 0;
}

static int fs_write_bmp(const char *path, const uint32_t *buf) {
  unsigned int row_bytes = ((FS_TEST_W * 3) + 3) & ~3u;
  unsigned char *row = fio_malloc(row_bytes);
  if (!row)
    return -1;

  unsigned int data_size = row_bytes * FS_TEST_H;
  unsigned int file_size = 54 + data_size;
  unsigned char hdr[54];
  memset(hdr, 0, sizeof(hdr));

  hdr[0] = 'B';
  hdr[1] = 'M';
  hdr[2] = file_size & 0xFF;
  hdr[3] = (file_size >> 8) & 0xFF;
  hdr[4] = (file_size >> 16) & 0xFF;
  hdr[5] = (file_size >> 24) & 0xFF;
  hdr[10] = 54;
  hdr[14] = 40;
  hdr[18] = FS_TEST_W & 0xFF;
  hdr[19] = (FS_TEST_W >> 8) & 0xFF;
  hdr[22] = FS_TEST_H & 0xFF;
  hdr[23] = (FS_TEST_H >> 8) & 0xFF;
  hdr[26] = 1;  /* planes */
  hdr[28] = 24; /* bpp */
  hdr[34] = data_size & 0xFF;
  hdr[35] = (data_size >> 8) & 0xFF;
  hdr[36] = (data_size >> 16) & 0xFF;
  hdr[37] = (data_size >> 24) & 0xFF;
  hdr[38] = 2835 & 0xFF;
  hdr[39] = (2835 >> 8) & 0xFF;
  hdr[42] = 2835 & 0xFF;
  hdr[43] = (2835 >> 8) & 0xFF;

  FILE *f = FIO_CreateFile(path);
  if (!f) {
    fio_free(row);
    return -1;
  }

  FIO_WriteFile(f, hdr, sizeof(hdr));

  unsigned int words_per_row = (unsigned int)FS_TEST_PITCH / 4;
  for (int y = FS_TEST_H - 1; y >= 0; y--) {
    const uint32_t *p = buf + (unsigned int)y * words_per_row;
    unsigned char *o = row;
    for (int x = 0; x < FS_TEST_W / 2; x++) {
      uint32_t px = p[x];
      /* yuv2rgb_* tables reinterpret the byte as int8_t themselves */
      int u = px & 0xFF;
      int v = (px >> 16) & 0xFF;
      int ya = (px >> 8) & 0xFF, yb = (px >> 24) & 0xFF;
      int Y[2] = {ya, yb};

      for (int k = 0; k < 2; k++) {
        int R = Y[k] + yuv2rgb_RV[v];
        int G = Y[k] + yuv2rgb_GU[u] + yuv2rgb_GV[v];
        int B = Y[k] + yuv2rgb_BU[u];
        *o++ = (unsigned char)fs_clamp_i(B, 0, 255);
        *o++ = (unsigned char)fs_clamp_i(G, 0, 255);
        *o++ = (unsigned char)fs_clamp_i(R, 0, 255);
      }
    }
    while ((unsigned int)(o - row) < row_bytes)
      *o++ = 0;
    FIO_WriteFile(f, row, row_bytes);
  }

  FIO_CloseFile(f);
  fio_free(row);
  return 0;
}

static int fs_cfg_int(const char *buf, const char *key, int def) {
  const char *p = strstr(buf, key);
  if (!p)
    return def;
  p += strlen(key);
  while (*p == ' ' || *p == '=' || *p == '\t')
    p++;
  int neg = 0, v = 0, any = 0;
  if (*p == '-') {
    neg = 1;
    p++;
  }
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    p++;
    any = 1;
  }
  return any ? (neg ? -v : v) : def;
}

static void fs_progress(const char *dir, const char *stage) {
  char path[64];
  snprintf(path, sizeof(path), "%s/PROGRESS.TXT", dir);
  FILE *f = FIO_CreateFileOrAppend(path);
  if (!f)
    return;
  my_fprintf(f, "%s\n", stage);
  FIO_CloseFile(f);
}

static void fs_run_render_test(int preset, int strength, int grain) {
  preset = fs_clamp_i(preset, 0, FS_PRESET_COUNT - 1);
  strength = fs_clamp_i(strength, 0, 100);
  grain = fs_clamp_i(grain, 0, 100);

  uint32_t *frame = fio_malloc((size_t)FS_TEST_PITCH * FS_TEST_H);
  if (!frame) {
    NotifyBox(2000, "film_sim: no memory for test frame");
    return;
  }

  fs_lut_t lut;
  fs_core_satboost = film_sim_satboost;
  fs_core_build_luts(&lut, &fs_presets[preset], strength, grain);

  { static int ok; fs_ensure_dir("ML/FILMSIM", &ok); }

  int yy, mo, dd, hh, mi, ss;
  fs_timestamp(&yy, &mo, &dd, &hh, &mi, &ss);

  char dir[32];
  int base = hh * 10000 + mi * 100 + ss;
  { char pad[12]; fs_pad_num(pad, sizeof pad, base, 6);
    snprintf(dir, sizeof(dir), "ML/FILMSIM/R%s", pad); }
  for (int n = 1; FIO_CreateDirectory(dir) != 0 && n <= 20; n++) {
    /* no RTC on the bench, or a second run in the same second */
    { char pad[12]; fs_pad_num(pad, sizeof pad, base + n, 6);
      snprintf(dir, sizeof(dir), "ML/FILMSIM/R%s", pad); }
  }

  char path[64];
  int my0, mc0, lo0, hi0, my1, mc1, lo1, hi1;
  unsigned int rnd = 0x9E3779B9;

  fs_progress(dir, "start");
  fs_fill_scene(frame);
  fs_progress(dir, "scene");
  fs_scene_stats(frame, &my0, &mc0, &lo0, &hi0);

  snprintf(path, sizeof(path), "%s/BEFORE.BMP", dir);
  fs_write_bmp(path, frame);
  fs_progress(dir, "before-bmp");

  fs_progress(dir, "process-start");
  int t0 = get_ms_clock();
  fs_core_process_rows(&lut, frame, frame, FS_TEST_PITCH, 0, FS_TEST_H, &rnd);
  int t1 = get_ms_clock();
  fs_progress(dir, "process-done");

  fs_scene_stats(frame, &my1, &mc1, &lo1, &hi1);

  snprintf(path, sizeof(path), "%s/AFTER.BMP", dir);
  fs_write_bmp(path, frame);
  fs_progress(dir, "after-bmp");

  snprintf(path, sizeof(path), "%s/REPORT.TXT", dir);
  FILE *f = FIO_CreateFile(path);
  if (f) {
    my_fprintf(f, "film_sim offline render test\n");
    {
      char y4[8], m2[4], d2[4], h2[4], n2[4], s2[4];
      fs_pad_num(y4, sizeof y4, yy, 4); fs_pad_num(m2, sizeof m2, mo, 2);
      fs_pad_num(d2, sizeof d2, dd, 2); fs_pad_num(h2, sizeof h2, hh, 2);
      fs_pad_num(n2, sizeof n2, mi, 2); fs_pad_num(s2, sizeof s2, ss, 2);
      my_fprintf(f, "date %s-%s-%s %s:%s:%s\n", y4, m2, d2, h2, n2, s2);
    }
    my_fprintf(f, "frame %dx%d UYVY (pitch %d), synthetic scene\n", FS_TEST_W,
               FS_TEST_H, FS_TEST_PITCH);
    my_fprintf(f, "\nwhat this does\n");
    my_fprintf(f, "  Runs the LiveView display-filter math (fs_core.h, the "
                  "same code the\n");
    my_fprintf(
        f,
        "  camera runs per frame) over one 720x480 UYVY frame, on this CPU.\n");
    my_fprintf(f,
               "  BEFORE.BMP is the untouched input, AFTER.BMP the result.\n");
    my_fprintf(f, "  In LiveView the same math rewrites "
                  "YUV422_LV_BUFFER_DISPLAY_ADDR.\n");
    my_fprintf(f, "\nfilm simulation\n");
    my_fprintf(f, "  stock      %s\n", fs_preset_names[preset]);
    my_fprintf(f, "  strength   %d%%\n", strength);
    my_fprintf(f, "  grain      %d  (amplitude %d)\n", grain, lut.grain_amp);
    const fs_params_t *p = &fs_presets[preset];
    my_fprintf(f,
               "  contrast %d  black %d  shoulder %d  saturation %d  hue %d\n",
               p->contrast, p->black, p->shoulder, p->saturation, p->hue);
    my_fprintf(f,
               "  split tone: shadows cb %d cr %d, highlights cb %d cr %d, hi "
               "desat %d\n",
               p->shadow_cb, p->shadow_cr, p->high_cb, p->high_cr, p->hi_desat);
    my_fprintf(f, "  hue matrix %d %d %d %d (8.8 fixed point)\n", lut.matrix[0],
               lut.matrix[1], lut.matrix[2], lut.matrix[3]);
    my_fprintf(f, "\ndata\n");
    my_fprintf(f,
               "  luma        before avg %d (%d..%d)  after avg %d (%d..%d)\n",
               my0, lo0, hi0, my1, lo1, hi1);
    my_fprintf(f, "  mean |chroma| before %d  after %d\n", mc0, mc1);
    my_fprintf(f, "  delta luma %d  delta chroma %d\n", my1 - my0, mc1 - mc0);
    my_fprintf(f, "\ntiming\n");
    my_fprintf(f, "  %d ms for one frame (ticks %d..%d)\n", t1 - t0, t0, t1);
    my_fprintf(
        f,
        "  note: under QEMU (TCI interpreter) this is orders of magnitude\n");
    my_fprintf(f, "  slower than DIGIC 5+; host sim gives ~0.5 ms/frame.\n");
    FIO_CloseFile(f);
  }

  fs_progress(dir, "report-done");
  fio_free(frame);

  NotifyBox(2500, "film_sim test done -> %s", dir);
}

static int fs_pending_preset, fs_pending_strength, fs_pending_grain,
    fs_pending_run;

/* Runs the test if ML/SETTINGS/FILM.TST exists; returns 1 if it did. */
static int fs_test_flag(void) {
  static const char *const flag = "ML/SETTINGS/FILM.TST";
  uint32_t size = 0;
  if (FIO_GetFileSize(flag, &size) != 0)
    return 0;

  char *buf = fio_malloc(128);
  if (!buf)
    return 0;
  memset(buf, 0, 128);

  FILE *f = FIO_OpenFile(flag, O_RDONLY);
  if (f) {
    int n = FIO_ReadFile(f, buf, 127);
    FIO_CloseFile(f);
    if (n > 0)
      buf[n] = 0;
  }
  FIO_RemoveFile(flag);

  fs_pending_preset = fs_cfg_int(buf, "preset", film_sim_preset);
  fs_pending_strength = fs_cfg_int(buf, "strength", film_sim_strength);
  fs_pending_grain = fs_cfg_int(buf, "grain", film_sim_grain);
  fs_pending_run = 1;
  fio_free(buf);
  return 1;
}

static void fs_test_task(void *unused) {
  msleep(3000);
  if (fs_pending_run) {
    fs_run_render_test(fs_pending_preset, fs_pending_strength,
                       fs_pending_grain);
    fs_pending_run = 0;
  }
  /* nothing else to do; the task simply ends */
}

static int fs_inject_user1(int preset, const char *name);
static int fs_slot1_ready(void);

static int fs_develop_stock_index(int count);
static int fs_lv_preset(void);
static int fs_can_index(const char *name);
static int fs_photo_number(const char *name);
static int fs_starts_with_ci(const char *s, const char *prefix);

/*
 * Commit tmp -> out.
 *
 * FAT will not rename onto an existing name, and with Replace Camera JPEG on
 * the target is exactly that: the JPEG Canon wrote for this frame. Delete it
 * only at the moment of the commit, so a develop that fails half way leaves the
 * camera's own file alone.
 */
static int fs_commit(const char *tmp, const char *out) {
  uint32_t sz = 0;
  if (!strcasecmp(tmp, out))
    return 0;
  if (FIO_GetFileSize(out, &sz) == 0 && sz > 0) {
    fs_log("develop: replacing %s (%d bytes)", out, (int)sz);
    fs_log_flush();
    FIO_RemoveFile(out);
  }
  return FIO_RenameFile(tmp, out);
}

static void fs_photo_paths(const char *photo, char *cr2, int c1, char *out,
                           int c2, char *tmp, int c3);

/* The DCIM listing, needed early: the output name is the next free IMG_ number
 * and that is read from this list. */
#define MAX_DCIM_PHOTOS 256
static char dcim_photo_list[MAX_DCIM_PHOTOS][64];
static int dcim_photo_count;
static int dcim_photo_index;

/*
 * The output name.
 *
 * Canon's playback only lists what it recognises, and FILMSIM.LOG/experience
 * both say "FLM_nnnn.JPG" is written but never shown. The camera's own files
 * are IMG_nnnn.JPG, so the develop writes the next free number in that series:
 * nothing of the camera's is overwritten, and the frame appears where every
 * other picture is.
 *
 * Because the name is now new every time, "already developed" can no longer be
 * an existing file -- that would re-develop the same shot forever. A marker per
 * source frame records it instead.
 */
static int fs_next_img_number(void) {
  /* The cached listing is a snapshot: two develops in one session both picked
   * IMG_3258 and the second overwrote the first (FILMSIM.LOG lines 338/393).
   * Rescan first, then still probe the card, because the listing only holds the
   * extensions it was asked for. */
  char path[64];
  const char *dir = get_dcim_dir();
  int mx = 0, n;

  if (!dir || !dir[0])
    dir = "DCIM/100CANON";
  for (int i = 0; i < dcim_photo_count; i++) {
    const char *nm = dcim_photo_list[i];
    if (!fs_starts_with_ci(nm, "IMG_"))
      continue;
    int v = fs_photo_number(nm);
    if (v > mx)
      mx = v;
  }

  /* The camera's own numbering is what matters, so probe the card rather than
   * trusting only what the list happens to contain. */
  for (n = mx + 1; n < 10000; n++) {
    uint32_t sz = 0;
    { char pad[8]; fs_pad_num(pad, sizeof pad, n, 4);
      snprintf(path, sizeof path, "%s/IMG_%s.JPG", dir, pad); }
    if (FIO_GetFileSize(path, &sz) == 0 && sz > 0)
      continue;
    { char pad[8]; fs_pad_num(pad, sizeof pad, n, 4);
      snprintf(path, sizeof path, "%s/IMG_%s.CR2", dir, pad); }
    if (FIO_GetFileSize(path, &sz) == 0 && sz > 0)
      continue;
    break;
  }
  return n < 10000 ? n : 0;
}

static int fs_src_number(const char *photo) {
  const char *dot = photo ? strrchr(photo, '.') : NULL;
  int end = dot ? (int)(dot - photo) : (photo ? (int)strlen(photo) : 0);
  int start = end;
  while (start > 0 && photo[start - 1] >= '0' && photo[start - 1] <= '9')
    start--;
  if (end - start == 4)
    return fs_photo_number(photo);
  return 0;
}

static void fs_dev_marker(int srcnum, char *buf, int cap) {
  { char pad[8]; fs_pad_num(pad, sizeof pad, srcnum, 4);
    snprintf(buf, cap, "ML/SETTINGS/DEV_%s.MRK", pad); }
}

static int fs_dev_is_done(int srcnum) {
  char m[40];
  uint32_t sz = 0;
  if (srcnum <= 0)
    return 0;
  fs_dev_marker(srcnum, m, sizeof m);
  return FIO_GetFileSize(m, &sz) == 0;
}

static void fs_dev_mark_done(int srcnum) {
  char m[40];
  FILE *f;
  if (srcnum <= 0)
    return;
  fs_dev_marker(srcnum, m, sizeof m);
  f = FIO_CreateFile(m);
  if (f)
    FIO_CloseFile(f);
}

static void fs_sync() {
  /* Bit fields, not decimal weights: stock*100000000 overflows int32 for the
   * 33-stock list, and a wrapped signature means the look stops updating. */
  int preset_now = fs_lv_preset();
  /* The stock field is 7 bits, not 6: importing M-Film's catalogue takes the
   * shelf past 63, and `stock & 0x3F` made stock 64 look like stock 0, so
   * selecting it changed nothing. */
  unsigned int sig =
      ((unsigned)preset_now & 0x0F) << 28 |
      ((unsigned)fs_clamp_i(film_sim_stock, 0, MAX_DYNAMIC_FILMS - 1) & 0x7F) << 21 |
      ((unsigned)fs_clamp_i(film_sim_strength, 0, 100) & 0x7F) << 14 |
      ((unsigned)fs_clamp_i(film_sim_grain, 0, 100) & 0x7F) << 7 |
      ((unsigned)fs_clamp_i(film_sim_enable, 0, 1) & 1);
  /* Saturation gets its own word: the first signature has no spare bits and a
   * wrapped signature is worse than a second compare. */
  unsigned int sig2 = ((unsigned)fs_clamp_i(film_sim_satboost, 20, 200) & 0xFF) |
                       ((unsigned)fs_clamp_i(film_sim_grainsize + 1, 0, 101) << 8);

  if (fs_signature_valid && sig == fs_signature && sig2 == fs_signature2)
    return;
  fs_signature = sig;
  fs_signature2 = sig2;
  fs_signature_valid = 1;

  int preset = preset_now;
  fs_core_satboost = film_sim_satboost;
  fs_core_build_luts(&fs_lut, &fs_presets[preset], film_sim_strength,
                     film_sim_grain);

  fs_log("fs_sync: enable=%d preset=%d (%s) strength=%d pct grain=%d (amp=%d) "
         "mat=[%d,%d,%d,%d]",
         film_sim_enable, preset, fs_preset_names[preset], film_sim_strength,
         film_sim_grain, fs_lut.grain_amp, fs_lut.matrix[0], fs_lut.matrix[1],
         fs_lut.matrix[2], fs_lut.matrix[3]);

  /* Flag background worker task to inject User 1 outside real-time filter
   * only when the film stock preset itself changed (not on strength/grain
   * tweaks) */
  if (film_sim_user1 && preset != fs_user1_last_preset) {
    fs_user1_dirty = 1;
  }
  if (film_sim_yuvrows > 0 || film_sim_lv3d) {
    fs_yuvlook_dirty = 1;
  }
  /* Framing is an aspect mode: the hook needs it even when nothing else moved.
   *
   * Two different bar sets use this number and they are gated differently:
   *
   *  - fs_aspect_bars() draws the LiveView overlay and is NOT gated on
   *    replace: it is a composition guide and writes no pixels to the file,
   *    so you see the frame either way.
   *  - fs_yuvhook_bars() burns black bars into Canon's in-shutter YUV, which
   *    is the JPEG on the card. With replace off that JPEG stays a clean full
   *    3:2 frame -- no bars burned in -- and with replace on the bars are
   *    redundant because our own cropped develop replaces the file. */
  fs_yuvhook_bars(film_sim_replace ? film_sim_aspect : 0);

  /*
   * Which 3D LUT the develop uses.
   *
   * A stock may carry its own: the <lut> tag in its ML/FILMS XML, which is
   * the imported stocks keep their colour (tools/mfilm_import.py). It is part
   * of the stock's definition, not a session option, so it is used whether or
   * not film.sim.lut is on -- those recipes are flat by design and would come
   * out grey without it.
   *
   * Otherwise the session-wide ML/FILMSIM/LOOK.BIN, still behind its switch:
   * it is read once at the first develop, turning it off releases it, and
   * turning it on again picks up a file that changed. A missing file is
   * logged by the loader, which then develops without it.
   */
  {
    /* A string test, not a file test: fs_sync also runs from the stock menu
     * handler, and this module does no card I/O in a key handler. */
    const char *own = fs_own_lut_path();
    se_cam_lut_path(own ? own : (film_sim_lut ? "ML/FILMSIM/LOOK.BIN" : NULL));
  }
}

/*
 * Aspect framing for LiveView.
 *
 * A UYVY word is [U][Ya][V][Yb], all four bytes zero for neutral black, so a
 * bar is a word fill -- no per-pixel maths, no cost next to the grade.
 * 16:9 keeps the full width and takes 9/16 of it in height; XPan is 65:24.
 */
/*
 * Aspect framing for LiveView.
 *
 * The geometry lives in fs_core_aspect_bars() because the in-shutter burn uses
 * the same function: the bars drawn on the preview and the bars written into
 * the frame Canon encodes have to be in the same place.
 */
static void fs_aspect_bars(void *dst, int pitch, int row0, int row1) {
  int num, den;
  if (!fs_core_aspect_ratio(film_sim_aspect, &num, &den))
    return;
  fs_core_aspect_bars(dst, pitch, pitch / 2, row0, row1, num, den);
}

/* The apply lives in fs_core.h and the bake in engine/look.h; if their node
 * counts ever drift the filter would read past the table the bake filled. */
typedef char fs_lv3d_shape_check[(SE_YUV3_NY == FS_UV3_NY &&
                                  SE_YUV3_NC == FS_UV3_NC) ? 1 : -1];

static int fs_lv3d_logged = -1;

/*
 * Which form grades a LiveView row range.
 *
 * The joint lattice is baked from the same stock look the develop uses, so the
 * panel and the file agree about what is loaded. The preset tables are the
 * older separable form. Measured on the host against the develop on the same
 * frame (se/hosttest/lv3d.c): lattice 4.12 levels mean, preset tables 8.82 --
 * but the lattice costs ~3.5x the table lookups, and the camera's budget for
 * that has not been measured, so it is behind a switch and off by default.
 * Whichever runs, ML/LOGS/FILMSIM.LOG carries the per-frame grade_ms.
 */
static void fs_lv_grade(const void *src, void *dst, int pitch, int row0, int row1)
{
  const se_yuv3 *t3 = film_sim_lv3d ? fs_yuvlook_uv3() : NULL;

  if (fs_lv3d_logged != (t3 ? 1 : 0)) {
    fs_lv3d_logged = t3 ? 1 : 0;
    fs_log("lv: grading with %s",
           t3 ? "the joint (y,u,v) lattice" : "the preset tables");
    fs_log_flush();
  }

  if (t3)
    fs_core_process_rows3(t3->y, t3->v, src, dst, pitch, row0, row1,
                          fs_lut.grain_amp, &fs_rnd);
  else
    fs_core_process_rows(&fs_lut, src, dst, pitch, row0, row1, &fs_rnd);
}

static void FAST fs_process_frame(void *src, void *dst) {
  if (!src || !dst)
    return;

  struct vram_info *vr = get_yuv422_vram();
  int pitch = (vr && vr->pitch > 0) ? vr->pitch : 1440;
  int height = (vr && vr->height > 0) ? vr->height : 480;

  /* only filter the active image rows; copy letterbox bars when src != dst
   * so display_filter_buffer never shows uninitialized heap memory */
  int row0 = BM2LV_Y(os.y0);
  int row1 = BM2LV_Y(os.y_max);
  if (row1 <= row0 || row0 < 0 || row1 > height) {
    row0 = 0;
    row1 = height;
  }

  int t0 = get_ms_clock();
  int copy_ms = 0;
  int grade_ms = 0;

  /*
   * Grade a copy, not Canon's buffer, and copy it a band at a time.
   *
   * One pass over this frame measures 82-98 ms on the 6D (lv_frame dt) while
   * LiveView hands over a new plane every 33 ms, so a single pass reads a
   * buffer that has already been rewritten under it: the bottom of the graded
   * frame came from the next sensor frame. That was the seam in LV_LUT.BIN.
   *
   * Two attempts at a fix, both measured:
   *   whole frame copied first, fio_malloc:  copy 15 ms + pass 134 ms
   * fio_malloc returns the UNCACHEABLE alias (mem.c: MEM_DMA -> UNCACHEABLE),
   * so the pass then read 691 kB uncached and was slower than before. So: a
   * 16-row band in ordinary (cached) memory, copied and graded in turn. The
   * band is 23 kB and stays in the D-cache between the copy and the pass, and
   * the window in which the source can change is one band, not a whole frame.
   */
  /*
   * A requested dump owns this frame: Canon's plane copied once, graded from
   * that copy, and the result copied out. Both files then hold the same frame.
   * Reading the display buffer from a task (the first attempt) gave two
   * pictures of different frames, each a mixture of several.
   */
  if (fs_dump_want && fs_dump_raw && fs_dump_graded &&
      fs_dump_total == (size_t)pitch * (size_t)height) {
    uint32_t tc = get_ms_clock();
    memcpy(fs_dump_raw, src, fs_dump_total);
    copy_ms = get_ms_clock() - tc;
    fs_core_process_rows(&fs_lut, fs_dump_raw, dst, pitch, row0, row1, &fs_rnd);
    if (src != dst) {
      if (row0 > 0)
        memcpy(dst, fs_dump_raw, (size_t)row0 * pitch);
      if (row1 < height)
        memcpy((char *)dst + (size_t)row1 * pitch,
               (const char *)fs_dump_raw + (size_t)row1 * pitch,
               (size_t)(height - row1) * pitch);
    }
    fs_aspect_bars(dst, pitch, row0, row1);
    memcpy(fs_dump_graded, dst, fs_dump_total);
    fs_dump_want = 0;
    fs_log("lvdump: captured rows %d..%d of %d, aspect %d, copy %dms", row0,
           row1, height, film_sim_aspect, copy_ms);
    fs_log_flush();
  } else if (film_sim_enable && src != dst) {
    /*
     * Grading needs a frame that holds still.
     *
     * One pass costs 95-101 ms on this body (lv_frame dt) against a LiveView
     * period of 33 ms, so a pass that reads Canon's buffer directly reads it
     * while it is being rewritten: the graded frame was stitched from two or
     * three sensor frames, which is the horizontal banding in LV_LUT.BIN.
     *
     * So: one copy of the whole plane first -- a single burst read, 10-15 ms,
     * comfortably inside one LiveView frame -- and the pass then reads that.
     * The cost of the copy is the price of a coherent picture; skipping frames
     * would not help, because a skipped frame changes nothing about how long
     * one pass takes to read its source.
     *
     * Both buffers are already the CACHEABLE aliases (src 0x1F227800 is
     * 0x5F227800 with bit 30 cleared, dst is CACHEABLE(display_filter_buffer)),
     * so this is not a cache fix -- the copy is only about coherence.
     */
    /*
     * A band at a time, not the whole plane: copy 16 rows, grade those 16 rows,
     * next band. The copy still costs the same traffic, but the grade then reads
     * 23 KB that are still in L1 instead of walking 691 KB that has already
     * been evicted -- and the window in which Canon can rewrite the source
     * shrinks from a whole frame to one band, which is the tearing fix the
     * comment above asks for. The buffer itself shrinks from 691 KB of the
     * temporary pool to 23 KB.
     */
    /*
     * One copy for the whole frame, then one grade.
     *
     * This was banded (copy 16 rows, grade 16 rows) to keep the grade reading
     * cache-hot data. The camera says it bought nothing: grade stayed at 89 ms
     * of a 101 ms pass either way, because the cost is arithmetic, not memory.
     * What banding did add was a risk: each band is copied at a different
     * moment while Canon rewrites the plane every 33 ms, so bands can come from
     * different sensor frames and show as seams every 16 rows. A single 11 ms
     * copy cannot do that.
     */
    size_t total = (size_t)pitch * (size_t)height;

    if (!fs_snap && !fs_snap_failed) {
      fs_snap = tmp_malloc(total);
      fs_snap_size = fs_snap ? total : 0;
      if (!fs_snap) {
        fs_snap_failed = 1;
        fs_log("lv: no memory for the %d byte frame copy; grading Canon's "
               "buffer directly (can read a half-written frame)",
               (int)total);
        fs_log_flush();
      }
    } else if (fs_snap && fs_snap_size < total) {
      /* the panel geometry changed under us (movie mode, HDMI) */
      free(fs_snap);
      fs_snap = tmp_malloc(total);
      fs_snap_size = fs_snap ? total : 0;
    }

    if (fs_snap && fs_snap_size >= total) {
      uint32_t tc = get_ms_clock();
      memcpy(fs_snap, src, total);
      copy_ms += get_ms_clock() - tc;
      tc = get_ms_clock();
      fs_lv_grade(fs_snap, dst, pitch, row0, row1);
      grade_ms += get_ms_clock() - tc;
      if (row0 > 0)
        memcpy(dst, src, (size_t)row0 * pitch);
      if (row1 < height)
        memcpy((char *)dst + (size_t)row1 * pitch,
               (const char *)src + (size_t)row1 * pitch,
               (size_t)(height - row1) * pitch);
    } else {
      uint32_t tg = get_ms_clock();
      fs_lv_grade(src, dst, pitch, row0, row1);
      grade_ms += get_ms_clock() - tg;
      if (row0 > 0)
        memcpy(dst, src, (size_t)row0 * pitch);
      if (row1 < height)
        memcpy((char *)dst + (size_t)row1 * pitch,
               (const char *)src + (size_t)row1 * pitch,
               (size_t)(height - row1) * pitch);
    }
    fs_aspect_bars(dst, pitch, row0, row1);
  } else if (film_sim_enable) {
    fs_lv_grade(src, dst, pitch, row0, row1);
    fs_aspect_bars(dst, pitch, row0, row1);
  } else {
    if (src != dst) {
      if (row0 > 0)
        memcpy(dst, src, (size_t)row0 * pitch);
      if (row1 < height)
        memcpy((char *)dst + (size_t)row1 * pitch,
               (const char *)src + (size_t)row1 * pitch,
               (size_t)(height - row1) * pitch);
      memcpy((char *)dst + (size_t)row0 * pitch,
             (const char *)src + (size_t)row0 * pitch,
             (size_t)(row1 - row0) * pitch);
    }
    fs_aspect_bars(dst, pitch, row0, row1);
  }

  int dt = get_ms_clock() - t0;
  if (dt < 0)
    dt = 0;
  fs_stat_copy_ms = copy_ms;
  fs_stat_grade_ms = grade_ms;

  uint32_t fnum = ++fs_stat_frames;
  fs_stat_last_ms = dt;
  if (dt > fs_stat_max_ms)
    fs_stat_max_ms = dt;
  fs_stat_src = (uint32_t)src;
  fs_stat_dst = (uint32_t)dst;
  fs_stat_pitch = pitch;
  fs_stat_height = height;
  fs_stat_row0 = row0;
  fs_stat_row1 = row1;

  /* One line per boot even with dbglog off: "LiveView is slow" is then a
   * measurement in the log instead of a guess, and a single write costs
   * nothing. dbglog turns it into a running trace. */
  if (fnum == 120 || (film_sim_dbglog && (fnum % 120) == 0)) {
    fs_log("lv_frame #%d: src=0x%08x dst=0x%08x %dx%d rows=%d..%d copy=%dms "
           "grade=%dms dt=%dms (max=%dms, skipped=%d)",
           (int)fnum, (unsigned)src, (unsigned)dst, pitch / 2, height, row0,
           row1, (int)fs_stat_copy_ms, (int)fs_stat_grade_ms, dt,
           fs_stat_max_ms, (int)fs_stat_skipped);
  }
}

/*
 * Write one UYVY plane to the card.
 *
 * 8 KB a call, not 16: FIO asserts above 8 KB when the source is ordinary
 * memory, and a dump task that dies silently on the one shot the user was told
 * to take is worse than a slow one.
 */
static int fs_write_plane(const char *path, const void *src, int pitch,
                          int height) {
  const unsigned char *p = (const unsigned char *)src;
  unsigned int total = (unsigned int)pitch * (unsigned int)height;
  unsigned int chunk = 8 * 1024;

  FILE *f = FIO_CreateFile(path);
  if (!f)
    return 0;
  for (unsigned int off = 0; off < total; off += chunk) {
    unsigned int n = total - off;
    if (n > chunk)
      n = chunk;
    if (FIO_WriteFile(f, p + off, (int)n) != (int)n) {
      FIO_CloseFile(f);
      return 0;
    }
  }
  FIO_CloseFile(f);
  return 1;
}

/*
 * Dump one graded frame: the plane the filter was handed, and the plane it
 * wrote.
 *
 * Both copies are taken inside the filter, from the same frame, because a
 * buffer that LiveView is still writing cannot be dumped from a task: the
 * first version read vr->vram after the fact and produced two pictures of
 * different frames, each of them a horizontal mixture of several. Reading
 * Canon's plane directly was the bug; the torn bands in LV_RAW.BIN and
 * LV_LUT.BIN were the module reporting it.
 *
 * The task allocates, then asks; the filter fills the two copies on the next
 * frame it processes and clears the request.
 */
static void fs_dump_task(void *unused) {
  NotifyBox(2000, "film_sim: waiting for LiveView to dump");

  /* up to ten minutes: on a real body the user starts LiveView when ready */
  for (int i = 0; i < 1200; i++) {
    struct vram_info *vr = get_yuv422_vram();
    if (vr && vr->vram && vr->pitch >= 64 && vr->height > 0) {
      int pitch = vr->pitch, height = vr->height;
      size_t total = (size_t)pitch * (size_t)height;

      if (!film_sim_enable) {
        fs_log("lvdump: film.sim.enable is 0, so no frame is graded; turn "
               "LiveView Filter on first");
        fs_log_flush();
        NotifyBox(3000, "film_sim: LiveView Filter is off");
        return;
      }

      fs_dump_raw = fio_malloc(total);
      fs_dump_graded = fio_malloc(total);
      if (!fs_dump_raw || !fs_dump_graded) {
        fio_free(fs_dump_raw);
        fio_free(fs_dump_graded);
        fs_dump_raw = fs_dump_graded = 0;
        fs_log("lvdump: no memory for %d bytes twice", (int)total);
        fs_log_flush();
        return;
      }
      fs_dump_total = total;
      fs_dump_want = 1;

      /* Up to 6 s: the filter runs once per processed frame, and one pass is
       * ~100 ms, so a few frames is all this should ever need. */
      for (int w = 0; w < 600 && fs_dump_want; w++)
        msleep(10);

      if (fs_dump_want) {
        fs_log("lvdump: the filter never ran -- ML needs Global Draw on and "
               "Canon's LV overlays off (disp=0)");
        fs_log_flush();
        NotifyBox(3000, "film_sim: no graded frame to dump");
      } else {
        int a =
            fs_write_plane("ML/FILMSIM/LV_RAW.BIN", fs_dump_raw, pitch, height);
        int b = fs_write_plane("ML/FILMSIM/LV_LUT.BIN", fs_dump_graded, pitch,
                               height);
        fs_log("lvdump: %dx%d pitch %d, preset %d, grain amp %d, "
               "LV_RAW.BIN %s, LV_LUT.BIN %s",
               pitch / 2, height, pitch, fs_lv_preset(), fs_lut.grain_amp,
               a ? "written" : "FAILED", b ? "written" : "FAILED");
        fs_log_flush();
        NotifyBox(3000, "film_sim: LV dumped + graded");
      }

      fio_free(fs_dump_raw);
      fio_free(fs_dump_graded);
      fs_dump_raw = fs_dump_graded = 0;
      return;
    }
    msleep(500);
  }
  NotifyBox(3000, "film_sim: no LiveView buffer to dump");
}

/* Set by ML core; called once the frame in dst_buf is complete. */
extern void display_filter_mark_written(void);

/* Declared where the screens are; read here so the filter can stand down. */
static volatile int fs_gui_active;
static volatile int fs_darkroom_active;

/* Set for the whole of a develop (see fs_develop_run); declared here because
 * the display filter has to stand down while it is set. */
static volatile int fs_develop_active = 0;

/* defined further down, next to the screen that owns them */
static volatile int fs_creator_active;
static volatile int fs_creator_dirty;
static volatile int fs_creator_save;
static volatile int fs_creator_exit;
static volatile int fs_creator_saving;
/* Four slots, like the Fuji page's FUJI1..4: one fixed filename meant the
 * second look silently overwrote the first. Slot 0 keeps the old name so
 * existing logs and docs still make sense. */
static int fs_creator_slot;
static int fs_creator_initialised;
static char fs_creator_saved_name[12];
static const char *fs_slot_names[] = { "WHEEL (1)", "WHEEL2", "WHEEL3", "WHEEL4" };

/* The ring's motion: same spring the shelf uses, in units of one wedge. */
static fs_spring_t      fs_creator_spring;
static fs_creator_spin_t fs_creator_spin;
static uint32_t         fs_creator_anim_ms;

/* The Fuji WB shift screen. */
static volatile int fs_wb_active = 0;
static volatile int fs_wb_dirty = 0;
static volatile int fs_wb_apply = 0;
static volatile int fs_wb_exit = 0;
static volatile int fs_wb_applied = 0;
static fs_wb_t       fs_wb;

/* The Fuji recipe page's state: one bundle, saved into a .fs. */
static fs_fuji_t     fs_fuji;
static volatile int  fs_fuji_save;

/* A recipe name has to be typeable on a camera with no keyboard, so it is
 * chosen from a short list instead of entered. Four slots, like a Fuji body's
 * C1..C4, and the .fs filename is the name. */
static const char *fs_fuji_names[] = { "FUJI1", "FUJI2", "FUJI3", "FUJI4" };
static int   fs_fuji_slot;
static int   fs_fuji_inited;

static fs_creator_t   fs_creator;

static unsigned int fs_display_filter(unsigned int ctx) {
  /* ctx = 0: report whether we want to run (must be cheap) */
  if (ctx == 0) {
    /* ML keeps exactly one display filter: the first module returning non-zero,
     * and every other module's filter is then ignored. module.c re-asks this on
     * every step (module_display_filter_enabled -> handler(0)), so the answer
     * here is live: claim only while there is something to draw. */
    int active = (film_sim_enable || film_sim_aspect) ? 1 : 0;
    if (!active)
      fs_valid_frame = 0;
    /* ML runs no display filter at all unless get_global_draw() is true: see
     * display_filter_enabled() in tweaks.c, which returns 0 when
     * zebra_should_run() is false, and zebra_should_run() is
     * "liveview_display_idle() && get_global_draw()" on a real body. So a
     * claimed filter with Global Draw off is silently never called, and
     * "frames=0" in the log is the whole story. Log the reason, not just the
     * number, so the next session says which switch is wrong. */
    int gd = get_global_draw() ? 1 : 0;
    int poll_state = (active << 8) | (gd << 7) | ((lv ? 1 : 0) << 6) |
                     (lv_disp_mode << 3) | (liveview_display_idle() ? 4 : 0) |
                     ((bmp_is_on() ? 1 : 0) << 1) |
                     ((ext_monitor_hdmi || hdmi_code) ? 1 : 0);
    if (film_sim_dbglog && poll_state != fs_last_poll_state) {
      fs_last_poll_state = poll_state;
      fs_log("filter_poll: want=%d lv=%d global_draw=%d setting=%d disp=%d "
             "idle=%d bmp_on=%d hdmi=%d/%d",
             active, lv, gd, get_global_draw_setting(), lv_disp_mode,
             liveview_display_idle(), bmp_is_on(), ext_monitor_hdmi, hdmi_code);
      if (active && !gd)
        fs_log("filter_poll: ML runs no display filter while Global Draw is "
               "off (tweaks.c display_filter_enabled -> zebra_should_run). "
               "Turn Global Draw on, and with Canon's own LV overlays showing "
               "press INFO until they are off (disp=%d).",
               lv_disp_mode);
      fs_log_flush();
    }
    return active; /* decline when there is nothing to draw */
  }

  struct display_filter_buffers *buffers = (struct display_filter_buffers *)ctx;
  if (!buffers->src_buf || !buffers->dst_buf) {
    return 0;
  }

  /*
   * A develop is running: leave the panel to Canon.
   *
   * Both jobs run on the one core and this is the hungrier of the two. The log
   * of the first develop with the probe in place shows what that costs -- LV
   * frames still arriving, at about 10 fps instead of 30, for the whole 75 s
   * while the decoder was trying to work, and the same frame developing in 30 s
   * when LiveView was idle. Neither job needs the other; the decoder gets the
   * core and the preview shows Canon's own frame until the develop is done.
   *
   * Deliberately no display_filter_mark_written(): nothing was written, so
   * nothing should be flipped to.
   */
  if (fs_develop_active) {
    fs_valid_frame = 0;
    return 0;
  }

  /*
   * A module screen is up: leave the panel to it as well.
   *
   * The shelf and the darkroom are full-screen and opaque, so nothing of the
   * graded LiveView frame is visible while they are open -- but the filter kept
   * running at ~30 fps behind them, and the log from the 6D says what that
   * cost: `shelf: paint` lines showing EVERY repaint as a full 480-row one at
   * 160-215 ms, 3-4 a second, while `[livev_hiprio_task] fs_sync` and
   * `yuvgrade: re-baked` fired underneath. Both jobs want the one core. The
   * develop already declines for exactly this reason.
   *
   * It also kept fs_stat_frames moving, and the shelf's footer shows that
   * count -- see fs_lv_status_key() for what that did to the repaint rate.
   */
  if (fs_gui_active || fs_darkroom_active || fs_creator_active ||
      fs_wb_active) {
    fs_valid_frame = 0;
    return 0;
  }

  /* Claimed but idle: pass the frame through so the panel is never blank. */
  if (!film_sim_enable && !film_sim_aspect) {
    struct vram_info *vr = get_yuv422_vram();
    int p2 = (vr && vr->pitch > 0) ? vr->pitch : 1440;
    int h2 = (vr && vr->height > 0) ? vr->height : 480;
    if (buffers->src_buf != buffers->dst_buf)
      memcpy(buffers->dst_buf, buffers->src_buf, (size_t)p2 * h2);
    display_filter_mark_written();
    return 0;
  }

  fs_sync();

  /* rate limit: the step function runs faster than the sensor.  The 3/4
   * factor compensates for the step interval, so "30 fps" really runs on
   * (almost) every frame instead of alternating on/off. */
  int fps = fs_clamp_i(film_sim_fps, 10, 60);
  int t = get_ms_clock();
  int due = (t - fs_prev_t >= 1000 / fps * 3 / 4) || (t < fs_prev_t);

  /*
   * A skipped frame leaves the buffer alone.
   *
   * This used to copy Canon's ungraded frame over ours "so no stale picture is
   * shown", and that is precisely the flicker: the panel is pinned to dst_buf
   * (tweaks.c), so every skipped frame switched the picture back to the
   * ungraded look and the next processed frame switched it forward again --
   * graded, ungraded, graded, at the skip rate. Leaving dst_buf untouched keeps
   * the last graded frame on screen, which is what a dropped frame should look
   * like: a slightly older picture, never a different one.
   */
  if (!due && fs_valid_frame) {
    fs_stat_skipped++;
    return 0;
  }

  fs_prev_t = t;
  if (!fs_stat_frames)
    fs_log("lv: first filtered frame src=0x%08x dst=0x%08x",
           (unsigned)buffers->src_buf, (unsigned)buffers->dst_buf);
  fs_process_frame(buffers->src_buf, buffers->dst_buf);

  fs_valid_frame = 1;
  /* Tell the core the frame is complete, so it only moves the display onto a
   * buffer that holds a finished picture. A skipped frame stays silent and the
   * panel is left where it is. */
  display_filter_mark_written();
  /*
   * Heartbeat: turns "frames=0 at boot" into a rate you can check.
   *
   * Behind film.sim.dbglog like every other periodic line, because each one is
   * a card write: at 30 fps this fired every 10 s of LiveView and the body's
   * access lamp blinked with it, which reads exactly like the camera still
   * being busy long after the shot.
   */
  if (film_sim_dbglog && (fs_stat_frames % 300) == 0) {
    fs_log("lv: %d frames, %dms last, %dms max, %d skipped",
           (int)fs_stat_frames, fs_stat_last_ms, fs_stat_max_ms,
           (int)fs_stat_skipped);
    fs_log_flush();
  }
  return 0;
}

/*
 * Framing, spelled out.
 *
 * The menu has to offer these as .choices, not as a bare number: editing a
 * numeric .max=5 item shows "0".."5", and "0 1 2 3 4 5" says nothing about
 * what the frame will look like. The same list is used by both entries and by
 * the update callback below, so the menu and the screen cannot disagree.
 */
static const char *fs_aspect_names[] = {
    "3:2 full (native)", "1:1 square", "4:5 portrait", "9:16 story",
    "16:9 video",        "65:24 XPan", "2.39:1 Cinemascope",
};
#define FS_ASPECT_COUNT                                                        \
  ((int)(sizeof fs_aspect_names / sizeof fs_aspect_names[0]))

static MENU_UPDATE_FUNC(fs_pad_update) {
  MENU_SET_VALUE(film_sim_pad ? "On" : "Off");
  MENU_SET_ICON(film_sim_pad ? MNI_ON : MNI_OFF, 0);
}

static MENU_UPDATE_FUNC(fs_aspect_update) {
  int a = fs_clamp_i(film_sim_aspect, 0, FS_ASPECT_MAX);
  MENU_SET_VALUE("%s", fs_aspect_names[a]);
  MENU_SET_ICON(a ? MNI_ON : MNI_OFF, 0);
}



static MENU_UPDATE_FUNC(fs_enable_update) {
  if (film_sim_enable && !get_global_draw()) {
    MENU_SET_VALUE("On, needs Global Draw");
    MENU_SET_ICON(MNI_ORANGE_CIRCLE, 0);
  } else {
    MENU_SET_VALUE(film_sim_enable ? "On" : "Off");
    MENU_SET_ICON(film_sim_enable ? MNI_ON : MNI_OFF, 0);
  }

  if (film_sim_enable && !get_global_draw()) {
    MENU_SET_WARNING(MENU_WARN_NOT_WORKING,
                     "Global Draw is off. Display menu -> Global Draw -> ON, "
                     "or the LiveView filter never runs.");
  } else if (film_sim_enable) {
    /* note: EXT_MONITOR_CONNECTED cannot be used here; _ext_monitor_rca
     * starts with an underscore, so it is not exported to modules */
    if (ext_monitor_hdmi || hdmi_code) {
      MENU_SET_WARNING(
          MENU_WARN_NOT_WORKING,
          "Display filters are off while an external monitor is connected.");
    } else if (get_global_draw() == 0) {
      MENU_SET_WARNING(MENU_WARN_NOT_WORKING,
                       "Enable Display -> Global Draw, and use LiveView.");
    }
  }
}

static MENU_SELECT_FUNC(fs_enable_toggle) {
  menu_numeric_toggle(&film_sim_enable, delta, 0, 1);
  fs_signature_valid = 0;
  fs_valid_frame = 0;
}

static MENU_UPDATE_FUNC(fs_fps_update) {
  MENU_SET_VALUE("%d fps", film_sim_fps);
  if (film_sim_fps < 30) {
    MENU_SET_WARNING(MENU_WARN_INFO,
                     "Lower rates leave more CPU time to other ML features.");
  }
}

static MENU_SELECT_FUNC(fs_fps_toggle) {
  menu_numeric_toggle(&film_sim_fps, delta * 10, 10, 60);
}

/* Canon In-Camera Picture Style Control (.pf2/.pf3 in User 1, 2, 3) */
static const int canon_picstyle_ids[] = {
    0x21, /* User 1 (.pf2 / .pf3) */
    0x22, /* User 2 (.pf2 / .pf3) */
    0x23, /* User 3 (.pf2 / .pf3) */
    0x84, /* Neutral */
    0x81, /* Standard */
    0x82, /* Portrait */
    0x83, /* Landscape */
    0x85, /* Faithful */
    0x86, /* Monochrome */
    0x87, /* Auto */
};

static void fs_canon_style_toggle(void *priv, int delta) {
  int current_id = lens_info.raw_picstyle;
  int idx = 0;
  int total = (int)COUNT(canon_picstyle_ids);
  for (int i = 0; i < total; i++) {
    if (canon_picstyle_ids[i] == current_id) {
      idx = i;
      break;
    }
  }
  idx = (idx + delta) % total;
  if (idx < 0)
    idx += total;
  int new_id = canon_picstyle_ids[idx];
  prop_request_change(PROP_PICTURE_STYLE, &new_id, 4);
}

static MENU_UPDATE_FUNC(fs_canon_style_update) {
  const char *name = picstyle_get_current_name();
  MENU_SET_VALUE("%s", (name && name[0]) ? name : "Unknown");
  int raw_id = lens_info.raw_picstyle;
  if (raw_id >= 0x21 && raw_id <= 0x23) {
    MENU_SET_ICON(MNI_ON, 0);
    MENU_SET_HELP(
        "In-Camera hardware Picture Style: User %d (.pf2/.pf3 loaded via USB).",
        raw_id - 0x20);
  } else {
    MENU_SET_ICON(MNI_NONE, 0);
    MENU_SET_HELP("In-Camera hardware Picture Style (SET/arrows to cycle to "
                  "User 1/2/3).");
  }
}

static void fs_canon_contrast_toggle(void *priv, int delta) {
  int c = picstyle_get_current_contrast();
  if (c < -4 || c > 4)
    c = 0;
  c = fs_clamp_i(c + delta, -4, 4);
  picstyle_set_current_contrast(c);
}

static MENU_UPDATE_FUNC(fs_canon_contrast_update) {
  int c = picstyle_get_current_contrast();
  if (c >= -4 && c <= 4) {
    MENU_SET_VALUE("%+d", c);
    MENU_SET_ICON(MNI_PERCENT, (c + 4) * 100 / 8);
  } else {
    MENU_SET_VALUE("N/A");
  }
}











static void fs_ps_write_dump(void) {
  FILE *f = FIO_CreateFile("ML/LOGS/PSTYLE.TXT");
  if (!f)
    return;

  my_fprintf(f, "film_sim picture style slot probe\n");
  my_fprintf(f,
             "captured in core, where the property pushes actually arrive\n\n");

  for (int i = 0; i < 6; i++) {
    unsigned len = picstyle_flavor_len[i];
    my_fprintf(f, "%s: len=%d", fs_ps_prop_name[i], (int)len);

    unsigned n = len > 64 ? 64 : len;
    for (unsigned k = 0; k < n; k++)
      my_fprintf(f, " %02x", picstyle_flavor_head[i][k]);

    /* the style name sits at +4 when a .pf2/.pf3 is registered */
    if (len >= 54)
      my_fprintf(f, "  name='%s'", (const char *)&picstyle_flavor_head[i][4]);

    my_fprintf(f, "\n");
  }
  FIO_CloseFile(f);

  NotifyBox(2000, "PSTYLE.TXT written");
}


/*
 * Picture style slot backup and cloning.
 *
 * Core hands us the full 16,704-byte Slot 1 flavor blob the camera pushes at
 * boot.  We mirror it to ML/LOGS/SLOT1.BIN and ML/LOGS/SLOT1_ORIG.BIN (so a
 * slot can always be restored), and can push a cloned/re-slidered blob back
 * with prop_request_change_wait(0x04010001, ...).  Only the established
 * header (+0x00..+0x1B) and slider struct (+0x24..+0x3B) are edited.
 */
static unsigned char fs_blob[PICSTYLE_BLOB_SIZE];
static int fs_blob_cached = 0;
static volatile uint32_t fs_self_inject_seq = 0;

static void fs_log_camera_snapshot(const char *reason) {
  struct vram_info *vr = get_yuv422_vram();
  const char *ps_name = picstyle_get_current_name();
  fs_log("=== SNAPSHOT (%s) ===", reason ? reason : "manual");
  fs_log("  config: enable=%d preset=%d (%s) strength=%d pct grain=%d fps=%d "
         "user1=%d",
         film_sim_enable, film_sim_preset,
         fs_preset_names[fs_clamp_i(film_sim_preset, 0, FS_PRESET_COUNT - 1)],
         film_sim_strength, film_sim_grain, film_sim_fps, film_sim_user1);
  fs_log("  camera: lv=%d global_draw=%d hdmi=%d/%d raw_picstyle=0x%x "
         "picstyle_idx=%d name='%s'",
         lv, get_global_draw(), ext_monitor_hdmi, hdmi_code,
         lens_info.raw_picstyle, lens_info.picstyle,
         ps_name ? ps_name : "NULL");
  fs_log("  sliders: contrast=%d sharpness=%d saturation=%d color_tone=%d",
         picstyle_get_current_contrast(), picstyle_get_current_sharpness(),
         picstyle_get_current_saturation(), picstyle_get_current_color_tone());
  fs_log("  vram: ptr=0x%08x pitch=%d height=%d | os: y0=%d y_max=%d -> "
         "rows=%d..%d",
         (unsigned)(vr ? vr->vram : 0), vr ? vr->pitch : -1,
         vr ? vr->height : -1, os.y0, os.y_max, fs_stat_row0, fs_stat_row1);
  fs_log("  filter_stats: frames=%d skipped=%d last_dt=%dms max_dt=%dms "
         "src=0x%08x dst=0x%08x",
         (int)fs_stat_frames, (int)fs_stat_skipped, fs_stat_last_ms,
         fs_stat_max_ms, (unsigned)fs_stat_src, (unsigned)fs_stat_dst);
  fs_log("  slot1: ready=%d cached=%d src='%s' last_inject_rc=%d blob_seq=%d",
         fs_slot1_avail, fs_blob_cached, fs_stat_slot1_src, fs_stat_inject_rc,
         (int)picstyle_blob_seq);
  for (int i = 0; i < 6; i++) {
    int flen = (int)picstyle_flavor_len[i];
    const unsigned char *h = picstyle_flavor_head[i];
    fs_log("  prop %s: len=%d head=[%02x %02x %02x %02x] name='%s'",
           fs_ps_prop_name[i], flen, h[0], h[1], h[2], h[3],
           (flen >= 54) ? (const char *)&h[4] : "");
  }
}

static int fs_blob_cached_preset = -2;

static int fs_load_slot1_for_preset(int preset, unsigned char *buf,
                                    uint32_t *out_len) {
  char preset_path[32];
  preset_path[0] = 0;
  uint32_t len = 0;
  const char *found_path = 0;

  if (preset >= 0 && preset < FS_PRESET_COUNT) {
    snprintf(preset_path, sizeof(preset_path), "ML/FILMSIM/PRESET%d.BIN",
             preset);
    if (fs_blob_cached && fs_blob_cached_preset == preset) {
      *out_len = PICSTYLE_BLOB_SIZE;
      fs_slot1_avail = 1;
      return 0;
    }
    if (FIO_GetFileSize(preset_path, &len) == 0 && len == PICSTYLE_BLOB_SIZE) {
      found_path = preset_path;
    }
  }

  /* If no per-preset PRESETn.BIN exists and a shared template is already
   * cached, reuse it */
  if (!found_path && fs_blob_cached && fs_blob_cached_preset == -1) {
    *out_len = PICSTYLE_BLOB_SIZE;
    fs_slot1_avail = 1;
    return 0;
  }

  if (!found_path) {
    static const char *const shared_paths[4] = {
        "ML/FILMSIM/CUSTOM.PF2",
        "ML/FILMSIM/SLOT1.BIN",
        "ML/LOGS/SLOT1.BIN",
        "ML/DATA/SLOT1.BIN",
    };
    for (int i = 0; i < 4; i++) {
      if (FIO_GetFileSize(shared_paths[i], &len) == 0 &&
          len == PICSTYLE_BLOB_SIZE) {
        found_path = shared_paths[i];
        break;
      }
    }
  }

  if (!found_path) {
    if (fs_blob_cached) {
      /* Keep existing camera-ram cached blob if no card file found */
      *out_len = PICSTYLE_BLOB_SIZE;
      fs_slot1_avail = 1;
      return 0;
    }
    fs_slot1_avail = 0;
    return -1;
  }

  unsigned char *ubuf = fio_malloc(PICSTYLE_BLOB_SIZE);
  if (!ubuf)
    return -2;

  FILE *f = FIO_OpenFile(found_path, O_RDONLY);
  if (!f) {
    fio_free(ubuf);
    return -3;
  }
  int n = FIO_ReadFile(f, ubuf, len);
  FIO_CloseFile(f);
  if (n != (int)len) {
    fio_free(ubuf);
    return -4;
  }

  memcpy(buf, ubuf, len);
  fio_free(ubuf);
  fs_blob_cached = 1;
  fs_blob_cached_preset = (found_path == preset_path) ? preset : -1;
  fs_slot1_avail = 1;
  fs_stat_slot1_src =
      (found_path == preset_path) ? "ML/FILMSIM/PRESETn.BIN" : found_path;
  *out_len = len;
  buf[27] = 0; /* guarantee NUL termination within 24-byte name field
                  (+0x04..+0x1B) */
  fs_log("fs_load_slot1: loaded %d bytes from %s (head=%02x %02x %02x %02x "
         "name='%s')",
         (int)len, found_path, buf[0], buf[1], buf[2], buf[3],
         (const char *)&buf[4]);
  return 0;
}

static int fs_load_slot1(unsigned char *buf, uint32_t *out_len) {
  return fs_load_slot1_for_preset(-1, buf, out_len);
}

static int fs_slot1_ready(void) {
  /* Safe for MENU_UPDATE_FUNC: reads cached state without SD card I/O */
  return fs_slot1_avail || fs_blob_cached;
}

/* Clone/re-slider the slot 1 flavor blob into User 1 and select User 1. */
static int fs_inject_user1(int preset, const char *name) {
  const fs_params_t *p =
      &fs_presets[fs_clamp_i(preset, 0, FS_PRESET_COUNT - 1)];
  int c_contrast = fs_clamp_i(p->contrast / 25, -4, 4);
  int c_saturation = (preset == FS_PRESET_BW)
                         ? -4
                         : fs_clamp_i((p->saturation - 100) / 25, -4, 4);
  int c_color_tone = fs_clamp_i(p->hue / 3, -4, 4);
  int c_sharpness = 0;

  uint32_t len = 0;
  int load_rc = fs_load_slot1_for_preset(preset, fs_blob, &len);
  int flavor_ack = 0;
  int style_ack = 0;
  int slider_ack = 0;

  if (load_rc == 0 && len == PICSTYLE_BLOB_SIZE) {
    /*
     * Canon PROP_PC_FLAVOR1_PARAM (0x04010001) layout in 6D serial flash
     * (ROM0.BIN / SFDATA.BIN records at 0x61b38, 0x65c8c, 0x69de0):
     *   +0x00..+0x01 : uint16_le self-length 0x4140 (40 41 = 16,704 bytes)
     *   +0x02        : base style ID (0x84 Neutral, 0x81 Standard, 0x82
     * Portrait) +0x03        : 0x00 +0x04..+0x1B : 24-byte NUL-padded ASCII
     * style name (max 23 chars + NUL) +0x1C..+0x23 : two int32_t
     * fineness/threshold params (preserved) +0x24..+0x3B : 24-byte slider
     * struct (contrast, sharpness, saturation, color_tone, 0xDEADBEEF,
     * 0xDEADBEEF) +0x3C..+0x47 : 12-byte PF2 header (40 00 fc 00 ff 2f ff ff 00
     * 00 00 00) +0x48..+0x413F: 16,632-byte compressed/enciphered PF2 body (73
     * d1 4a f6...), cloned intact from the source slot/PF2 file.
     */
    fs_blob[0] = 0x40;
    fs_blob[1] = 0x41;
    if (fs_blob[2] == 0)
      fs_blob[2] = 0x84; /* default Neutral base */
    fs_blob[3] = 0x00;

    if (name && name[0]) {
      int i = 0;
      for (; i < 23 && name[i]; i++)
        fs_blob[4 + i] = name[i];
      for (; i < 24; i++)
        fs_blob[4 + i] = 0;
    }

    int32_t *sl = (int32_t *)&fs_blob[0x24];
    sl[0] = c_contrast;
    sl[1] = c_sharpness;
    sl[2] = c_saturation;
    sl[3] = c_color_tone;
    ((uint32_t *)sl)[4] = 0xDEADBEEF;
    ((uint32_t *)sl)[5] = 0xDEADBEEF;

    /* Push flavor 1 back to camera and wait for PropMgr acknowledgement.
     * Pre-set fs_self_inject_seq = seq + 1 before calling so both synchronous
     * and delayed asynchronous echo deliveries are recognized as self-inject.
     */
    fs_self_inject_seq = picstyle_blob_seq + 1;
    flavor_ack = prop_request_change_wait(0x04010001, fs_blob, len, 1000);
    if (picstyle_blob_seq > fs_self_inject_seq)
      fs_self_inject_seq = picstyle_blob_seq;

    /* Select User 1 (0x21) */
    int pic_id = 0x21;
    style_ack = prop_request_change_wait(PROP_PICTURE_STYLE, &pic_id, 4, 500);

    /*
     * Send the 24-byte slider struct to PROP_PICSTYLE_SETTINGS_USERDEF1
     * (0x02060007 on DIGIC 4/5/6/7).  Per src/picstyle.c, the two unused
     * trailing fields in ML's struct prop_picstyle_settings are 0xDEADBEEF.
     */
    struct {
      int32_t contrast;
      uint32_t sharpness;
      int32_t saturation;
      int32_t color_tone;
      uint32_t off_0x10;
      uint32_t off_0x14;
    } user1_settings = {
        .contrast = c_contrast,
        .sharpness = c_sharpness,
        .saturation = c_saturation,
        .color_tone = c_color_tone,
        .off_0x10 = 0xDEADBEEF,
        .off_0x14 = 0xDEADBEEF,
    };
    slider_ack = prop_request_change_wait(0x02060007, &user1_settings,
                                          sizeof(user1_settings), 500);
  }

  /* Record combined result: 0 = load + flavor_ack succeeded, <0 = failure */
  if (load_rc != 0)
    fs_stat_inject_rc = load_rc;
  else if (!flavor_ack)
    fs_stat_inject_rc = -5; /* property write unacknowledged / timed out */
  else {
    fs_stat_inject_rc = 0;
    fs_user1_last_preset = preset;
  }

  fs_log(
      "fs_inject_user1: preset=%d ('%s') load_rc=%d flavor_ack=%d style_ack=%d "
      "slider_ack=%d inject_rc=%d src=%s sliders=[C:%+d S:%d Sat:%+d T:%+d]",
      preset, name ? name : "", load_rc, flavor_ack, style_ack, slider_ack,
      fs_stat_inject_rc, fs_stat_slot1_src, c_contrast, c_sharpness,
      c_saturation, c_color_tone);

  return fs_stat_inject_rc;
}

static void fs_stock_wb_shift(int *ba, int *gm);

/*
 * Push the recipe's WB shift into the camera.
 *
 * Both properties are signed 8-bit in a 4-byte word (property.h: "signed
 * 8-bit, len=4"), which is why ML's own shoot.c masks with 0xFF before the
 * request: a bare negative int puts 0xFFFFFF in the low three bytes and the
 * firmware reads the wrong field.
 *
 * Property writes block on an ICU round-trip, so this is called from the
 * backup task and from the menu handler's action, never from a key callback.
 */
static void fs_push_wb_shift(void)
{
  int sba, sgm, ba, gm;

  fs_stock_wb_shift(&sba, &sgm);
  ba = fs_clamp_i(sba + fs_clamp_i(film_sim_wbs_ba, -9, 9), -9, 9) & 0xFF;
  gm = fs_clamp_i(sgm + fs_clamp_i(film_sim_wbs_gm, -9, 9), -9, 9) & 0xFF;

  prop_request_change(PROP_WBS_BA, &ba, 4);
  prop_request_change(PROP_WBS_GM, &gm, 4);
  /* both numbers, because the screen shows the offset and the camera holds
   * the sum: if these disagree with the screen, that is the bug to chase */
  fs_log("fuji: WB shift stock B/A %+d G/M %+d, offset B/A %+d G/M %+d "
         "-> camera B/A %+d G/M %+d",
         sba, sgm, (int)film_sim_wbs_ba, (int)film_sim_wbs_gm,
         (int)(signed char)ba, (int)(signed char)gm);
  fs_log_flush();
}

static void fs_slot_backup_task(void *unused) {
  uint32_t seen = 0;

  /* Give DryOS property service time to deliver initial camera state */
  msleep(1000);
  { static int ok; fs_ensure_dir("ML/LOGS", &ok); }

  /* Check if a pre-existing SLOT1.BIN is on card */
  uint32_t init_len = 0;
  if (fs_load_slot1(fs_blob, &init_len) == 0) {
    fs_slot1_avail = 1;
  }

  fs_log_camera_snapshot("boot-init");
  fs_log_flush();

  for (int iter = 0;; iter++) {
    uint32_t seq = picstyle_blob_seq;
    if (seq && seq != seen) {
      seen = seq;
      int slot = picstyle_blob_slot;
      uint32_t len = picstyle_blob_len;
      int is_self_inject = (seq == fs_self_inject_seq);
      fs_log("prop_deliver: slot=%d len=%d seq=%d self=%d", slot, (int)len,
             (int)seq, is_self_inject);
      if (!is_self_inject && slot >= 1 && slot <= 3 && len > 0 &&
          len <= PICSTYLE_BLOB_SIZE) {
        if (!fs_blob_cached && slot == 1 && len == PICSTYLE_BLOB_SIZE) {
          memcpy(fs_blob, picstyle_slot_blob[0], len);
          fs_blob_cached = 1;
          fs_blob_cached_preset = -1;
          fs_slot1_avail = 1;
          fs_stat_slot1_src = "camera-ram";
          if (film_sim_user1 && fs_user1_last_preset < 0) {
            fs_user1_dirty = 1;
          }
        }
        char path[32];
        snprintf(path, sizeof(path), "ML/LOGS/SLOT%d.BIN", slot);
        FILE *f = FIO_CreateFile(path);
        if (f) {
          if (slot <= 2) {
            FIO_WriteFile(f, picstyle_slot_blob[slot - 1], len);
          }
          FIO_CloseFile(f);
        }
        if (slot == 1 && len == PICSTYLE_BLOB_SIZE) {
          /* Preserve factory/original camera slot 1 once */
          uint32_t orig_sz = 0;
          if (FIO_GetFileSize("ML/LOGS/SLOT1_ORIG.BIN", &orig_sz) != 0) {
            FILE *fo = FIO_CreateFile("ML/LOGS/SLOT1_ORIG.BIN");
            if (fo) {
              FIO_WriteFile(fo, picstyle_slot_blob[0], len);
              FIO_CloseFile(fo);
              fs_log("saved factory backup ML/LOGS/SLOT1_ORIG.BIN (%d bytes)",
                     (int)len);
            }
          }
        }
      }
      /* Release slot lock in picstyle_flavor_capture so later slot 2/3 updates
       * can be captured */
      picstyle_blob_slot = 0;
    }

    if (fs_user1_dirty) {
      fs_user1_dirty = 0;
      if (film_sim_user1 && fs_grade_in_shutter()) {
        /*
         * Neutral capture means Canon contributes no look, so the engine's LUT
         * is the only thing grading the frame.  Injecting a PF2 blob here
         * would put Canon's preset rendering over the Neutral style that
         * fs_apply_film_now() just wrote, and the frame would then carry a
         * preset look on top of the engine's LUT instead of the LUT alone.
         */
        fs_log("fs_inject_user1: skipped, neutral capture (engine LUT only)");
      } else if (film_sim_user1) {
        /*
         * Use the preset the STOCK maps to, not film.sim.preset.
         *
         * fs_apply_film_now() writes the stock's own sliders, and then this
         * injects a PF2 blob over them.  It used film_sim_preset -- which is 1,
         * "Fuji Superia 400", on a card that has never had that changed -- so
         * whatever stock was picked, the style ended up being Fuji Superia.
         * FILMSIM.LOG shows it directly: 'Bleach Bypass', 'Fomapan 100',
         * 'Pan F Plus 50' and 'ColorPlus 200' each wrote neutral=0 with their
         * own sliders at 20.6-22.9 s, and at 25.9 s fs_inject_user1 replaced
         * the lot with preset=1 'Fuji Superia 400'.
         */
        int preset = fs_lv_preset();
        fs_inject_user1(preset, fs_preset_names[preset]);
        fs_user1_last_preset = preset;
      }
    }

    /* The bake serves two consumers: the in-shutter grade (yuvrows) and the
     * LiveView lattice. Gating it on yuvrows alone left LiveView on the
     * preset tables when the shutter grade was off, i.e. the switch did
     * nothing exactly when a user would try it. */
    if (fs_yuvlook_dirty && (film_sim_yuvrows > 0 || film_sim_lv3d)) {
      fs_yuvlook_dirty = 0;
      char stk[FS_PATH_LEN];
      if (fs_active_stock_path(stk, sizeof stk, NULL)) {
        if (fs_bake_stock(stk, film_sim_strength, fs_own_lut_path()) == 0 &&
            fs_yuvlook_name()[0]) {
          fs_log("yuvgrade: re-baked '%s' from %s at strength %d",
                 fs_yuvlook_name(), stk, film_sim_strength);
          /* Same rule as boot: grade in place only when the capture style was
           * Neutral. Without the neutralcap test this re-armed grading after
           * any stock change, so a stock capture style got the engine's look
           * applied on top of it -- measured on IMG_9973_F.JPG, sat 63.2
           * against the camera JPEG's 29.6. */
          fs_yuvhook_bars(film_sim_aspect);
          fs_yuvhook_need_neutral(fs_grade_in_shutter());
          fs_yuvhook_grade(fs_grade_in_shutter() ? film_sim_yuvrows : 0);
        }
      }
    }

    if (fs_force_dump_log) {
      fs_force_dump_log = 0;
      fs_log_camera_snapshot("user-menu-dump");
      fs_log_flush();
      NotifyBox(2000, "FILMSIM.LOG written (%d frames)", (int)fs_stat_frames);
    } else {
      fs_log_flush();
    }

    msleep(500);
  }
}


static MENU_SELECT_FUNC(fs_debug_log_select) { fs_force_dump_log = 1; }


/* Flag-driven run, so the probe does not depend on menu navigation. */
static void fs_ps_probe_task(void *unused) {
  /* property handlers are registered after the module loads; give the
   * property manager time to deliver the current values */
  msleep(6000);
  fs_ps_write_dump();
}

static struct menu_entry film_sim_liveview_children[] = {
    {
        .name = "LiveView Filter",
        .priv = &film_sim_enable,
        .select = fs_enable_toggle,
        .update = fs_enable_update,
        .help = "Apply the film look to the LiveView preview in real time.",
        .help2 = "RAW/JPEG untouched; export matching LUT with filmsim_lut.py.",
    },
    {
        .name = "Frame Rate",
        .priv = &film_sim_fps,
        .select = fs_fps_toggle,
        .update = fs_fps_update,
        .help = "Upper limit for simulation rate (30 fps recommended).",
        .help2 = "Reduce if LiveView preview feels sluggish.",
    },
    {
        .name = "In-Camera Style",
        .select = fs_canon_style_toggle,
        .update = fs_canon_style_update,
        .help = "Switch active Canon Picture Style (User 1/2/3 for preloaded "
                ".pf2/.pf3).",
        .help2 = "Bakes directly into in-camera JPEGs and H.264 video!",
        .icon_type = IT_DICE,
    },
    {
        .name = "JPEG Contrast",
        .select = fs_canon_contrast_toggle,
        .update = fs_canon_contrast_update,
        .help =
            "Adjust hardware JPEG contrast (-4..+4) for active Picture Style.",
    },
    MENU_EOL,
};

/* LiveView gets its own top-level menu: it is a different job from developing
 * a file, and the two were being read as one confusing list. */

/*
 * The one reason the LiveView grade can be enabled and still never run.
 *
 * get_global_draw() (zebra.c) returns
 *     lv && global_draw && lv_disp_mode == 0 && ...
 * and lv_disp_mode is 1 whenever Canon's own LiveView overlays are on the
 * panel. ML's own menu says the same thing: "Press INFO (outside ML menu) to
 * turn Canon displays off." Rather than make the user find that, this sends the
 * INFO button and reports what the switch did.
 */
static MENU_SELECT_FUNC(fs_lv_clear_overlays) {
  int before_disp = lv_disp_mode;
  int before_gd = get_global_draw();
  fs_log("lvfix: before lv=%d disp=%d setting=%d gd=%d frames=%d", lv,
         before_disp, get_global_draw_setting(), before_gd,
         (int)fs_stat_frames);
  fs_log_flush();
  if (!lv) {
    NotifyBox(2500, "Not in LiveView");
    return;
  }
  /* BGMT_INFO is 0x7 on the 6D (platform/6D.116/gui.h). Modules do not get
   * that header -- dryos.h includes it only for the core build -- so the code
   * is written out, as the raw key table already does. */
  fake_simple_button(0x7);
  msleep(700);
  fs_log("lvfix: after  lv=%d disp=%d setting=%d gd=%d frames=%d", lv,
         lv_disp_mode, get_global_draw_setting(), get_global_draw(),
         (int)fs_stat_frames);
  fs_log_flush();
  NotifyBox(3000, "LV overlays: %s -> %s\n% GlobalDraw %d",
            before_disp ? "on" : "off", lv_disp_mode ? "on" : "off",
            get_global_draw_setting());
}

static MENU_UPDATE_FUNC(fs_lv_clear_update) {
  if (!lv) {
    MENU_SET_WARNING(MENU_WARN_NOT_WORKING,
                     "Enter LiveView first: this sends the INFO button.");
  } else if (lv_disp_mode == 0) {
    MENU_SET_WARNING(MENU_WARN_INFO,
                     "Canon overlays already off (disp=0). The grade can run.");
  } else {
    MENU_SET_WARNING(
        MENU_WARN_INFO,
        "Canon overlays are ON (disp=%d), which switches the grade "
        "off. SET sends INFO.",
        lv_disp_mode);
  }
  MENU_SET_VALUE("%d fr", (int)fs_stat_frames);
}



/* ---------------------------------------------------------------- film sim
 *
 * The primary feature: put a film stock into the ACTIVE picture style, so the
 * JPEG carries it.  Applied straight from the menu callback - no command
 * queue, no task hand-off.  The queue is why selecting a stock needed a couple
 * of seconds before anything happened: the press only set a flag, and a
 * background task applied it at its next tick.
 */

/* Off by default: ON fires a 30 s develop on every shot and on every
 * switch to level 2/3, which should be a deliberate choice. */
static CONFIG_INT("film.sim.auto", film_sim_auto, 0);
/*
 * Write the developed frame over the camera's own JPEG for that frame.
 *
 * Canon's playback index is keyed by file number and built when the card is
 * mounted, so a JPEG the module creates under a *new* number is invisible until
 * the card is re-inserted. A JPEG under the *same* number as a raw the camera
 * already knows is the one case the firmware handles natively -- RAW+JPEG
 * pairing -- but with the camera set to RAW+JPEG that slot is already taken by
 * Canon's own JPEG. With this on, the film version takes the slot: the raw is
 * never touched, the camera JPEG for that frame is replaced.
 *
 * Off by default. It destroys a file the camera wrote, so it is opt-in.
 */
/*
 * May a shot that incremented the hook's counter skip the develop?
 *
 * Only a hint, because the check it was waiting for cannot work: a shot's JPEG
 * and the preview inside its own CR2 are encoded from the *same* YUV buffer by
 * the same Canon block, so comparing them always gives re-encode noise
 * (mean |diff| ~1/765) whether the grade ran or not. That comparison was made,
 * saw nothing, and produced the wrong conclusion -- "the pixels never arrive".
 *
 * The check that does work is a black-and-white recipe: if the shot's JPEG
 * comes back R = G = B over the whole frame (saturation 0.0) and "yuvcap" from
 * the same run logged the capture style as built-in Neutral, Canon's encoder
 * consumed graded pixels -- a monochrome *style* would have logged 0x86. With a colour stock it cannot be done that way, so
 * trust "yuvgrade: in-shutter grade ON" plus "yuvcap: cache alias used" in
 * ML/LOGS/FILMSIM.LOG, and keep this at 0 until the file itself has been
 * measured. With it at 0 and Auto Develop on, the JPEG is graded either way:
 * the in-shutter path if it ran, the develop if it did not.
 */
static CONFIG_INT("film.sim.shotgrade", film_sim_shotgrade, 0);


/*
 * Write a JPEG from the frame the camera built, after grading it in place.
 *
 * This is the shot path's exit and it does not open the CR2: the pixels are
 * already in RAM and already graded, so the file costs one encode, not a
 * decode. Off means "grade the buffer and trust Canon's encoder to read it".
 * To check that on a card, use a black-and-white recipe: the shot must come
 * back R = G = B over the whole frame, and "yuvcap" from the same run must log
 * the capture style as built-in Neutral (0x84) -- with 0x86 it would be Canon's
 * Monochrome style instead, and the pixels would prove nothing.
 */
/* 1 = full resolution, 2 = half (4x less encode work). 20 MP did not finish
 * inside the time the camera stayed on, and the cost is linear in pixels. */
static CONFIG_INT("film.sim.rename", film_sim_rename,
                  1); /* stock name into the style */
static CONFIG_INT(
    "film.sim.dev", film_sim_dev,
    FS_DEV_STANDARD); /* The Soup: Standard, Push+1, Push+2, Pull-1 */
static CONFIG_INT("film.sim.wb_shift", film_sim_wb_shift,
                  1); /* Apply Kelvin + Amber/Green shift */
static CONFIG_INT("film.sim.lock_iso", film_sim_lock_iso,
                  0); /* Lock ISO to film stock speed */

static void fs_stock_wb_shift(int *ba, int *gm)
{
  film_style_t st;
  int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
  int idx = fs_clamp_i(film_sim_stock, 0, count - 1);

  film_active_to_style(idx, film_sim_dev, &st);
  *ba = st.wbs_ba;
  *gm = st.wbs_gm;
}


static int fs_active_slot(void) {
  int raw = lens_info.raw_picstyle;
  if (raw == 0x21)
    return 1;
  if (raw == 0x22)
    return 2;
  if (raw == 0x23)
    return 3;
  return 0;
}

static uint32_t fs_base_prop_of(int slot) {
  return slot == 2 ? 0x0206000b : slot == 3 ? 0x0206000c : 0x0206000a;
}

static uint32_t fs_flavor_prop_of(int slot) {
  return slot == 2 ? 0x04010003 : slot == 3 ? 0x04010005 : 0x04010001;
}

static uint32_t fs_set_prop_of(int slot) {
  return slot == 2 ? 0x02060008 : slot == 3 ? 0x02060009 : 0x02060007;
}

/*
 * Put the camera on the picture style this mode actually needs.
 *
 * "Neutral capture" only ever wrote base=0x84 into User Def 1's settings prop,
 * and that write lives in fs_apply_film_now(), which nothing calls at boot. The
 * camera therefore kept whichever style was selected last -- User Def 1, holding
 * a 16704-byte PF2 blob from an earlier session ('Fuji Superia 400' in the boot
 * snapshot) -- so every shot carried Fuji Superia while the log said "capture
 * style Neutral" and the _F file carried ColorPlus 200. Two looks, two writers.
 *
 * PROP_PICTURE_STYLE is Canon's PROP_FLAVOR_MODE: the style the camera is ON, as
 * opposed to the settings of a style it is not using. Neutral takes it to the
 * built-in Neutral (0x84) so no User Def blob can reach a shot; otherwise back
 * to User Def 1 (0x21), which is where the stock's sliders are written.
 */
static int fs_force_capture_style(void)
{
  int want = fs_grade_in_shutter() ? 0x84 : 0x21;
  if ((int)lens_info.raw_picstyle == want) {
    fs_log("capset: already on picture style 0x%02x (%s)", want,
           want == 0x84 ? "Neutral" : "UserDef1");
    return 0;
  }
  int ack = prop_request_change_wait(PROP_PICTURE_STYLE, &want, 4, 400);
  fs_log("capset: picture style 0x%02x -> 0x%02x (%s) ack=%d",
         (unsigned)lens_info.raw_picstyle, want,
         want == 0x84 ? "Neutral" : "UserDef1", ack);
  fs_log_flush();
  return ack;
}

static int fs_apply_film_now(void) {
  int slot = fs_active_slot();
  if (!slot) {
    /* No style selected: use User Def 1, or Neutral when neutralizing. */
    fs_force_capture_style();
    slot = 1;
  }

  int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
  int idx = fs_clamp_i(film_sim_stock, 0, count - 1);
  film_style_t st;
  film_active_to_style(idx, film_sim_dev, &st);

  /*
   * Neutral Capture exists for one reason: at level 2/3 the develop grades the
   * frame with the stock's .fs recipe, and if the picture style had already
   * applied the same look the frame would be graded twice (measured 20.8/255
   * for Velvia). So the style is forced Neutral there.
   *
   * At level 1 there is no develop -- the style IS the look. Forcing Neutral
   * there made every stock write base=0x84 con=0 shp=1 sat=0 tone=0, so the
   * shelf could not change the colour at all: FILMSIM.LOG has eight different
   * films in a row writing byte-identical sliders and only the style name
   * changed. So the override must not apply at level 1.
   */
  int neutral_style = fs_grade_in_shutter();
  if (neutral_style) {
    st.base = 0x84; /* Neutral */
    st.contrast = 0;
    /* Neutral's own sharpness is the minimum, so a Neutral capture is the one
     * JPEG this camera writes with no sharpening at all -- measured at half the
     * detail energy of a develop of the same frame (mean |laplacian| 2.91
     * against 6.44). The look is the engine's, but the sharpening is Canon's,
     * and it is a per-style setting, not part of the look. */
    st.sharpness = fs_clamp_i(film_sim_sharp, 0, 7);
    st.saturation = 0;
    st.tone = 0;
    /* Kelvin and WB shift are NOT neutralised: every stock in film_table.h
     * carries its own balance (4900-6000 K plus a blue/amber and green/magenta
     * shift) and that is a property of the emulsion, not of the look. Zeroing
     * them here left every stock shooting at whatever WB the camera was on. */

  }

  int base = st.base;
  prop_request_change_wait(fs_base_prop_of(slot), &base, 4, 400);

  /* The sliders above only matter if the camera is ON this slot's style. */
  fs_force_capture_style();

  int32_t raw[6];
  raw[0] = st.contrast;
  raw[1] = st.sharpness;
  raw[2] = (st.saturation == FILM_NA) ? (int32_t)0xDEADBEEF : st.saturation;
  raw[3] = (st.tone == FILM_NA) ? (int32_t)0xDEADBEEF : st.tone;
  raw[4] = (int32_t)0xDEADBEEF;
  raw[5] = (int32_t)0xDEADBEEF;

  int ack = prop_request_change_wait(fs_set_prop_of(slot), raw, 24, 400);

  /*
   * Apply hardware White Balance Shift and Kelvin (from Florin / film_xml)
   */
  if (film_sim_wb_shift) {
    /*
     * With the engine grading the frame, the stock's balance is the look's
     * business, not the camera's.
     *
     * At level 2/3 the capture style is forced Neutral so Canon contributes no
     * look -- but this block still pushed the stock's Kelvin and WB shift onto
     * the sensor, so the engine's recipe landed on a frame the camera had
     * already warmed and greened. Superia 400 shows it plainly: 5200 K with
     * +2 blue/amber and +2 green/magenta under a recipe that already lifts
     * greens and puts +8 green in the shadows comes out green everywhere, while
     * the shelf, the darkroom and the develop -- which see only the recipe --
     * promise something else. The same stock cannot look like two films.
     *
     * So at level 2/3 the balance stays as shot; the user's own offsets on the
     * WB screen still apply, because that is a decision, not a recipe.
     */
    if (st.kelvin > 0 && !neutral_style) {
      lens_set_kelvin(st.kelvin);
    }
    /*
     * The stock's OWN shift, plus the creator's offset.
     *
     * These two lines used to write st.wbs_ba alone, which meant a shift made
     * on the WB screen survived only until the next stock was applied -- the
     * screen was replacing the recipe's balance rather than sitting on top of
     * it. Every stock in film_table.h carries a balance of its own (blue/amber
     * and green/magenta, part of the emulsion), so an edit has to be a DELTA
     * against that, which is also what a Fuji recipe is: custom values layered
     * on a base simulation.
     */
    {
      int sba = neutral_style ? 0 : st.wbs_ba;
      int sgm = neutral_style ? 0 : st.wbs_gm;
      int ba = fs_clamp_i(sba + fs_clamp_i(film_sim_wbs_ba, -9, 9), -9, 9);
      int gm = fs_clamp_i(sgm + fs_clamp_i(film_sim_wbs_gm, -9, 9), -9, 9);
      lens_set_wbs_ba(ba);
      lens_set_wbs_gm(gm);
      if (neutral_style && (st.wbs_ba || st.wbs_gm || st.kelvin > 0))
        fs_log("wb: stock balance left to the engine (%d K, %+d/%+d not written; "
               "camera %+d/%+d)", st.kelvin, st.wbs_ba, st.wbs_gm, ba, gm);
    }
  }

  /* Lock camera ISO to the film stock's rated speed */
  if (film_sim_lock_iso && st.raw_iso > 0) {
    lens_set_rawiso(st.raw_iso);
  }

  /*
   * Put the stock name into the style itself, so the camera's own Picture
   * Style menu shows "Kodak Portra 400" instead of "User Def. 1".
   */
  if (film_sim_rename && slot <= 2 &&
      picstyle_slot_len[slot - 1] == PICSTYLE_BLOB_SIZE) {
    unsigned char *body = picstyle_slot_blob[slot - 1];
    memset(body + 4, 0, 24);
    int n = 0;
    const char *nm = film_active_table[idx].name;
    for (; n < 23 && nm[n]; n++)
      body[4 + n] = nm[n];
    uint32_t prop = fs_flavor_prop_of(slot);
    int nack = prop_request_change_wait(prop, body, PICSTYLE_BLOB_SIZE, 800);
    fs_log("film: name '%s' -> User %d body (+4) ack=%d", nm, slot, nack);
  }

  fs_log("film: '%s' -> User %d base=0x%02x con=%d shp=%d sat=%d tone=%d "
         "kelvin=%d level=%d neutral=%d ack=%d",
         film_active_table[idx].name, slot, base, st.contrast, st.sharpness,
         st.saturation, st.tone, st.kelvin, film_sim_level, neutral_style, ack);
  fs_log_flush();
  return ack;
}

static MENU_SELECT_FUNC(fs_film_next) {
  int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
  int i = fs_clamp_i(film_sim_stock, 0, count - 1);
  i += (delta < 0) ? -1 : 1;
  if (i < 0)
    i = count - 1;
  if (i >= count)
    i = 0;
  film_sim_stock = i;
  fs_apply_film_now(); /* instant: no queue */
}

static MENU_UPDATE_FUNC(fs_film_update) {
  int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
  int idx = fs_clamp_i(film_sim_stock, 0, count - 1);
  film_style_t st;
  film_active_to_style(idx, film_sim_dev, &st);

  const char *soup_str = (film_sim_dev == FS_DEV_PUSH_1)   ? " [Push+1]"
                         : (film_sim_dev == FS_DEV_PUSH_2) ? " [Push+2]"
                         : (film_sim_dev == FS_DEV_PULL_1) ? " [Pull-1]"
                                                           : "";
  MENU_SET_VALUE("%s%s", film_active_table[idx].name, soup_str);

  int slot = fs_active_slot();
  if (!slot) {
    MENU_SET_WARNING(
        MENU_WARN_NOT_WORKING,
        "Select User Def. 1, 2 or 3 in the Canon menu. This writes into that "
        "style; it refuses on a built-in style.");
    return;
  }

  MENU_SET_WARNING(
      MENU_WARN_INFO,
      "In User %d: base 0x%02x, C:%+d S:%d Sat:%d Tone:%d | %dK BA:%+d GM:%+d",
      slot, st.base, st.contrast, st.sharpness, st.saturation, st.tone,
      st.kelvin, st.wbs_ba, st.wbs_gm);
}



/* ---------------------------------------------------------------- darkroom
 *
 * 10-Second Chemical Bath Developing Screen & Full 3D LUT Application.
 * Recreates the authentic darkroom developing cycle for the last captured
 * photo.
 */

/*
 * One task owns the screen at a time.
 *
 * The key handler used to spawn the develop task straight out of the keypress
 * callback while the darkroom task was still holding the display, so two tasks
 * fought over the front buffer, the log flush and the FIFO for FIO. Now the
 * key handler only writes flags; the ui task walks shelf -> darkroom -> develop
 * in one task and hands the panel over once per step.
 */
#define FS_VIEW_NONE 0
#define FS_VIEW_SHELF 1
#define FS_VIEW_DARKROOM 2
#define FS_VIEW_DEVELOP 3
#define FS_VIEW_CREATOR 4
#define FS_VIEW_WB 5

/*
 * Task stack for anything that can reach the develop.
 *
 * 0x4000 (16 KB) died inside the decode: se_cam_develop held an 8 KB header
 * buffer on the stack and se_jpeg_dec_info held an 8.7 KB jd_state, ~18 KB
 * before the decoder's own frames. Both are static now, and the largest single
 * frame left in the whole develop is 1,064 bytes -- the same order as the
 * reference module, whose biggest frame is 900 bytes and which runs its shelf
 * on 16 KB and its develop worker on 8 KB. 32 KB here is that margin doubled,
 * because this is the path that already crashed once.
 */
#define FS_DEV_STACK 0x8000

static volatile int fs_gui_active = 0;

/* Redraw request. The key handler only raises this; the shelf task owns the
 * drawing, so the screen is never written from two contexts at once. */
static volatile int fs_gui_dirty = 0;
/* Which band moved. Cheaper than the whole screen, and set by the two things
 * that animate on their own: the spring and the header sheen. */
static int fs_strip_dirty = 0;
static int fs_header_dirty = 0;
static int fs_gui_logged = 0;
static volatile int fs_darkroom_active = 0;
static volatile int fs_darkroom_dirty = 0;
static int fs_darkroom_logged = 0;
static int fs_darkroom_focus = 0; /* 0 = Photo, 1 = LUT, 2 = Chemistry */

/* What the key handler asks the ui task to do next. */
static volatile int fs_apply_request = 0; /* SET in the shelf */

/* dcim_photo_list and its counters are declared with the key state above. */

/*
 * 0x54: "a button was pressed", with the identity stripped.
 *
 * This body sends it for the d-pad, whose real code Canon swallows while a
 * module owns the panel, and also for buttons whose real code follows in the
 * same millisecond. That is why navigation worked "sometimes": a press with
 * identity acted at once, a press without it had to wait for a timer.
 *
 * So a 0x54 marks a step as pending and takes nothing. If the real key arrives
 * inside FS_PROVISIONAL_MS it does the job and clears the mark; if nothing
 * follows, the screen loop takes the step when the window expires. Exactly one
 * of the two carries it, which is what stops the shelf landing on a stock the
 * dial never stopped at.
 */
#define FS_PROVISIONAL_MS 150

static volatile int fs_prov_active; /* a provisional step is pending */
static volatile uint32_t fs_prov_ms;
static volatile uint32_t fs_prov_add_ms; /* second 0x54 of the same press? */

/* The real code arrived: the step is its business, so drop the marker. */
static void fs_prov_clear(void) { fs_prov_active = 0; }

/* The step a lone 0x54 asked for, taken when the real code never came. */
static int fs_prov_take(void) {
  fs_prov_active = 0;
  if (fs_gui_active) {
    int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
    film_sim_stock = (film_sim_stock + 1) % count;
    fs_apply_request = 1;
    return 1;
  }
  if (fs_darkroom_active) {
    if (dcim_photo_count > 0)
      dcim_photo_index = (dcim_photo_index + 1) % dcim_photo_count;
    return 1;
  }
  return 0;
}

/*
 * Called from the screen loops: a 0x54 that nobody followed up carries the
 * step after the window.
 *
 * The step itself used to be taken here, provisionally, and the real code that
 * follows one millisecond later undid it and applied its own -- two writers for
 * one detent, with a single saved value to undo from. Spinning the dial queued
 * several of those and the undo could put back a stock that had already been
 * superseded, which is what "choosing the film stock is bugged" looks like: the
 * shelf landing on a stock the dial never stopped at.
 *
 * Now a 0x54 only marks that a step is pending; exactly one of the two paths
 * takes it, so one detent is one step whichever form the body sends.
 */
static int fs_prov_tick(void) {
  if (!fs_prov_active)
    return 0;
  if (get_ms_clock() - fs_prov_ms < FS_PROVISIONAL_MS)
    return 0;
  return fs_prov_take();
}

/* A 0x54 arrived. Nothing is taken yet: the body follows it with the real code
 * about a millisecond later, and fs_prov_tick() covers the case where it does
 * not. Returns 1 when the screen needs redrawing. */
static int fs_prov_step(void) {
  uint32_t now = get_ms_clock();

  /* Two 0x54 in a row with nothing between them are one physical press. */
  if (fs_prov_active && now - fs_prov_add_ms < 60)
    return 0;
  fs_prov_active = 1;
  fs_prov_ms = fs_prov_add_ms = now;
  return 0;
}

/* Choosing a stock on the shelf writes the film into the camera: the style
 * body, the sliders, the WB balance -- 16 KB of property traffic per step.
 * Doing that on every detent is what the card's log shows (capset x2, wb,
 * film body, prop_deliver 16704 bytes, per detent), and it is why a fast
 * dial leaves the screen unable to keep up. The screen still shows the new
 * stock at once; the camera gets it when the dial settles. */
static int fs_apply_wait;
static volatile uint32_t fs_apply_due_ms;
#define FS_APPLY_SETTLE_MS 350

static volatile int fs_dev_request = 0; /* SET in the darkroom */
static volatile int fs_view_request = FS_VIEW_NONE;
static volatile int fs_ui_running = 0;

/* Shown on screen: proof that keypress callbacks reach the module at all while
 * our task owns the display. No log file needed to read it. */
static volatile unsigned fs_last_key = 0;
static volatile unsigned fs_key_count = 0;
/* A develop left running by a crash or a pull-and-reseat must not lock the
 * screen out for ever: if the flag has been up longer than any develop can
 * last, clear it instead of refusing every later keypress. */
static uint32_t fs_develop_since = 0;
#define FS_DEVELOP_TIMEOUT_MS 600000u

static volatile int fs_last_act = -1;
static volatile int fs_last_raw = -1;

static volatile unsigned fs_anim_tick = 0;
static uint32_t fs_anim_ms = 0;
static uint32_t fs_sheen_ms = 0;

static void fs_ui_launch(int view);
/* A press that arrives while something else owns the panel is queued, not
 * dropped: "nothing happened" is the worst answer a key can give. */
static volatile int fs_pending_view = FS_VIEW_NONE;
static int fs_develop_stock_index(int count);

/* Was 32. Every frame on the card is a CR2 + JPG pair and the dedupe keeps one
 * of each pair, so 32 directory entries left 16 usable names -- all of them
 * from the oldest end of the directory, because FIO lists in storage order. The
 * newest frames were never candidates at all: the auto batch could not be told
 * to develop IMG_3219 because IMG_3219 was not in the list.
 * 256 entries is 16 KB of .bss and covers a full card of 8.3 names. */
/* MAX_DCIM_PHOTOS, dcim_photo_list and the two counters are declared with the
 * key state above. */

/* Prefix match without strncasecmp: ML does not export it to modules, and an
 * unresolved import makes the whole module fail to load ("module load error"),
 * silently and with no log. */
static int fs_starts_with_ci(const char *s, const char *prefix) {
  while (*prefix) {
    char a = *s++;
    char b = *prefix++;
    if (a >= 'a' && a <= 'z')
      a = (char)(a - 32);
    if (b >= 'a' && b <= 'z')
      b = (char)(b - 32);
    if (a != b)
      return 0;
  }
  return 1;
}

static int fs_add_photo(const char *name) {
  if (dcim_photo_count < MAX_DCIM_PHOTOS) {
    snprintf(dcim_photo_list[dcim_photo_count++], 64, "%s", name);
    return 1;
  }
  /* Full: drop the oldest so the newest frame is never the one that falls off
   * the end. A 136-frame card is 272 directory entries, which used to fill the
   * list before the CR2/JPG dedupe ever ran -- the newest frames were simply
   * missing from the menu and could not be developed at all. */
  for (int k = 0; k < MAX_DCIM_PHOTOS - 1; k++)
    memcpy(dcim_photo_list[k], dcim_photo_list[k + 1], 64);
  snprintf(dcim_photo_list[MAX_DCIM_PHOTOS - 1], 64, "%s", name);
  return 1;
}

/* Frames on the card, raws only: a develop needs a .CR2, and listing the
 * companion .JPG doubled the list for nothing. If a card has no raws at all
 * (small JPEG mode), the .JPG names are listed so the menu is not empty --
 * developing one says "no raw" instead of pretending. */
static void fs_scan_dcim_photos(void) {
  dcim_photo_count = 0;
  dcim_photo_index = 0;
  const char *dcim = get_dcim_dir();
  if (!dcim)
    return;

  struct fio_file *file = alloc_fio_file();
  if (!file)
    return;

  for (int pass = 0; pass < 2 && dcim_photo_count == 0; pass++) {
    const char *want = pass ? ".JPG" : ".CR2";
    struct fio_dirent *dirent = FIO_FindFirstEx(dcim, file);
    if (IS_ERROR(dirent))
      break;
    do {
      struct file_info file_info = convert_fio_file_info(file);
      if (file_info.mode & ATTR_DIRECTORY)
        continue;
      /* Skip our own results and half-written temporaries */
      if (fs_starts_with_ci(file_info.name, "FLM") ||
          fs_starts_with_ci(file_info.name, "FILM") ||
          fs_ends_with(file_info.name, ".FLM.JPG") ||
          fs_ends_with(file_info.name, ".TMP"))
        continue;
      int len = strlen(file_info.name);
      if (len > 4 && !strcasecmp(file_info.name + len - 4, want))
        fs_add_photo(file_info.name);
    } while (FIO_FindNextEx(dirent, file) == 0);
    FIO_FindClose(dirent);
  }
  free(file);

  /* Default to latest photo */
  if (dcim_photo_count > 0)
    dcim_photo_index = dcim_photo_count - 1;
}

static const char *fs_stock_alias(const film_xml_t *f);

/* A recipe this module wrote itself, rather than one that shipped with it. */
/* First line of a recipe this module generated, as opposed to one written by
 * hand -- a hand-written recipe starts with its own comment. Two call sites
 * used to compare 25 and 26 bytes of this string against each other. */
#define FS_RECIPE_MARKER "# Soup Engine Stock Recipe"


/* The shelf screen. All drawing lives in gui/fsg.c so the same pixels can be
 * rendered on a desktop; this only feeds it the current state. */
static fs_spring_t fs_anim;   /* strip position and velocity, see gui/fs_spring.h */
static int fs_anim_frames = 0;  /* frames drawn in the current glide */
static uint32_t fs_anim_start = 0;

/* ---- the film's own colours --------------------------------------------
 *
 * The palette itself is computed by the drawer (gui/fsg.c, fsg_stock_palette)
 * so the desktop preview and the camera derive it from the same code. This is
 * only the handover: the fields of the stock's XML that decide its look.
 */
static void fs_stock_palette(const film_xml_t *f, int out[FSG_NSLOT]) {
  fsg_film_t d;
  memset(&d, 0, sizeof d);
  d.brand = f->brand;
  d.iso = f->iso;
  d.kelvin = f->kelvin;
  d.wbs_gm = f->wbs_gm;
  d.saturation = f->saturation;
  d.mono = f->mono;
  fsg_stock_palette(&d, out);
}

/* Short message shown on the shelf after SET, so "it did something" is visible
 * without leaving the screen. */
static char fs_toast[40];
static volatile uint32_t fs_toast_until;

/* One short line for the footer: what LiveView is doing, or what stops it. */

/*
 * Hand the panel back and open Canon's playback.
 *
 * The point of developing is to look at the result, and doing that used to take
 * MENU, PLAY and two more presses. PLAY on the shelf, on the darkroom and at
 * the end of a manual develop now lands there directly. Never from the auto
 * watcher: that runs straight after a shot and stealing the screen would make
 * the camera feel possessed.
 */
static volatile int fs_want_playback;

/* ---------------------------------------------------------------- viewer
 *
 * Show the developed frame, in the module.
 *
 * Canon's playback will not list a file this module writes. The proof is on the
 * card: IMG_3280.JPG is a byte-for-byte copy of the module's output made from a
 * Mac, and it lists and plays, while the module's own file never appears. The
 * JPEG is not the problem -- IMG_9902.JPG is the module's image with its Exif
 * stripped entirely and it plays too. Whatever the body caches about a
 * directory it built during the develop, a module cannot reach into it.
 *
 * So the picture is drawn here. The decoder that reads the CR2 preview already
 * streams RGB rows; this decodes the finished JPEG the same way and writes it
 * into the panel's YUV buffer -- the buffer the firmware itself displays in
 * play mode, which is how MLV playback shows frames -- and the OSD is drawn on
 * top with the BMP layer.
 */

static void fs_screen_take(void);
static void fs_screen_release(void);
static MENU_SELECT_FUNC(fs_creator_trigger);
static MENU_SELECT_FUNC(fs_wb_trigger);
static void fs_push_wb_shift(void);

#define FS_VIEW_MAXW 720
#define FS_VIEW_MAXH 480
#define FS_VIEW_TOPBAR 24
#define FS_VIEW_BOTBAR 0 /* the hint sits over the picture, not in a band */

static char fs_last_dev[FS_PATH_LEN]; /* the last file Develop committed */

static volatile int fs_view_active;
static volatile int fs_view_exit;
static char fs_view_path[FS_PATH_LEN];
static char fs_view_note[96];

static unsigned char *fs_view_vram;
static int fs_view_pitch, fs_view_sw, fs_view_sh;
static int fs_view_step = 1; /* decoded pixels per screen pixel */
static int fs_view_pw, fs_view_ph, fs_view_x0, fs_view_y0;
static int fs_view_row;
static uint32_t fs_view_sum;

/* One output pixel: average `step` decoded columns, then RGB -> YUV (BT.601,
 * chroma offset by 128).
 *
 * That offset is NOT what the sensor-path buffers use: the LiveView plane and
 * the still YUV frame carry SIGNED chroma, 0 neutral (see fs_fill_black_yuv and
 * fs_yuvlook_apply). These are different buffers with different writers -- this
 * one is the play-mode YUV422 VRAM, which Canon fills from its JPEG decoder,
 * whose data is JPEG-conformant and therefore offset -- and no dump of it has
 * been taken. So the two conventions are both in use here on purpose.
 *
 * If the view screen ever shows magenta and cyan instead of the picture, the
 * +128 below is why: that would mean this VRAM is signed too.
 */
static void fs_view_px(const unsigned char *src, int width, int ox, int *yy,
                       int *uu, int *vv) {
  int c0 = ox * fs_view_step;
  int c1 = c0 + fs_view_step - 1;
  int r, g, b;
  if (c1 >= width)
    c1 = width - 1;
  if (c0 >= width)
    c0 = width - 1;
  r = src[c0 * 3 + 0];
  g = src[c0 * 3 + 1];
  b = src[c0 * 3 + 2];
  if (c1 != c0) {
    r = (r + src[c1 * 3 + 0] + 1) >> 1;
    g = (g + src[c1 * 3 + 1] + 1) >> 1;
    b = (b + src[c1 * 3 + 2] + 1) >> 1;
  }
  *yy = (77 * r + 150 * g + 29 * b) >> 8;
  *uu = ((-43 * r - 85 * g + 128 * b) >> 8) + 128;
  *vv = ((128 * r - 107 * g - 21 * b) >> 8) + 128;
}

/* Rows arrive as the decoder produces them, top to bottom. */
static int fs_view_sink(void *ctx, const unsigned char *rgb, int nrows,
                        int width) {
  (void)ctx;
  for (int r = 0; r < nrows; r++) {
    int gy = fs_view_row++;
    int oy;
    const unsigned char *src;
    unsigned char *dst;
    if (gy % fs_view_step)
      continue; /* vertical decimation */
    oy = fs_view_y0 + gy / fs_view_step;
    if (oy < 0 || oy >= fs_view_sh)
      continue;
    if (oy - fs_view_y0 >= fs_view_ph)
      continue;
    src = rgb + (size_t)r * (size_t)width * 3;
    dst = fs_view_vram + (size_t)oy * (size_t)fs_view_pitch +
          (size_t)fs_view_x0 * 2;
    for (int ox = 0; ox + 1 < fs_view_pw; ox += 2) {
      int y0, u0, v0, y1, u1, v1;
      fs_view_px(src, width, ox, &y0, &u0, &v0);
      fs_view_px(src, width, ox + 1, &y1, &u1, &v1);
      dst[0] = (unsigned char)((u0 + u1) >> 1); /* UYVY */
      dst[1] = (unsigned char)y0;
      dst[2] = (unsigned char)((v0 + v1) >> 1);
      dst[3] = (unsigned char)y1;
      dst += 4;
    }
    if (fs_view_pw & 1) { /* odd width: last pixel on its own */
      int ox = fs_view_pw - 1;
      int y0, u0, v0;
      fs_view_px(src, width, ox, &y0, &u0, &v0);
      if (fs_view_x0 + ox < fs_view_sw) {
        dst[0] = (unsigned char)u0;
        dst[1] = (unsigned char)y0;
      }
    }
  }
  return 0;
}

static void fs_view_clear(void) {
  for (int y = 0; y < fs_view_sh; y++) {
    unsigned int *p =
        (unsigned int *)(fs_view_vram + (size_t)y * (size_t)fs_view_pitch);
    for (int i = 0; i < (fs_view_pitch >> 2); i++)
      p[i] = 0x10801080u; /* U=128, Y=16 (black), V=128 (fs_view_px writes offset chroma) */
  }
}

static uint32_t fs_view_checksum(void) {
  uint32_t sum = 0;
  for (int y = fs_view_y0; y < fs_view_y0 + fs_view_ph && y < fs_view_sh;
       y += 37) {
    const unsigned char *p = fs_view_vram + (size_t)y * (size_t)fs_view_pitch +
                             (size_t)fs_view_x0 * 2;
    for (int i = 0; i < 64 && i < fs_view_pw * 2; i++)
      sum = sum * 31u + p[i];
  }
  return sum;
}

static void fs_view_osd(void) {
  /*
   * Clear the middle first, and with COLOR_EMPTY rather than BLACK.
   *
   * The overlay is not reset when this screen opens: when the develop hands the
   * panel over (fs_view_run(out, 1)) the bath screen's pixels are still in the
   * bitmap buffer, and they sat opaque over the middle of the picture. Index 0
   * is the display's own "transparent", which is what lets the YUV frame show.
   */
  BMP_LOCK(bmp_fill(COLOR_EMPTY, 0, FS_VIEW_TOPBAR, 720,
                    FS_VIEW_MAXH - 20 - FS_VIEW_TOPBAR);
           bmp_fill(COLOR_BLACK, 0, 0, 720, FS_VIEW_TOPBAR);
           bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_BLACK), 10, 4, "%s",
                      fs_view_note[0] ? fs_view_note : fs_view_path);
           bmp_fill(COLOR_BLACK, 0, FS_VIEW_MAXH - 20, 720, 20);
           bmp_printf(FONT(FONT_SMALL, COLOR_GRAY(60), COLOR_BLACK), 10,
                      FS_VIEW_MAXH - 16, "MENU / PLAY = back     %dx%d",
                      fs_view_pw, fs_view_ph);)
  ml_refresh_display_needed = 1;
}

/* Returns 1 if a picture was drawn. */
static int fs_view_decode(void) {
  se_err e;
  int dw = 0, dh = 0;
  int scale = 1;

  fs_view_clear();
  fs_view_row = 0;
  e = se_cam_view_info(fs_view_path, 1, &dw, &dh);
  if (e != SE_OK) {
    fs_log("view: %s is unreadable (%s)", fs_view_path, se_strerror(e));
    fs_log_flush();
    return 0;
  }
  fs_view_step = (dw > FS_VIEW_MAXW || dh > FS_VIEW_MAXH - 50) ? 2 : 1;
  if (fs_view_step == 2 && (dw / 2) > FS_VIEW_MAXW)
    scale = 2;
  if (se_cam_view_info(fs_view_path, scale, &dw, &dh) != SE_OK)
    return 0;

  fs_view_pw = dw / fs_view_step;
  fs_view_ph = dh / fs_view_step;
  if (fs_view_pw > FS_VIEW_MAXW)
    fs_view_pw = FS_VIEW_MAXW;
  {
    int room = FS_VIEW_MAXH - FS_VIEW_TOPBAR - FS_VIEW_BOTBAR;
    if (fs_view_ph > room)
      fs_view_ph = room;
  }
  fs_view_x0 = (FS_VIEW_MAXW - fs_view_pw) / 2;
  fs_view_y0 =
      FS_VIEW_TOPBAR +
      (FS_VIEW_MAXH - FS_VIEW_TOPBAR - FS_VIEW_BOTBAR - fs_view_ph) / 2;

  fs_log("view: %s %dx%d step %d -> %dx%d at %d,%d", fs_view_path, dw, dh,
         fs_view_step, fs_view_pw, fs_view_ph, fs_view_x0, fs_view_y0);
  fs_log_flush();

  e = se_cam_view(fs_view_path, scale, fs_view_sink, NULL, &dw, &dh);
  if (e != SE_OK) {
    fs_log("view: decode failed (%s)", se_strerror(e));
    fs_log_flush();
    return 0;
  }
  fs_view_sum = fs_view_checksum();
  return 1;
}

static void fs_view_run(const char *path, int screen_taken) {
  uint32_t t;
  uint32_t last_redraw = 0;

  /*
   * Every way out of here has to give the panel back. When the develop hands
   * the screen over (screen_taken) the camera is in play mode with the front
   * buffer taken and the shutter locked; returning early without releasing
   * would leave it like that until the battery came out.
   */
  if (!path || !path[0]) {
    if (screen_taken)
      fs_screen_release();
    return;
  }
  snprintf(fs_view_path, sizeof fs_view_path, "%s", path);
  {
    const char *leaf = strrchr(path, '/');
    int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
    const film_xml_t *f =
        &film_active_table[fs_clamp_i(film_sim_stock, 0, count - 1)];
    snprintf(fs_view_note, sizeof fs_view_note, "%s   -   %s",
             leaf ? leaf + 1 : path, f->name);
  }

  struct vram_info *vr = get_yuv422_vram();
  if (!vr || !vr->vram || vr->pitch < 2) {
    fs_log("view: no YUV buffer, cannot show the picture");
    fs_log_flush();
    if (screen_taken)
      fs_screen_release();
    return;
  }
  /* Uncached alias: the display controller reads this buffer itself and would
   * never see writes that are still sitting in the CPU cache. */
  fs_view_vram = (unsigned char *)UNCACHEABLE(vr->vram);
  fs_view_pitch = vr->pitch;
  fs_view_sh = vr->height > 0 ? vr->height : FS_VIEW_MAXH;
  fs_view_sw = fs_view_pitch / 2;

  if (!screen_taken)
    fs_screen_take();
  fs_view_active = 1;
  fs_view_exit = 0;
  fs_view_decode();
  fs_view_osd();

  t = get_ms_clock();
  while (!fs_view_exit) {
    msleep(100);
    if (get_ms_clock() - t > 250) {
      t = get_ms_clock();
      /* Canon repaints the panel on its own schedule; if it has taken the
       * buffer back, put the picture there again rather than leave a blank.
       * At most once a second: a decode is a second of CPU by itself, and if
       * the buffer keeps being taken there is nothing to gain by fighting it
       * faster than that. */
      if (fs_view_checksum() != fs_view_sum &&
          (!last_redraw || get_ms_clock() - last_redraw > 1000)) {
        last_redraw = get_ms_clock();
        if (fs_view_decode())
          fs_view_osd();
        else
          break;
      }
    }
  }
  fs_view_active = 0;
  /*
   * Hand the picture back black, not as it was left.
   *
   * This VRAM is Canon's own: get_yuv422_vram() is the buffer play mode shows,
   * and Canon's playback decodes into the same memory. It only writes the
   * picture it draws, so anything of ours left around it stays on the panel --
   * which is how the last developed photo kept appearing inside Canon's
   * playback afterwards, and why the bars of a 65:24 file showed a ghost of the
   * frame before. fs_view_decode() already blacks it before painting; nothing
   * did so on the way out.
   */
  fs_view_clear();
  fs_screen_release();
  fs_log("view: closed");
  fs_log_flush();
}

static void fs_open_canon_playback(void) {
  if (!fs_want_playback)
    return;
  fs_want_playback = 0;
  /* BGMT_PLAY is 0xb on the 6D (platform/6D.116/gui.h). */
  fs_log("playback: opening Canon playback for the developed frame");
  fs_log_flush();
  msleep(250);
  fake_simple_button(0xb);
}

static const char *fs_lv_status(void) {
  static char buf[26];
  if (fs_stat_frames > 0)
    snprintf(buf, sizeof buf, "LV %d fr", (int)fs_stat_frames);
  else if (!get_global_draw_setting())
    snprintf(buf, sizeof buf, "GlobalDraw off");
  else if (lv && lv_disp_mode)
    snprintf(buf, sizeof buf, "LV: press INFO");
  else if (!lv)
    snprintf(buf, sizeof buf, "LV off");
  else
    snprintf(buf, sizeof buf, "LV waiting");
  return buf;
}

/*
 * Put the stocks that have a canister photograph first.
 *
 * FILMCAN.DAT covers fifteen emulsions; the card carries more (C200, Provia,
 * T-Max, Delta, Foma, XP2 ...). Rather than mix "a picture of the box" and "a
 * drawing of a cassette" through the whole list, the fifteen with artwork are
 * grouped at the front, in the file's own order, and everything else follows.
 * Stable: the relative order inside each group is what the loader produced.
 */
/*
 * Stocks with canister artwork first, the rest after, each keeping its own
 * order. Moving entries changes what every index means, so the current
 * selection has to be carried over -- otherwise loading the XML silently
 * switches the active film.
 */
static void fs_group_by_artwork(void) {
  int n = film_active_count;
  int ord[MAX_DYNAMIC_FILMS]; /* ord[new] = old */
  int dst[MAX_DYNAMIC_FILMS]; /* dst[old] = new */
  if (n < 2 || n > MAX_DYNAMIC_FILMS)
    return;

  int k = 0;
  for (int i = 0; i < n; i++)
    if (fs_can_index(film_active_table[i].name) >= 0)
      ord[k++] = i;
  for (int i = 0; i < n; i++)
    if (fs_can_index(film_active_table[i].name) < 0)
      ord[k++] = i;

  int moved = 0;
  for (int i = 0; i < n; i++) {
    dst[ord[i]] = i;
    if (ord[i] != i)
      moved = 1;
  }
  if (!moved)
    return;

  int sel = film_sim_stock;
  if (sel >= 0 && sel < n)
    film_sim_stock = dst[sel];

  /* Rotate each cycle in place; dst doubles as the visited marker. */
  for (int i = 0; i < n; i++) {
    if (dst[i] < 0 || dst[i] == i) {
      dst[i] = -1;
      continue;
    }
    film_xml_t t = film_active_table[i];
    int j = i;
    for (;;) {
      int nxt = dst[j];
      dst[j] = -1;
      if (nxt == i) {
        film_active_table[j] = t;
        break;
      }
      film_active_table[j] = film_active_table[nxt];
      j = nxt;
    }
  }
}

static void fs_shelf_state(fsg_shelf_t *st) {
  int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
  int idx = fs_clamp_i(film_sim_stock, 0, count - 1);
  const film_xml_t *f = &film_active_table[fs_develop_stock_index(count)];

  memset(st, 0, sizeof *st);
  st->stock.name = f->name;
  st->stock.brand = f->brand;
  st->stock.kind = f->type;
  st->stock.desc = f->description;
  st->stock.iso = f->iso;
  st->stock.code = f->code;
  st->stock.kelvin = f->kelvin;
  st->stock.wbs_gm = f->wbs_gm;
  st->stock.saturation = f->saturation;
  fs_stock_palette(f, st->stock.palette);
  st->stock.soup = (film_sim_dev == FS_DEV_PUSH_1)   ? "Push +1 (+1 EV)"
                   : (film_sim_dev == FS_DEV_PUSH_2) ? "Push +2 (+2 EV)"
                   : (film_sim_dev == FS_DEV_PULL_1) ? "Pull -1 (-1 EV)"
                                                     : "Standard C-41";
  st->grain = film_sim_grain;
  st->strength = film_sim_strength;
  {
    uint32_t now = get_ms_clock();
    if (fs_toast[0] && (int32_t)(fs_toast_until - now) > 0)
      st->badge = fs_toast;
    else
      st->badge = film_sim_level == FS_LEVEL_STYLE ? "LEVEL 1  -  PICTURE STYLE"
                  : film_sim_level == FS_LEVEL_RAW
                      ? "LEVEL 3  -  RAW DEVELOP"
                      : "LEVEL 2  -  STYLE + GRADE";
  }
  st->count = count;
  st->sel = idx;
  for (int i = 0; i < count && i < FSG_MAX_LABELS; i++) {
    st->labels[i] = film_active_table[i].name;
    st->art[i] = (signed char)fs_can_index(film_active_table[i].name);
  }
  {
    static char counter[16];
    { char a[4], b[4];
      fs_pad_num(a, sizeof a, idx + 1, 2);
      fs_pad_num(b, sizeof b, count, 2);
      snprintf(counter, sizeof counter, "%s / %s", a, b); }
    st->counter = counter;
  }
  st->scroll = fs_anim.x >> 8; /* Q16 frames -> 1/256 frame units */
  st->tick = get_ms_clock();
  st->hint_r = fs_lv_status();
}

/* The shelf's picture is the manufacturer's own canister, read from the
 * FILMCAN.DAT that ships with the FilmRoll module. That file carries no names,
 * so the order of its fifteen records is fixed and a film finds its artwork by
 * name -- the only label the two lists share. Stocks without artwork (C200,
 * Provia, the T-Max/Delia/Foma lines) fall back to the drawn cassette.
 *
 * Indices are what the file actually contains, checked by reading each record
 * back as an image, so this table is data about the file and not a guess. */
static const struct {
  const char *name;
  int index;
} fs_can_map[] = {
    /* FILMCAN.DAT's own fifteen, in its own order */
    {"Portra 400", 0},
    {"Portra 160", 1},
    {"Portra 800", 2},
    {"Ektar 100", 3},
    {"Gold 200", 4},
    {"ColorPlus 200", 5},
    {"UltraMax 400", 6},
    {"Superia 400", 7},
    {"Pro 400H", 8},
    {"CineStill 800T", 9},
    {"Velvia 50", 10},
    {"Ektachrome E100", 11},
    {"Kodachrome 64", 12},
    {"Tri-X 400", 13},
    {"HP5 Plus", 14},
    /* drawn by tools/make_can_art.py, appended in tools/merge_cans.py's ORDER
     */
    {"CineStill 50D", 15},
    {"Color 800", 16},
    {"C200", 17},
    {"Provia 100F", 18},
    {"T-Max 100", 19},
    {"T-Max P3200", 20},
    {"Delta 100", 21},
    {"Pan F Plus 50", 22},
    {"Acros 100", 23},
    {"Fomapan 100", 24},
    {"XP2 Super", 25},
};

static int fs_can_index(const char *name) {
  if (!name)
    return -1;
  for (int i = 0; i < (int)(sizeof fs_can_map / sizeof fs_can_map[0]); i++)
    if (!strcasecmp(name, fs_can_map[i].name))
      return fs_can_map[i].index;
  return -1;
}

static char fs_pic_code[16];
static volatile int fs_last_pic_idx = -1;

static int fs_can_open_tried;

static void fs_picture_sync(void) {
  int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
  const film_xml_t *f = &film_active_table[fs_develop_stock_index(count)];
  int idx;

  if (!f->code[0])
    return;
  if (!strcmp(fs_pic_code, f->code))
    return;
  snprintf(fs_pic_code, sizeof fs_pic_code, "%s", f->code);

  if (!fs_can_open_tried) {
    fs_can_open_tried = 1;
    if (!film_sim_art) {
      fs_log("picture: artwork disabled (film.sim.art=0), drawing cassettes");
      fs_log_flush();
      fsg_can_hide();
      return;
    }
    /* Our merged file first: it covers every stock. The manufacturer file is
     * the fallback, so a card without CANS.DAT behaves as it always did. */
    int ok = fsg_can_load(FSG_CAN_PATH);
    if (!ok)
      ok = fsg_can_load(FSG_CAN_PATH2);
    if (!ok) {
      fs_log("picture: no artwork file (%s or %s), drawing cassettes",
             FSG_CAN_PATH, FSG_CAN_PATH2);
    } else if (!fsg_can_thumbs()) {
      /* names in the strip instead of canisters; the shelf still works */
      fs_log("picture: no thumbnails (%d records), strip keeps names",
             fsg_can_count());
    } else {
      fs_log("picture: %d canisters from %s", fsg_can_count(), fsg_can_using());
    }
  }

  idx = fs_can_index(f->name);
  fs_last_pic_idx = idx;
  if (idx < 0 || !fsg_can_pick(idx))
    fsg_can_hide(); /* no artwork for this stock: draw the cassette */
}

/* Re-program the borrowed palette entries for whatever the picture is: the
 * canister's own palette when one is loaded, else the fifteen stock colours the
 * drawn cassette needs. Every frame, because bmp_on() restores all 256 entries
 * from ML's shadows and would otherwise wipe them. */
/* the cache above has to be reachable from fs_screen_release() */
static int fs_pal_last_code;
static uint32_t fs_pal_last_ms;

static void fs_picture_palette(const int *slots) {
  /*
   * Two different jobs, with two different costs.
   *
   * A canister photograph carries up to 175 colours, which is 340 ENGIO writes,
   * so that one is cached per stock and refreshed twice a second in case Canon
   * has put its own palette back.
   *
   * The drawn cassette is fifteen colours -- 30 writes -- and bmp_on() restores
   * all 256 entries from ML's shadows on every paint, exactly as the comment
   * above this function warned. Caching that for 250 ms is what left the shelf
   * showing the *previous* film's cassette colours after a change: the drawing
   * had already happened, the palette had not. Fifteen entries are cheap enough
   * to write every time, which is what the note says was the original design.
   */
  if (fsg_can_ready()) {
    int code = fs_last_pic_idx;
    if (code == fs_pal_last_code && get_ms_clock() - fs_pal_last_ms < 250)
      return;
    fs_pal_last_code = code;
    fs_pal_last_ms = get_ms_clock();
    fsg_can_reprogram();
    return;
  }
  fsg_pal(slots, FSG_NSLOT);
  /* One line per stock change: the cassette's own colours, so a wrong picture
   * can be told apart from a wrong palette without another round trip. */
  {
    static int told_idx = -2;
    if (told_idx != fs_last_pic_idx) {
      told_idx = fs_last_pic_idx;
      fs_log("picture: cassette palette for idx %d: paper %06x border %06x "
             "shadow %06x mid %06x tongue %06x", fs_last_pic_idx,
             (unsigned)slots[FSG_SL_PAPER], (unsigned)slots[FSG_SL_BORDER],
             (unsigned)slots[FSG_SL_SHADOW], (unsigned)slots[FSG_SL_MID],
             (unsigned)slots[FSG_SL_TONGUE]);
      fs_log_flush();
    }
  }
}

/*
 * Is the drawing actually in the overlay buffer?
 *
 * "the screen shows nothing" while the task draws, takes keys and stays open
 * means one of two very different things: the pixels are not in BMP VRAM at all
 * (wrong buffer / wrong offset), or they are there and the panel is not showing
 * them (palette or compositor). This samples the real buffer where the drawing
 * was copied to and reports what it finds, once per screen, so the next log
 * answers it instead of another guess.
 */
static void fs_bmp_probe(const char *tag) {
  uint8_t *real = bmp_vram_real();
  uint8_t *idle = bmp_vram_idle();
  if (!real) {
    fs_log("%s: bmp_vram_real is NULL", tag);
    return;
  }
  int nz = 0, n = 0;
  uint32_t sum = 0;
  for (int i = 0; i < 720 * 480; i += 1009) {
    uint8_t v = real[i];
    n++;
    if (v)
      nz++;
    sum = sum * 31u + v;
  }
  /*
   * Sample the idle buffer too. With film.sim.direct = 0 the frame is drawn
   * there and then copied to real, so "drawn but not shown" and "never drawn"
   * look identical in the real buffer alone -- and telling those apart is the
   * whole point of this probe.
   */
  int nzi = 0;
  if (idle)
    for (int i = 0; i < 720 * 480; i += 1009)
      if (idle[i])
        nzi++;
  /* %p prints a bare "p" in this libc; the addresses are the whole point. */
  fs_log("%s: bmp real=0x%x idle=0x%x sampled=%d nonzero=%d nzidle=%d "
         "sum=0x%08x pal[9]=0x%08x pal[38]=0x%08x pal[80]=0x%08x pal[200]=0x%08x",
         tag, (unsigned)real, (unsigned)idle, n, nz, nzi, sum,
         LCD_Palette[3 * 9 + 2], LCD_Palette[3 * 38 + 2],
         LCD_Palette[3 * 80 + 2], LCD_Palette[3 * 200 + 2]);
  fs_log_flush();
}

/*
 * How the panel is painted.
 *
 * The idle-buffer dance (copy real->idle, draw, copy idle->real) is what ML's
 * own menu and lua_display do, and it is what this module has always done --
 * yet the shelf comes up blank while the task is demonstrably alive. So the
 * mode is now a setting: "direct" draws into the buffer the hardware is
 * scanning out, which cannot be lost to a bad copy, and costs a visible redraw
 * instead of a clean one.
 */

/* What the last second of painting cost, so the log can say it. */
static uint32_t fs_paint_ms = 0;
static int fs_paint_rows_drawn = 0;
static int fs_paint_frames = 0;
static uint32_t fs_paint_since = 0;

/* The part of the bitmap buffer that bmp_idle_copy(direction, 0) moves, and the
 * same numbers it uses: 720 bytes of each 960-byte row, 480 rows. */
#define FS_BMP_W 720
#define FS_BMP_H 480

/*
 * Copy rows y0..y1-1 between the live and the idle bitmap buffer.
 *
 * bmp_idle_copy(direction, 0) copies the whole screen -- 691,200 bytes -- and
 * the shelf called it twice for every animation frame in order to move a strip
 * that occupies 94 rows. This is that copy restricted to the rows that
 * changed; BMPPITCH (960 here) is the row stride of the real VRAM, and
 * bmp_vram_real() already points at the visible origin, which is why the row
 * length is FS_BMP_W and not the pitch.
 */

static void fs_blit_rows(int direction, int y0, int y1) {
  uint8_t *real = bmp_vram_real();
  uint8_t *idle = bmp_vram_idle();
  int w = os.x_max;
  int y;

  if (!real || !idle)
    return;
  if (w <= 0 || w > FS_BMP_W)
    w = FS_BMP_W;
  if (y0 < 0)
    y0 = 0;
  if (y1 > FS_BMP_H)
    y1 = FS_BMP_H;
  for (y = y0; y < y1; y++) {
    uint8_t *d = (direction ? real : idle) + (size_t)y * BMPPITCH;
    const uint8_t *s = (direction ? idle : real) + (size_t)y * BMPPITCH;
    memcpy(d, s, (size_t)w);
  }
}

/* rows = rows COPIED between the two buffers; in direct mode there are none,
 * and the log is evidence, so it must not claim copies that did not happen. */
static void fs_paint_account(uint32_t ms, int rows) {
  fs_paint_ms += ms;
  fs_paint_rows_drawn += rows;
  fs_paint_frames++;
}

static int fs_fb_reasserts;

/*
 * Called before every frame: take the buffer back if Canon has it, and make
 * sure the palette is still ours.
 *
 * Both can be undone under a long-lived screen by code this module does not
 * call: redraw()/_redraw_do() enables the front buffer and dialog_redraw()s,
 * and any bmp_off()/bmp_on() pair runs palette_disable(0), which copies every
 * palette shadow -- Canon's, not ours, unless fsg_pal_write() updated it --
 * back to the hardware. Re-asserting costs a few register writes.
 */
static void fs_frame_prep(void) {
  if (!canon_gui_front_buffer_disabled()) {
    canon_gui_disable_front_buffer();
    fs_fb_reasserts++;
  }
  fsg_chrome_init();
}

/* src/menu.c. Nonzero stops ML's menu drawing itself while a module screen
 * owns the panel; see the note further down for why the menu is left open. */
extern int menu_redraw_blocked;

/*
 * Cheap insurance for a screen that has stopped painting.
 *
 * Darkroom, Color Creator and WB Shift repaint only when something changes, so
 * if Canon or ML puts anything into the real buffer after the last frame -- an
 * asynchronous dialog, a menu pass that slipped through, a resize -- nothing
 * would restore it. Once every 250 ms the already-rendered idle buffer is
 * copied back onto the panel: 345 KB of memcpy, no rendering, no card I/O.
 *
 * If ML's menu closed underneath us (menu_redraw_task does exactly that when
 * menu_ensure_canon_dialog() fails) the flag is cleared and the front buffer
 * taken back, so the state the screens rely on is restored rather than assumed.
 */
static void fs_keep_panel(void) {
  static uint32_t last;
  uint32_t now = get_ms_clock();
  if ((uint32_t)(now - last) < 250)
    return;
  last = now;
  if (!gui_menu_shown()) {
    menu_redraw_blocked = 0;
    canon_gui_disable_front_buffer();
  }
  fsg_chrome_init();
  BMP_LOCK(bmp_idle_copy(1, 0);)
  ml_refresh_display_needed = 1;
}

static void fs_paint(void (*fn)(void)) {
  uint32_t t0 = get_ms_clock();
  fs_frame_prep();
  if (film_sim_direct) {
    BMP_LOCK(bmp_draw_to_idle(0); fn();)
  } else {
    BMP_LOCK(bmp_idle_copy(0, 0); bmp_draw_to_idle(1); fn();
             bmp_draw_to_idle(0); bmp_idle_copy(1, 0);)
  }
  ml_refresh_display_needed = 1;
  fs_paint_account(get_ms_clock() - t0, film_sim_direct ? 0 : FS_BMP_H);
}

/*
 * Paint one band: the same double-buffering as fs_paint(), over y0..y1-1 only.
 *
 * Only valid for a band that paints its own background -- fsg_draw_shelf_strip
 * and fsg_draw_shelf_header do, everything else in fsg.c assumes it owns the
 * whole screen.
 */
static void fs_paint_band(int y0, int y1, void (*fn)(void)) {
  uint32_t t0 = get_ms_clock();
  fs_frame_prep();
  if (film_sim_direct) {
    BMP_LOCK(bmp_draw_to_idle(0); fn();)
  } else {
    BMP_LOCK(fs_blit_rows(0, y0, y1); bmp_draw_to_idle(1); fn();
             bmp_draw_to_idle(0); fs_blit_rows(1, y0, y1);)
  }
  ml_refresh_display_needed = 1;
  fs_paint_account(get_ms_clock() - t0, film_sim_direct ? 0 : (y1 - y0));
}

/*
 * The footer carries the LiveView status, so if that line changed the whole
 * screen has to be redrawn -- a band repaint would leave the old text.
 *
 * What is compared is the BRANCH of fs_lv_status(), not its text. The text of
 * the active case is "LV %d fr" and that count moves with every filtered
 * frame, so comparing the string turned nearly every pass into a full-screen
 * repaint: the camera's log shows 118 `shelf: paint` lines and all but three
 * of them at 480 rows per frame, never the 94-row band this was built for.
 */
#define FS_HINT_KEY_TEXTS 5
static int fs_painted_hint = -1;
static int fs_strip_moving;   /* the strip is still gliding: bands only */

/* 0 = frames flowing, 1 = Global Draw off, 2 = press INFO, 3 = LV off,
 * 4 = waiting. Same order as the branches in fs_lv_status(). */
static int fs_lv_status_key(void) {
  if (fs_stat_frames > 0)
    return 0;
  if (!get_global_draw_setting())
    return 1;
  if (lv && lv_disp_mode)
    return 2;
  if (!lv)
    return 3;
  return 4;
}

static void fs_gui_render(void) {
  fsg_shelf_t st;
  fs_shelf_state(&st);
  fs_picture_palette(st.stock.palette);
  fsg_draw_shelf(&st);
  fs_painted_hint = fs_lv_status_key();
}

static void fs_gui_render_strip(void) {
  fsg_shelf_t st;
  fs_shelf_state(&st);
  fs_picture_palette(st.stock.palette);
  fsg_draw_shelf_strip(&st);
}

static void fs_gui_render_header(void) {
  fsg_shelf_t st;
  fs_shelf_state(&st);
  fsg_draw_shelf_header(&st);
}

/*
 * Move the strip towards the selected stock; returns 1 while it is moving.
 *
 * A spring, not an ease. `step = d/3` had no velocity, so the first tick of a
 * move was already a third of it -- at three stocks away that is a whole shelf
 * width in one frame, then a crawl. Measured against the old code at 30 ms:
 *
 *   1 shelf   510 ms, biggest step 17%      was 1050 ms, 33%
 *   3 shelves 690 ms, biggest step 17%      was 1260 ms, 33% (and 100% of the
 *                                                    first frame's travel)
 *
 * The integrator and its constants live in gui/fs_spring.h so that
 * se/hosttest/springtest can run this exact code.
 */
static int fs_shelf_animate(int dt_ms) {
  int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
  int idx = fs_clamp_i(film_sim_stock, 0, count - 1);
  int moving = fs_spring_step(&fs_anim, idx * 65536, dt_ms);

  /* One line per glide, so a camera run says how long the strip actually took
   * and how many frames it drew, instead of it having to be believed. */
  if (moving) {
    if (!fs_anim_frames)
      fs_anim_start = get_ms_clock();
    fs_anim_frames++;
    return 1;
  }
  if (fs_anim_frames) {
    /* The step that settled snapped the strip onto the stock; paint that
     * position rather than leaving the last moving frame on the panel. */
    fs_log("shelf: glide %d frames, %d ms, to stock %d",
           fs_anim_frames, (int)(get_ms_clock() - fs_anim_start), idx);
    fs_anim_frames = 0;
    return 1;
  }
  return 0;
}

/*
 * The shelf animates: a perforated film strip runs along the bottom of the
 * screen, and the exposed "frame" window lights up in the current stock's
 * colour as it passes. It is drawn with fills only -- no sprites, no palette
 * poking -- so it costs a few hundred microseconds and cannot disturb the
 * composition above it.
 */

static void fs_gui_close(void) { fs_gui_active = 0; }

/* Take the panel away from Canon's GUI, then give it back. Every full-screen
 * step does exactly one take and one release, so the front buffer
 * enable/disable and the powersave inhibit always balance out.
 *
 * gui_uilock() is used, but only UILOCK_SHUTTER. UILOCK_EVERYTHING is an
 * ICU-level lock: the firmware stops processing buttons, module keypress
 * callbacks are dispatched from Canon's GUI event task, and the whole screen
 * would stop answering. The reference module locks the shutter alone while its
 * shelf is open ("lock: %x -> %x" around gui_uilock(1) in filmroll.mo), which
 * is all that is wanted here: no accidental exposure while the shelf owns the
 * panel, arrows and SET untouched. */

/*
 * Piggyback ML's own menu, the way every full-screen module does.
 *
 * arkanoid, lua_menu, ime_std and flexinfo all leave gui_menu_shown() at 1 and
 * set menu_redraw_blocked = 1 while they own the panel. That combination is
 * what keeps ML's own machinery off the canvas:
 *
 *   _redraw_do() (zebra.c) starts with
 *       if (gui_menu_shown()) { menu_redraw(); return; }
 *   With the menu closed -- the state gui_stop_menu() put the body in -- every
 *   redraw() issued anywhere in ML falls through to
 *       canon_gui_enable_front_buffer(0); dialog_redraw(dialog);
 *   which is Canon's dialog painted over a module screen. That is the wipe this
 *   module has been chasing: not Canon repainting on a timer, but ML painting
 *   Canon's dialog over us on request.
 *
 *   With the menu open and its redraws blocked, menu_redraw_task() also keeps
 *   running menu_ensure_canon_dialog(), which re-pokes
 *   SetGUIRequestMode(GUIMODE_ML_MENU) every 2000 ms because
 *   CONFIG_MENU_TIMEOUT_FIX says the 6D MPU otherwise turns the dialog off.
 *   That is the keepalive this module was hand-rolling -- already correct, and
 *   already gated on lv, unlike the version here.
 *
 * Nothing here calls SetGUIRequestMode() or PauseLiveView() any more. The menu
 * settled the GUI-mode question in piggyback_canon_menu() when it opened, and
 * PauseLiveView() sets lv = 0 (PROP_LV_ACTION, propvalues.c) while also setting
 * lv_paused, after which gui-common.c drops every key event before
 * handle_module_keys() -- this module's own key callback -- is reached.
 */
static int fs_menu_opened;    /* the menu was opened here; close it on release */
static int fs_menu_was_open;  /* it was already open when the screen took over */
/* The one thing the key callback asks: is the panel this module's right now?
 * Enumerating the per-screen flags there was wrong -- the develop path calls
 * fs_screen_release() while fs_develop_active and fs_bath_ui are still set, so
 * the redraw that puts ML's menu back was swallowed. */
static volatile int fs_holding_panel;

#define FS_KEY_GUARD_MS 400
static volatile uint32_t fs_key_guard_until;


static void fs_screen_take(void) {
  fs_holding_panel = 1;
  powersave_prohibit();
  display_on();
  msleep(150);
  /*
   * Keep ML's menu open and block its redraws, instead of closing it. See the
   * note above menu_redraw_blocked: with the menu closed every redraw() in ML
   * paints Canon's dialog over this module's canvas. Opened without the menu
   * (a shortcut), the menu is opened so the same state holds.
   */
  fs_menu_was_open = gui_menu_shown() ? 1 : 0;
  fs_menu_opened = 0;
  if (!fs_menu_was_open) {
    int waited;
    gui_open_menu();
    for (waited = 0; waited < 400 && !gui_menu_shown(); waited += 20)
      msleep(20);
    fs_menu_opened = gui_menu_shown() ? 1 : 0;
  }
  menu_redraw_blocked = 1;
  canon_gui_disable_front_buffer();
  fs_log("screen: menu shown=%d (was %d) blocked=%d fb_disabled=%d mode=%d lv=%d",
         gui_menu_shown(), fs_menu_was_open, menu_redraw_blocked,
         canon_gui_front_buffer_disabled(), get_gui_mode(), lv ? 1 : 0);
  fs_log_flush();
  /*
   * Program the chrome entries opaque before anything is drawn with them, and
   * log the readback of the REGISTER (shamem_read) next to Canon's RAM shadow:
   * EngDrvOut() writes the hardware and never updates LCD_Palette[3*e+2], so
   * the shadow alone reports Canon's 0x3f no matter what was just written.
   */
  fsg_chrome_init();
  {
    uint32_t got = shamem_read((uint32_t)LCD_Palette[3 * fsg_chrome_entry(4)]);
    fs_log("screen: chrome entry %d: hw=0x%08x shadow=0x%08x (wrote 0xff......)",
           fsg_chrome_entry(4), (unsigned)got,
           (unsigned)LCD_Palette[3 * fsg_chrome_entry(4) + 2]);
    fs_log_flush();
  }
  gui_uilock(UILOCK_SHUTTER); /* the shutter cannot fire under this screen */
  bmp_on();
  msleep(30);
  clrscr();
  /*
   * Palette calibration strip, when Debug ▸ Calibration is on: eleven equal
   * bars, one per palette index the drawer uses, so "how many bars and where do
   * they stop" answers "which entries can this panel show" without a guess.
   */
  if (film_sim_calib) {
    static const unsigned char idx[11] = {1,  8,  15,  38,  45, 60,
                                          79, 80, 120, 200, 254};
    int bw = 720 / 11;
    BMP_LOCK(bmp_draw_to_idle(0); bmp_fill(COLOR_BLUE, 0, 0, 720, 480);
             for (int i = 0; i < 11; i++) bmp_fill(idx[i], i * bw, 0, bw, 480);)
    ml_refresh_display_needed = 1;
    msleep(2500);
  }

  fs_key_guard_until = get_ms_clock() + FS_KEY_GUARD_MS;
}

static void fs_screen_release(void) {
  /* Cleared first: everything below, including the redraw() that hands the
   * menu back, must not be swallowed by the key callback. */
  fs_holding_panel = 0;
  fsg_pal_reset();  /* hand the borrowed palette entries back */
  /* and invalidate the cache that decides when to program them again: it was
   * comparing against the stock code only, so coming back to the same stock
   * within 250 ms of leaving a screen skipped the reprogram and drew one or
   * two frames with Canon's palette instead of the film's. */
  fs_pal_last_code = -1;
  fs_pal_last_ms = 0;
  fsg_can_unload(); /* and let go of the artwork file */
  fsg_chrome_reset(); /* chrome entries back to Canon's values */
  gui_uilock(UILOCK_NONE); /* keys belong to Canon again */
  /*
   * Hand the menu back before the panel. If this screen opened the menu, close
   * it; if the menu was already there, let it draw itself again now that
   * menu_redraw_blocked is clear.
   */
  menu_redraw_blocked = 0;
  if (fs_menu_opened && !fs_menu_was_open) {
    fs_menu_opened = 0;
    gui_stop_menu();
    msleep(120);
  } else {
    redraw();
  }
  fs_log("screen: released (menu=%d blocked=%d)",
         gui_menu_shown(), menu_redraw_blocked);
  fs_log_flush();
  canon_gui_enable_front_buffer(1); /* give Canon its panel back, redrawn */
  /* Both the file and the picture are gone, so the next screen must load them
   * again: without this the stock looks unchanged and no record is read. */
  fs_pic_code[0] = '\0';
  fs_can_open_tried = 0;
  clrscr();
  powersave_permit();
}

static int fs_shelf_run(void) {
  if (fs_gui_active)
    return FS_VIEW_NONE;

  fs_keep_panel(); /* see its comment: restores a wiped idle frame */

  fs_log("shelf: running");
  fs_log_flush();
  beep_times(1); /* heard = the handler ran and the task started */

  /* Open with the strip already on the selected stock. A spring starting at 0
   * would glide in from the first stock every time the screen came up, which
   * after a card change means a sweep across the whole shelf before the user
   * has touched anything. */
  {
    int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
    fs_anim.x = fs_clamp_i(film_sim_stock, 0, count - 1) * 65536;
    fs_anim.v = 0;
  }
  fs_screen_take();
  fs_log("shelf: screen taken (vram=0x%x on=%d lv=%d gd=%d)",
         (unsigned)bmp_vram_real(), bmp_is_on(), lv, get_global_draw());
  fs_log_flush();

  fs_gui_active = 1;
  fs_gui_dirty = 1;
  fs_gui_logged = 0;
  fs_apply_request = 0;
  fs_view_request = FS_VIEW_NONE;

  /* If not one key arrives, this task would own the panel for ever and the
   * camera would look bricked. Release it and say why in the log instead. */
  unsigned kc0 = fs_key_count;
  uint32_t t0 = get_ms_clock();
  fs_paint_since = t0;
  fs_paint_ms = 0;
  fs_paint_rows_drawn = 0;
  fs_paint_frames = 0;

  while (fs_gui_active) {
    /* The strip springs towards the selected stock and the header sheen moves
     * on its own, so the screen is redrawn on a clock, not only on a keypress.
     *
     * 30 ms, not 70: the loop already sleeps 30 ms at the end of every pass,
     * so this is the fastest rate it can draw without changing the sleep, and
     * a spring needs the rate to look like one. The repaint is skipped the
     * moment the spring settles, so the cost is paid only while moving. */
    if (get_ms_clock() - fs_anim_ms > 30) {
      int dt = (int)(get_ms_clock() - fs_anim_ms);
      fs_anim_ms = get_ms_clock();
      /* The header sheen keeps its own, slower clock. */
      fs_strip_moving = fs_shelf_animate(dt);
      if (fs_strip_moving)
        fs_strip_dirty = 1;
      if (get_ms_clock() - fs_sheen_ms > 260) {
        fs_sheen_ms = get_ms_clock();
        fs_anim_tick++;
        fs_header_dirty = 1;
      }
      /* The footer's LiveView status can change under the screen; if it did,
       * only a full redraw will show the new text. */
      if (fs_lv_status_key() != fs_painted_hint)
        fs_gui_dirty = 1;
    }
    if (fs_prov_tick())
      fs_gui_dirty = 1;
    if (fs_apply_wait && (uint32_t)(get_ms_clock() - fs_apply_due_ms) >= FS_APPLY_SETTLE_MS) {
      fs_apply_wait = 0;
      fs_apply_request = 1;
    }
    if (fs_apply_request) {
      fs_apply_request = 0;
      fs_apply_film_now();
      {
        int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
        int idx = fs_clamp_i(film_sim_stock, 0, count - 1);
        /*
         * Not NotifyBox() here, and this is the whole "the film stock image is
         * repainted a second after a click".
         *
         * NotifyBox_task() draws its text, then calls redraw() to erase it.
         * redraw() -> _redraw_do() (zebra.c): it enables Canon's front buffer,
         * calls dialog_redraw() over the entire bitmap canvas, and disables it
         * again. Our frame is gone, and the only thing repainting is the 260 ms
         * header band -- so the shelf card disappears and the top strip keeps
         * updating, which is exactly what the panel did.
         *
         * The shelf already has its own badge for this, drawn by fsg.c as part
         * of a frame this module owns.
         */
        fs_toast_until = get_ms_clock() + 1500;
        snprintf(fs_toast, sizeof fs_toast, "APPLIED  %s",
                 film_active_table[idx].name);
        fs_gui_dirty = 1;
      }
    }
    /*
     * A fast dial sends steps faster than a full frame can be drawn, and every
     * step used to schedule one: the loop then did nothing but full repaints,
     * each with its card read for the artwork, and the screen stopped answering
     * until the dial stopped turning -- "turn fast and the GUI will not load".
     *
     * Coalesce instead: one full frame per 100 ms at most, and while a full
     * frame is being deferred the strip band still repaints, so the shelf moves
     * with the dial and only the text behind it catches up.
     */
    {
      static uint32_t last_full;
      uint32_t now = get_ms_clock();
      /*
       * A full frame is rate limited, not deferred until the strip stops.
       *
       * Deferring it was wrong: the card's log shows what happened -- "paint 20
       * frames, 130 ms, 2122 rows", i.e. ~106 rows a frame, all of them bands,
       * and no 480-row frame at all after a stock change. The stock's name and
       * description live in the card (rows 54..342), so they simply never
       * redrew: the shelf showed the film it had before the click, for as long
       * as the strip kept easing. The pixels behind the text changed, nothing
       * said so, and only a full frame can put it right.
       *
       * The work that made fast turning unusable was never this paint -- it was
       * the 16 KB picture-style write per detent, now debounced.
       */
      int full_due = fs_gui_dirty &&
                     (!last_full || (uint32_t)(now - last_full) >= 100);
      if (!fs_gui_logged)
        full_due = 1;
      if (full_due)
        last_full = now;
      else if (fs_gui_dirty)
        fs_strip_dirty = 1;   /* keep moving while the text waits its turn */

      if (full_due) {
        fs_gui_dirty = 0;
        fs_strip_dirty = 0;
        fs_header_dirty = 0;
        fs_picture_sync(); /* outside the lock: this one reads the card */
        fs_paint(fs_gui_render);
        if (!fs_gui_logged) {
          fs_gui_logged = 1;
          beep_times(2);
          fs_log("shelf: first frame drawn (vram=0x%x on=%d direct=%d)",
                 (unsigned)bmp_vram_real(), bmp_is_on(), film_sim_direct);
          fs_bmp_probe("shelf");
        }
      } else {
        /* Only a band moved: copy and draw those rows, not the whole screen. */
        if (fs_strip_dirty) {
          fs_strip_dirty = 0;
          fs_paint_band(FSG_STRIP_Y, FSG_STRIP_Y + FSG_STRIP_H, fs_gui_render_strip);
        }
        if (fs_header_dirty) {
          fs_header_dirty = 0;
          fs_paint_band(0, FSG_HEADER_H, fs_gui_render_header);
        }
      }
    }
    /* Once a second, what the painting cost -- the number the log has to carry
     * for "the strip redraws 94 rows, not 480" to be checkable on the camera. */
    if (fs_paint_frames && get_ms_clock() - fs_paint_since > 1000) {
      fs_log("shelf: paint %d frames, %d ms, %d rows (full=%d rows, band=%d+%d)"
             " reassert=%d", fs_paint_frames, (int)fs_paint_ms,
             fs_paint_rows_drawn, FS_BMP_H, FSG_STRIP_H, FSG_HEADER_H,
             fs_fb_reasserts);
      fs_fb_reasserts = 0;
      fs_paint_ms = 0;
      fs_paint_rows_drawn = 0;
      fs_paint_frames = 0;
      fs_paint_since = get_ms_clock();
    }
    if (fs_key_count == kc0 && get_ms_clock() - t0 > 20000) {
      fs_log("shelf: no key received in 20 s, releasing the screen");
      fs_gui_active = 0;
      break;
    }
    msleep(30);
  }

  fs_gui_active = 0;
  fs_screen_release();

  /* Property writes block on an ICU round-trip, so they must never run inside
   * the keypress callback: that is Canon's GUI task and stalling it kills every
   * later key. They run here, after the panel is back with the firmware. */
  if (fs_apply_wait) {   /* a choice made just before leaving still counts */
    fs_apply_wait = 0;
    fs_apply_request = 1;
  }
  if (fs_apply_request) {
    fs_apply_request = 0;
    fs_apply_film_now();
    beep_times(2);
  }

  /* film.sim.stock is a MODULE_CONFIG entry, so ML writes it out with the rest
   * of the config on shutdown; nothing to save by hand here. Log the value so
   * the choice made on this screen can be checked afterwards. */
  fs_log("shelf: closed (next view %d, stock=%d, playback=%d)", fs_view_request,
         (int)film_sim_stock, (int)fs_want_playback);
  fs_open_canon_playback();
  return fs_view_request;
}

static MENU_SELECT_FUNC(fs_gui_open) {
  /* Logged before task_create: if the handler is never reached we see nothing,
   * if it is reached but the task cannot be created we still see this line. */
  beep_times(1); /* heard = the menu handler really ran */
  fs_log("shelf: handler entered (delta=%d)", delta);
  fs_log_flush();
  fs_ui_launch(FS_VIEW_SHELF);
  fs_log_flush();
}

/* ---------------------------------------------------------------- darkroom
 *
 * Darkroom Photo & LUT Selector View + 10-Second Chemical Bath Developing
 * Screen.
 */

/* Case-insensitive substring test without strcasestr, which ML does not
 * export to modules. */
static int fs_name_has_ci(const char *hay, const char *needle) {
  if (!hay || !needle || !needle[0])
    return 0;
  for (const char *h = hay; *h; h++) {
    const char *a = h, *b = needle;
    while (*a && *b) {
      char ca = *a, cb = *b;
      if (ca >= 'A' && ca <= 'Z')
        ca += 32;
      if (cb >= 'A' && cb <= 'Z')
        cb += 32;
      if (ca != cb)
        break;
      a++;
      b++;
    }
    if (!*b)
      return 1;
  }
  return 0;
}

/*
 * Which stock the develop should use.
 *
 * There are two settings: film_sim_preset picks the picture style that was
 * applied when the photo was taken, and film_sim_stock indexed the develop.
 * They drifted apart -- the log showed preset=1 (Fuji Superia 400) while the
 * develop graded with PANF.fs (Pan F Plus 50), i.e. a black-and-white film
 * used on a colour shot. The develop now follows the preset whenever the
 * preset's name matches a stock in the table ("Kodak Portra 400" -> "Portra
 * 400"), and only falls back to film_sim_stock when it does not.
 */
static int fs_develop_stock_index(int count) {
  if (count <= 0)
    return 0;
  return fs_clamp_i(film_sim_stock, 0, count - 1);
}

/* Which built-in grade the selected .fs stock corresponds to. There are 33
 * stocks and 10 looks, so the preview follows the stock by name instead of a
 * second menu that silently disagreed with the first one. */
static int fs_stock_preset(const char *name) {
  if (!name)
    return -1;
  /*
   * Every stock in the table has to answer here.
   *
   * There are 33 stocks and 10 built-in grades, so this is a nearest-neighbour
   * map, not a set of identities -- but it must cover them all.  It did not:
   * Nostalgic Neg and most of the consumer negatives fell through to NEUTRAL,
   * fs_lv_preset() then used film.sim.preset instead, and the log showed
   * "preset=1 (Fuji Superia 400)" while the develop baked
   * ML/FILM/nostalgicneg.fs.  The picture style at capture and the look on
   * the develop were two different films and nothing said so.
   */
  if (fs_name_has_ci(name, "Portra"))      return FS_PRESET_KODAK_PORTRA;
  if (fs_name_has_ci(name, "Ektar"))       return FS_PRESET_FUJI_VELVIA;
  if (fs_name_has_ci(name, "Velvia"))      return FS_PRESET_FUJI_VELVIA;
  if (fs_name_has_ci(name, "Ektachrome") ||
      fs_name_has_ci(name, "Kodachrome") ||
      fs_name_has_ci(name, "Provia"))      return FS_PRESET_NEUTRAL;
  if (fs_name_has_ci(name, "Superia") ||
      fs_name_has_ci(name, "C200"))        return FS_PRESET_FUJI_SUPERIA;
  if (fs_name_has_ci(name, "Classic Neg") ||
      fs_name_has_ci(name, "Nostalgic"))   return FS_PRESET_FUJI_CLASSIC_NEG;
  if (fs_name_has_ci(name, "Pro 400H"))    return FS_PRESET_FUJI_PRO400H;
  if (fs_name_has_ci(name, "Gold") ||
      fs_name_has_ci(name, "ColorPlus") ||
      fs_name_has_ci(name, "UltraMax"))    return FS_PRESET_KODAK_GOLD;
  if (fs_name_has_ci(name, "CineStill 800T") ||
      fs_name_has_ci(name, "Vision") ||
      fs_name_has_ci(name, "Eterna") ||
      fs_name_has_ci(name, "Color 800"))   return FS_PRESET_CINEMA;
  if (fs_name_has_ci(name, "CineStill 50D")) return FS_PRESET_NEUTRAL;
  if (fs_name_has_ci(name, "Tungsten"))    return FS_PRESET_TUNGSTEN;
  /* The B&W list has to cover the imported stocks too: APX 25, Neopan 1600,
   * HPS 800, FP4, BW400CN, Ortho 25, Retro 80S/100 and the two infrared films
   * all fell through to NEUTRAL, so a black-and-white emulsion was previewed
   * with a colour preset. */
  if (fs_name_has_ci(name, "Tri-X") || fs_name_has_ci(name, "T-Max") ||
      fs_name_has_ci(name, "HP5") || fs_name_has_ci(name, "Pan F") ||
      fs_name_has_ci(name, "Acros") || fs_name_has_ci(name, "Delta") ||
      fs_name_has_ci(name, "Fomapan") || fs_name_has_ci(name, "XP2") ||
      fs_name_has_ci(name, "APX") || fs_name_has_ci(name, "Neopan") ||
      fs_name_has_ci(name, "HPS") || fs_name_has_ci(name, "FP4") ||
      fs_name_has_ci(name, "BW400") || fs_name_has_ci(name, "Ortho") ||
      fs_name_has_ci(name, "Retro") || fs_name_has_ci(name, "Infrared") ||
      fs_name_has_ci(name, "HIE"))
    return FS_PRESET_BW;
  /* -1, not NEUTRAL: "this stock is neutral" and "nothing here knows this
   * stock" are different answers, and only the second one should fall back to
   * film.sim.preset. Returning NEUTRAL for both is how a slide film ended up
   * previewed as Fuji Superia. */
  if (fs_name_has_ci(name, "Olympus"))
    return FS_PRESET_NEUTRAL;
  return -1;
}

static int fs_film_gen = 0; /* bumped whenever the film table is reloaded */

static int fs_lv_preset(void) {
  static int cache_key = -1;
  static int cache_val = FS_PRESET_NEUTRAL;
  int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
  int idx = fs_develop_stock_index(count);
  int key = (count << 16) | (idx << 8) | (film_sim_preset & 0xFF) |
            ((fs_film_gen & 0x3F) << 24);
  if (key != cache_key) {
    cache_key = key;
    int p = fs_stock_preset(film_active_table[idx].name);
    if (p < 0)
      p = fs_clamp_i(film_sim_preset, 0, FS_PRESET_COUNT - 1);
    cache_val = p;
  }
  return cache_val;
}

/* The darkroom is the same screen as the shelf, pointed at a frame: the strip
 * still picks the film, the left panel names the photo that SET will develop.
 * One drawer means the two screens cannot drift apart. */
static void fs_darkroom_render(void) {
  fsg_shelf_t st;
  static char counter[24];

  fs_shelf_state(&st);
  st.title = "DARKROOM";
  st.photo = (dcim_photo_count > 0) ? dcim_photo_list[dcim_photo_index]
                                    : "(no photos)";
  snprintf(counter, sizeof counter, "%d/%d",
           dcim_photo_count > 0 ? dcim_photo_index + 1 : 0, dcim_photo_count);
  st.counter = counter;
  st.hint_l = "SET = develop  press = next frame  MENU = shelf  PLAY = view";
  st.hint_r = fs_lv_status();
  fs_picture_palette(st.stock.palette);
  fsg_draw_shelf(&st);
}

static void fs_darkroom_close(void) { fs_darkroom_active = 0; }

static int fs_darkroom_run(void) {
  if (fs_darkroom_active)
    return FS_VIEW_NONE;

  fs_keep_panel(); /* see its comment: restores a wiped idle frame */

  fs_log("darkroom: running");
  fs_log_flush();
  /* A stale request from whichever screen opened this one must not be read
   * back as "open me again" when the darkroom closes. */
  fs_view_request = FS_VIEW_NONE;
  fs_dev_request = 0;
  fs_prov_active = 0;
  {
    int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
    fs_anim.x = fs_clamp_i(film_sim_stock, 0, count - 1) * 65536;
    fs_anim.v = 0;
  }
  fs_screen_take();
  fs_log("darkroom: screen taken (vram=0x%x on=%d lv=%d gd=%d)",
         (unsigned)bmp_vram_real(), bmp_is_on(), lv, get_global_draw());
  fs_log_flush();

  fs_scan_dcim_photos();
  fs_darkroom_active = 1;
  fs_darkroom_focus = 0;
  fs_darkroom_dirty = 1;
  fs_strip_dirty = 0;
  fs_dev_request = 0;

  unsigned kc0 = fs_key_count;
  uint32_t t0 = get_ms_clock();

  while (fs_darkroom_active) {
    if (fs_dev_request) {
      fs_darkroom_active = 0;
      fs_darkroom_dirty = 0;
      break;
    }
    if (get_ms_clock() - fs_anim_ms > 30) {
      int dt = (int)(get_ms_clock() - fs_anim_ms);
      fs_anim_ms = get_ms_clock();
      if (fs_shelf_animate(dt))
        fs_strip_dirty = 1;
    }
    if (fs_prov_tick())
      fs_darkroom_dirty = 1;
    if (fs_darkroom_dirty) {
      fs_darkroom_dirty = 0;
      fs_strip_dirty = 0;
      fs_picture_sync();
      if (!fs_darkroom_logged) {
        fs_darkroom_logged = 1;
        fs_log("darkroom: first frame drawn (vram=0x%x on=%d)",
               (unsigned)bmp_vram_real(), bmp_is_on());
        fs_log_flush();
      }
      fs_paint(fs_darkroom_render);
    } else if (fs_strip_dirty) {
      fs_strip_dirty = 0;
      fs_paint_band(FSG_STRIP_Y, FSG_STRIP_Y + FSG_STRIP_H, fs_gui_render_strip);
    }
    if (fs_key_count == kc0 && get_ms_clock() - t0 > 20000) {
      fs_log("darkroom: no key received in 20 s, releasing the screen");
      fs_darkroom_active = 0;
      break;
    }
    msleep(30);
  }

  fs_darkroom_active = 0;
  fs_screen_release();

  /* The panel is back with the firmware before any file work starts, so the
   * develop banner is drawn by the develop step and nothing else is copying the
   * idle buffer underneath it. */
  int request = fs_dev_request                    ? FS_VIEW_DEVELOP
                : fs_view_request != FS_VIEW_NONE ? fs_view_request
                                                  : FS_VIEW_SHELF;
  fs_dev_request = 0;
  fs_view_request = FS_VIEW_NONE;
  fs_log("darkroom: closed (next view %d, playback=%d)", request,
         (int)fs_want_playback);
  fs_open_canon_playback();
  return request;
}

static void fs_view_task(void *arg) {
  (void)arg;
  fs_view_run(fs_last_dev, 0);
  fs_ui_running = 0;
}

static MENU_SELECT_FUNC(fs_view_last) {
  if (!fs_last_dev[0]) {
    NotifyBox(2000, "Nothing developed yet");
    return;
  }
  if (fs_view_active || fs_ui_running || fs_gui_active || fs_darkroom_active ||
      fs_develop_active) {
    NotifyBox(2000, "Film: busy");
    return;
  }
  fs_ui_running = 1;
  task_create("fs_view", 0x1e, FS_DEV_STACK, fs_view_task, 0);
}

static MENU_SELECT_FUNC(fs_darkroom_open) {
  beep_times(1);
  fs_log("darkroom: handler entered (delta=%d)", delta);
  fs_log_flush();
  fs_ui_launch(FS_VIEW_DARKROOM);
  fs_log_flush();
}

static const char *fs_stock_alias(const film_xml_t *f) {
  /*
   * Exact codes, never substrings.
   *
   * This used to test strstr(f->code, "P"), which is true for CP200, PRO100,
   * SUP400, PANF, HP5 and XP2 as well as the three Portras, so six films plus
   * Provia were all rendered with portra.fs. Names first, then exact codes.
   */
  /* Before the name test: "Portra 160" and "Portra 800" both contain
   * "Portra", so a name check would send them to portra.fs, which is the 400
   * recipe.  Those two are different films and get their own files; 400 keeps
   * the shared one. */
  if (!strcasecmp(f->code, "P160"))
    return "P160";
  if (!strcasecmp(f->code, "P800"))
    return "P800";
  if (strstr(f->name, "Portra"))
    return "portra";
  if (strstr(f->name, "Gold"))
    return "gold";
  if (strstr(f->name, "Velvia"))
    return "velvia";
  if (strstr(f->name, "Provia"))
    return "provia";
  if (strstr(f->name, "Acros"))
    return "acros";
  if (strstr(f->name, "Astia"))
    return "astia";
  if (strstr(f->name, "Classic Chrome"))
    return "classicchrome";
  if (strstr(f->name, "Classic Neg"))
    return "classicneg";
  if (strstr(f->name, "Nostalgic Neg"))
    return "nostalgicneg";
  if (strstr(f->name, "Reala"))
    return "realaace";
  if (strstr(f->name, "Eterna"))
    return "eterna";
  if (strstr(f->name, "Bleach"))
    return "bleachbypass";
  if (strstr(f->name, "CineStill 800T"))
    return "soup800";

  if (!strcasecmp(f->code, "P160") || !strcasecmp(f->code, "P400") ||
      !strcasecmp(f->code, "P800"))
    return "portra";
  if (!strcasecmp(f->code, "GOLD"))
    return "gold";
  if (!strcasecmp(f->code, "VEL50"))
    return "velvia";
  if (!strcasecmp(f->code, "PRO100"))
    return "provia";
  if (!strcasecmp(f->code, "ACROS"))
    return "acros";
  if (!strcasecmp(f->code, "800T"))
    return "soup800";
  if (!strcasecmp(f->code, "ASTIA"))
    return "astia";
  if (!strcasecmp(f->code, "CCHR"))
    return "classicchrome";
  if (!strcasecmp(f->code, "CNEG"))
    return "classicneg";
  if (!strcasecmp(f->code, "NNEG"))
    return "nostalgicneg";
  if (!strcasecmp(f->code, "RACE"))
    return "realaace";
  if (!strcasecmp(f->code, "ETRN"))
    return "eterna";
  if (!strcasecmp(f->code, "BLPAS"))
    return "bleachbypass";

  return NULL;
}

/* Resolve or auto-generate a valid .fs stock recipe for Soup Engine camera
 * develop */
static int fs_resolve_stock_path(const film_xml_t *f, char *out_path,
                                 size_t out_len) {
  char path[FS_PATH_LEN];
  uint32_t sz = 0;

  const char *alias = fs_stock_alias(f);
  if (alias) {
    snprintf(path, sizeof(path), "ML/FILM/%s.fs", alias);
    if (FIO_GetFileSize(path, &sz) == 0 && sz > 0) {
      snprintf(out_path, out_len, "%s", path);
      return 1;
    }
    snprintf(path, sizeof(path), "ML/FILMS/%s.fs", alias);
    if (FIO_GetFileSize(path, &sz) == 0 && sz > 0) {
      snprintf(out_path, out_len, "%s", path);
      return 1;
    }
  }

  snprintf(path, sizeof(path), "ML/FILM/%s.fs", f->code);
  if (FIO_GetFileSize(path, &sz) == 0 && sz > 0) {
    /* A generated stub from an older build says "# Soup Engine Stock Recipe"
     * and carries no AUTOGEN line; those are worth less than the recipe the
     * generator can write now, so rebuild them. A hand-written recipe starts
     * with its own comment and is never touched. */
    char head[64];
    FILE *fp = FIO_OpenFile(path, O_RDONLY);
    int n = fp ? FIO_ReadFile(fp, head, sizeof head - 1) : 0;
    if (fp)
      FIO_CloseFile(fp);
    if (n > 0)
      head[n] = 0;
    int stale = (n > 0 && !strncmp(head, FS_RECIPE_MARKER,
                                   sizeof FS_RECIPE_MARKER - 1) &&
                 !strstr(head, "AUTOGEN"));
    if (!stale) {
      snprintf(out_path, out_len, "%s", path);
      return 1;
    }
    fs_log("develop: %s is an old generated stub, rebuilding it", path);
    fs_log_flush();
  }
  snprintf(path, sizeof(path), "ML/FILMS/%s.fs", f->code);
  if (FIO_GetFileSize(path, &sz) == 0 && sz > 0) {
    snprintf(out_path, out_len, "%s", path);
    return 1;
  }

  /* ---------------------------------------------------------- generated
   *
   * A recipe made from the stock table, for the emulsions with no hand-tuned
   * .fs on the card.
   *
   * This used to write a near-identity curve and SAT 100, which is to say no
   * look at all: a develop of UltraMax 400 came out an almost untouched colour
   * frame and the honest reaction was "the difference is not much". Everything
   * the engine can do is in the table already -- box speed, contrast, the
   * saturation it should be run at, and the white balance the film is balanced
   * for -- so the generated recipe uses all of it: a curve that actually bends,
   * a cast in the correct direction, the stock's own saturation, split toning
   * and grain that scales with the ISO.
   *
   * AUTOGEN is written into the file so an older generated stub can be spotted
   * and replaced; hand-written recipes (they start with their own comment) are
   * never touched.
   */
  {
    int con = fs_clamp_i(f->contrast, -4, 4);
    int hum = f->mono ? 1 : 0;
    int kelvin = f->kelvin;
    int warm = (kelvin > 0 && kelvin <= 5400);
    int sat = hum ? 0 : fs_clamp_i(100 + f->saturation * 10, 55, 150);
    int grain = fs_clamp_i(6 + f->iso / 40 + (hum ? 6 : 0), 6, 45);
    int gsize = fs_clamp_i(26 + f->iso / 120, 24, 60);
    const char *wb = "AS_SHOT";
    char curve[96], curver[64], curveb[64], spl[48], sphi[48];
    int y0, y64, y192, y255;
    int r_lo, b_hi;

    if (kelvin > 0) {
      if (kelvin < 3600)
        wb = "TUNGSTEN";
      else if (kelvin < 4600)
        wb = "FLUORESCENT";
      else if (kelvin < 5600)
        wb = "DAYLIGHT";
      else if (kelvin < 6200)
        wb = "CLOUDY";
      else
        wb = "SHADE";
    }

    /* 0.12 of extra slope per step of the table's contrast, pivoting on
     * mid-grey, with a filmic toe and shoulder rather than a straight line. */
    {
      int slope = 100 + con * 12; /* percent */
      y64 = 128 - (64 * slope) / 100;
      y192 = 128 + (64 * slope) / 100;
      y255 = 128 + (127 * slope) / 100 - (hum ? 0 : 4);
      y0 = hum ? 12 : 5;
      y64 = fs_clamp_i(y64, 1, 127);
      y192 = fs_clamp_i(y192, 130, 254);
      y255 = fs_clamp_i(y255, 200, 255);
    }
    snprintf(curve, sizeof curve, "%d,%d 64,%d 128,128 192,%d 255,%d", y0, y0,
             y64, y192, y255);

    /* A warm film gets a little more red in the highlights and less blue; a
     * daylight-balanced or cool film the other way round. */
    r_lo = warm ? 2 : 0;
    b_hi = warm ? -8 : 4;
    snprintf(curver, sizeof curver, "0,%d 128,%d 255,%d", hum ? 12 : 6,
             128 + r_lo, fs_clamp_i(255 + r_lo, 200, 255));
    snprintf(curveb, sizeof curveb, "0,%d 128,128 255,%d", hum ? 10 : 4,
             fs_clamp_i(255 + b_hi, 180, 255));

    if (hum) {
      snprintf(spl, sizeof spl, "%d,%d,%d", 4, 1, 6);
      snprintf(sphi, sizeof sphi, "%d,%d,%d", 5, 2, -3);
    } else if (warm) {
      snprintf(spl, sizeof spl, "%d,%d,%d", 10, 4, -6);
      snprintf(sphi, sizeof sphi, "%d,%d,%d", 7, 3, -5);
    } else {
      snprintf(spl, sizeof spl, "%d,%d,%d", 3, 2, 8);
      snprintf(sphi, sizeof sphi, "%d,%d,%d", 4, 2, 3);
    }

    const char *dirs[2] = {"ML/FILM", "ML/FILMS"};
    for (int d = 0; d < 2; d++) {
      snprintf(path, sizeof(path), "%s/%s.fs", dirs[d], f->code);
      FILE *fp = FIO_CreateFile(path);
      if (!fp)
        continue;
      char buf[640];
      int len = snprintf(
          buf, sizeof(buf),
          "# Soup Engine Stock Recipe (generated from the stock table)\n"
          "AUTOGEN 2\n"
          "VERSION 3\n"
          "NAME %s\n"
          "DESC %s\n"
          "WB %s\n"
          "CURVE %s\n"
          "CURVE_R %s\n"
          "CURVE_B %s\n"
          "SAT %d\n"
          "SPLIT_SH %s\n"
          "SPLIT_HI %s\n"
          "GRAIN %d\n"
          "GRAIN_SIZE %d\n",
          f->name, f->description, wb, curve, curver, curveb, sat, spl, sphi,
          grain, gsize);
      FIO_WriteFile(fp, buf, len);
      FIO_CloseFile(fp);
      snprintf(out_path, out_len, "%s", path);
      fs_log("develop: generated %s (WB %s, sat %d, grain %d)", path, wb, sat,
             grain);
      fs_log_flush();
      return 1;
    }
  }
  return 0;
}

/* --- develop progress, drawn by the engine's own callbacks -------------- */

static int fs_bath_ui = 0;
static int fs_bath_pct = 0;
static char fs_bath_stage[40] = "";

/* State of the develop screen. fsg draws it; this is only what it shows. */
static char fs_dev_photo_buf[64] = "-";
static const char *fs_dev_photo = fs_dev_photo_buf;
static const char *fs_dev_stock = "-";
static const char *fs_dev_brand = "";
static const char *fs_dev_soup = "";
static const char *fs_dev_result = NULL;
static int fs_dev_iso = 0;
static int fs_dev_index = 0, fs_dev_total = 0;
static int fs_dev_ok = 0, fs_dev_skipped = 0, fs_dev_failed = 0;
static int fs_dev_done = 0;

static void fs_develop_screen(void) {
  fsg_dev_t d;
  static unsigned tick;
  /* Called from the engine's progress callback, i.e. mid-develop: compose in
   * the mirror and flip, or the panel shows a half-drawn screen. */
  int idle = fs_bath_ui;
  memset(&d, 0, sizeof d);
  d.photo = fs_dev_photo;
  d.stock = fs_dev_stock;
  d.brand = fs_dev_brand;
  d.soup = fs_dev_soup;
  d.stage = fs_bath_stage;
  d.result = fs_dev_result;
  d.iso = fs_dev_iso;
  d.pct = fs_bath_pct;
  d.index = fs_dev_index;
  d.total = fs_dev_total;
  d.ok = fs_dev_ok;
  d.skipped = fs_dev_skipped;
  d.failed = fs_dev_failed;
  d.done = fs_dev_done;
  d.tick = ++tick * 90; /* moves the tank bubbles between progress steps */
  if (idle) {
    /* fsg_draw_develop paints every pixel (vgrad covers the frame), so the
     * "copy the screen in first" step that used to be here was 345 KB of
     * pointless traffic per repaint -- upstream ML says the same thing in
     * menu.c: "no need, drawing is fullscreen anyway". */
    BMP_LOCK(bmp_draw_to_idle(1); fsg_draw_develop(&d); bmp_draw_to_idle(0);
             bmp_idle_copy(1, 0);)
  } else {
    fsg_draw_develop(&d);
  }
}

static void fs_bath_show_progress(int pct, const char *stage) {
  if (pct < 0)
    pct = 0;
  if (pct > 100)
    pct = 100;
  fs_bath_pct = pct;
  if (stage && stage[0])
    snprintf(fs_bath_stage, sizeof fs_bath_stage, "%s", stage);
  if (!fs_bath_ui)
    return;
  /*
   * Repaint on a stage change, on ten points of progress, or every two
   * seconds -- whichever comes first.
   *
   * This runs from inside the engine's row callback, so the cost lands in the
   * middle of the decode: a full 720x480 compose over every pixel and an
   * idle-buffer flip. FILMSIM.LOG shows what over-painting did -- two develops
   * of the same frame reporting "profile look 954 ms, encode 10954 ms" and
   * "look 8209 ms, encode 9220 ms" for work that takes a fraction of that when
   * nothing else is running. The bar does not need four updates a second.
   */
  {
    static uint32_t drawn_ms = 0;
    static char drawn_stage[40];
    static int drawn_pct = -1;
    uint32_t now = get_ms_clock();
    int stage_changed = stage && stage[0] && strcmp(stage, drawn_stage);
    /* Repaint on a stage change, on ten points of progress, or every two
     * seconds -- whichever comes first. Each repaint is a full 720x480
     * compose plus a buffer flip, and the develop calls this from between
     * decoder row bands, so its cost lands in the middle of the decode. */
    if (pct < 100 && !stage_changed && drawn_ms && now - drawn_ms < 2000 &&
        drawn_pct >= 0 && pct - drawn_pct < 10)
      return;
    drawn_ms = now;
    drawn_pct = pct;
    snprintf(drawn_stage, sizeof drawn_stage, "%s", stage ? stage : "");
    if (pct >= 100) {
      drawn_ms = 0;
      drawn_pct = -1;
    }
  }
  fs_develop_screen();
}

static void fs_bath_progress(void *ctx, const char *stage, int pct) {
  static const char *last;
  (void)ctx;
  fs_bath_show_progress(pct, stage);
  /* Write the log out at every stage change. If the camera dies inside the
   * develop, the card still says which stage it died in -- a single flush at
   * the end tells us nothing when there is no end. */
  if (stage && stage != last) {
    last = stage;
    fs_log("develop: stage %s %d%%", stage, pct);
    fs_log_flush();
  }
}

/* Engine diagnostics: without a sink here every se_log() line was discarded. */
static void fs_develop_note(void *ctx, const char *msg) {
  (void)ctx;
  fs_log("  engine: %s", msg);
}

/*
 * Exposure line for the frame: shutter, aperture and ISO, decoded from the
 * same tables ML uses to display them. Raw values that do not appear in a
 * table (bulb, an adapted lens) fall back to "-".
 */
static int fs_code_index(const uint8_t *codes, int n, int raw) {
  int i;
  for (i = 0; i < n; i++) if (codes[i] == (uint8_t)raw) return i;
  return -1;
}

static void fs_frame_exposure(se_frame *f) {
  int i;

  snprintf(f->label[0], sizeof f->label[0], "f");
  snprintf(f->label[1], sizeof f->label[1], "SS");
  snprintf(f->label[2], sizeof f->label[2], "ISO");
  snprintf(f->value[0], sizeof f->value[0], "-");
  snprintf(f->value[1], sizeof f->value[1], "-");
  snprintf(f->value[2], sizeof f->value[2], "-");

  i = fs_code_index(codes_aperture, APERTURE_ARRAY_LEN, lens_info.raw_aperture);
  if (i >= 0) {
    int a = values_aperture[i];              /* aperture * 10 */
    if (a % 10) snprintf(f->value[0], sizeof f->value[0], "%d.%d", a / 10, a % 10);
    else        snprintf(f->value[0], sizeof f->value[0], "%d", a / 10);
  }
  i = fs_code_index(codes_shutter, SHUTTER_ARRAY_LEN, lens_info.raw_shutter);
  if (i >= 0) {
    int s = values_shutter[i];               /* denominator, 1/s */
    if (s > 0) snprintf(f->value[1], sizeof f->value[1], "1/%d", s);
  }
  i = fs_code_index(codes_iso, ISO_ARRAY_LEN, lens_info.raw_iso);
  if (i >= 0) snprintf(f->value[2], sizeof f->value[2], "%d", values_iso[i]);
}

/* Apply the film frame, or clear it, before a develop. Opt-in: the config
 * defaults to off, and a null frame leaves the file exactly as before. */
/* Brand mark for a stock: 0 Kodak, 1 Fujifilm, 2 Ilford, -1 none (canister). */
static int fs_frame_brand(const char *name, uint8_t rgb[3]) {
  if (strstr(name, "PORTRA") || strstr(name, "GOLD") ||
      strstr(name, "COLORPLUS") || strstr(name, "EKTAR") ||
      strstr(name, "ULTRAMAX") || strstr(name, "KODACHROME") ||
      strstr(name, "EKTACHROME") || strstr(name, "TRI-X") ||
      strstr(name, "T-MAX")) {
    rgb[0] = 0xED; rgb[1] = 0x00; rgb[2] = 0x00;   /* Kodak red */
    return 0;
  }
  if (strstr(name, "PROVIA") || strstr(name, "VELVIA") ||
      strstr(name, "ACROS") || strstr(name, "SUPERIA") ||
      strstr(name, "C200") || strstr(name, "ASTIA")) {
    rgb[0] = 0x00; rgb[1] = 0x84; rgb[2] = 0x3D;   /* Fujifilm green */
    return 1;
  }
  if (strstr(name, "HP5") || strstr(name, "DELTA") ||
      strstr(name, "PANF") || strstr(name, "XP2")) {
    rgb[0] = 0x10; rgb[1] = 0x10; rgb[2] = 0x10;   /* Ilford black */
    return 2;
  }
  return -1;
}

/* The stock's canister from CANS.DAT, the same art the shelf shows. */
static const unsigned char *fs_frame_art(const char *name, int *w, int *h) {
  static int tried;
  int idx;

  if (!film_sim_art) return NULL;
  if (!tried) {
    tried = 1;
    if (!fsg_can_load(FSG_CAN_PATH))
      fsg_can_load(FSG_CAN_PATH2);
    fsg_can_thumbs();
  }
  idx = fs_can_index(name);
  if (idx < 0) return NULL;
  return fsg_can_thumb(idx, w, h);
}

static void fs_frame_apply(const char *stock_path) {
  se_frame f;
  const char *base;
  int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
  const film_xml_t *film = &film_active_table[fs_develop_stock_index(count)];
  int aw = 0, ah = 0;
  const unsigned char *art;

  (void)stock_path;
  if (!film_sim_frame) { se_cam_set_frame(NULL); return; }

  base = camera_model[0] ? camera_model : "EOS 6D";
  memset(&f, 0, sizeof f);
  f.on = 1;
  f.scale = 2;
  se_frame_layout(&f);
  f.bg[0] = f.bg[1] = f.bg[2] = 255;      /* paper white */
  se_frame_contrast(f.bg, f.fg);
  f.dim[0] = f.dim[1] = f.dim[2] = 120;   /* model line and labels */
  f.brand_colour[0] = 0xCC;               /* Canon red */
  f.brand_colour[1] = 0x00;
  f.brand_colour[2] = 0x00;
  snprintf(f.model, sizeof f.model, "%s", base);
  snprintf(f.stock, sizeof f.stock, "%s", film->name);
  /*
   * Per stock first: the canister in CANS.DAT is that stock's own artwork.
   * The brand mark is the fallback, for a card with no art file or a stock
   * the artwork does not cover.
   */
  art = fs_frame_art(film->name, &aw, &ah);
  if (art) {
    f.sprite = art;
    f.sprite_w = aw;
    f.sprite_h = ah;
    f.sprite_scale = 2;
    f.brand_i = -1;
  } else {
    f.brand_i = fs_frame_brand(film->name, f.brand_fg);
  }
  se_frame_layout(&f);
  fs_frame_exposure(&f);
  se_cam_set_frame(&f);
}

static void fs_develop_run(void) {
  if (fs_develop_active)
    return;
  fs_develop_active = 1;
  fs_develop_since = get_ms_clock();

  /*
   * Level 1 has nothing to do here: the stock was written into the picture
   * style slots and the camera baked it into the JPEG it shot. Spending 50 s
   * re-grading that JPEG would apply the look twice, which is the mistake this
   * level exists to avoid.
   */
  if (fs_level() == FS_LEVEL_STYLE) {
    const char *photo1 =
        (dcim_photo_count > 0) ? dcim_photo_list[dcim_photo_index] : "(none)";
    fs_log("develop: level 1 - look is in the captured JPEG, nothing to do");
    fs_screen_take();
    bmp_fill(COLOR_BLACK, 0, 0, 720, 480);
    bmp_draw_rect(COLOR_GREEN2, 12, 60, 696, 150);
    bmp_printf(FONT(FONT_MED_LARGE, COLOR_WHITE, COLOR_BLACK), 30, 80,
               "LEVEL 1 - PICTURE STYLE ONLY");
    bmp_printf(FONT(FONT_MED, COLOR_YELLOW, COLOR_BLACK), 30, 120,
               "The look is already in %s", photo1);
    bmp_printf(FONT(FONT_SMALL, COLOR_WHITE, COLOR_BLACK), 30, 156,
               "Switch Level to 2 or 3 to develop a file.");
    msleep(2500);
    if (!film_sim_queue)
      fs_screen_release();
    fs_develop_active = 0;
    return;
  }

  fs_log("develop: running%s", film_sim_queue ? " (queued)" : "");
  fs_log_flush();

  if (!film_sim_queue)
    fs_screen_take();

  /* Develop can be started from the menu without visiting the darkroom, in
   * which case nothing has walked DCIM yet. */
  if (dcim_photo_count == 0)
    fs_scan_dcim_photos();

  /* Copied, not pointed at: fs_photo_paths() rescans DCIM further down, which
   * rewrites dcim_photo_list and would leave this dangling--and the name is
   * still needed for the marker and the draw after that point. */
  char last_photo[64];
  snprintf(last_photo, sizeof last_photo, "%s",
           (dcim_photo_count > 0) ? dcim_photo_list[dcim_photo_index]
                                  : "IMG_0001.JPG");
  /* Kept in static storage because the screen may be redrawn after this frame
   * has gone. */
  snprintf(fs_dev_photo_buf, sizeof fs_dev_photo_buf, "%s", last_photo);

  int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
  int idx = fs_develop_stock_index(count);
  const film_xml_t *f = &film_active_table[idx];
  const char *soup_str = (film_sim_dev == FS_DEV_PUSH_1)   ? "Push +1 (+1 EV)"
                         : (film_sim_dev == FS_DEV_PUSH_2) ? "Push +2 (+2 EV)"
                         : (film_sim_dev == FS_DEV_PULL_1) ? "Pull -1 (-1 EV)"
                                                           : "Standard C-41";

  /* The screen itself: the same drawing code as the advertising renders. */
  fs_dev_photo = fs_dev_photo_buf;
  fs_dev_stock = f->name;
  fs_dev_brand = f->brand;
  fs_dev_iso = f->iso;
  fs_dev_soup = soup_str;
  fs_dev_result = NULL;
  fs_dev_done = 0;
  /* Clear the outcome of the previous run: the flag is sticky, so a failure
   * would tint every later develop screen red. */
  fs_dev_ok = fs_dev_failed = fs_dev_skipped = 0;
  fs_dev_index = 1;
  fs_dev_total = 0;
  fs_bath_pct = 0;
  snprintf(fs_bath_stage, sizeof fs_bath_stage, "Starting");
  fs_develop_screen();

  /* One place builds the names: the queue, the auto watcher and this screen
   * all develop through fs_photo_paths(), so they cannot disagree about where
   * the result goes. */
  char cr2[FS_PATH_LEN], out[FS_PATH_LEN], tmp[FS_PATH_LEN], stk[FS_PATH_LEN];
  fs_photo_paths(last_photo, cr2, sizeof cr2, out, sizeof out, tmp, sizeof tmp);

  /* Nothing to develop if the raw is not actually there: bail before the
   * animation rather than bathing for ten seconds and claiming success. */
  {
    uint32_t raw_sz = 0;
    if (fs_ends_with(cr2, ".JPG") || fs_ends_with(cr2, ".jpg") ||
        FIO_GetFileSize(cr2, &raw_sz) != 0 || raw_sz == 0) {
      fs_dev_done = 1;
      fs_dev_failed = 1;
      fs_dev_result = "NO RAW (CR2) FOR THIS FRAME";
      fs_develop_screen();
      NotifyBox(2500, "No raw file");
      msleep(2500);
      if (!film_sim_queue)
        fs_screen_release();
      fs_develop_active = 0;
      return;
    }
  }

  /* 1. Idempotence: a marker per source frame, because the output name is a
   * fresh IMG_ number every time and could never serve as its own marker. */
  if (fs_dev_is_done(fs_src_number(last_photo))) {
    fs_dev_done = 1;
    fs_dev_skipped = 1;
    fs_dev_result = "SKIPPED  -  already developed";
    fs_develop_screen();
    NotifyBox(2000, "Already developed");
    msleep(2000);
    if (!film_sim_queue)
      fs_screen_release();
    fs_develop_active = 0;
    return;
  }
  FIO_RemoveFile(tmp);

  if (!fs_resolve_stock_path(f, stk, sizeof(stk))) {
    /* Used to be ignored: stk then held whatever was on the stack and the
     * engine opened a garbage path, which surfaced as a bare "I/O error". */
    fs_dev_done = 1;
    fs_dev_failed = 1;
    {
      static char norecipe[64];
      snprintf(norecipe, sizeof norecipe, "NO RECIPE FOR %s", f->code);
      fs_dev_result = norecipe;
    }
    fs_develop_screen();
    fs_log("develop: no recipe for %s (%s)", f->name, f->code);
    fs_log_flush();
    NotifyBox(2500, "No recipe for %s", f->code);
    msleep(2500);
    if (!film_sim_queue)
      fs_screen_release();
    fs_develop_active = 0;
    return;
  }

  /*
   * Run the develop for real, and let it drive the bar.
   *
   * This used to animate a fixed 100-step, 10-second "chemical bath" and call
   * se_cam_develop() at step 50, so the percentage had nothing to do with the
   * work, the stage names described things the code never does ("Loading Bayer
   * CFA Plane", "Tetrahedral 3D LUT Color Grading"), and the screen froze for
   * however long develop actually took. The engine reports Opening / Stock /
   * Decoding / Grading / Done with a real percentage.
   */
  se_err dev_err = SE_OK;
  int dev_attempted = 1;

  /* No bar to drive when nobody is looking at the screen. */
  fs_bath_ui = film_sim_queue ? 0 : 1;
  fs_bath_pct = 0;
  snprintf(fs_bath_stage, sizeof fs_bath_stage, "Starting");
  /* The engine's single allocation is ~680 KB; this says what the body had
   * before and after it, so "use more RAM" can be answered with numbers
   * instead of a datasheet. */
  fs_log("develop: heap free before %d KB, largest block %d KB",
         (int)(GetFreeMemForAllocateMemory() / 1024),
         (int)(GetFreeMemForMalloc() / 1024));
  fs_log("develop: %s + %s -> %s (via %s)", cr2, stk, out, tmp);
  uint32_t t0 = get_ms_clock();
  se_cam_set_aspect(film_sim_aspect);
  se_cam_set_pad32(film_sim_pad);
  fs_frame_apply(stk);
  se_cam_set_strength(film_sim_strength);
  se_cam_set_satboost(film_sim_satboost);
  se_cam_set_grainsize(film_sim_grainsize);
  fs_yuvlook_set_satboost(film_sim_satboost);
  if (fs_level() == FS_LEVEL_RAW) {
    dev_err = se_cam_develop_raw(cr2, stk, tmp, 2, fs_bath_progress,
                                 fs_develop_note, NULL);
    if (dev_err != SE_OK) {
      fs_log("develop: raw develop failed (%d: %s), falling back to preview "
             "develop",
             (int)dev_err, se_strerror(dev_err));
      dev_err = se_cam_develop(cr2, stk, tmp, fs_bath_progress, fs_develop_note,
                               NULL);
    }
  } else {
    dev_err =
        se_cam_develop(cr2, stk, tmp, fs_bath_progress, fs_develop_note, NULL);
  }
  uint32_t dt = get_ms_clock() - t0;
  fs_log("develop: battery %d%% before this develop", GetBatteryLevel());
  fs_log("develop: level %d returned %d (%s) in %d ms", film_sim_level,
         (int)dev_err, se_strerror(dev_err), (int)dt);

  /*
   * A raw payload the decoder does not recognise is a real failure and the
   * FAILED message below says so. That used to be every mRAW frame; the
   * subsampled decoder now handles them, so there is no special case left.
   */
  fs_bath_ui = 0;
  fs_bath_show_progress(100, dev_err == SE_OK ? "Done" : se_strerror(dev_err));
  fs_log_flush();

  info_led_off();
  beep_times(2);

  /* Finalize atomic file commit */
  if (dev_attempted) {
    if (dev_err == SE_OK) {
      if (fs_commit(tmp, out) == 0) {
        const char *leaf = strrchr(out, '/');
        snprintf(fs_last_dev, sizeof fs_last_dev, "%s", out);
        fs_dev_mark_done(fs_src_number(last_photo));
        fs_log("develop: committed %s", out);
        NotifyBox(3000, "Film: %s", leaf ? leaf + 1 : out);
      } else {
        FIO_RemoveFile(tmp);
        fs_log("develop: rename %s -> %s failed", tmp, out);
      }
    } else {
      FIO_RemoveFile(tmp);
      fs_log("develop: failed (%d: %s)", dev_err, se_strerror(dev_err));
    }
  }

  fs_dev_done = 1;
  if (dev_attempted && dev_err == SE_OK) {
    static char okmsg[64];
    const char *leaf = strrchr(out, '/');
    snprintf(okmsg, sizeof okmsg, "DONE  -  %s", leaf ? leaf + 1 : out);
    fs_dev_result = okmsg;
  } else {
    static char badmsg[64];
    snprintf(badmsg, sizeof badmsg, "FAILED  -  %s", se_strerror(dev_err));
    fs_dev_failed = 1;
    fs_dev_result = badmsg;
  }
  fs_develop_screen();

  fs_develop_active = 0;
  fs_log("develop: finished");
  fs_log_flush();
  /* Looking at the result is the next thing anyone wants, and the body's
   * playback does not list what this module writes -- so show it here, and
   * hand the panel over directly: releasing it and taking it again would go
   * back to shooting mode and pay for the transition twice. */
  if (dev_attempted && dev_err == SE_OK) {
    fs_want_playback = 0;
    if (film_sim_queue) {
      /* Showing it would take the panel and lock the shutter, which is the
       * whole thing queue mode exists to avoid. */
      fs_log("develop: %s developed, not shown (queued; shutter stays live)",
             out);
      fs_log_flush();
    } else {
      fs_view_run(out, 1);
    }
  } else {
    msleep(1500);
    if (!film_sim_queue)
      fs_screen_release();
  }
}

static MENU_SELECT_FUNC(fs_develop_trigger) { fs_ui_launch(FS_VIEW_DEVELOP); }

static MENU_SELECT_FUNC(fs_creator_trigger) { fs_ui_launch(FS_VIEW_CREATOR); }

static MENU_SELECT_FUNC(fs_wb_trigger) { fs_ui_launch(FS_VIEW_WB); }

/* ---------------------------------------------------------------- key handler
 */

/* ---------------------------------------------------------------- ui task
 *
 * Tests run = one task, one screen owner. The menu handlers and the key handler
 * only publish what should happen next here; nothing else creates tasks.
 */
/* ------------------------------------------------------- color creator
 *
 * A full-screen editor, not a HUD over LiveView: the LV plane is 1440x480 of
 * uncached memory and the display filter already costs 89 ms a frame, so a
 * screen that redraws on a key press is the only one that can keep up. What
 * OM shows behind the wheel is the live scene; what this shows is the wheel.
 */

/*
 * Redraw into the idle buffer and copy back only the ring's rows.
 *
 * fs_paint() copies all 480 rows in and out; a spin frame changes 12 small
 * squares inside the disc, so paying that twice per frame is what made the
 * screen feel like a still. The idle buffer already holds the previous frame,
 * which is the same property that lets the shelf blit a band.
 */
static void fs_paint_beads(void (*fn)(void)) {
  uint32_t t0 = get_ms_clock();
  fs_frame_prep();
  BMP_LOCK(bmp_draw_to_idle(1); fn(); bmp_draw_to_idle(0);
           fs_blit_rows(1, FSG_CREATOR_RING_Y0, FSG_CREATOR_RING_Y1);)
  ml_refresh_display_needed = 1;
  fs_paint_account(get_ms_clock() - t0,
                   FSG_CREATOR_RING_Y1 - FSG_CREATOR_RING_Y0);
}

/* ---- the four wheels ------------------------------------------------- */

/* Defined further down, next to the menu rows that use them. */
static void fs_slot_file_name(int slot, char *out, int cap);
static int  fs_slot_default(void);
static void fs_creator_scan_slots(void);

static char fs_slot_label_buf[FS_CREATOR_SLOTS][SE_STOCK_NAME];

/*
 * Read one slot's recipe into the creator state.
 *
 * The writer stores twelve hue values per line and derives its CURVE from
 * (shadow, highlight), so the two tone handles are recovered by searching the
 * same function the writer used rather than by inventing keys the engine would
 * complain about. Everything the parsed stock does not carry -- halation is not
 * in se_stock -- is read from the text directly.
 */
static int fs_creator_open_slot(int slot)
{
  char path[64];
  char text[4096];
  se_stock st;
  size_t got = 0;
  int i, sh, hi, best_sh = 0, best_hi = 0, best_err = 1 << 30;
  FILE *f;

  if (slot < 0 || slot >= FS_CREATOR_SLOTS) return 0;
  fs_slot_file_name(slot, path, sizeof path);        /* into path as a name */
  {
    char file[12];
    snprintf(file, sizeof file, "%s", path);
    snprintf(path, sizeof path, "ML/FILM/%s.fs", file);
  }

  f = FIO_OpenFile(path, O_RDONLY);
  if (!f) {
    /* Nothing there: a blank wheel on that slot. */
    for (i = 0; i < FS_CREATOR_WEDGES; i++) {
      fs_creator.sat[i] = 100;
      fs_creator.rot[i] = 0;
      fs_creator.lum[i] = 100;
    }
    fs_creator.shadow = fs_creator.highlight = 0;
    fs_log("slots: %s does not exist, slot %d starts blank", path, slot);
    return 0;
  }
  got = FIO_ReadFile(f, text, sizeof text - 1);
  FIO_CloseFile(f);
  if (got <= 0) return 0;
  text[got] = 0;

  if (se_stock_parse(text, got, &st) != SE_OK || !st.name[0]) {
    fs_log("slots: %s did not parse, slot left alone", path);
    fs_log_flush();
    return 0;
  }

  for (i = 0; i < FS_CREATOR_WEDGES && i < SE_HUE_SECTORS; i++) {
    fs_creator.sat[i] = (int)(st.hue_sat[i] + (st.hue_sat[i] >= 0 ? 0.5f : -0.5f));
    fs_creator.rot[i] = (int)(st.hue_rot[i] + (st.hue_rot[i] >= 0 ? 0.5f : -0.5f)) ;
    fs_creator.lum[i] = (int)(st.hue_lum[i] + (st.hue_lum[i] >= 0 ? 0.5f : -0.5f));
  }
  fs_creator.grain = (int)(st.grain + 0.5f);
  fs_creator.grain_size = (int)(st.grain_size + 0.5f);
  fs_creator.chrome = st.color_chrome;
  fs_creator.fx_blue = st.fx_blue;

  /* Tone: the curve in the file was produced by tone_out() at five points. */
  for (sh = -60; sh <= 60; sh += 2)
    for (hi = -60; hi <= 60; hi += 2) {
      int err = 0, k;
      for (k = 0; k < st.master.n && err < best_err; k++) {
        int in = st.master.x[k];
        int want = st.master.y[k];
        int t = in * 100 / 255;
        int got_v = in + (sh * (100 - t) * (100 - t)) / 10000
                       - (hi * t * t) / 10000;
        if (got_v < 0) got_v = 0;
        if (got_v > 255) got_v = 255;
        err += (got_v - want) * (got_v - want);
      }
      if (err < best_err) { best_err = err; best_sh = sh; best_hi = hi; }
    }
  fs_creator.shadow = best_sh;
  fs_creator.highlight = best_hi;

  /* HALATION is not in se_stock; take the first number of its line. */
  {
    const char *h = strstr(text, "HALATION");
    fs_creator.halation = h ? atoi(h + 8) : 0;
  }

  /* The name travels with the slot: fs_look_name is the module's own buffer
   * and is declared after this point, so the label array carries it. */
  snprintf(fs_slot_label_buf[slot], sizeof fs_slot_label_buf[slot], "%s",
           st.name);
  fs_creator.look_name = fs_slot_label_buf[slot];
  /* The slot the menu has selected is the one this screen writes; the labels
   * come from the four files on the card so the slot screen can say what each
   * one holds instead of just ML/FILM/WHEEL2.fs. */
  fs_creator.slot_cur = fs_slot_default();
  fs_creator.slot_pick = fs_creator.slot_cur;
  fs_creator_scan_slots();
  fs_creator.slot_label[slot] = fs_slot_label_buf[slot];
  fs_log("slots: loaded slot %d from %s as '%s' (tone %d/%d, curve err %d)",
         slot, path, st.name, best_sh, best_hi, best_err);
  fs_log_flush();
  return 1;
}

/* What is in each slot, for the slot screen. Names only: opening four files
 * completely to print a label would be work for nothing. */
static void fs_creator_scan_slots(void)
{
  int slot;
  for (slot = 0; slot < FS_CREATOR_SLOTS; slot++) {
    char path[64], text[1024];
    FILE *f;
    size_t got;
    char file[12];

    fs_slot_file_name(slot, file, sizeof file);
    snprintf(path, sizeof path, "ML/FILM/%s.fs", file);
    f = FIO_OpenFile(path, O_RDONLY);
    fs_creator.slot_label[slot] = NULL;
    if (!f) continue;
    got = FIO_ReadFile(f, text, sizeof text - 1);
    FIO_CloseFile(f);
    if (got <= 0) continue;
    text[got] = 0;
    {
      const char *n = strstr(text, "NAME");
      if (n) {
        char nm[SE_STOCK_NAME];
        int k = 0;
        n += 4;
        while (*n == ' ') n++;
        while (*n && *n != '\n' && *n != '\r' && k < (int)sizeof nm - 1)
          nm[k++] = *n++;
        nm[k] = 0;
        while (k > 0 && nm[k - 1] == ' ') nm[--k] = 0;
        if (k) {
          snprintf(fs_slot_label_buf[slot], sizeof fs_slot_label_buf[slot],
                   "%s", nm);
          fs_creator.slot_label[slot] = fs_slot_label_buf[slot];
        }
      }
    }
  }
}

static void fs_creator_render(void)
{
  if (fs_creator.screen == 0) {
    fsg_draw_creator(&fs_creator);
    /* fsg_draw_creator() drew the ring at the settled offset; tell the spin
     * that is where the beads now are */
    fs_creator_spin.valid = 0;
    if (fs_creator_spring.x != fs_creator.sel * 65536)
      fsg_draw_creator_beads(&fs_creator, fs_creator_spring.x, &fs_creator_spin);
  } else if (fs_creator.screen == 2) {
    fsg_draw_fx(&fs_creator);
  } else if (fs_creator.screen == 3) {
    fsg_draw_slots(&fs_creator);
  } else {
    fsg_draw_tone(&fs_creator);
  }
}

/* One animation frame: advance the spring and redraw the beads if it moved. */
/* the draw has to happen INSIDE the lock, with the idle buffer selected */
static void fs_creator_draw_beads(void)
{
  fsg_draw_creator_beads(&fs_creator, fs_creator_spring.x, &fs_creator_spin);
}

static void fs_creator_anim(void)
{
  const int turn = FS_CREATOR_WEDGES * 65536;
  int target = fs_creator.sel * 65536;
  while (target - fs_creator_spring.x > turn / 2)
    fs_creator_spring.x += turn;
  while (fs_creator_spring.x - target > turn / 2)
    fs_creator_spring.x -= turn;
  if (fs_spring_step(&fs_creator_spring, target, 30))
    fs_paint_beads(fs_creator_draw_beads);
}

/* Runs with the panel released. Formats the recipe off the key path. */
/* ---- naming a look on camera ------------------------------------------ */
/*
 * ML has no text-entry widget: `edit_mode` only shapes how a NUMBER is
 * rounded. So a name is spelled out one character per menu row, the way the
 * rest of this module does everything else -- SET advances, Q goes back. The
 * alphabet is A-Z, 0-9 and '-' because the file lands on a FAT card as
 * ML/FILM/<NAME>.fs and 8.3 is what every other stock already uses.
 */
#define FS_NAME_LEN 8
static const char *fs_name_chars[] = {
  "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "-",
};
#define FS_NAME_N ((int)(sizeof fs_name_chars / sizeof fs_name_chars[0]))

/* fs_slot_names[] is a MENU LABEL ("WHEEL (1)"); the file is WHEEL.fs. Those
 * are not the same string and using the label as a name wrote "WHEEL (1". */
static void fs_slot_file_name(int slot, char *out, int cap)
{
  if (slot <= 0) snprintf(out, cap, "WHEEL");
  else           snprintf(out, cap, "WHEEL%d", slot + 1);
}

static int fs_slot_default(void);
static MENU_SELECT_FUNC(fs_name_reset);

static int fs_name_len = 4;
/* "LOOK": L O O K */
static int fs_name_pos[FS_NAME_LEN] = { 11, 14, 14, 10, 0, 0, 0, 0 };
static char fs_look_name[FS_NAME_LEN + 1] = "LOOK";

static void fs_name_sync(void)
{
  int i;
  if (fs_name_len < 1) fs_name_len = 1;
  if (fs_name_len > FS_NAME_LEN) fs_name_len = FS_NAME_LEN;
  for (i = 0; i < fs_name_len; i++) {
    int c = fs_clamp_i(fs_name_pos[i], 0, FS_NAME_N - 1);
    fs_look_name[i] = fs_name_chars[c][0];
  }
  fs_look_name[fs_name_len] = 0;
}

static MENU_SELECT_FUNC(fs_slot_cycle) {
  fs_creator_slot = (fs_slot_default() + (delta < 0 ? 3 : 1)) & 3;
  fs_name_reset(NULL, 0);
}

static MENU_UPDATE_FUNC(fs_slot_update) {
  char file[12];
  fs_slot_file_name(fs_slot_default(), file, sizeof file);
  MENU_SET_VALUE("%s -> %s.fs", fs_slot_names[fs_slot_default()], file);
}

static MENU_UPDATE_FUNC(fs_name_char_update) {
  int c = fs_clamp_i(*(int *)entry->priv, 0, FS_NAME_N - 1);
  fs_name_sync();
  MENU_SET_VALUE("%s", fs_name_chars[c]);
}

static MENU_UPDATE_FUNC(fs_name_len_update) {
  fs_name_sync();
  MENU_SET_VALUE("%d of %d", fs_name_len, FS_NAME_LEN);
}

static MENU_UPDATE_FUNC(fs_name_show_update) {
  fs_name_sync();
  MENU_SET_VALUE("%s.fs", fs_look_name);
}

static int fs_slot_default(void) { return fs_clamp_i(fs_creator_slot, 0, 3); }

static MENU_SELECT_FUNC(fs_creator_reset_look_action) {
  (void)priv; (void)delta;
  fs_creator_initialised = 0;
  NotifyBox(1200, "Creator resets on next open");
}

static MENU_SELECT_FUNC(fs_name_reset) {
  int i;
  char src[12];
  (void)delta;
  fs_slot_file_name(fs_slot_default(), src, sizeof src);
  for (i = 0; i < FS_NAME_LEN; i++) fs_name_pos[i] = 6;
  for (i = 0; i < FS_NAME_LEN && src[i]; i++) {
    int k;
    for (k = 0; k < FS_NAME_N; k++)
      if (fs_name_chars[k][0] == src[i]) { fs_name_pos[i] = k; break; }
  }
  fs_name_len = i;
  fs_name_sync();
}

#define NAME_CHAR_ROW(n)                                                       \
    {                                                                          \
        .name = "Char " #n,                                                    \
        .priv = &fs_name_pos[(n) - 1],                                          \
        .max = FS_NAME_N - 1,                                                  \
        .choices = fs_name_chars,                                              \
        .update = fs_name_char_update,                                         \
        .help = "SET: next character, Q: previous. A-Z, 0-9 and '-'.",          \
    },

static struct menu_entry fs_menu_name[] = {
    {
        .name = "Name",
        .update = fs_name_show_update,
        .help = "What the look is called: ML/FILM/<NAME>.fs. Eight characters "
                "at most, because that is the 8.3 limit every stock on the "
                "card already uses.",
    },
    {
        .name = "Length",
        .priv = &fs_name_len,
        .min = 1, .max = FS_NAME_LEN,
        .update = fs_name_len_update,
        .help = "How many characters of the name are used.",
    },
    NAME_CHAR_ROW(1)
    NAME_CHAR_ROW(2)
    NAME_CHAR_ROW(3)
    NAME_CHAR_ROW(4)
    NAME_CHAR_ROW(5)
    NAME_CHAR_ROW(6)
    NAME_CHAR_ROW(7)
    NAME_CHAR_ROW(8)
    {
        .name = "Reset Look",
        .select = fs_creator_reset_look_action,
        .icon_type = IT_ACTION,
        .help = "Start the Color Creator from flat again. Leaving the screen "
                "keeps your work; this is the only thing that discards it.",
    },
    {
        .name = "Use Slot Name",
        .select = fs_name_reset,
        .icon_type = IT_ACTION,
        .help = "Reset the name to WHEEL / WHEEL2..4, the slot the creator "
                "saves into.",
    },
    MENU_EOL,
};

/*
 * A saved .fs is invisible until something names it: the shelf is built from
 * ML/FILMS XML entries only (fs_load_films_xml never scans ML/FILM), so a look
 * written as LOOK.fs existed on the card and could not be chosen. Writing the
 * companion XML is what puts it on the shelf, and it survives a reboot.
 */
static void fs_register_look(const char *file, const char *label,
                             const char *desc)
{
  film_xml_t f;
  char path[64];

  if (!file || !file[0]) return;
  memset(&f, 0, sizeof f);
  /* code is how fs_resolve_stock_path() finds ML/FILM/<code>.fs, so it has to
   * be the file name; name is what the shelf prints and what a user
   * recognises. They are different on purpose. */
  snprintf(f.code, sizeof f.code, "%s", file);
  snprintf(f.name, sizeof f.name, "%s",
           (label && label[0]) ? label : file);
  snprintf(f.brand, sizeof f.brand, "User");
  snprintf(f.type, sizeof f.type, "Created");
  snprintf(f.description, sizeof f.description, "%s", desc ? desc : "MADE IN COLOR CREATOR");
  f.base = 0x84;        /* Neutral: the look lives in the .fs, not the style */
  f.sharpness = 3;
  /* One XML per slot: re-saving a slot overwrites its entry instead of
   * leaving a second one behind under the previous name. */
  snprintf(path, sizeof path, FILM_DIR "/90_%s.xml", file);
  fs_write_film_xml(path, &f);
  fs_log("look: registered %s ('%s') as %s", file, f.name, path);
  fs_log_flush();
}


static int fs_watch_hits;

/* ------------------------------------------------- recipe inbox watcher
 *
 * The transport is somebody else's problem: an Eye-Fi card uploads, a card
 * reader copies, a phone with an SD slot writes. What none of them can do is
 * tell the module that a file appeared -- fs_load_films_xml() runs at boot and
 * after a save, so a recipe dropped on the card needs a reboot before it can
 * be shot. This is that missing half.
 *
 * It exists because the 6D cannot do better. strings on the firmware's own
 * ROM1.BIN finds Eye-Fi menus and status icons and no WLAN stack: this body
 * has no socket API at all -- 6D_116.sym has zero wlan or socket symbols --
 * so there is nothing in ML to call. The card is the interface.
 *
 * The rule is the same one fs_auto_task uses for a new frame -- a file is
 * only touched once its size has been the same across two polls -- so a
 * half-written .fs is never parsed.
 */
/* Hash of "every .fs in ML/FILM, name and size". Cheap, and it changes when a
 * file is added, removed, renamed or rewritten. */
static unsigned int fs_film_dir_sig(int *count_out)
{
  struct fio_file *file = alloc_fio_file();
  struct fio_dirent *dirent;
  unsigned int sig = 2166136261u;
  int n = 0;

  /* -1 means "could not read it", which is NOT the same as "no files":
   * the card's filesystem is not mounted for the first seconds after boot,
   * and a corrupt directory can also fail here. Reporting 0 for either makes
   * the watcher believe every recipe was deleted. */
  if (count_out) *count_out = -1;
  if (!file) return 0;
  dirent = FIO_FindFirstEx("ML/FILM", file);
  if (IS_ERROR(dirent)) { free(file); return 0; }

  do {
    struct file_info fi = convert_fio_file_info(file);
    int len;
    if (fi.mode & ATTR_DIRECTORY) continue;
    len = strlen(fi.name);
    if (len < 4) continue;
    if (strcasecmp(fi.name + len - 3, ".fs") != 0) continue;
    {
      char full[96];
      uint32_t size = 0;
      int k;
      snprintf(full, sizeof full, "ML/FILM/%s", fi.name);
      FIO_GetFileSize(full, &size);
      for (k = 0; fi.name[k]; k++)
        sig = (sig ^ (unsigned char)fi.name[k]) * 16777619u;
      sig = (sig ^ size) * 16777619u;
      n++;
    }
  } while (FIO_FindNextEx(dirent, file) == 0);

  /* Every other directory walk in this file closes its handle. This one did
   * not, and the watcher calls it every 3 seconds: one leaked DryOS fio_dirent
   * per 3 s until the handle table is full, after which every FIO call
   * camera-wide fails. */
  FIO_FindClose(dirent);
  free(file);
  if (count_out) *count_out = n;
  return sig;
}

static void fs_film_watch_task(void *unused)
{
  unsigned int known = 0;
  unsigned int pending = 0;

  /* the catalogue was just loaded, so this is the state on disk */
  known = fs_film_dir_sig(NULL);   /* no count: this runs before the card is
                                    * necessarily mounted, and a false "0"
                                    * here would be reported as fact */

  while (1) {
    unsigned int sig;
    msleep(3000);

    /* never reload the table out from under a screen that is reading it */
    if (fs_gui_active || fs_darkroom_active || fs_creator_active || fs_wb_active)
      continue;

    {
      /* If the directory cannot be read, say nothing and try again: the card
       * may not be mounted yet, and a replace-everything reload on a false
       * reading is worse than waiting three more seconds. */
      int n = 0;
      unsigned int probe = fs_film_dir_sig(&n);
      if (n < 0) {
        static int told;
        if (!told) {
          told = 1;
          fs_log("watch: ML/FILM not readable yet, will keep trying");
          fs_log_flush();
        }
        continue;
      }
      sig = probe;
    }
    if (sig != pending) {
      /* changed since last poll: not stable yet, wait for the next one */
      pending = sig;
      fs_watch_hits = 0;
      continue;
    }
    if (++fs_watch_hits < 2)
      continue;
    if (sig == known)
      continue;     /* stable, but nothing new */

    fs_watch_hits = 0;
    known = sig;
    /*
     * Remember the active stock BY NAME. fs_load_films_xml() rebuilds the table
     * from a directory listing, so an index that was valid before the reload
     * can point at a different film afterwards -- dropping FILM/AAA.fs in
     * front of the current stock would silently change what the camera is
     * shooting, with nothing on screen to say so.
     */
    {
      char was[40];
      int was_idx = (film_active_count > 0)
                  ? fs_clamp_i(film_sim_stock, 0, film_active_count - 1) : -1;
      was[0] = 0;
      if (was_idx >= 0) snprintf(was, sizeof was, "%s", film_active_table[was_idx].name);

      fs_log("watch: ML/FILM changed, reloading the catalogue");
      fs_log_flush();
      fs_load_films_xml();
      fs_group_by_artwork();

      if (was[0] && film_active_count > 0) {
        int i, found = -1;
        for (i = 0; i < film_active_count; i++)
          if (!strcasecmp(film_active_table[i].name, was)) { found = i; break; }
        if (found >= 0 && found != was_idx) {
          film_sim_stock = found;
          fs_log("watch: '%s' moved %d -> %d, followed it", was, was_idx, found);
          fs_log_flush();
        } else if (found < 0) {
          fs_log("watch: '%s' is no longer on the card, stock index reset", was);
          fs_log_flush();
          film_sim_stock = fs_clamp_i(film_sim_stock, 0, film_active_count - 1);
        }
      }
    }
    {
      int n = 0;
      fs_film_dir_sig(&n);
      fs_log("watch: reloaded, %s recipe(s) on the card, %d stocks loaded",
             n < 0 ? "unreadable" : "counted", film_active_count);
      if (n >= 0)
        fs_log("watch: %d .fs files in ML/FILM", n);
      fs_log_flush();
    }
  }
}


/* ------------------------------------------------ sharing a look off the card
 *
 * "Transfer the recipe over WiFi" has two halves and only one of them is
 * blocked. Inbound already works: fs_film_watch_task() adopts any .fs that
 * appears in ML/FILM. Outbound is the blocked half -- Canon's stack is not
 * exported, and docs/wifi-functions.md records how far that got.
 *
 * What a module CAN do today is put the look somewhere a transport can find
 * it. ML/SHARE is that place: one flat folder, the recipe under its own name,
 * plus a readable summary, so a card reader, a phone with an SD slot, or
 * Canon's desktop utility can pull it without knowing this module exists.
 *
 * It is deliberately NOT DCIM: Canon's file listing is object-shaped (PTP),
 * and dropping a .fs among the photos would put a non-image where the camera
 * expects images. Keeping it in ML/SHARE means the card stays a card.
 */
#define FS_SHARE_DIR "ML/SHARE"

static void fs_share_look_task(void *unused)
{
  char src[FS_PATH_LEN];
  char dst[80], txt[80];
  char name[40];
  uint32_t size = 0;
  int n = 0;
  int len;

  { static int ok; fs_ensure_dir(FS_SHARE_DIR, &ok); }

  if (!fs_active_stock_path(src, sizeof src, NULL)) {
    fs_log("share: no active stock, nothing to share");
    fs_log_flush();
    NotifyBox(1800, "Share: no active stock");
    return;
  }

  /* the recipe's own name, so the file that arrives is self-describing */
  {
    const char *leaf = strrchr(src, '/');
    leaf = leaf ? leaf + 1 : src;
    snprintf(name, sizeof name, "%s", leaf);
    /* The leaf is a recipe file, so it already ends in ".fs"; appending
     * another produced "ML/SHARE/portra.fs.fs". */
    len = (int)strlen(name);
    if (len > 3 && !strcasecmp(name + len - 3, ".fs"))
      name[len - 3] = 0;
  }

  {
    FILE *in = FIO_OpenFile(src, O_RDONLY | O_SYNC);
    FILE *out;
    char buf[512];
    int got;
    if (!in) {
      fs_log("share: cannot open %s", src);
      fs_log_flush();
      NotifyBox(1800, "Share: cannot read the recipe");
      return;
    }
    snprintf(dst, sizeof dst, FS_SHARE_DIR "/%s.fs", name);
    out = FIO_CreateFile(dst);
    if (!out) {
      FIO_CloseFile(in);
      fs_log("share: cannot create %s", dst);
      fs_log_flush();
      NotifyBox(1800, "Share: cannot write ML/SHARE");
      return;
    }
    {
      int wrote_all = 1;
      while ((got = FIO_ReadFile(in, buf, sizeof buf)) > 0) {
        if (FIO_WriteFile(out, buf, got) != got) { wrote_all = 0; break; }
        n += got;
      }
      FIO_CloseFile(in);
      FIO_CloseFile(out);
      if (!wrote_all) {
        FIO_RemoveFile(dst);      /* never leave a half recipe behind */
        fs_log("share: write of %s failed after %d bytes", dst, n);
        fs_log_flush();
        NotifyBox(1800, "Share: write failed");
        return;
      }
    }
    FIO_GetFileSize(dst, &size);
  }

  /* a plain-text note beside it: the same look, readable without this module */
  snprintf(txt, sizeof txt, FS_SHARE_DIR "/%s.txt", name);
  {
    FILE *f = FIO_CreateFile(txt);
    if (f) {
      char line[200];
      int m = snprintf(line, sizeof line,
          "Film simulation recipe shared from Magic Lantern film_sim.\n"
          "File:    ML/SHARE/%s.fs (%d bytes)\n"
          "Source:  %s\n"
          "Use:     copy the .fs into ML/FILM on another camera; the watch task\n"
          "         adopts it within a few seconds, no reboot.\n"
          "Look:    selected with Film Dev > Film Look > Stock, or 12 wedge\n"
          "         values in Color Creator (Custom Look).\n",
          name, (int)size, src);
      if (m > 0) FIO_WriteFile(f, line, m);
      FIO_CloseFile(f);
    }
  }

  fs_log("share: wrote %s (%d bytes) and %s", dst, n, txt);
  fs_log_flush();
  NotifyBox(2000, "Shared %s", name);
}

static void fs_share_look_trigger(void *priv, int delta)
{
  (void)priv; (void)delta;
  task_create("fs_share", 0x1e, 0x2000, fs_share_look_task, 0);
}

static void fs_creator_save_task(void *unused) {
  char buf[2048];
  char path[64];
  char file[12];      /* the slot's file: WHEEL .. WHEEL4 */
  char label[16];     /* what the shelf will call it     */
  int n;

  /*
   * The slot owns the file name and the typed name is only a label.
   *
   * They used to be the same string, and that is what broke Apply WHEEL: the
   * look was written as ML/FILM/<typed>.fs while fs_user_look_apply() looked
   * for a stock called WHEEL, so a look saved under any other name was correct
   * on the card and unreachable from the menu for ever after. The file is now
   * always the slot, and the name rides in the XML where the shelf reads it.
   */
  fs_slot_file_name(fs_slot_default(), file, sizeof file);
  if (fs_look_name[0]) snprintf(label, sizeof label, "%s", fs_look_name);
  else                 snprintf(label, sizeof label, "%s", file);

  fs_log("creator: save task started (slot %s, name %s)", file, label);
  fs_log_flush();

  n = fs_creator_write(&fs_creator, buf, sizeof buf, label);
  if (n < 0) {
    NotifyBox(1800, "Creator: recipe did not fit");
    fs_log("creator: format failed");
    fs_log_flush();
    fs_creator.save_err = 1;
    fs_creator_dirty = 1;
    fs_creator_saving = 0;
    return;
  }
  snprintf(path, sizeof path, "ML/FILM/%s.fs", file);
  { static int ok; fs_ensure_dir("ML/FILM", &ok); }
  {
    FILE *f = FIO_CreateFile(path);
    int wrote = f ? FIO_WriteFile(f, buf, (size_t)n) : -1;
    if (f) FIO_CloseFile(f);
    if (wrote != n) {
      NotifyBox(1800, "Creator: write failed");
      fs_log("creator: writing '%s' gave %d of %d bytes", path, wrote, n);
      fs_log_flush();
      fs_creator.save_err = 1;
      fs_creator_dirty = 1;
      /* Without this the flag stays set and every later save is refused by
       * fs_creator_save_trigger's `if (!fs_creator_saving)`, silently, until
       * the camera is restarted. */
      fs_creator_saving = 0;
      return;
    }
  }
  fs_log("creator: wrote %s (%d bytes, %d wedges, name %s)", path, n,
         FS_CREATOR_WEDGES, label);
  fs_log_flush();
  fs_register_look(file, label, "MADE IN COLOR CREATOR");

  /*
   * Report success BEFORE rebuilding the catalogue.
   *
   * The bytes are on the card at this point -- that is what "saved" means. The
   * two reloads below walk ML/FILMS (85 XML files) and the artwork directory,
   * which is seconds on this card, and they used to run first: the user waited
   * through a full catalogue rebuild before the screen said SAVED, which is
   * exactly "saving is slow".
   *
   * No NotifyBox: the creator repaints the whole frame from its own loop and
   * would erase it. The screen draws this itself, and keeps it until the next
   * value change.
   */
  snprintf(fs_creator_saved_name, sizeof fs_creator_saved_name, "%s", file);
  fs_creator.saved_name = fs_creator_saved_name;
  fs_creator.saved = 1;
  fs_creator.save_err = 0;
  beep_times(1);
  fs_creator.dirty = 0;
  fs_creator_dirty = 1;   /* repaint with SAVED showing */

  /* Now the slow part, behind the badge, so the shelf has the new stock
   * without a reboot. Timed, because "it feels slow" needs a number. */
  {
    uint32_t t0 = get_ms_clock();
    fs_load_films_xml();
    uint32_t t1 = get_ms_clock();
    fs_group_by_artwork();
    fs_log("creator: catalogue reload %d ms, artwork %d ms (%d stocks)",
           (int)(t1 - t0), (int)(get_ms_clock() - t1), film_active_count);
    fs_log_flush();
  }
  /* only now is it safe for another save: the catalogue the shelf reads is
   * consistent again, and a second save cannot race this one. */
  fs_creator_saving = 0;
}

static int fs_creator_run(void) {
  if (fs_gui_active || fs_creator_active)
    return FS_VIEW_NONE;

  fs_keep_panel(); /* see its comment: restores a wiped idle frame */

  fs_log("creator: running");
  fs_log_flush();
  beep_times(1);

  /* Reopening the screen used to call fs_creator_defaults() every time, which
   * threw away twelve wedges of work with no warning. The look now survives
   * leaving the screen; User Look > Reset Look is how it starts over.
   *
   * The subtitle says FLAT because that is what the recipe is: fs_creator_write
   * emits its own identity curve and neutral wheel, WB AS_SHOT -- nothing of a
   * stock is carried in. It used to read "BASE: CLASSIC CHROME", which named a
   * stock the saved .fs does not contain. */
  if (!fs_creator_initialised) {
    fs_creator_defaults(&fs_creator, "BASE: FLAT - SELF-CONTAINED");
    fs_creator_initialised = 1;
  }
  fs_name_sync();
  /* The screen prints the slot's file next to RECIPE SLOT -- so it must be the
   * slot's file, not the typed name. Both are shown now: the file it will
   * write, and the name the shelf will use for it. */
  {
    static char slot_file[12];
    fs_slot_file_name(fs_slot_default(), slot_file, sizeof slot_file);
    fs_creator.slot_name = slot_file;
  }
  fs_creator.look_name = fs_look_name;
  /* start settled: a spring beginning at 0 would spin the whole ring into
   * place every time the screen opens */
  fs_creator_spring.x = 0;
  fs_creator_spring.v = 0;
  fs_creator_spring.rem = 0;
  fs_creator_spin.valid = 0;
  fs_creator_anim_ms = get_ms_clock();
  fs_creator.sel = 0;
  fs_creator.field = 0;
  fs_creator.screen = 0;

  fs_screen_take();
  fs_creator_active = 1;
  fs_creator_dirty = 1;
  fs_creator_exit = 0;
  fs_creator_save = 0;

  unsigned kc0 = fs_key_count;
  uint32_t t0 = get_ms_clock();

  while (!fs_creator_exit) {
    if (fs_creator_dirty) {
      fs_creator_dirty = 0;
      fs_paint(fs_creator_render);
    }
    /*
     * The ring turns on a 30 ms clock, the same one the shelf scrolls on: a
     * key press moves the TARGET and the ring springs to it, so a quick turn
     * of the dial piles into one glide instead of jumping 30 degrees a press.
     * Nothing repaints once the spring settles.
     */
    /* A slot was picked on the slot screen: load it here, not in the key
     * handler, because it is a file read. */
    if (fs_creator.slot_request) {
      fs_creator.slot_request = 0;
      fs_creator.slot_cur = fs_creator.slot_pick;
      fs_creator_slot = fs_creator.slot_cur;
      {
        char file[12];
        static char slot_file_buf[12];
        fs_slot_file_name(fs_creator_slot, slot_file_buf, sizeof slot_file_buf);
        snprintf(file, sizeof file, "%s", slot_file_buf);
        fs_creator.slot_name = slot_file_buf;
      }
      fs_creator_open_slot(fs_creator.slot_cur);
      fs_creator.dirty = 0;
      fs_creator.saved = 0;
      fs_creator_dirty = 1;
    }
    if (fs_creator.screen == 0 &&
        get_ms_clock() - fs_creator_anim_ms >= 30) {
      fs_creator_anim_ms = get_ms_clock();
      fs_creator_anim();
    }
    if (fs_creator_save) {
      fs_creator_save = 0;
      /* Spawn it and STAY here. Returning out of the screen and back in
       * would re-run fs_creator_defaults() and silently throw away every
       * wedge the user just set -- which is what the first version did.
       * The panel is only locked inside fs_paint(), so the write below
       * happens between paints, not under the lock. */
      if (!fs_creator_saving) {
        fs_creator_saving = 1;
        if (!task_create("fs_creator_save", 0x1e, 0x4000,
                       fs_creator_save_task, 0)) {
          fs_creator_saving = 0;
          fs_creator.save_err = 1;
          fs_log("creator: task_create failed for the save task");
          fs_log_flush();
        }
      }
      fs_creator_dirty = 1;
    }
    /* Nothing here animates, so the loop only has to notice keys. The 20 s
     * heartbeat matters as much as it does on the shelf: a screen nobody can
     * leave is a bricked camera. */
    if (fs_key_count == kc0 && get_ms_clock() - t0 > 30000) {
      fs_log("creator: no key in 30 s, releasing the screen");
      break;
    }
    msleep(30);
  }

  fs_creator_active = 0;
  fs_screen_release();
  fs_log("creator: closed");
  fs_log_flush();
  return FS_VIEW_NONE;
}

/* ---------------------------------------------------- Fuji WB shift
 *
 * The same two numbers as the menu entries, but as one decision rather than
 * two sliders -- and, crucially, written into the CAMERA's white balance so
 * they travel with the exposure the way a Fuji recipe does.
 */

static void fs_wb_render(void)
{
  fs_stock_wb_shift(&fs_wb.stock_ba, &fs_wb.stock_gm);
  /*
   * The config holds Canon's axes (it is written straight into PROP_WBS_*),
   * and the screen shows Fuji's. Converting in one place keeps the two from
   * drifting: Canon B/A = -Fuji Blue, Canon G/M = -Fuji Red.
   */
  fs_wb.ba = fs_clamp_i(film_sim_wbs_ba, -9, 9);
  fs_wb.gm = fs_clamp_i(film_sim_wbs_gm, -9, 9);
  fs_wb.r  = -fs_wb.gm;
  fs_wb.b  = -fs_wb.ba;
  fs_wb.moved  = (fs_wb.ba || fs_wb.gm);
  fs_wb.applied = fs_wb_applied;
  fsg_draw_wb(&fs_wb);
}

static void fs_wb_apply_task(void *unused) {
  int want_ba, want_gm;

  fs_log("fuji: apply task started");
  fs_log_flush();

  /* what the camera should hold after this, so it can be checked rather
   * than assumed */
  {
    int sba, sgm;
    fs_stock_wb_shift(&sba, &sgm);
    want_ba = fs_clamp_i(sba + fs_clamp_i(film_sim_wbs_ba, -9, 9), -9, 9);
    want_gm = fs_clamp_i(sgm + fs_clamp_i(film_sim_wbs_gm, -9, 9), -9, 9);
  }

  fs_push_wb_shift();

  /*
   * prop_request_change() is fire-and-forget: it returns as soon as the
   * request is queued. Read the property back before the screen claims
   * anything, so "IN CAMERA" means the camera agreed, not that a message
   * was sent.
   */
  msleep(80);
  if (lens_info.wbs_ba == want_ba && lens_info.wbs_gm == want_gm) {
    fs_wb_applied = 1;
    fs_log("fuji: camera reports B/A %+d G/M %+d, as asked",
           (int)lens_info.wbs_ba, (int)lens_info.wbs_gm);
  } else {
    fs_wb_applied = 0;
    fs_log("fuji: camera reports B/A %+d G/M %+d, asked for %+d %+d -- "
           "not applied", (int)lens_info.wbs_ba, (int)lens_info.wbs_gm,
           want_ba, want_gm);
  }
  fs_log_flush();
  fs_wb_dirty = 1;
}

static int fs_wb_run(void) {
  if (fs_gui_active || fs_creator_active || fs_wb_active)
    return FS_VIEW_NONE;

  fs_keep_panel(); /* see its comment: restores a wiped idle frame */

  fs_log("wb: running (ba=%d gm=%d)", (int)film_sim_wbs_ba, (int)film_sim_wbs_gm);
  fs_log_flush();
  beep_times(1);

  fs_screen_take();
  fs_wb_active = 1;
  fs_wb_dirty = 1;
  fs_wb_exit = 0;
  fs_wb_apply = 0;
  fs_wb_applied = 0;

  unsigned kc0 = fs_key_count;
  uint32_t t0 = get_ms_clock();

  while (!fs_wb_exit) {
    if (fs_wb_dirty) {
      fs_wb_dirty = 0;
      fs_paint(fs_wb_render);
    }
    if (fs_wb_apply) {
      fs_wb_apply = 0;
      /* A property write blocks on the ICU round-trip, so it cannot happen in
       * the key callback or under the BMP lock that fs_paint() holds. */
      fs_wb_applied = 0;
      task_create("fs_wb_apply", 0x1e, 0x1000, fs_wb_apply_task, 0);
      fs_wb_dirty = 1;
    }
    if (fs_key_count == kc0 && get_ms_clock() - t0 > 30000) {
      fs_log("wb: no key in 30 s, releasing the screen");
      break;
    }
    msleep(30);
  }

  fs_wb_active = 0;
  fs_screen_release();
  fs_log("wb: closed (ba=%d gm=%d)", (int)film_sim_wbs_ba, (int)film_sim_wbs_gm);
  fs_log_flush();
  return FS_VIEW_NONE;
}

/* ------------------------------------------------------- Fuji recipe
 *
 * Not a screen: a Fuji body has no recipe screen either. Their recipe is a
 * bundle of IMAGE QUALITY SETTING menu items saved in a C1..C7 slot, and the
 * .fs file is this module's slot. So this is a menu page whose Save writes
 * ML/FILM/<NAME>.fs, and the shelf picks it up like any other stock.
 */

static void fs_fuji_ensure_init(void)
{
  if (!fs_fuji_inited) {
    fs_fuji_inited = 1;
    fs_fuji_defaults(&fs_fuji);
    snprintf(fs_fuji.name, sizeof fs_fuji.name, "%s", fs_fuji_names[fs_fuji_slot]);
  }
}

static void fs_fuji_save_task(void *unused) {
  char buf[2048];
  char path[64];
  char file[12];
  int n;

  fs_log("fuji: recipe save task started");
  fs_log_flush();
  n = fs_fuji_write(&fs_fuji, buf, sizeof buf);
  if (n < 0) {
    NotifyBox(1600, "Fuji: recipe did not fit");
    fs_fuji_save = 0;
    return;
  }
  if (!fs_fuji.name[0])   /* the Name row was never displayed */
    snprintf(fs_fuji.name, sizeof fs_fuji.name, "%s", fs_fuji_names[fs_fuji_slot]);
  /* Same rule as the colour creator: the slot owns the file name, the typed
   * name is the label the shelf prints. Writing the label as the file name is
   * what made a saved look unfindable -- fs_resolve_stock_path() looks up
   * ML/FILM/<code>.fs and the code came from the label. */
  snprintf(file, sizeof file, "%s", fs_fuji_names[fs_fuji_slot]);
  snprintf(path, sizeof path, "ML/FILM/%s.fs", file);
  { static int ok; fs_ensure_dir("ML/FILM", &ok); }
  {
    FILE *f = FIO_CreateFile(path);
    int wrote = f ? FIO_WriteFile(f, buf, (size_t)n) : -1;
    if (f) FIO_CloseFile(f);
    if (wrote != n) {
      NotifyBox(1600, "Fuji: write failed");
      fs_log("fuji: writing '%s' gave %d of %d bytes", path, wrote, n);
      fs_log_flush();
      fs_fuji_save = 0;
      return;
    }
  }
  fs_log("fuji: wrote %s (%d bytes)", path, n);
  fs_log_flush();
  fs_register_look(file, fs_fuji.name, "MADE IN FUJI RECIPE");
  /* Confirm first, rebuild the catalogue after -- same reason as the creator:
   * the bytes are on the card, and the reload walks every XML on it. */
  NotifyBox(1500, "Saved %s", path);
  fs_log("fuji: wrote %s", path);
  fs_log_flush();
  {
    uint32_t t0 = get_ms_clock();
    fs_load_films_xml();
    fs_log("fuji: catalogue reload %d ms (%d stocks)",
           (int)(get_ms_clock() - t0), film_active_count);
    fs_log_flush();
  }
  fs_group_by_artwork();
  fs_fuji_save = 0;
}

static MENU_SELECT_FUNC(fs_fuji_save_trigger) {
  /* FIO on a task, never in a menu handler. */
  fs_fuji_ensure_init();
  if (!fs_fuji_save) {
    fs_fuji_save = 1;
    task_create("fs_fuji_save", 0x1e, 0x2000, fs_fuji_save_task, 0);
  }
}

/* The three fields a Fuji recipe has that this body cannot honour. Pressing
 * one says why instead of moving a knob that does nothing. */
static MENU_SELECT_FUNC(fs_fuji_unavailable) {
  const char *what = (const char *)priv;
  NotifyBox(2500, "%s", what);
}

static void fs_ui_task(void *arg) {
  int view = (int)(intptr_t)arg;
  fs_log("ui: task started view=%d", view);
  fs_log_flush();

  while (view != FS_VIEW_NONE) {
    if (view == FS_VIEW_SHELF)
      view = fs_shelf_run();
    else if (view == FS_VIEW_DARKROOM)
      view = fs_darkroom_run();
    else if (view == FS_VIEW_CREATOR)
      view = fs_creator_run();
    else if (view == FS_VIEW_WB)
      view = fs_wb_run();
    else if (view == FS_VIEW_DEVELOP) {
      fs_develop_run();
      /* back to the frame list: the panel stays in the module instead of
       * dropping the user into Canon's playback with no way back */
      view = FS_VIEW_DARKROOM;
    } else {
      break;
    }
  }

  fs_log("ui: task exit");
  fs_log_flush();
  fs_ui_running = 0;
  if (fs_pending_view != FS_VIEW_NONE) {
    int next = fs_pending_view;
    fs_pending_view = FS_VIEW_NONE;
    fs_log("ui: opening queued view %d", next);
    fs_log_flush();
    msleep(120);
    fs_ui_launch(next);
  }
}

static void fs_develop_flag_clear_if_stale(void) {
  if (!fs_develop_active)
    return;
  uint32_t now = get_ms_clock();
  if (!fs_develop_since) {
    fs_develop_since = now;
    return;
  }
  if (now - fs_develop_since > FS_DEVELOP_TIMEOUT_MS) {
    fs_log("ui: develop flag stale for %d ms, clearing",
           (int)(now - fs_develop_since));
    fs_log_flush();
    fs_develop_active = 0;
    fs_develop_since = 0;
  }
}

static void fs_ui_launch(int view) {
  fs_develop_flag_clear_if_stale();
  if (fs_ui_running || fs_gui_active || fs_darkroom_active ||
      fs_develop_active || fs_view_active) {
    /* Say which one is holding the screen, for long enough to read it. */
    fs_log("ui: launch %d ignored (ui=%d gui=%d dark=%d dev=%d view=%d)", view,
           fs_ui_running, fs_gui_active, fs_darkroom_active, fs_develop_active,
           fs_view_active);
    fs_log_flush();
    if (view != FS_VIEW_NONE) {
      fs_pending_view = view;
      NotifyBox(2500, "Film: %s - will open next",
                fs_develop_active    ? "developing"
                : fs_darkroom_active ? "darkroom busy"
                : fs_gui_active      ? "shelf busy"
                                     : "busy");
    }
    return;
  }
  /* Raise the guard before the task exists: task_create() returns before the
   * task runs, so two menu presses in that window used to start two ui tasks
   * fighting for the same panel. */
  fs_ui_running = 1;
  uint32_t t = task_create("fs_ui", 0x1e, FS_DEV_STACK, fs_ui_task,
                           (void *)(intptr_t)view);
  fs_log("ui: task_create(%d) -> 0x%x", view, (unsigned)t);
  if (!t) {
    fs_ui_running = 0;
    NotifyBox(2000, "Film: cannot start task");
  }
}

/* ---------------------------------------------------------------- autodev
 *
 * Unattended develop, triggered by a file instead of a button.
 *
 * Create ML/SETTINGS/AUTODEV.TST on the card and power the camera on: the
 * module deletes the flag, develops the newest raw in DCIM with the active
 * stock, and writes every step to ML/LOGS/FILMSIM.LOG. Nothing in the GUI,
 * the key handler or the screen handover is involved, so if this produces a
 * file the whole engine and the file writing are good and any remaining fault
 * is in the interactive path; if it does not, the log names the step that
 * failed.
 */
/* Frame number from a file name, so "newest" means newest and not whichever
 * entry the directory happened to list first. */
static int fs_photo_number(const char *n) {
  int num = 0, seen = 0;
  for (const char *q = n; q && *q; q++) {
    if (*q >= '0' && *q <= '9') {
      num = num * 10 + (*q - '0');
      seen = 1;
    } else if (seen) {
      if (num > 9999)
        num %= 10000;
      break;
    }
  }
  return seen ? num : -1;
}

static int fs_pick_newest_raw(void) {
  int best = 0, best_num = -1;
  for (int i = 0; i < dcim_photo_count; i++) {
    int n = fs_photo_number(dcim_photo_list[i]);
    if (n > best_num) {
      best_num = n;
      best = i;
    }
  }
  return best;
}

/* Read a small text file into buf, trimmed of CR/LF. Returns the length, or 0.
 */
static int fs_read_text_file(const char *path, char *buf, int cap) {
  uint32_t sz = 0;
  if (cap < 2)
    return 0;
  buf[0] = 0;
  if (FIO_GetFileSize(path, &sz) != 0 || sz == 0)
    return 0;
  if (sz > (uint32_t)(cap - 1))
    sz = (uint32_t)(cap - 1);
  FILE *f = FIO_OpenFile(path, O_RDONLY);
  if (!f)
    return 0;
  int n = FIO_ReadFile(f, buf, (int)sz);
  FIO_CloseFile(f);
  if (n <= 0)
    return 0;
  while (n > 0 &&
         (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
    n--;
  buf[n] = 0;
  return n;
}

/* Leaf name of a path, for comparing what DEVPHOTO.TXT says against the list.
 */
static const char *fc_basename(const char *p) {
  const char *b = p;
  for (const char *q = p; q && *q; q++)
    if (*q == '/' || *q == '\\')
      b = q + 1;
  return b;
}

/*
 * Advertising batch: develop the same frame with four stocks that cannot be
 * confused with each other -- warm pastel, saturated slide, tungsten with the
 * strongest halation, and the desaturated bleach look -- into FLC1..FLC4.JPG
 * beside the original. 8.3 names on purpose; FIO_CreateFile rejects long ones.
 */
static void fs_develop_batch(void) {
  static const char *const codes[4] = {"portra", "velvia", "soup800",
                                       "bleachbypass"};
  static const char *const labels[4] = {"Portra 400", "Velvia 50",
                                        "CineStill 800T", "Bleach Bypass"};
  if (fs_develop_active)
    return;
  fs_develop_active = 1;
  fs_develop_since = get_ms_clock();

  if (dcim_photo_count == 0)
    fs_scan_dcim_photos();
  const char *dir = get_dcim_dir();
  if (!dir || !dir[0])
    dir = "B:/DCIM/100CANON";
  if (dcim_photo_count <= 0) {
    fs_log("batch: nothing to develop");
    fs_log_flush();
    fs_develop_active = 0;
    return;
  }
  /*
   * ML/SETTINGS/DEVPHOTO.TXT names the frame to develop ("IMG_3219.CR2").
   * A file rather than a config entry because this is a test lever: drop the
   * file, reboot, and the same frame is developed every time. Without it the
   * newest frame wins.
   */
  dcim_photo_index = fs_pick_newest_raw();
  {
    char want[64];
    int got = fs_read_text_file("ML/SETTINGS/DEVPHOTO.TXT", want, sizeof want);
    if (got > 0) {
      for (int i = 0; i < dcim_photo_count; i++) {
        if (fs_name_has_ci(dcim_photo_list[i], want) &&
            fs_name_has_ci(want, fc_basename(dcim_photo_list[i]))) {
          dcim_photo_index = i;
          fs_log("batch: DEVPHOTO picked %s", dcim_photo_list[i]);
          break;
        }
      }
    }
  }
  const char *photo = dcim_photo_list[dcim_photo_index];

  char cr2[FS_PATH_LEN];
  snprintf(cr2, sizeof cr2, "%s/%s", dir, photo);

  fs_screen_take();
  fs_bath_ui = 1;

  for (int i = 0; i < 4; i++) {
    char stock[FS_PATH_LEN], out[FS_PATH_LEN], tmp[FS_PATH_LEN];
    snprintf(stock, sizeof stock, "ML/FILM/%s.fs", codes[i]);
    snprintf(out, sizeof out, "%s/FLC%d.JPG", dir, i + 1);
    snprintf(tmp, sizeof tmp, "%s/FLC%d.TMP", dir, i + 1);

    uint32_t sz = 0;
    if (FIO_GetFileSize(stock, &sz) != 0 || sz == 0) {
      fs_log("batch: %s missing, skipped", stock);
      continue;
    }
    FIO_RemoveFile(tmp);
    FIO_RemoveFile(out);

    bmp_fill(COLOR_BLACK, 0, 0, 720, 480);
    bmp_draw_rect(COLOR_RED, 12, 60, 696, 120);
    bmp_printf(FONT(FONT_MED_LARGE, COLOR_WHITE, COLOR_BLACK), 24, 74,
               "FILM COMPARISON %d of 4", i + 1);
    bmp_printf(FONT(FONT_MED, COLOR_YELLOW, COLOR_BLACK), 24, 106, "%s",
               labels[i]);
    bmp_printf(FONT(FONT_SMALL, COLOR_WHITE, COLOR_BLACK), 24, 138,
               "%s -> FLC%d.JPG", photo, i + 1);

    fs_log("batch: %s + %s -> %s", cr2, stock, out);
    fs_log_flush();

    fs_bath_pct = 0;
    snprintf(fs_bath_stage, sizeof fs_bath_stage, "Starting");
    uint32_t t0 = get_ms_clock();
    se_err e;
    if (film_sim_level == FS_LEVEL_STYLE) {
      /* nothing to develop: the look is already in the captured JPEG */
      fs_log("batch: level 1, nothing to develop");
      e = SE_E_UNSUPPORTED;
    } else if (film_sim_level == FS_LEVEL_RAW) {
      se_cam_set_aspect(film_sim_aspect);
      se_cam_set_pad32(film_sim_pad);
      fs_frame_apply(stock);
      se_cam_set_strength(film_sim_strength);
      se_cam_set_satboost(film_sim_satboost);
  se_cam_set_grainsize(film_sim_grainsize);
      e = se_cam_develop_raw(cr2, stock, tmp, 2, fs_bath_progress,
                             fs_develop_note, NULL);
      if (e != SE_OK) {
        fs_log("batch: raw develop failed (%d: %s), falling back to preview "
               "develop",
               (int)e, se_strerror(e));
        e = se_cam_develop(cr2, stock, tmp, fs_bath_progress, fs_develop_note,
                           NULL);
      }
    } else {
      se_cam_set_aspect(film_sim_aspect);
      se_cam_set_pad32(film_sim_pad);
      se_cam_set_strength(film_sim_strength);
      se_cam_set_satboost(film_sim_satboost);
  se_cam_set_grainsize(film_sim_grainsize);
      e = se_cam_develop(cr2, stock, tmp, fs_bath_progress, fs_develop_note,
                         NULL);
    }
    fs_log("batch: %s returned %d (%s) in %d ms", labels[i], (int)e,
           se_strerror(e), (int)(get_ms_clock() - t0));
    if (e == SE_OK && fs_commit(tmp, out) == 0) {
      fs_log("batch: committed %s", out);
    } else {
      FIO_RemoveFile(tmp);
      fs_log("batch: FAILED %s", labels[i]);
    }
    fs_log_flush();
  }

  fs_bath_ui = 0;
  fs_screen_release();
  fs_develop_active = 0;
  fs_log("batch: finished");
  fs_log_flush();
}

/* ---------------------------------------------------------------- multi image
 *
 * A develop set: photos ticked with the `Mark` entry and developed one after
 * another with the active stock. Stored as leaf names, not directory indexes,
 * because directory order and count change every time a frame is shot.
 */
#define FS_MAX_MARKS 24
static char fs_marks[FS_MAX_MARKS][16];
static int fs_mark_count = 0;

static int fs_mark_find(const char *name) {
  for (int i = 0; i < fs_mark_count; i++)
    if (!strcasecmp(fs_marks[i], name))
      return i;
  return -1;
}

static void fs_mark_toggle(const char *name) {
  int i = fs_mark_find(name);
  if (i >= 0) {
    for (int k = i; k < fs_mark_count - 1; k++)
      memcpy(fs_marks[k], fs_marks[k + 1], sizeof fs_marks[0]);
    fs_mark_count--;
    return;
  }
  if (fs_mark_count >= FS_MAX_MARKS)
    return;
  snprintf(fs_marks[fs_mark_count], sizeof fs_marks[0], "%s", name);
  fs_mark_count++;
}

/* "IMG_3219.CR2" in DCIM -> the raw to read, the 8.3 result beside it and the
 * temporary the result is committed from. Same rules as the interactive
 * develop, so a set run and a single run produce identical file names. */
static void fs_photo_paths(const char *photo, char *cr2, int c1, char *out,
                           int c2, char *tmp, int c3) {
  /* `photo` is normally a pointer INTO dcim_photo_list, and the rescan below
   * rewrites that list -- which would silently rename the frame we are about to
   * develop. Copy the name before touching the list. */
  char name[64];
  const char *dir;
  snprintf(name, sizeof name, "%s", photo ? photo : "");
  photo = name;
  dir = get_dcim_dir();
  /* Refresh the listing before asking for the next free number: a develop
   * earlier in this session added a file the list does not know about yet.
   * The scan also moves the darkroom cursor to the newest frame, which must not
   * happen under the user's hands, so it is put back. */
  {
    int keep = dcim_photo_index;
    fs_scan_dcim_photos();
    if (keep >= 0 && keep < dcim_photo_count)
      dcim_photo_index = keep;
  }
  if (!dir || !dir[0])
    dir = "B:/DCIM/100CANON";
  snprintf(cr2, c1, "%s/%s", dir, photo);

  int plen = strlen(cr2);
  if (plen > 4 && !strcasecmp(cr2 + plen - 4, ".jpg"))
    memcpy(cr2 + plen - 4, ".CR2", 4);

  {
    /*
     * Same number as the frame it came from: IMG_3271.CR2 -> IMG_3271.JPG.
     *
     * Canon's playback index is keyed by the file number and is built when the
     * card is mounted; a develop takes twenty seconds, so a fresh number can
     * never be in that index and the file stays invisible until the card is
     * re-inserted (measured: IMG_3273 absent, card out and back, listed and
     * playing). A JPEG sharing a number with a raw the camera already knows is
     * the one case the firmware is built to handle -- it is exactly RAW+JPEG
     * pairing -- so this is the only numbering that has a chance of appearing
     * without a remount.
     *
     * Never at the cost of an existing file: if the camera already wrote a JPEG
     * for this frame (RAW+JPEG mode), fall back to the next free number.
     */
    int n = fs_src_number(photo);
    uint32_t exists = 0;
    if (n > 0 && n < 10000) {
      { char pad[8]; fs_pad_num(pad, sizeof pad, n, 4);
        snprintf(out, c2, "%s/IMG_%s.JPG", dir, pad); }
      if (!film_sim_replace && FIO_GetFileSize(out, &exists) == 0 &&
          exists > 0) {
        fs_log("develop: %s exists (%d bytes), using a free number", out,
               (int)exists);
        fs_log_flush();
        n = 0;
      }
    }
    if (n <= 0 || n >= 10000)
      n = fs_next_img_number();
    if (n > 0 && n < 10000)
      { char pad[8]; fs_pad_num(pad, sizeof pad, n, 4);
        snprintf(out, c2, "%s/IMG_%s.JPG", dir, pad); }
    else
      snprintf(out, c2, "%s/FLM_0000.JPG", dir);
  }

  /*
   * "IMG_nnnn.TMP" while it is being written, renamed to .JPG on success.
   *
   * Not because of the playback list -- that turned out to be built at card
   * mount and to be indifferent to names -- but because a develop takes twenty
   * seconds and a power-off in the middle would otherwise leave a truncated
   * "IMG_nnnn.JPG" that Canon and this module's own viewer would both try to
   * open. fs_recover_tmp_files() already knows what to do with a .TMP at boot.
   */
  snprintf(tmp, c3, "%s", out);
  {
    int tlen = (int)strlen(tmp);
    if (tlen > 4 && !strcasecmp(tmp + tlen - 4, ".JPG"))
      memcpy(tmp + tlen - 4, ".TMP", 4);
    else
      snprintf(tmp, c3, "%s.TMP", out);
  }
}

/*
 * One develop, display-free: paths, refusal checks, engine call, atomic rename.
 * `prog` may be NULL for an unattended run. `skipped` is set when the result
 * was already on the card, so callers can say so instead of re-grading it.
 */
static se_err fs_develop_photo(const char *photo, const char *stock_path,
                               se_cam_progress_fn prog, int *skipped) {
  char cr2[FS_PATH_LEN], out[FS_PATH_LEN], tmp[FS_PATH_LEN];
  if (skipped)
    *skipped = 0;
  fs_photo_paths(photo, cr2, sizeof cr2, out, sizeof out, tmp, sizeof tmp);

  uint32_t sz = 0;
  if (FIO_GetFileSize(cr2, &sz) != 0 || sz == 0) {
    fs_log("develop: no raw for %s", photo);
    fs_log_flush();
    return SE_E_IO;
  }
  if (fs_dev_is_done(fs_src_number(photo))) {
    fs_log("develop: %s already developed (marker)", photo);
    fs_log_flush();
    if (skipped)
      *skipped = 1;
    return SE_OK;
  }
  FIO_RemoveFile(tmp);

  fs_log("develop: %s + %s -> %s", cr2, stock_path, out);
  fs_log_flush();
  se_err e;
  se_cam_set_aspect(film_sim_aspect);
  se_cam_set_pad32(film_sim_pad);
  se_cam_set_strength(film_sim_strength);
  se_cam_set_satboost(film_sim_satboost);
  se_cam_set_grainsize(film_sim_grainsize);
  if (film_sim_level == FS_LEVEL_RAW) {
    e = se_cam_develop_raw(cr2, stock_path, tmp, 2, prog, fs_develop_note,
                           NULL);
    if (e != SE_OK) {
      fs_log("develop: raw develop failed (%d: %s), falling back to preview "
             "develop",
             (int)e, se_strerror(e));
      e = se_cam_develop(cr2, stock_path, tmp, prog, fs_develop_note, NULL);
    }
  } else {
    e = se_cam_develop(cr2, stock_path, tmp, prog, fs_develop_note, NULL);
  }
  fs_log("develop: level %d returned %d (%s)", film_sim_level, (int)e,
         se_strerror(e));

  if (e == SE_OK && fs_commit(tmp, out) == 0) {
    fs_dev_mark_done(fs_src_number(photo));
    snprintf(fs_last_dev, sizeof fs_last_dev, "%s", out);
    fs_log("develop: committed %s", out);
    fs_log_flush();
    return SE_OK;
  }
  FIO_RemoveFile(tmp);
  fs_log_flush();
  return (e == SE_OK) ? SE_E_IO : e;
}

/* Resolve the active stock into a path, or say why there is none. */
static int fs_active_stock_path(char *stk, int cap, const film_xml_t **out_f) {
  int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
  const film_xml_t *f = &film_active_table[fs_develop_stock_index(count)];
  if (out_f)
    *out_f = f;
  return fs_resolve_stock_path(f, stk, cap);
}

/* Marked set: develop every ticked photo, one screen for the whole run. */
static void fs_develop_set_task(void *unused);

/*
 * Unattended lever for the develop set, the twin of DEVPHOTO.TXT/AUTODEV.TST:
 * ML/SETTINGS/DEVSET.TXT holds one 8.3 photo name per line and marks them,
 * ML/SETTINGS/AUTOSET.TST develops the set once. Delayed because at module
 * init the card filesystem is not mounted yet and every FIO call fails.
 */
static void fs_lever_task(void *unused) {
  (void)unused;
  msleep(5000);

  /*
   * The picture style is a camera setting that survives a power cycle, so a
   * mode we did not apply this boot is still the one being used. This runs here
   * rather than in init for the same reason the DEVSET read does: at init the
   * property ICU is not up yet.
   */
  fs_force_capture_style();

  uint32_t sz = 0;
  if (FIO_GetFileSize("ML/SETTINGS/DEVSET.TXT", &sz) == 0 && sz > 0) {
    static char listbuf[512];
    FILE *fp = FIO_OpenFile("ML/SETTINGS/DEVSET.TXT", O_RDONLY);
    if (fp) {
      int n = FIO_ReadFile(fp, listbuf, sizeof listbuf - 1);
      FIO_CloseFile(fp);
      if (n > 0) {
        listbuf[n] = 0;
        char *q = listbuf;
        while (*q) {
          char *e = q;
          while (*e && *e != '\n' && *e != '\r')
            e++;
          char save = *e;
          *e = 0;
          while (*q == ' ' || *q == '\t')
            q++;
          if (*q)
            fs_mark_toggle(q);
          if (!save)
            break;
          q = e + 1;
        }
      }
    }
  }

  /* Drop names that are not on the card, so a stale list cannot turn into a
   * run of "no raw" failures. */
  fs_scan_dcim_photos();
  for (int i = 0; i < fs_mark_count;) {
    int found = 0;
    for (int j = 0; j < dcim_photo_count; j++)
      if (!strcasecmp(fs_marks[i], dcim_photo_list[j])) {
        found = 1;
        break;
      }
    if (found) {
      i++;
      continue;
    }
    for (int k = i; k < fs_mark_count - 1; k++)
      memcpy(fs_marks[k], fs_marks[k + 1], sizeof fs_marks[0]);
    fs_mark_count--;
  }
  if (fs_mark_count > 0) {
    fs_log("devset: %d photos marked from DEVSET.TXT", fs_mark_count);
    fs_log_flush();
  }

  if (FIO_GetFileSize("ML/SETTINGS/AUTOSET.TST", &sz) == 0) {
    FIO_RemoveFile("ML/SETTINGS/AUTOSET.TST");
    fs_log("devset: AUTOSET.TST found, starting set develop");
    fs_log_flush();
    fs_develop_set_task(NULL);
  }
}

static void fs_develop_set_task(void *unused) {
  (void)unused;
  if (fs_develop_active)
    return;
  if (fs_mark_count <= 0) {
    NotifyBox(2000, "No photos marked");
    return;
  }
  if (film_sim_level < FS_LEVEL_GRADE) {
    /* Level 1 baked the look into the JPEG at capture; grading it again would
     * apply the look twice. */
    NotifyBox(2500, "Level 1: look is already in the JPEG");
    return;
  }

  fs_develop_active = 1;
  fs_develop_since = get_ms_clock();
  fs_log("set: %d marked photos", fs_mark_count);
  fs_log_flush();

  const film_xml_t *f = NULL;
  char stk[FS_PATH_LEN];
  if (!fs_active_stock_path(stk, sizeof stk, &f)) {
    fs_log("set: no recipe for %s", f ? f->code : "?");
    fs_log_flush();
    NotifyBox(2500, "No recipe for %s", f ? f->code : "?");
    fs_develop_active = 0;
    return;
  }

  fs_screen_take();
  fs_bath_ui = 1;

  fs_dev_stock = f->name;
  fs_dev_brand = f->brand;
  fs_dev_iso = f->iso;
  fs_dev_soup = (film_sim_dev == FS_DEV_PUSH_1)   ? "Push +1 (+1 EV)"
                : (film_sim_dev == FS_DEV_PUSH_2) ? "Push +2 (+2 EV)"
                : (film_sim_dev == FS_DEV_PULL_1) ? "Pull -1 (-1 EV)"
                                                  : "Standard C-41";
  fs_dev_total = fs_mark_count;
  fs_dev_result = NULL;
  fs_dev_done = 0;
  /* Clear the outcome of the previous run: the flag is sticky, so a failure
   * would tint every later develop screen red. */
  fs_dev_ok = fs_dev_failed = fs_dev_skipped = 0;

  int ok = 0, skipped = 0, failed = 0;
  for (int i = 0; i < fs_mark_count; i++) {
    fs_dev_photo = fs_marks[i];
    fs_dev_index = i + 1;
    fs_dev_ok = ok;
    fs_dev_skipped = skipped;
    fs_dev_failed = failed;
    fs_bath_pct = 0;
    snprintf(fs_bath_stage, sizeof fs_bath_stage, "Starting");
    fs_develop_screen();
    fs_log("set: %d/%d %s", i + 1, fs_mark_count, fs_marks[i]);
    fs_log_flush();

    int sk = 0;
    se_err e = fs_develop_photo(fs_marks[i], stk, fs_bath_progress, &sk);
    if (sk)
      skipped++;
    else if (e == SE_OK)
      ok++;
    else
      failed++;

    static char msg[48];
    snprintf(msg, sizeof msg, "%s  -  %s", fs_marks[i],
             sk ? "already developed"
                : (e == SE_OK ? "developed" : se_strerror(e)));
    fs_dev_ok = ok;
    fs_dev_skipped = skipped;
    fs_dev_failed = failed;
    fs_dev_result = msg;
    fs_develop_screen();
    fs_log("set: %s -> %s", fs_marks[i], msg);
    fs_log_flush();
  }

  fs_bath_ui = 0;
  fs_dev_done = 1;
  {
    static char done[64];
    snprintf(done, sizeof done, "SET DONE  %d ok, %d skipped, %d failed", ok,
             skipped, failed);
    fs_dev_result = done;
  }
  fs_dev_failed = failed;
  fs_develop_screen();
  fs_log("set: finished ok=%d skipped=%d failed=%d", ok, skipped, failed);
  fs_log_flush();
  beep_times(2);
  msleep(3000);
  fs_screen_release();
  fs_develop_active = 0;
  NotifyBox(3000, "Film set: %d developed", ok);
}

/* ---------------------------------------------------------------- auto develop
 *
 * Unattended: with film.sim.auto on, a new frame and a switch to level 2/3 are
 * reasons to develop without a keypress. The engine call is the same one the
 * menu uses, only without the progress callbacks, so it can run while the
 * camera is in LiveView.
 */
static int fs_auto_running = 0;
/* The queue worker is a singleton: film.sim.queue can be on at boot and the
 * menu entry can be toggled again later, and two workers would race for
 * fs_queue[0]. */
static int fs_queue_running = 0;
static int fs_auto_level = 0;
static char fs_auto_last[16] = "";
/* The newest frame that existed when the watcher started: the backlog sweep
 * never looks below this, or it would try to develop the whole card. */
static int fs_auto_base;

/* Newest .CR2 leaf name, without touching the list or the index the menu has
 * selected -- the watcher must be invisible to the user's cursor. */
static int fs_newest_raw_name(char *out, int cap) {
  out[0] = 0;
  const char *dcim = get_dcim_dir();
  if (!dcim)
    return 0;
  struct fio_file *file = alloc_fio_file();
  if (!file)
    return 0;
  struct fio_dirent *de = FIO_FindFirstEx(dcim, file);
  if (IS_ERROR(de)) {
    free(file);
    return 0;
  }
  int best = -1;
  do {
    struct file_info fi = convert_fio_file_info(file);
    if (fi.mode & ATTR_DIRECTORY)
      continue;
    int l = strlen(fi.name);
    if (l <= 4 || strcasecmp(fi.name + l - 4, ".CR2"))
      continue;
    int n = fs_photo_number(fi.name);
    if (n > best) {
      best = n;
      snprintf(out, cap, "%s", fi.name);
    }
  } while (FIO_FindNextEx(de, file) == 0);
  FIO_FindClose(de);
  free(file);
  return out[0] != 0;
}

static void fs_auto_develop_now(const char *photo) {
  const film_xml_t *f = NULL;
  char stk[FS_PATH_LEN];
  if (!fs_active_stock_path(stk, sizeof stk, &f)) {
    fs_log("auto: no recipe for %s", f ? f->code : "?");
    fs_log_flush();
    return;
  }

  /* A 50-70 s grade with nothing on the panel looks like a dead camera, so the
   * bath is shown while it runs -- the same screen the darkroom draws, which is
   * also the only honest progress indicator we have: every number on it comes
   * from the engine's own callback. Only when no other screen owns the panel.
   */
  /* Queue mode exists so the shutter stays live: taking the panel here would
   * lock it again, and this is the function the queue actually calls. */
  int own_screen = !film_sim_queue && !fs_ui_running && !fs_gui_active &&
                   !fs_darkroom_active;

  fs_develop_active = 1;
  fs_develop_since = get_ms_clock();

  snprintf(fs_dev_photo_buf, sizeof fs_dev_photo_buf, "%s", photo);
  fs_dev_photo = fs_dev_photo_buf;
  fs_dev_stock = f ? f->name : "";
  fs_dev_brand = f ? f->brand : "";
  fs_dev_iso = f ? f->iso : 0;
  fs_dev_soup = (film_sim_dev == FS_DEV_PUSH_1)   ? "Push +1 (+1 EV)"
                : (film_sim_dev == FS_DEV_PUSH_2) ? "Push +2 (+2 EV)"
                : (film_sim_dev == FS_DEV_PULL_1) ? "Pull -1 (-1 EV)"
                                                  : "Standard C-41";
  fs_dev_result = NULL;
  fs_dev_done = 0;
  /* Clear the outcome of the previous run: the flag is sticky, so a failure
   * would tint every later develop screen red. */
  fs_dev_ok = fs_dev_failed = fs_dev_skipped = 0;
  fs_dev_total = 0;
  fs_dev_index = 1;
  fs_bath_pct = 0;
  snprintf(fs_bath_stage, sizeof fs_bath_stage, "Waiting for the card");

  if (own_screen) {
    fs_screen_take();
    fs_bath_ui = 1;
    fs_develop_screen();
  } else {
    NotifyBox(1500, "Film: %s", photo);
  }

  int skipped = 0;
  se_err e = fs_develop_photo(photo, stk, fs_bath_progress, &skipped);

  fs_dev_done = 1;
  if (skipped) {
    fs_dev_skipped = 1;
    fs_dev_result = "SKIPPED  -  already developed";
  } else if (e == SE_OK) {
    fs_dev_ok = 1;
    fs_bath_pct = 100;
    snprintf(fs_bath_stage, sizeof fs_bath_stage, "Dried");
    fs_dev_result = "DONE";
  } else {
    fs_dev_failed = 1;
    fs_dev_result = se_strerror(e);
  }

  if (own_screen) {
    fs_develop_screen();
    fs_bath_ui = 0;
    if (!skipped && e == SE_OK) {
      fs_view_run(fs_last_dev, 1); /* panel is already ours */
    } else {
      msleep(1200);
      fs_screen_release();
    }
  }

  fs_log("auto: %s -> %d (%s)%s", photo, (int)e, se_strerror(e),
         skipped ? " [already done]" : "");
  fs_log_flush();
  fs_develop_active = 0;
  if (!skipped && e == SE_OK)
    beep_times(2);
}

/*
 * The frame after the one just shot.
 *
 * The watcher below only ever looks at the NEWEST raw, so two shots inside one
 * 2.5 s poll -- or a shot taken while a develop holds the watcher for twenty
 * seconds -- leaves the earlier frame undeveloped, with the camera's own JPEG
 * sitting there: full size, uncropped, ungraded. IMG_9943 is exactly that: it
 * appears nowhere in FILMSIM.LOG, and the file on the card is 5472x3648 while
 * every developed neighbour is 2736x1824.
 *
 * So once a poll, look for the oldest frame that has no DEV_xxxx.MRK yet, back
 * as far as a few frames before the newest one at boot. The window is what
 * keeps this from trying to develop the whole card: the 180 older shots on a
 * card like this one carry no marker either, and developing them would take
 * hours. Eight frames is more than a burst, and small enough to be harmless.
 */
#define FS_AUTO_LOOKBACK 8
static int fs_auto_backlog(char *out, int cap) {
  const char *dcim = get_dcim_dir();
  int best = 0;
  if (!dcim || !fs_auto_base)
    return 0;
  out[0] = 0;
  struct fio_file *file = alloc_fio_file();
  if (!file)
    return 0;
  struct fio_dirent *de = FIO_FindFirstEx(dcim, file);
  if (IS_ERROR(de)) {
    free(file);
    return 0;
  }
  do {
    struct file_info fi = convert_fio_file_info(file);
    if (fi.mode & ATTR_DIRECTORY)
      continue;
    int l = strlen(fi.name);
    if (l <= 4 || strcasecmp(fi.name + l - 4, ".CR2"))
      continue;
    int n = fs_photo_number(fi.name);
    if (n <= fs_auto_base - FS_AUTO_LOOKBACK || fs_dev_is_done(n))
      continue;
    if (!best || n < best) {
      best = n;
      snprintf(out, cap, "%s", fi.name);
    }
  } while (FIO_FindNextEx(de, file) == 0);
  FIO_FindClose(de);
  free(file);
  return best;
}

/* Append a frame to the develop queue. Duplicates and a full queue are both
 * reported rather than silently dropped -- a frame that never develops is a
 * file the user thinks is a film frame and is not. */
static int fs_queue_push(const char *name) {
  uint32_t flags = cli();
  for (int i = 0; i < fs_queue_n; i++)
    if (!strcasecmp(fs_queue[i], name)) {
      sei(flags);
      return 0;
    }
  if (fs_queue_n >= FS_QUEUE_MAX) {
    sei(flags);
    fs_log("queue: full (%d), %s left for the next poll", FS_QUEUE_MAX, name);
    fs_log_flush();
    return 0;
  }
  snprintf(fs_queue[fs_queue_n], sizeof fs_queue[0], "%s", name);
  fs_queue_n++;
  sei(flags);
  fs_log("queue: %s added (%d waiting)", name, fs_queue_n);
  fs_log_flush();
  return 1;
}

static void fs_queue_task(void *unused) {
  (void)unused;
  fs_queue_running = 1;
  while (film_sim_queue) {
    msleep(2000);
    if (fs_queue_n <= 0)
      continue;
    /* Never two at once, and never over a screen the user is using. */
    if (fs_develop_active || fs_ui_running)
      continue;

    char name[64];
    uint32_t flags = cli();
    snprintf(name, sizeof name, "%s", fs_queue[0]);
    for (int i = 1; i < fs_queue_n; i++)
      snprintf(fs_queue[i - 1], sizeof fs_queue[0], "%s", fs_queue[i]);
    fs_queue_n--;
    sei(flags);

    fs_log("queue: developing %s (%d left)", name, fs_queue_n);
    fs_log_flush();
    fs_auto_develop_now(name);
  }
  fs_queue_running = 0;
  fs_log("queue: task stopped");
  fs_log_flush();
}

/* One worker, however many times the menu is toggled. */
static void fs_queue_start(void) {
  if (fs_queue_running)
    return;
  fs_queue_running = 1; /* before task_create: it can run immediately */
  if (!task_create("fs_queue", 0x1c, FS_DEV_STACK, fs_queue_task, 0))
    fs_queue_running = 0;
}

static void fs_auto_task(void *unused) {
  (void)unused;
  fs_auto_running = 1;
  fs_auto_level = film_sim_level;
  fs_auto_last[0] = 0;
  char cand[16] = "";
  uint32_t cand_sz = 0;

  fs_log("auto: watcher started (level %d)", film_sim_level);
  fs_log_flush();
  /* Whatever is already on the card is not news. */
  fs_newest_raw_name(fs_auto_last, sizeof fs_auto_last);
  fs_auto_base = fs_photo_number(fs_auto_last);

  while (film_sim_auto) {
    msleep(2500);

    /* Never develop underneath an open view or a running develop. */
    if (fs_develop_active || fs_ui_running) {
      fs_auto_level = film_sim_level;
      fs_newest_raw_name(fs_auto_last, sizeof fs_auto_last);
      cand[0] = 0;
      continue;
    }
    if (film_sim_level < FS_LEVEL_GRADE) {
      fs_auto_level = film_sim_level;
      continue;
    }

    /* Level switched to 2 (or 3) from the menu: develop what is selected, so
     * picking the level is enough to get a film frame. */
    if (film_sim_level != fs_auto_level) {
      fs_auto_level = film_sim_level;
      char photo[16];
      if (dcim_photo_count > 0) {
        snprintf(photo, sizeof photo, "%s", dcim_photo_list[dcim_photo_index]);
      } else if (!fs_newest_raw_name(photo, sizeof photo)) {
        continue;
      }
      fs_log("auto: level %d picked, developing %s", film_sim_level, photo);
      fs_auto_develop_now(photo);
      cand[0] = 0;
      continue;
    }

    /*
     * Graded in the shutter: nothing left to do.
     *
     * When the look has been applied to the frame Canon encoded, the JPEG on
     * the card is already the film frame, and developing it again would grade
     * twice and take twenty seconds to do it. Framing is the exception -- the
     * crop and the letterbox are ours, not Canon's -- so a framing other than
     * full 3:2 still needs the develop.
     */
    if (film_sim_yuvrows > 0 && film_sim_aspect == 0 && film_sim_shotgrade) {
      char newest[16];
      if (fs_newest_raw_name(newest, sizeof newest) &&
          strcasecmp(newest, fs_auto_last)) {
        int graded = fs_yuvhook_graded();
        snprintf(fs_auto_last, sizeof fs_auto_last, "%s", newest);
        if (graded != fs_auto_graded_seen) {
          fs_auto_graded_seen = graded;
          fs_log("auto: %s was graded in the shutter, not developing it",
                 newest);
          fs_log_flush();
          fs_dev_mark_done(fs_photo_number(newest));
        } else {
          /* The hook is installed but this frame did not go through it --
           * refused patch, missing tables, changed firmware. Develop it
           * rather than leave Canon's JPEG posing as a film frame. */
          fs_log("auto: %s was not graded in the shutter; developing", newest);
          fs_log_flush();
        }
      }
    }

    /* A frame that was shot while the watcher was busy, or between two polls,
     * comes first: the camera's JPEG is still on the card ungraded. */
    {
      char late[16];
      if (fs_auto_backlog(late, sizeof late)) {
        fs_log("auto: catching up on %s (shot while unpaid)", late);
        if (film_sim_queue)
          fs_queue_push(late);
        else
          fs_auto_develop_now(late);
        cand[0] = 0;
        continue;
      }
    }

    /* New frame: require the same name at the same size on two polls, so the
     * develop never starts on a file the camera is still writing. */
    char now[16];
    if (!fs_newest_raw_name(now, sizeof now))
      continue;
    if (!strcasecmp(now, fs_auto_last))
      continue;

    const char *dir = get_dcim_dir();
    if (!dir || !dir[0])
      dir = "B:/DCIM/100CANON";
    char path[FS_PATH_LEN];
    snprintf(path, sizeof path, "%s/%s", dir, now);
    uint32_t sz = 0;
    FIO_GetFileSize(path, &sz);

    if (!strcasecmp(now, cand) && sz == cand_sz && sz > 0) {
      fs_log("auto: new frame %s settled at %d bytes", now, (int)sz);
      snprintf(fs_auto_last, sizeof fs_auto_last, "%s", now);
      cand[0] = 0;
      if (film_sim_queue)
        fs_queue_push(now);
      else
        fs_auto_develop_now(now);
    } else {
      snprintf(cand, sizeof cand, "%s", now);
      cand_sz = sz;
    }
  }

  fs_auto_running = 0;
  fs_log("auto: watcher stopped");
  fs_log_flush();
}

static void fs_autodev_task(void *unused) {
  /* Let ML finish starting before touching the display or the card layer. */
  msleep(4000);

  fs_log("autodev: start");
  fs_log_flush();

  fs_scan_dcim_photos();
  fs_log("autodev: %d candidate files in DCIM", dcim_photo_count);
  fs_log_flush();

  if (dcim_photo_count <= 0) {
    fs_log("autodev: nothing to develop");
    fs_log_flush();
    return;
  }

  dcim_photo_index = fs_pick_newest_raw();
  fs_log("autodev: selected %s", dcim_photo_list[dcim_photo_index]);
  fs_log_flush();

  fs_develop_batch();

  fs_log("autodev: finished");
  fs_log_flush();
}

/* ---------------------------------------------------------------- key handler
 *
 * Runs in Canon's GUI event task through ML's CBR_KEYPRESS dispatch. Nothing
 * here may block: no FIO, no log flush, no property writes, no task_create.
 * A stall in this callback stops every later key press on the camera, which is
 * exactly the "GUI is up but no button does anything" symptom.
 *
 * Return 0 = consumed, 1 = let the event through to the firmware.
 */
/* Key input.
 *
 * FILMSIM.LOG shows the module receiving numbers like 17, 15, 55, 5 and 105
 * while the shelf owned the screen, and 52/4 while the darkroom did -- so the
 * raw event code is not reliable across screens and builds. Instead of
 * guessing a second time, every event is mapped through BOTH tables:
 *
 *   - ML's portable codes (MODULE_KEY_*), via module_translate_key();
 *   - this body's raw BGMT codes from platform/6D.116/gui.h.
 *
 * Either match produces the same action, so a press works whichever form
 * arrives. Anything unrecognised is still swallowed so Canon's menu cannot
 * move underneath our screen, and its code is shown on screen and logged.
 */
enum fs_act {
  ACT_NONE = 0,
  ACT_LEFT,
  ACT_RIGHT,
  ACT_UP,
  ACT_DOWN,
  ACT_UP_WHEEL,
  ACT_DOWN_WHEEL,
  ACT_SET,
  ACT_Q,
  ACT_PLAY,
  ACT_MENU,
  ACT_TRASH
};

/*
 * Raw BGMT codes only.
 *
 * This used to test the portable MODULE_KEY_* codes first, in the same switch
 * as the raw ones, and the two number spaces collide: BGMT_MENU is 0x06 and
 * MODULE_KEY_DOWN_WHEEL is also 6, so MENU was decoded as "wheel down" and fell
 * into the shelf's default case, which only redraws. FILMSIM.LOG has it
 * literally: "rawkey p=6 act=6" (6 = ACT_DOWN_WHEEL). That is why there was no
 * way out of the shelf. Raw codes are decoded here and nowhere else; the
 * translated callback has its own table.
 */
static int fs_act_of(int k) {
  switch (k) {
  case 0x34:
    return ACT_LEFT; /* BGMT_PRESS_LEFT  */
  case 0x30:
    return ACT_RIGHT; /* BGMT_PRESS_RIGHT */
  case 0x2e:
    return ACT_UP; /* BGMT_PRESS_UP    */
  case 0x32:
    return ACT_DOWN; /* BGMT_PRESS_DOWN  */
  case 0x02:
    return ACT_LEFT; /* BGMT_WHEEL_LEFT  */
  case 0x03:
    return ACT_RIGHT; /* BGMT_WHEEL_RIGHT */
  case 0x00:
    return ACT_UP_WHEEL; /* BGMT_WHEEL_UP   */
  case 0x01:
    return ACT_DOWN_WHEEL; /* BGMT_WHEEL_DOWN */
  case 0x04:
    /* PRESS_SET only. UNPRESS_SET (0x05) used to map here too, so one press
     * stepped the creator's field twice and skipped Luminance entirely:
     * FILMSIM.LOG has "p=4 act=7" and "p=5 act=7" as a pair, ~150 ms apart,
     * for every single press. An unpress carries no new information. */
    return ACT_SET;
  case 0x05:
    return ACT_NONE; /* BGMT_UNPRESS_SET */
  case 0x1d:
    return ACT_Q;
  case 0x0b:
    return ACT_PLAY;
  case 0x06:
    return ACT_MENU;
  case 0x0d:
    return ACT_TRASH;
  /* 0x54, half-shutter and ZOOM_IN are deliberately NOT actions.
   *
   * 0x54 (GMT_GUICMD_PRESS_BUTTON_SOMETHING) is emitted for *every* press and
   * the real code follows in the same millisecond -- FILMSIM.LOG has
   * "p=84 act=2" immediately before "p=29 (Q)", "p=11 (PLAY)", "p=13 (TRASH)"
   * and "p=6 (MENU)". Treating it as "next film" therefore moved the film one
   * step before every real key, which is why SET applied a different stock than
   * the one on screen and why left/right appeared to roll wrongly. It is now
   * held for FS_SOMETHING_WAIT_MS and only used when no real key arrives.
   *
   * Half-shutter is the autofocus/exposure button; mapping it to "back" rewound
   * the film on every half-press (12 of them in one session). ZOOM_IN is the
   * zoom control. Neither belongs to this screen. */
  default:
    return ACT_NONE;
  }
}

static int fs_key_handle(int k);

static int fs_key_handle(int k);

static int fs_stat_redraw_swallowed;

static unsigned int fs_keypress_raw_cbr(unsigned int event_ptr) {
  struct event *ev = (struct event *)event_ptr;
  int k = ev ? ev->param : -1;

  /*
   * MLEV_REDRAW (-7) is ML asking for a full redraw, and it travels through
   * this very callback: redraw() posts it with fake_simple_button(), and
   * handle_ml_events() (debug.c) turns it into _redraw_do() -- which enables
   * Canon's front buffer and calls dialog_redraw() over the whole bitmap
   * canvas. Any ML task that calls redraw() therefore paints Canon's dialog
   * on top of whatever this module has drawn: NotifyBox_task, the LiveView
   * hiprio task, lens_display_set_dirty(), menu_set_dirty(). Passing it
   * through is why screens were wiped from outside this module, which no
   * amount of front-buffer juggling here could prevent.
   *
   * While a screen owns the panel it is swallowed, and the screen's own loop
   * repaints from its idle buffer instead.
   */
  if (k == MLEV_REDRAW) {
    if (fs_holding_panel) {
      fs_gui_dirty = 1;
      fs_darkroom_dirty = 1;
      fs_creator_dirty = 1;
      fs_wb_dirty = 1;
      if (!fs_stat_redraw_swallowed++) {
        /* Logged, not flushed: this runs on Canon's GuiMainTask, and a
         * synchronous FIO write here stalls the body. fs_slot_backup_task
         * writes the ring buffer out within 500 ms. */
        fs_log("rawkey: MLEV_REDRAW swallowed, a module screen owns the panel");
      }
      return 0;
    }
    return 1;
  }

  /* Events the body never should send. FILMSIM.LOG had twenty of these per
   * session -- "p=-7 x-123456" -- and each one forced a redraw. They are
   * counted once and then ignored, so a real regression still shows up. */
  if (!ev || k < 0 || k > 0xFF) {
    if (!fs_stat_junk_keys++) {
      /* No flush here either -- same callback, same reason. */
      fs_log("rawkey: ignoring impossible events (p=%d arg=%d); further ones "
             "are counted only",
             k, ev ? ev->arg : 0);
    }
    return 1;
  }

  fs_last_key = k;
  fs_key_count++;
  int act = fs_act_of(k);

  if (k == 0x54) {
    if (fs_gui_active || fs_darkroom_active) {
      int changed = fs_prov_step();
      fs_log("rawkey p=84: step pending (changed=%d)", changed);
      if (changed) {
        if (fs_gui_active)
          fs_gui_dirty = 1;
        if (fs_darkroom_active)
          fs_darkroom_dirty = 1;
      }
    }
    return 0;
  }

  if (act != ACT_NONE) {
    /* A key with identity inside the provisional window means Canon did send
     * the real code after all: this event owns the step, so drop the mark and
     * let the switch below take it exactly once. */
    if (fs_prov_active && get_ms_clock() - fs_prov_ms <= FS_PROVISIONAL_MS)
      fs_prov_clear();
    /*
     * Every screen logs its raw keys, not just the shelf and the darkroom.
     *
     * The creator and the WB grid were missing from this test, so a screen
     * whose dial handling I had just changed left no trace of which code the
     * body actually sends -- and "spinning the dial does nothing" would have
     * had to be diagnosed with no evidence at all. The log is the instrument
     * here; a screen that can be driven must be a screen that can be read.
     */
    if (fs_gui_active || fs_darkroom_active || fs_creator_active || fs_wb_active)
      fs_log("rawkey p=%d act=%d", k, act);
  }

  /* The main dial arrives grouped: one event can carry several detents. */
  int n = (ev->arg > 0 && ev->arg < 16) ? ev->arg : 1;
  int handled = 0;
  for (int i = 0; i < n; i++)
    handled |= (fs_key_handle(k) == 0);
  return (unsigned int)(handled ? 0 : 1);
}

static int fs_key_handle(int k) {
  int act = fs_act_of(k);

  /* The picture viewer: any of the exit keys closes it, nothing else is used.
   */
  if (fs_view_active) {
    if (act == ACT_MENU || act == ACT_PLAY || act == ACT_TRASH ||
        act == ACT_SET || act == ACT_Q)
      fs_view_exit = 1;
    return 0;
  }

  if ((fs_gui_active || fs_darkroom_active) &&
      (int32_t)(fs_key_guard_until - get_ms_clock()) > 0) {
    static int told;
    if (!told) {
      told = 1;
      fs_log("rawkey p=%d ignored: %dms after the screen was taken", k,
             (int)(get_ms_clock() - (fs_key_guard_until - FS_KEY_GUARD_MS)));
    }
    return 0;
  }
  fs_last_act = act;
  fs_last_raw = k;

  if (fs_wb_active) {
    switch (act) {
    /* LEFT/RIGHT are Fuji's Red axis, UP/DOWN its Blue axis, so the grid
     * moves the way the numbers in a published recipe read */
    case ACT_LEFT:
    case ACT_UP_WHEEL:             /* the other rotating control */
                    film_sim_wbs_gm = fs_clamp_i(film_sim_wbs_gm + 1, -9, 9);
                    fs_wb_applied = 0; fs_wb_dirty = 1; return 0;
    case ACT_RIGHT:
    case ACT_DOWN_WHEEL:
                    film_sim_wbs_gm = fs_clamp_i(film_sim_wbs_gm - 1, -9, 9);
                    fs_wb_applied = 0; fs_wb_dirty = 1; return 0;
    case ACT_UP:    film_sim_wbs_ba = fs_clamp_i(film_sim_wbs_ba - 1, -9, 9);
                    fs_wb_applied = 0; fs_wb_dirty = 1; return 0;
    case ACT_DOWN:  film_sim_wbs_ba = fs_clamp_i(film_sim_wbs_ba + 1, -9, 9);
                    fs_wb_applied = 0; fs_wb_dirty = 1; return 0;
    case ACT_SET:
      fs_wb_apply = 1;
      return 0;
    case ACT_Q:
      film_sim_wbs_ba = 0;
      film_sim_wbs_gm = 0;
      fs_wb_applied = 0;
      fs_wb_dirty = 1;
      return 0;
    case ACT_MENU:
    case ACT_TRASH:
      fs_wb_exit = 1;
      return 0;
    default:
      return 0;
    }
  }

  if (fs_creator_active) {
    switch (act) {
    /*
     * Both rotating controls, not one. The body sends WHEEL_LEFT/RIGHT as
     * 0x02/0x03 and WHEEL_UP/DOWN as 0x00/0x01; fs_act_of() decodes the
     * second pair as ACT_UP_WHEEL/ACT_DOWN_WHEEL and NOTHING in the module
     * handled those, so one of the two dials was dead on every screen that
     * offered rotation. ML's own menus treat up/left and down/right as the
     * same direction, which is why the pairing below is up=left.
     */
    case ACT_LEFT:
    case ACT_UP_WHEEL:             fs_creator_step(&fs_creator, 1); fs_creator_dirty = 1; return 0;
    case ACT_RIGHT:
    case ACT_DOWN_WHEEL:           fs_creator_step(&fs_creator, 2); fs_creator_dirty = 1; return 0;
    case ACT_UP:                   fs_creator_step(&fs_creator, 3); fs_creator_dirty = 1; return 0;
    case ACT_DOWN:                 fs_creator_step(&fs_creator, 4); fs_creator_dirty = 1; return 0;
    case ACT_Q:                    fs_creator_step(&fs_creator, 6); fs_creator_dirty = 1; return 0;
    case ACT_SET:
      /*
       * SET means one thing on this screen, whichever page is showing: move
       * to the next field. It used to write the card on the tone page, so a
       * key that is harmless on the wheel started an FIO write one press
       * away. Saving is PLAY, on both pages.
       */
      fs_creator_step(&fs_creator, 5);
      fs_creator_dirty = 1;
      return 0;
    case ACT_PLAY:
      fs_creator_save = 1;
      return 0;
    case ACT_MENU:
    case ACT_TRASH:
      fs_creator_exit = 1;
      return 0;
    default:
      return 0;
    }
  }
  if (fs_gui_active) {
    int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
    switch (act) {
    case ACT_LEFT:
    case ACT_UP:
    case ACT_UP_WHEEL:
      film_sim_stock = (film_sim_stock + count - 1) % count;
      /* the choice reaches the camera when the dial settles -- see fs_apply_wait */
      fs_apply_wait = 1;
      fs_apply_due_ms = get_ms_clock();
      fs_gui_dirty = 1;
      return 0;
    case ACT_RIGHT:
    case ACT_DOWN:
    case ACT_DOWN_WHEEL:
      film_sim_stock = (film_sim_stock + 1) % count;
      fs_apply_wait = 1;
      fs_apply_due_ms = get_ms_clock();
      fs_gui_dirty = 1;
      return 0;
    case ACT_Q:
      film_sim_dev = (film_sim_dev + 1) % FS_DEV_COUNT;
      fs_gui_dirty = 1;
      return 0;
    case ACT_SET:
      /* Apply, stay. This used to close the screen, which read as "the GUI
       * does not show": FILMSIM.LOG has "first frame drawn" and "shelf: closed"
       * 9 ms apart, with the SET press between them. */
      fs_apply_wait = 0;
      fs_apply_request = 1;
      fs_toast_until = get_ms_clock() + 1500;
      snprintf(fs_toast, sizeof fs_toast, "APPLIED  %s",
               film_active_table[fs_develop_stock_index(count)].name);
      fs_gui_dirty = 1;
      return 0;
    case ACT_PLAY:
      fs_view_request = FS_VIEW_NONE;
      fs_want_playback = 1; /* look at the pictures, not another screen */
      fs_gui_close();
      return 0;
    case ACT_MENU:
    case ACT_TRASH:
      fs_view_request = FS_VIEW_NONE;
      fs_gui_close();
      return 0;
    default:
      fs_gui_dirty = 1; /* show the unrecognised code */
      return 0;
    }
  }

  if (fs_darkroom_active) {
    int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
    switch (act) {
    case ACT_UP:
    case ACT_UP_WHEEL:
      film_sim_stock = (film_sim_stock + count - 1) % count;
      fs_apply_request = 1;
      fs_darkroom_dirty = 1;
      return 0;
    case ACT_DOWN:
    case ACT_DOWN_WHEEL:
      film_sim_stock = (film_sim_stock + 1) % count;
      fs_apply_request = 1;
      fs_darkroom_dirty = 1;
      return 0;
    case ACT_LEFT:
      if (dcim_photo_count > 0)
        dcim_photo_index =
            (dcim_photo_index + dcim_photo_count - 1) % dcim_photo_count;
      fs_darkroom_dirty = 1;
      return 0;
    case ACT_RIGHT:
    case ACT_PLAY:
      if (dcim_photo_count > 0)
        dcim_photo_index = (dcim_photo_index + 1) % dcim_photo_count;
      fs_darkroom_dirty = 1;
      return 0;
    case ACT_Q:
      film_sim_dev = (film_sim_dev + 1) % FS_DEV_COUNT;
      fs_darkroom_dirty = 1;
      return 0;
    case ACT_SET:
      fs_dev_request = 1;
      return 0;
    case ACT_MENU:
    case ACT_TRASH:
      fs_dev_request = 0;
      fs_view_request = FS_VIEW_SHELF; /* one key back to the film list */
      fs_darkroom_close();
      return 0;
    default:
      fs_darkroom_dirty = 1;
      return 0;
    }
  }

  return 1;
}

/* The translated callback stays registered only to keep the key counter alive
 * for the "did any key arrive" watchdog; the screens no longer read it. */
static unsigned int fs_keypress_cbr(unsigned int key) {
  (void)key;
  return 1;
}

/* Recover or clean up any interrupted .TMP develops from unexpected power-off
 */
static void fs_recover_tmp_files(void) {
  const char *dir = get_dcim_dir();
  if (!dir || !dir[0])
    return;

  struct fio_file *file = alloc_fio_file();
  if (!file)
    return;

  struct fio_dirent *de = FIO_FindFirstEx(dir, file);
  if (IS_ERROR(de)) {
    free(file);
    return;
  }

  do {
    struct file_info fi = convert_fio_file_info(file);
    if (!fi.name[0] || fi.name[0] == '.')
      continue;
    if (fs_ends_with(fi.name, ".TMP")) {
      char tmp_path[FS_PATH_LEN];
      snprintf(tmp_path, sizeof(tmp_path), "%s/%s", dir, fi.name);
      uint32_t sz = 0;
      if (FIO_GetFileSize(tmp_path, &sz) == 0 && sz > 65536) {
        char final_path[FS_PATH_LEN];
        int l = strlen(tmp_path);
        if (l > 4) {
          uint32_t have = 0;
          snprintf(final_path, sizeof(final_path), "%s", tmp_path);
          memcpy(final_path + l - 4, ".JPG", 4);
          /* FAT will not rename onto an existing name, and with Replace Camera
           * JPEG on that name is the camera's own file: keep the camera's and
           * drop the leftover rather than leave a .TMP behind for ever. */
          if (FIO_GetFileSize(final_path, &have) == 0 && have > 0) {
            FIO_RemoveFile(tmp_path);
            fs_log("develop: dropped stale tmp %s, %s already exists", tmp_path,
                   final_path);
          } else if (FIO_RenameFile(tmp_path, final_path) == 0) {
            fs_log("develop: recovered %s from tmp: %d bytes", final_path,
                   (int)sz);
          }
        }
      } else {
        FIO_RemoveFile(tmp_path);
        fs_log("develop: removed partial tmp %s (%d bytes)", tmp_path, (int)sz);
      }
    }
  } while (FIO_FindNextEx(de, file) == 0);

  FIO_FindClose(de);
  free(file);
}

/* ---------------------------------------------------------------- xml reload
 */

static MENU_SELECT_FUNC(fs_reload_xml_action) {
  int count = fs_load_films_xml();
  fs_group_by_artwork();
  fs_film_gen++;
  NotifyBox(2000, "Loaded %d XML Films from %s", count, FILM_DIR);
}


/* ---------------------------------------------------------------- liveview
 * info
 *
 * LiveView OSD overlay item (Florin's badge): displays active stock and
 * push/pull mode.
 */

static LVINFO_UPDATE_FUNC(film_lvinfo_update) {
  LVINFO_BUFFER(64);
  if (!film_active_count)
    return;
  int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
  int idx = fs_develop_stock_index(count);
  const film_xml_t *f = &film_active_table[idx];
  const char *soup_str = (film_sim_dev == FS_DEV_PUSH_1)   ? " [P+1]"
                         : (film_sim_dev == FS_DEV_PUSH_2) ? " [P+2]"
                         : (film_sim_dev == FS_DEV_PULL_1) ? " [P-1]"
                                                           : "";
  snprintf(buffer, sizeof(buffer), "%s%s", f->code[0] ? f->code : f->name,
           soup_str);
  item->color_fg = COLOR_YELLOW;
}

static struct lvinfo_item film_lvinfo_item = {
    .name = "Film Sim",
    .which_bar = LV_PREFER_TOP_BAR,
    .update = film_lvinfo_update,
    .priority = 10,
    .preferred_position = -1,
};

/* Which photo Develop uses, chosen from the menu instead of only inside the
 * darkroom. SET advances, Q goes back, same rule as every other entry. */
static MENU_UPDATE_FUNC(fs_photo_update) {
  if (dcim_photo_count <= 0)
    fs_scan_dcim_photos();
  if (dcim_photo_count <= 0) {
    MENU_SET_VALUE("(no photos in DCIM)");
    MENU_SET_HELP("No JPEG/CR2 pairs found in DCIM.");
    return;
  }
  dcim_photo_index = fs_clamp_i(dcim_photo_index, 0, dcim_photo_count - 1);
  MENU_SET_VALUE("%s  %d/%d", dcim_photo_list[dcim_photo_index],
                 dcim_photo_index + 1, dcim_photo_count);
  MENU_SET_HELP("SET: next photo, Q: previous. Develop uses this one.");
}

static MENU_SELECT_FUNC(fs_photo_next) {
  if (dcim_photo_count <= 0)
    fs_scan_dcim_photos();
  if (dcim_photo_count > 0)
    dcim_photo_index = (dcim_photo_index + 1) % dcim_photo_count;
}

static MENU_SELECT_FUNC(fs_photo_prev) {
  if (dcim_photo_count <= 0)
    fs_scan_dcim_photos();
  if (dcim_photo_count > 0)
    dcim_photo_index =
        (dcim_photo_index + dcim_photo_count - 1) % dcim_photo_count;
}

/* Stock: SET advances, Q goes back -- one consistent rule for every entry. */
static MENU_SELECT_FUNC(fs_film_prev) {
  int count = (film_active_count > 0) ? film_active_count : FILM_COUNT;
  menu_numeric_toggle(&film_sim_stock, -delta, 0, count - 1);
  fs_sync();
}

static MENU_UPDATE_FUNC(fs_level_update) {
  static const char *const names[4] = {"", "1 style only", "2 style + grade",
                                       "3 raw develop"};
  int lv = film_sim_level;
  if (lv < FS_LEVEL_STYLE || lv > FS_LEVEL_RAW)
    lv = FS_LEVEL_GRADE;
  MENU_SET_VALUE("%s", names[lv]);
  MENU_SET_HELP("1: the look is written into the picture-style slots and the "
                "camera bakes it into the JPEG - nothing to develop. "
                "2: style plus the engine's curves, hue axes, split tone, "
                "halation and grain on the preview. "
                "3: decode the sensor raw and grade it from scratch.");
}

/* Shown on every develop entry: with Level 1 nothing is developed, and that is
 * not visible from the entry itself. */
static MENU_UPDATE_FUNC(fs_current_update) {
  MENU_SET_VALUE("");
  if (film_sim_level < FS_LEVEL_GRADE)
    MENU_SET_WARNING(MENU_WARN_NOT_WORKING,
                     "Level is 1 (picture style only) - develop will skip. "
                     "Set Level to 2 or 3.");
}


static MENU_UPDATE_FUNC(fs_replace_update) {
  MENU_SET_VALUE(film_sim_replace ? "On" : "Off");
  MENU_SET_ICON(film_sim_replace ? MNI_ON : MNI_OFF, 0);
  MENU_SET_HELP(film_sim_replace
                    ? "The camera's own JPEG for a developed frame is replaced "
                      "by the film version. Only shoot RAW if you want to keep "
                      "the original JPEG."
                    : "Developed frames get a free IMG number; the camera does "
                      "not list them until the card is re-inserted.");
}

static MENU_UPDATE_FUNC(fs_satboost_update) {
  static int last = -1;
  MENU_SET_VALUE("%d", film_sim_satboost);
  fs_yuvlook_set_satboost(film_sim_satboost);
  if (film_sim_satboost != last) {
    /* Develop reads film_sim_satboost at develop time; the shot/LV tables are
     * baked, so they only follow if the bake is asked to run again. */
    last = film_sim_satboost;
    fs_yuvlook_dirty = 1;
  }
  MENU_SET_HELP("Saturation on top of every stock's own SAT line: 100 leaves "
                "the recipe as written, 160 is roughly one and a half times as "
                "saturated. Applies to developed files and to LiveView.");
}

static const char *fs_gradeq_name(int q) {
  return q == 0 ? "High" : (q == 1 ? "Normal" : "Fast");
}

static MENU_UPDATE_FUNC(fs_gradeq_update) {
  MENU_SET_VALUE("%s", fs_gradeq_name(film_sim_gradeq));
  MENU_SET_HELP(film_sim_gradeq == 0
                    ? "High: the full look, per-hue cells included. 21 ms of "
                      "grade per 5472x3648 frame on the host, ~1 s on the body."
                    : film_sim_gradeq == 1
                          ? "Normal: luma curve, global chroma gain and matrix; "
                            "the per-hue cells are dropped. 16.7 ms on the host."
                          : "Fast: tone curve only, chroma untouched. 4.1 ms on "
                            "the host -- but no colour shift, only tone.");
}

static MENU_SELECT_FUNC(fs_gradeq_select) {
  menu_numeric_toggle(&film_sim_gradeq, delta, 0, 2);
  fs_yuvlook_set_quality(film_sim_gradeq);
  fs_log("grade quality: %s (%d)", fs_gradeq_name(film_sim_gradeq),
         film_sim_gradeq);
  fs_log_flush();
}

static MENU_UPDATE_FUNC(fs_auto_update) {
  MENU_SET_VALUE(film_sim_auto ? "On" : "Off");
  MENU_SET_ICON(film_sim_auto ? MNI_ON : MNI_OFF, 0);
  MENU_SET_HELP("On: every new frame, and picking level 2 or 3, develops the "
                "film version with no keypress. Off: develop only when asked.");
}

static MENU_UPDATE_FUNC(fs_queue_update) {
  static char buf[40];
  if (film_sim_queue && fs_queue_n > 0)
    snprintf(buf, sizeof buf, "On, %d queued", (int)fs_queue_n);
  else
    snprintf(buf, sizeof buf, "%s", film_sim_queue ? "On" : "Off");
  MENU_SET_VALUE("%s", buf);
  MENU_SET_ICON(film_sim_queue ? MNI_ON : MNI_OFF, 0);
  MENU_SET_HELP("On: the develop runs without taking the screen -- no "
                "UILOCK_SHUTTER, so the shutter stays live -- and a worker "
                "drains the queue behind you. The JPEG on the card is Canon's "
                "until the queue reaches that frame. Off: the develop owns the "
                "panel for its whole run.");
}

static MENU_SELECT_FUNC(fs_queue_toggle) {
  menu_numeric_toggle(&film_sim_queue, delta, 0, 1);
  if (film_sim_queue)
    fs_queue_start();
  /* Turning it off does not kill a running develop; the worker's loop sees the
   * flag and stops after the frame in hand, which is the only frame that could
   * be left half-written. */
  fs_log("queue: %s (auto %s)", film_sim_queue ? "on" : "off",
         film_sim_auto ? "on" : "off");
  fs_log_flush();
}

static MENU_SELECT_FUNC(fs_auto_toggle) {
  menu_numeric_toggle(&film_sim_auto, delta, 0, 1);
  if (film_sim_auto && !fs_auto_running)
    task_create("fs_auto", 0x1e, FS_DEV_STACK, fs_auto_task, 0);
  if (film_sim_queue)
    fs_queue_start();
  fs_log("auto: %s", film_sim_auto ? "on" : "off");
  fs_log_flush();
}

/* The 4-stock advertising batch, from the menu. */
static void fs_compare_task(void *unused) {
  (void)unused;
  fs_develop_batch();
}
static MENU_SELECT_FUNC(fs_compare_trigger) {
  task_create("fs_cmp", 0x1e, FS_DEV_STACK, fs_compare_task, 0);
}

static struct menu_entry fs_menu_develop[] = {
    {
        .name = "Darkroom",
        .select = fs_darkroom_open,
        .icon_type = IT_ACTION,
        .help = "Pick photo, stock and chemistry, then SET to develop.",
    },
    {
        .name = "Current Photo",
        .select = fs_develop_trigger,
        .update = fs_current_update,
        .icon_type = IT_ACTION,
        .help = "Develop the photo chosen by the Photo entry, with the "
                "current stock.",
    },
    {
        .name = "External LUT",
        .priv = &film_sim_lut,
        .max = 1,
        .help = "Apply ML/FILMSIM/LOOK.BIN while developing. Make that file "
                "from a .cube with tools/cube2bin.py. Set Strength to 0 to "
                "develop with the cube alone.",
    },
    {
        .name = "Replace Camera JPEG",
        .priv = &film_sim_replace,
        .max = 1,
        .update = fs_replace_update,
        .help = "On: the developed frame is written over the camera's JPEG for "
                "that frame, so the film version sits at the number playback "
                "already knows. The raw is never touched. Off: a free number.",
    },
    {
        .name = "Auto Develop",
        .priv = &film_sim_auto,
        .max = 1,
        .select = fs_auto_toggle,
        .update = fs_auto_update,
        .help = "Develop by itself after each shot and when level 2/3 is "
                "picked.",
    },
    {
        /* film.sim.queue used to be reachable only as a config variable, which
         * meant the only documented way to get a non-blocking develop was to
         * know the variable's name. It is the difference between the shutter
         * being locked for the length of a develop and not. */
        .name = "Background Develop",
        .priv = &film_sim_queue,
        .max = 1,
        .select = fs_queue_toggle,
        .update = fs_queue_update,
        .help = "On: a develop never takes the screen, so the shutter keeps "
                "working and queued frames are developed behind you. Off: the "
                "develop owns the panel until it finishes.",
    },
    {
        .name = "Comparison x4",
        .select = fs_compare_trigger,
        .update = fs_current_update,
        .icon_type = IT_ACTION,
        .help = "Develop the newest photo with 4 distinct stocks into "
                "FLC1..FLC4.JPG. Takes about 3 minutes.",
    },
    {
        /* This sat after MENU_EOL, and menu iteration stops at that entry, so
         * the row was in the array and never on the screen. */
        .name = "View Last Develop",
        .select = fs_view_last,
        .icon_type = IT_ACTION,
        .help = "Show the last developed frame on the camera's own screen. "
                "MENU or PLAY closes it.",
    },
    MENU_EOL,
};

static struct menu_entry fs_menu_capture[] = {
    {
        .name = "Neutral Capture",
        .priv = &film_sim_capture,
        .max = 1,
        .help = "Level 2/3 only: write a neutral picture style so Develop "
                "grades once instead of stacking a look on a look. Ignored at "
                "level 1, where the style is the look.",
    },
    {
        .name = "WB Shift Look",
        .priv = &film_sim_wb_shift,
        .max = 1,
        .help = "Apply the emulsion colour cast through WB shift and Kelvin.",
    },
    {
        .name = "Lock Film ISO",
        .priv = &film_sim_lock_iso,
        .max = 1,
        .help = "Hold sensor ISO at the stock's box speed.",
    },
    MENU_EOL,
};

static struct menu_entry fs_menu_debug[] = {
    {
        .name = "Flush Log",
        .select = fs_debug_log_select,
        .icon_type = IT_ACTION,
        .help = "Write the diagnostic buffer to ML/LOGS/FILMSIM.LOG now.",
    },
    {
        .name = "Reload Films",
        .select = fs_reload_xml_action,
        .icon_type = IT_ACTION,
        .help = "Re-read the film list from ML/FILMS/*.xml.",
    },
    MENU_EOL,
};

/* The develop side: choose a frame, a film and run the engine. */
static const char *fs_level_names[] = { "Off", "Weak", "Strong" };

static MENU_UPDATE_FUNC(fs_fuji_init_update) { fs_fuji_ensure_init(); }

static MENU_UPDATE_FUNC(fs_fuji_name_update) {
  fs_fuji_ensure_init();
  if (fs_fuji.name[0] == 0) {
    snprintf(fs_fuji.name, sizeof fs_fuji.name, "%s", fs_fuji_names[fs_fuji_slot]);
  }
  MENU_SET_VALUE("%s", fs_fuji.name);
}

static MENU_SELECT_FUNC(fs_fuji_name_select) {
  fs_fuji_slot = (fs_fuji_slot + 1) % 4;
  snprintf(fs_fuji.name, sizeof fs_fuji.name, "%s", fs_fuji_names[fs_fuji_slot]);
}

static struct menu_entry fs_menu_fuji[] = {
    {
        .name = "Name",
        .update = fs_fuji_name_update,
        .select = fs_fuji_name_select,
        .help = "Which C-slot this recipe is saved as: ML/FILM/<NAME>.fs.",
    },
    {
        .name = "TONE CURVE Hi",
        .update = fs_fuji_init_update,
        .priv = &fs_fuji.highlight,
        .min = -2, .max = 4,
        .help = "Highlight -2..+4. Positive compresses the top end, as Fuji's "
                "does. Written into the recipe's CURVE.",
    },
    {
        .name = "TONE CURVE Sh",
        .priv = &fs_fuji.shadow,
        .min = -2, .max = 4,
        .help = "Shadow -2..+4. Positive deepens the bottom end.",
    },
    {
        .name = "Color",
        .priv = &fs_fuji.color,
        .min = -4, .max = 4,
        .help = "Colour -4..+4, written as SAT 52..148.",
    },
    {
        .name = "Color Chrome",
        .priv = &fs_fuji.chrome,
        .min = 0, .max = 2,
        .choices = fs_level_names,
        .help = "Fuji's COLOR CHROME EFFECT. Deepens colours that are already "
                "saturated and leaves dull ones alone.",
    },
    {
        .name = "Chrome FX Blue",
        .priv = &fs_fuji.fx_blue,
        .min = 0, .max = 2,
        .choices = fs_level_names,
        .help = "Fuji's COLOR CHROME FX BLUE: the same, restricted to blues.",
    },
    {
        .name = "Grain Effect",
        .priv = &fs_fuji.grain,
        .min = 0, .max = 2,
        .choices = fs_level_names,
        .help = "GRAIN EFFECT, off/weak/strong.",
    },
    {
        .name = "Grain Size",
        .priv = &fs_fuji.grain_size,
        .min = 0, .max = 1,
        .help = "0 = small, 1 = large.",
    },
    {
        .name = "WB Shift R/B",
        .select = fs_wb_trigger,
        .icon_type = IT_ACTION,
        .help = "Fuji's Red and Blue WB shift. Written to the camera, not to "
                "the recipe: it is a white balance, not part of a look.",
    },
    {
        .name = "Dynamic Range",
        .select = fs_fuji_unavailable,
        .priv = "Cannot be reproduced: DR200/400 raise ISO and pull "
                "highlights before digitising. On an 8-bit JPEG the headroom "
                "is already gone.",
        .icon_type = IT_ACTION,
        .help = "Present in a Fuji recipe, not reproducible on this body.",
    },
    {
        .name = "Clarity",
        .select = fs_fuji_unavailable,
        .priv = "Implemented and measured, then reverted: it needs a ring of "
                "GRADED luma, and the current ring sums input luma, so the "
                "tone curve's own lift was read as detail. See "
                "docs/fuji-recipe-fields.md.",
        .icon_type = IT_ACTION,
        .help = "Present in a Fuji recipe, not available here yet.",
    },
    {
        .name = "High ISO NR",
        .select = fs_fuji_unavailable,
        .priv = "Canon's noise reduction lives in the picture style and is "
                "not driven by this module yet.",
        .icon_type = IT_ACTION,
        .help = "Present in a Fuji recipe, not wired yet.",
    },
    {
        .name = "WB Grid",
        .select = fs_wb_trigger,
        .icon_type = IT_ACTION,
        .help = "The 2-D Blue/Amber vs Green/Magenta grid, written into the "
                "camera's own white balance the way a Fuji recipe does. The "
                "R/B row above is the same offset as numbers.",
    },
    {
        .name = "Save Recipe",
        .select = fs_fuji_save_trigger,
        .icon_type = IT_ACTION,
        .help = "Write ML/FILM/<NAME>.fs and reload the shelf. The recipe then "
                "shoots and develops like any other stock.",
    },
    MENU_EOL,
};



static struct menu_entry fs_menu_live[] = {
    {
        .name = "LiveView 3D",
        .priv = &film_sim_lv3d,
        .max = 1,
        .help = "Grade the preview through a joint (y,u,v) lattice baked from "
                "the stock instead of the preset tables: 3.3 levels off the "
                "develop against 8.8, with 5x less chroma error (host test "
                "se/hosttest/lv3d.c). Costs about 3.3x the table lookups, so "
                "watch FILMSIM.LOG's grade_ms before leaving it on.",
    },
    {
        .name = "LiveView Frame",
        .select = menu_open_submenu,
        .icon_type = IT_ACTION,
        .help = "Filter, frame rate and the in-camera style.",
        .children = film_sim_liveview_children,
    },
    {
        .name = "Canister Art",
        .priv = &film_sim_art,
        .max = 1,
        .help = "Photographs of the film from ML/FILMSIM/CANS.DAT. Off draws "
                "the cassette instead, which needs no file at all.",
    },
    {
        .name = "Clear Canon Overlays",
        .select = fs_lv_clear_overlays,
        .update = fs_lv_clear_update,
        .icon_type = IT_ACTION,
        .help = "The grade cannot run while Canon's own LV overlays have the "
                "panel. SET sends INFO once, then reports the switch.",
    },
    MENU_EOL,
};

/*
 * User Look: the looks the Color Creator saved, applied by name. The shelf
 * lists every stock as well, so "which of my four looks was that" used to
 * mean reading a shelf of thirty-four. These four rows say it outright.
 */
static int fs_user_slot[4] = { 0, 1, 2, 3 };

static void fs_user_look_apply(int slot)
{
  char want[12];
  int i, n = (film_active_count > 0) ? film_active_count : FILM_COUNT;

  if (slot <= 0) snprintf(want, sizeof want, "WHEEL");
  else           snprintf(want, sizeof want, "WHEEL%d", slot + 1);

  for (i = 0; i < n; i++) {
    /* The slot file name is the identity (see fs_register_look); the name is
     * the shelf label. Looks saved by an older build carry the typed name in
     * both fields, so those are still found by the second test. */
    if (!strcasecmp(film_active_table[i].code, want) ||
        !strcasecmp(film_active_table[i].name, want)) {
      film_sim_stock = i;
      fs_apply_film_now();
      beep_times(1);
      fs_log("userlook: applied '%s' (stock %d)", film_active_table[i].name, i);
      fs_log_flush();
      NotifyBox(1200, "Look: %s", film_active_table[i].name);
      return;
    }
  }
  fs_log("userlook: '%s' not in the film list (%d stocks)", want, n);
  fs_log_flush();
  NotifyBox(1800, "%s.fs not found -- not saved, or its NAME was changed", want);
}

static MENU_SELECT_FUNC(fs_user_look_select) {
  (void)delta;
  fs_user_look_apply(*(const int *)priv);
}

static struct menu_entry fs_menu_userlook[] = {
    {
        .name = "Apply WHEEL",
        .priv = &fs_user_slot[0],
        .select = fs_user_look_select,
        .icon_type = IT_ACTION,
        .help = "Make ML/FILM/WHEEL.fs the active look.",
    },
    {
        .name = "Apply WHEEL2",
        .priv = &fs_user_slot[1],
        .select = fs_user_look_select,
        .icon_type = IT_ACTION,
        .help = "Make ML/FILM/WHEEL2.fs the active look.",
    },
    {
        .name = "Apply WHEEL3",
        .priv = &fs_user_slot[2],
        .select = fs_user_look_select,
        .icon_type = IT_ACTION,
        .help = "Make ML/FILM/WHEEL3.fs the active look.",
    },
    {
        .name = "Apply WHEEL4",
        .priv = &fs_user_slot[3],
        .select = fs_user_look_select,
        .icon_type = IT_ACTION,
        .help = "Make ML/FILM/WHEEL4.fs the active look.",
    },
    {
        .name = "Share Active Look",
        .select = fs_share_look_trigger,
        .icon_type = IT_ACTION,
        .help = "Copy the active stock's recipe and a readable summary into "
                "ML/SHARE, where any transport can pick them up (card reader, "
                "phone with an SD slot, or Canon's desktop utility). Inbound "
                "already works: drop a .fs into ML/FILM and it is adopted "
                "within seconds. WiFi push straight from this menu is not "
                "implemented -- see docs/wifi-functions.md.",
    },
    {
        .name = "Look Name",
        .select = menu_open_submenu,
        .icon_type = IT_ACTION,
        .help = "Spell the name the Color Creator saves as, up to eight "
                "characters. Defaults to the slot name.",
        .children = fs_menu_name,
    },
    {
        .name = "Save Into",
        .select = fs_slot_cycle,
        .update = fs_slot_update,
        .help = "Which slot the Color Creator writes, and loads that slot's "
                "name. Type a different name in Look Name above.",
    },
    MENU_EOL,
};

/*
 * Everything that is not "pick a stock" or "develop", in one menu.
 *
 * The root used to carry nine submenus plus the shelf buried inside Film Look;
 * picking a stock is the thing done most often and it took three presses from
 * the ML menu. The shelf is now the first row of the root menu and every
 * setting lives here, one level down.
 */
static struct menu_entry fs_menu_header[] = {
    {
        .name = "Film Frame",
        .priv = &film_sim_frame,
        .max = 1,
        .help = "Draw the header around developed files: a margin above, and a "
                "band below with the stock's mark, the Canon wordmark and the "
                "exposure. Off by default; developed files only.",
    },
    {
        .name = "Art",
        .priv = &film_sim_art,
        .max = 1,
        .help = "Use the stock canisters from ML/FILMSIM/CANS.DAT in the header "
                "and on the shelf. Off falls back to the brand mark.",
    },
    MENU_EOL,
};

static struct menu_entry fs_menu_settings[] = {
    {
            .name = "Stock",
            .select = fs_film_next,
            .select_Q = fs_film_prev,
            .update = fs_film_update,
            .help = "SET: next stock, Q: previous stock.",
        },
    {
            .name = "Level",
            .priv = &film_sim_level,
            .max = FS_LEVEL_RAW,
            .update = fs_level_update,
            .help = "1 picture style, 2 style + grade, 3 raw develop.",
        },
    {
        .name = "Strength",
        .priv = &film_sim_strength,
        .max = 100,
        .help = "How much of the stock's look is applied.",
    },
    {
        .name = "Grade Quality",
        .priv = &film_sim_gradeq,
        .max = 2,
        .select = fs_gradeq_select,
        .update = fs_gradeq_update,
        .help = "How much of the look is graded into the shot, and therefore "
                "how long the next frame waits. High is the full look. Normal "
                "drops the per-hue work (greens no longer behave differently "
                "from reds) for about a fifth less time. Fast applies the tone "
                "curve only and leaves colour untouched -- 5x less time, no "
                "colour shift at all. Develop always uses the full look.",
    },
    {
        .name = "Saturation",
        .priv = &film_sim_satboost,
        .min = 20,
        .max = 200,
        .update = fs_satboost_update,
        .help = "Saturation on top of every stock's own SAT line: 100 leaves "
                "the recipe as written, 160 is about one and a half times as "
                "saturated. Applies to developed files and to LiveView.",
    },
    {
        .name = "Grain",
        .priv = &film_sim_grain,
        .max = 100,
        .help = "Grain amount on top of what the stock itself asks for.",
    },
    {
        .name = "Grain Size",
        .priv = &film_sim_grainsize,
        .min = -1, .max = 100,
        .help = "-1 keeps each stock's own GRAIN_SIZE. 0..100 overrides it for "
                "every stock: the engine turns it into a 1..9 pixel cell, and "
                "that cell shrinks with the frame so a half-size develop is "
                "not twice as grainy.",
    },
    {
        .name = "Pad to 3:2",
        .priv = &film_sim_pad,
        .max = 1,
        .update = fs_pad_update,
        .help = "Framings wider than 3:2 (16:9, 65:24 XPan, 2.39:1) are written "
                "into a 3:2 frame with black bars, so the body shows the whole "
                "frame instead of something that is not what you composed.",
    },
    {
        .name = "LiveView",
        .select = menu_open_submenu,
        .icon_type = IT_ACTION,
        .help = "Preview grading, frame and overlay options.",
        .children = fs_menu_live,
    },
    {
        .name = "Capture",
        .select = menu_open_submenu,
        .icon_type = IT_ACTION,
        .help = "Style, white balance and ISO behaviour at capture.",
        .children = fs_menu_capture,
    },
    {
        .name = "Debug",
        .select = menu_open_submenu,
        .icon_type = IT_ACTION,
        .help = "Log and film-list maintenance.",
        .children = fs_menu_debug,
    },
    MENU_EOL,
};
static struct menu_entry film_sim_menu[] = {
    {
        .name = "Film Shelf",
        .select = fs_gui_open,
        .icon_type = IT_ACTION,
        .help = "Open the shelf, choose a film, SET applies it. The choice "
                "stays as the active stock after you leave. Film Settings has "
                "everything else.",
    },
    {
        .name = "Film Settings",
        .select = menu_open_submenu,
        .icon_type = IT_ACTION,
        .help = "Stock, level, strength, saturation, grain, aspect, looks, "
                "LiveView, capture and debug.",
        .children = fs_menu_settings,
    },
    {
        .name = "Header",
        .select = menu_open_submenu,
        .icon_type = IT_ACTION,
        .help = "The border written around developed files: on or off, and "
                "whether the stock shows its canister or its brand mark.",
        .children = fs_menu_header,
    },
    {
        .name = "Aspect",
        .priv = &film_sim_aspect,
        .max = FS_ASPECT_COUNT - 1,
        .choices = fs_aspect_names,
        .update = fs_aspect_update,
        .help = "One setting for both halves of what-you-see-is-what-you-get: "
                "the bars on LiveView and the window a developed file is cut "
                "to are this same mode.",
    },
    {
        .name = "User Look",
        .select = menu_open_submenu,
        .icon_type = IT_ACTION,
        .help = "The looks the Color Creator saved, applied by name.",
        .children = fs_menu_userlook,
    },
    {
        .name = "Custom Look",
        .select = fs_creator_trigger,
        .icon_type = IT_ACTION,
        .help = "The 12-wedge colour creator: saturation, luminance and hue "
                "per colour, plus highlight/shadow. PLAY saves into User Look.",
    },
    {
        .name = "Recipe",
        .select = menu_open_submenu,
        .icon_type = IT_ACTION,
        .help = "A Fuji recipe -- the IMAGE QUALITY SETTING bundle, saved as a "
                ".fs -- plus the Blue/Amber vs Green/Magenta WB grid.",
        .children = fs_menu_fuji,
    },
    {
        .name = "Develop",
        .select = menu_open_submenu,
        .icon_type = IT_ACTION,
        .help = "Develop now: the darkroom, this photo, a 4-way comparison, or "
                "watch for new frames.",
        .children = fs_menu_develop,
    },
    {
        .name = "Photo",
        .select = fs_photo_next,
        .select_Q = fs_photo_prev,
        .update = fs_photo_update,
        .help = "Which frame Develop uses. SET: next, Q: previous.",
    },
    MENU_EOL,
};



/*
 * Why the LiveView grade needs its own eyes.
 *
 * Every module display filter is gated by ML: display_filter_enabled() returns
 * 0 when zebra_should_run() is false, and zebra_should_run() is
 * "liveview_display_idle() && get_global_draw()" on a real body. When Global
 * Draw is off, livev_hipriority_task parks in "while (!zebra_should_run())
 * msleep(100)" *before* it ever calls the filter, so the filter's own logging
 * never runs and the log only ever says frames=0 with no reason. This task
 * watches the same switches independently and reports which one is wrong.
 */
/*
 * Save the frame the shutter just graded.
 *
 * The buffer is Canon's, so the first thing this does after noticing a grade is
 * copy it: the encode takes seconds and the camera is free to reuse that
 * memory the moment the shot is over. 40 MB is only held for the length of one
 * save.
 */

static void fs_lv_monitor_task(void *unused) {
  /* Report what the in-shot grade did. The hook itself only fills the RAM log
   * ring: it runs inside the shutter, where opening a file is not allowed. */
  int last_graded = -1;
  int last_calls = -1;
  (void)unused;
  uint32_t last = 0xFFFFFFFFu;
  unsigned beat_counter = 0;
  while (1) {
    msleep(2000);

    {
      int g = fs_yuvhook_graded();
      int nc = fs_yuvhook_calls();
      /*
       * Report on EVERY change of the call count, not only when a frame was
       * graded.
       *
       * This used to be "if (graded changed) { if (graded > 0) log }", so a
       * shot the hook never saw, and a shot it saw but refused, produced the
       * same thing in the log: nothing at all.  Every check of the shutter
       * path since then has been unable to tell those two apart -- and a
       * missing line was read as "no photo was taken" when it means neither.
       */
      if (g != last_graded || nc != last_calls) {
        last_graded = g;
        last_calls = nc;
        if (g > 0) {
          fs_log("yuvgrade: %d shot(s) graded, buf=0x%08x size=%d, %dx%d "
                 "stride %d, %d rows in %d ms; chunks %d used %d [%d %d %d %d]; "
                 "not graded: %d (last bad size %d) sniff 0x%x "
                 "sites jpeg %d/%d ssraw %d/%d",
                 g, (unsigned)fs_yuvhook_last_buf(),
                 (int)fs_yuvhook_last_size(), fs_yuvhook_last_w(),
                 fs_yuvhook_last_h(), fs_yuvhook_last_stride(),
                 fs_yuvhook_last_rows(), (int)fs_yuvhook_grade_ms(),
                 fs_yuvhook_chunks(), fs_yuvhook_chunk_used(),
                 (int)fs_yuvhook_chunk_size(0), (int)fs_yuvhook_chunk_size(1),
                 (int)fs_yuvhook_chunk_size(2), (int)fs_yuvhook_chunk_size(3),
                 fs_yuvhook_nobuf(), (int)fs_yuvhook_unknown(),
                 fs_yuvhook_sniff_mask(),
                 fs_yuvhook_site_graded(0), fs_yuvhook_site_calls(0),
                 fs_yuvhook_site_graded(1), fs_yuvhook_site_calls(1));
          /*
           * Which picture style the camera itself was on for this shot.
           *
           * "Neutral capture" is only neutral if the camera is on built-in
           * Neutral (0x84). Nothing switches it there unless this module is
           * asked to; when the camera is left on User Def 1 it carries whatever
           * PF2 blob was persisted in it, and Canon bakes that into the JPEG as
           * well as whatever the engine graded. Printing the real value here
           * means the log cannot claim "capture style Neutral" for a frame that
           * was shot on a User Def style.
           */
          fs_log("yuvcost: hook %d ms at %d ms, worst %d ms (grade %d ms); "
                 "malloc pool free %d KB",
                 fs_yuvhook_hook_total_ms(), (int)fs_yuvhook_hook_ms(),
                 fs_yuvhook_hook_total_max(), (int)fs_yuvhook_grade_ms(),
                 GetFreeMemForMalloc() / 1024);
          fs_log("yuvcap: cache alias %s; camera style 0x%02x (%s); engine look %s; bars %d",
                 fs_yuvhook_dcache_used() ? "used (bit 30 cleared, D-cache cleaned)"
                                          : "NOT used",
                 (unsigned)lens_info.raw_picstyle,
                 lens_info.raw_picstyle == 0x84 ? "built-in Neutral"
                   : lens_info.raw_picstyle == 0x21 ? "User Def 1"
                   : lens_info.raw_picstyle == 0x22 ? "User Def 2"
                   : lens_info.raw_picstyle == 0x23 ? "User Def 3"
                   : "other",
                 fs_grade_in_shutter() ? "is the only look expected in the JPEG"
                                       : "adds to the stock written into the style",
                 fs_yuvhook_bars_burned());
          fs_log("yuvsites: pq=%08x (size lo=%d hi=%d) | jpeg r0=%08x r1=%08x diag %d "
                 "| ssraw r0=%08x r1=%08x diag %d",
                 (unsigned)fs_yuvhook_pic_quality(),
                 fs_yuvhook_pic_quality() & 0xFF,
                 (fs_yuvhook_pic_quality() >> 8) & 0xFF,
                 (unsigned)fs_yuvhook_site_reg(0, 0), (unsigned)fs_yuvhook_site_reg(0, 1),
                 fs_yuvhook_site_diag(0),
                 (unsigned)fs_yuvhook_site_reg(1, 0), (unsigned)fs_yuvhook_site_reg(1, 1),
                 fs_yuvhook_site_diag(1));
        }
        if (g == 0) {
          /* The per-site codes are the whole diagnosis: 1 graded, 2 not a
           * picture, 3 no stride, 4 plane/safe-rows, 5 fingerprint skip. */
          fs_log("yuvrefuse: jpeg site diag %d of %d calls, ssraw site diag %d "
                 "of %d calls", fs_yuvhook_site_diag(0),
                 fs_yuvhook_site_calls(0), fs_yuvhook_site_diag(1),
                 fs_yuvhook_site_calls(1));
          fs_log("yuvhook: %d call(s) (%d still, %d ssraw), 0 graded; armed "
                 "rows %d, ready %d, level %d; last pc=%08x regs %08x %08x %08x "
                 "%08x; chunks %d [%d %d %d %d] sniff 0x%x suite %d bare %d "
                 "nobuf %d badsize %d style %d",
                 nc, fs_yuvhook_site_calls(0), fs_yuvhook_site_calls(1),
                 (int)fs_yuvhook_grade_rows(), (int)fs_yuvlook_ready(),
                 fs_level(),
                 (unsigned)fs_yuvhook_last_pc(),
                 (unsigned)fs_yuvhook_last_reg(0), (unsigned)fs_yuvhook_last_reg(1),
                 (unsigned)fs_yuvhook_last_reg(2), (unsigned)fs_yuvhook_last_reg(3),
                 fs_yuvhook_chunks(),
                 (int)fs_yuvhook_chunk_size(0), (int)fs_yuvhook_chunk_size(1),
                 (int)fs_yuvhook_chunk_size(2), (int)fs_yuvhook_chunk_size(3),
                 fs_yuvhook_sniff_mask(), fs_yuvhook_suite_at(),
                 fs_yuvhook_bare_pic(), fs_yuvhook_nobuf(), (int)fs_yuvhook_unknown(),
                 fs_yuvhook_style_refused());
        }
        fs_log_flush();
      }
    }

    int gd = get_global_draw() ? 1 : 0;
    int idle = liveview_display_idle() ? 1 : 0;
    int bmp = bmp_is_on() ? 1 : 0;
    uint32_t st = ((uint32_t)gd << 7) | ((uint32_t)(lv ? 1 : 0) << 6) |
                  ((uint32_t)(lv_disp_mode & 7) << 3) | ((uint32_t)idle << 2) |
                  ((uint32_t)bmp << 1) | (fs_stat_frames ? 1u : 0u);
    /* While LiveView is on, a trail every 10 s: "only on change" printed
     * nothing at all in a session where the state never moved, which left the
     * question open. */
    int heartbeat = (lv && (st == last) && (++beat_counter % 5) == 0);
    if (st == last && !heartbeat)
      continue;
    last = st;
    if (!film_sim_dbglog)
      continue;
    {
      struct vram_info *vr = get_yuv422_vram();
      /* disp_addr is the register the panel actually scans out; vr->vram is
       * only Canon's capture-side pointer, and the two are not the same thing.
       * Logging the register is the only way to know whether the panel is ever
       * pointed at an ungraded frame. */
      fs_log(
          "lvwatch: lv=%d gd=%d setting=%d disp=%d idle=%d bmp=%d ds=%d "
          "gui=%d vram=0x%08x disp_addr=0x%08x ours=0x%08x pitch=%d frames=%d",
          lv, gd, get_global_draw_setting(), lv_disp_mode, idle, bmp,
          lv_dispsize, gui_state, (unsigned)(vr ? vr->vram : 0),
          (unsigned)FS_LV_DISPLAY_ADDR, (unsigned)fs_stat_dst,
          vr ? vr->pitch : -1, (int)fs_stat_frames);
    }
    /* Which buffer is the panel actually scanning out?
     *
     * Sampling once every 2.5 s cannot tell 30 Hz alternation from a stable
     * picture, so this takes 100 samples back to back and reports the duty
     * cycle. It runs on the state-change / 12.5 s heartbeat path, not on every
     * poll. "canon" above zero means the panel shows an ungraded
     * frame part of the time. */
    if (lv && fs_stat_frames) {
      int ours = 0, canon = 0;
      for (int i = 0; i < 100; i++) {
        uint32_t a = FS_LV_DISPLAY_ADDR;
        if (fs_is_canon_plane(a))
          canon++;
        else if (a)
          ours++;
        msleep(1);
      }
      fs_log("disp_poll: %d/100 on our buffer, %d/100 on Canon's, now 0x%08x",
             ours, canon, (unsigned)FS_LV_DISPLAY_ADDR);
    }

    if (lv && !gd && fs_stat_frames == 0)
      fs_log("lvwatch: LiveView grade cannot run; ML needs Global Draw on and "
             "disp=0 (press INFO until Canon's own LV overlays are off). "
             "Currently setting=%d disp=%d idle=%d bmp=%d",
             get_global_draw_setting(), lv_disp_mode, idle, bmp);
    fs_log_flush();
  }
}

/*
 * The YUV probe captures inside the shot and cannot write a file there, so the
 * band is copied to RAM by the hook and written out here, from a task, once
 * the frame is committed.
 */
static void fs_yuv_poll_task(void *unused) {
  (void)unused;
  { static int ok; fs_ensure_dir("ML/FILMSIM", &ok); }
  /* Grading telemetry. Without this the only sign that the hook ran was the
   * dump, which needs two calls -- so a session of single shots left no trace
   * either way and "is it grading?" was unanswerable from the log. */
  int last_calls = 0, last_ms = -1, idle = 0;

  while (film_sim_yuvprobe) {
    msleep(1500);
    {
      int calls = fs_yuvhook_calls();
      int ms = (int)fs_yuvhook_grade_ms();
      if (calls != last_calls) {
        if (calls > last_calls && film_sim_yuvrows > 0 && ms == last_ms) {
          fs_log("yuvgrade: hook called (%d) but did not grade", calls);
          fs_log_flush();
        }
        last_calls = calls;
      }
      if (ms != last_ms) {
        last_ms = ms;
        if (ms > 0) {
          fs_log("yuvgrade: graded %d rows in %d ms", film_sim_yuvrows, ms);
          fs_log_flush();
        }
      }
      if (film_sim_yuvrows > 0 && ++idle > 20 && calls == 0) {
        fs_log("yuvgrade: hook installed but never called after 30 s -- the "
               "patched entry point is not on the capture path this body uses");
        fs_log_flush();
        idle = -1000000; /* say it once */
      }
    }
    if (fs_yuvhook_mem1_ready()) {
      if (fs_yuvhook_mem1_write("ML/FILMSIM/MEM1.TXT")) {
        fs_log("mem1probe: wrote ML/FILMSIM/MEM1.TXT, %d job call(s), "
               "%d encoder call(s)", fs_yuvhook_mem1_calls(),
               fs_yuvhook_enc_calls());
        fs_log_flush();
        fs_yuvhook_uninstall();
        return;
      }
    }
    if (!fs_yuvhook_ready())
      continue;
    if (fs_yuvhook_write("ML/FILMSIM/YUV.BIN")) {
      fs_log("yuvprobe: %s", fs_yuvhook_note());
      if (film_sim_yuvrows > 0) {
        /* Grading is the point, so the hook has to stay: removing it here made
         * the first shot graded and every one after it plain. */
        fs_log("yuvgrade: %d rows graded in-shot (%d calls), hook kept",
               film_sim_yuvrows, fs_yuvhook_calls());
        fs_log_flush();
      } else {
        /* Diagnostic only: one shot is enough, and an entry point patched for
         * longer than necessary is a risk with no reward. */
        fs_yuvhook_uninstall();
        fs_log("yuvprobe: wrote ML/FILMSIM/YUV.BIN after %d hook calls, hook "
               "removed",
               fs_yuvhook_calls());
        fs_log_flush();
        return;
      }
    }
  }
  fs_yuvhook_uninstall();
  fs_log("yuvprobe: task stopped");
  fs_log_flush();
}

static unsigned int film_sim_init() {
  fs_log("film_sim_init: module loaded (enable=%d preset=%d strength=%d "
         "grain=%d fps=%d user1=%d gradeq=%d)",
         film_sim_enable, film_sim_preset, film_sim_strength, film_sim_grain,
         film_sim_fps, film_sim_user1, film_sim_gradeq);
  /* The config is read before the first bake, so the shot path runs at the
   * quality the card asked for rather than the default until somebody opens
   * the menu. */
  fs_yuvlook_set_quality(film_sim_gradeq);
  /* Write the boot line immediately.
   *
   * fs_log() only fills a RAM ring buffer; the first flush used to happen
   * inside fs_slot_backup_task a second later, so a module that died during
   * init left no trace at all -- which is exactly when a log is worth having.
   * The card still had 100 ASSERT files and no FILMSIM.LOG. */
  fs_log_flush();

  fs_signature_valid = 0;
  fs_sync();

  /* Load XML film stocks from ML/FILMS/ folder */
  fs_load_films_xml();
  fs_group_by_artwork();
  fs_film_gen++;

  /*
   * The shot-time look, installed *after* the film table exists.
   *
   * This block used to sit above fs_load_films_xml(), so the stock lookup ran
   * against an empty table and produced the path "ML/FILM/.fs" -- which was
   * reported as `baked '' from ML/FILM/.fs`, i.e. an identity look that
   * silently did nothing to every frame. The bake has to come after the table
   * is loaded, and it has to refuse a recipe that has no name.
   */
  /*
   * film.sim.yuvrows grades the frame Canon is about to encode;
   * film.sim.yuvprobe only asks for one dump of Canon's arguments.
   *
   * Everything here used to sit under `if (film_sim_yuvprobe)`, so with the
   * probe off -- the default, and the state of the card -- the Saturation-sized
   * lever `yuvrows = 512` did nothing at all and a shot saved Canon's own
   * render with no sign that anything had been skipped. The lever has to work
   * on its own; the probe adds the dump, nothing else.
   */
  if (film_sim_yuvprobe) {
    /*
     * Probe only: read the camera's own still-JPEG job struct and the parameter
     * block its encoder call is given, then write them out. Grading is OFF
     * until the geometry in those words has been read back from the camera --
     * writing a guessed number of rows into a pointer we have not confirmed is
     * how memory gets smashed mid-shot.
     */
    int e = fs_yuvhook_install_mem1();
    fs_log("%s (install %d)", fs_yuvhook_note(), e);
    fs_log_flush();
    task_create("fs_yuvpoll", 0x1e, 0x2000, fs_yuv_poll_task, 0);
  } else if (film_sim_yuvrows > 0 || film_sim_lv3d) {
    /* The shot hook needs the row path; the LiveView lattice does not. Gating
     * this on yuvrows alone left lv3d unbaked at boot whenever the shutter
     * grade was off -- the switch did nothing exactly when it was turned on. */
    if (film_sim_yuvrows > 0) {
      int e = fs_yuvhook_install();
      fs_log("%s (install %d)", fs_yuvhook_note(), e);
      fs_log_flush();
    }
    char stk[FS_PATH_LEN];
    if (fs_active_stock_path(stk, sizeof stk, NULL)) {
      if (fs_bake_stock(stk, film_sim_strength, fs_own_lut_path()) == 0 &&
          fs_yuvlook_name()[0]) {
        fs_log("yuvgrade: baked '%s' from %s at strength %d",
               fs_yuvlook_name(), stk, film_sim_strength);
        /*
         * Grade in place only when the capture style was Neutral.  With the
         * stock written into the picture style the YUV Canon hands us already
         * carries that look, and applying the engine's on top would grade the
         * frame twice.  The hook still runs and still finds the buffer either
         * way, so the buffer can be reported and compared with the camera's
         * JPEG -- that comparison is what says whether we are looking at the
         * frame Canon encodes.
         */
        fs_yuvhook_bars(film_sim_aspect);
        fs_yuvhook_need_neutral(fs_grade_in_shutter());
        fs_yuvhook_grade(fs_grade_in_shutter() ? film_sim_yuvrows : 0);
        /* Report the style that is actually selected, not the one we asked
         * for: the two disagreeing is exactly the double-grade case, and the
         * old wording asserted "Neutral" here whether or not it was true. */
        fs_log("yuvgrade: in-shutter grade %s (capture style 0x%02x%s)",
               fs_grade_in_shutter() ? "ON" : "off",
               (unsigned)lens_info.raw_picstyle,
               !fs_grade_in_shutter() ? ", stock"
                 : lens_info.raw_picstyle == 0x84 ? ", Neutral"
                 : ", NOT Neutral - the shot hook will refuse to grade");
      }
    }
  }

  /* Register LiveView OSD overlay badge (Florin's LV style) */
  lvinfo_add_item(&film_lvinfo_item);

  /* Independent witness for the LiveView grade, see fs_lv_monitor_task. */
  /* 2 KB was not enough: this task formats and writes a log line every two
   * seconds, and the firmware's vsnprintf alone can take 1 KB. */
  task_create("fs_lvwatch", 0x1c, 0x2000, fs_lv_monitor_task, 0);

  /* Recover or clean up any interrupted .TMP develops from unexpected power-off
   */
  fs_recover_tmp_files();

  /* COUNT() includes the MENU_EOL terminator, whose .name is NULL. ML's
   * check_duplicate_entries() asserts on that during menu_redraw_task, which
   * killed the menu task and with it every handler behind it. */
  /* One top-level menu, not two.
   *
   * This module used to publish "Film Dev" (18 rows) and "Film LV" (4), with
   * Framing written into both -- two places to change one setting, and two
   * entries in ML's menu bar for one module. Everything is reachable from the
   * one list below, one level deeper. */
  menu_add("Film Dev", film_sim_menu, COUNT(film_sim_menu) - 1);
  if (fs_test_flag()) {
    task_create("fs_test", 0x1e, 0x4000, fs_test_task, 0);
  }

  /* always mirror the flavor slots to the card: cheap and makes any
   * write reversible */
  task_create("fs_slots", 0x1e, 0x2000, fs_slot_backup_task, 0);
  task_create("fs_watch", 0x1e, 0x2000, fs_film_watch_task, 0);

  {
    static const char *const psflag = "ML/SETTINGS/PSPROBE.TST";
    uint32_t ps = 0;
    if (FIO_GetFileSize(psflag, &ps) == 0) {
      FIO_RemoveFile(psflag);
      task_create("fs_ps", 0x1e, 0x2000, fs_ps_probe_task, 0);
    }
  }

  /* film.sim.auto: watch for new frames and for level 2/3 being picked */
  if (film_sim_auto)
    task_create("fs_auto", 0x1e, FS_DEV_STACK, fs_auto_task, 0);

  /* film.sim.queue: the watcher only appends to the queue; without this worker
   * a frame is queued at boot and never developed, which looks exactly like a
   * develop that hangs. It never ran from here before -- only from the menu
   * toggle -- so a card with film.sim.queue = 1 and film.sim.auto = 1 sat on
   * "catching up on IMG_9943.CR2 (shot while unpaid)" every 2.5 s forever. */
  if (film_sim_queue) {
    fs_log("queue: worker starting (queue on)");
    fs_log_flush();
    fs_queue_start();
  }

  /* Test levers for the develop set (see fs_lever_task): the card is not
   * mounted yet when init runs, so the file check happens in a task. */
  /* This task runs a develop when AUTOSET.TST is present, so it needs the
   * develop stack, not the 8 KB one the smaller helpers use. */
  task_create("fs_lever", 0x1e, FS_DEV_STACK, fs_lever_task, 0);

  /* ML/SETTINGS/AUTODEV.TST: develop the newest raw at boot, no keys needed */
  {
    static const char *const devflag = "ML/SETTINGS/AUTODEV.TST";
    uint32_t flag = 0;
    if (FIO_GetFileSize(devflag, &flag) == 0) {
      FIO_RemoveFile(devflag);
      fs_log("autodev: flag found, starting worker");
      fs_log_flush();
      task_create("fs_autodev", 0x1e, FS_DEV_STACK, fs_autodev_task, 0);
    }
  }

  /* request one raw LiveView dump on the next filtered frame */
  {
    static const char *const dumpflag = "ML/SETTINGS/LVDUMP.TST";
    uint32_t sz = 0;
    if (FIO_GetFileSize(dumpflag, &sz) == 0) {
      FIO_RemoveFile(dumpflag);
      task_create("fs_dump", 0x1e, 0x2000, fs_dump_task, 0);
    }
  }
  return 0;
}

static unsigned int film_sim_deinit() {
  fs_yuvhook_uninstall();
  /*
   * Deliberately NOT freeing fs_snap / fs_dump_raw / fs_dump_graded here.
   *
   * deinit is only reached from _module_unload_all() in module_shutdown() --
   * src/module.c:913 and :2255, called from src/tasks.c:366 and :404 and from
   * debug.c:402, i.e. camera shutdown, PROP_ABORT and the remote-console path.
   * There is no menu action that unloads a module and keeps ML running
   * (module_unload() at :1060 only frees the compiled code).
   *
   * So the memory is needed for exactly as long as it is held: after shutdown
   * the heap is gone anyway, whereas the display filter runs in Canon's LV task
   * and can still be dispatched while this function runs -- module_list[].valid
   * is cleared *after* deinit returns, and ML exposes no display-filter lock.
   * Freeing 691 kB under a callback that may be memcpy'ing into it buys nothing
   * and risks a hang during power-off, which is the worst moment to have one.
   */
  return 0;
}

MODULE_INFO_START()
MODULE_INIT(film_sim_init)
MODULE_DEINIT(film_sim_deinit)
MODULE_INFO_END()

MODULE_CBRS_START()
MODULE_CBR(CBR_DISPLAY_FILTER, fs_display_filter, 0)
MODULE_CBR(CBR_KEYPRESS, fs_keypress_cbr, 0)
MODULE_CBR(CBR_KEYPRESS_RAW, fs_keypress_raw_cbr, 0)
MODULE_CBRS_END()

MODULE_CONFIGS_START()
MODULE_CONFIG(film_sim_enable)
/*
 * The level has to be in this list or it is not written to film_sim.cfg, and
 * every power cycle silently puts the module back on level 2. That is how the
 * first unattended level-3 test came back as a level-2 develop.
 */
MODULE_CONFIG(film_sim_level)
MODULE_CONFIG(film_sim_preset)
MODULE_CONFIG(film_sim_strength)
MODULE_CONFIG(film_sim_satboost)
MODULE_CONFIG(film_sim_grain)
MODULE_CONFIG(film_sim_grainsize)
MODULE_CONFIG(film_sim_fps)
MODULE_CONFIG(film_sim_user1)
MODULE_CONFIG(film_sim_dbglog)
MODULE_CONFIG(film_sim_art)
MODULE_CONFIG(film_sim_shotgrade)
MODULE_CONFIG(film_sim_lut)
MODULE_CONFIG(film_sim_lv3d)
MODULE_CONFIG(film_sim_wbs_ba)
MODULE_CONFIG(film_sim_wbs_gm)
MODULE_CONFIG(film_sim_neutralcap)
MODULE_CONFIG(film_sim_sharp)
MODULE_CONFIG(film_sim_direct)
MODULE_CONFIG(film_sim_calib)
MODULE_CONFIG(film_sim_stock)
MODULE_CONFIG(film_sim_dev)
MODULE_CONFIG(film_sim_wb_shift)
MODULE_CONFIG(film_sim_lock_iso)
MODULE_CONFIG(film_sim_capture)
MODULE_CONFIG(film_sim_auto)
MODULE_CONFIG(film_sim_replace)
MODULE_CONFIG(film_sim_aspect)
MODULE_CONFIG(film_sim_pad)
MODULE_CONFIG(film_sim_yuvprobe)
MODULE_CONFIG(film_sim_yuvrows)
MODULE_CONFIG(film_sim_queue)
MODULE_CONFIGS_END()
