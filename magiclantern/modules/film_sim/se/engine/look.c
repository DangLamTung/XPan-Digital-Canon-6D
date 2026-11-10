#include "look.h"
#include "se_libc.h"

/* ------------------------------------------------------------ helpers */

static int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* BT.601 luma, the same weights the palette conversion uses. Integer, so
 * the camera and the desktop cannot disagree. */
static int luma8(int r, int g, int b) {
  return (77 * r + 150 * g + 29 * b) >> 8;
}

/*
 * Film grain.
 *
 * A hash of the pixel's own coordinates: repeatable (the same frame always
 * grains the same way, so the desktop and the camera produce identical
 * bytes), needs no state, and costs a handful of integer ops. Amplitude is
 * weighted by 4v(1-v) because grain is a modulation of exposed silver --
 * it vanishes in clipped whites and in blacks where there is nothing to
 * modulate, which is what stops it looking like television noise.
 *
 * This is the same construction pipeline.c uses on the raw path, kept
 * deliberately identical so a stock grains the same from either source.
 */
static unsigned grain_hash(unsigned x, unsigned y, unsigned seed) {
  unsigned h = x * 374761393u + y * 668265263u + seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

/*
 * 65536 / d, for the hue calculation.
 *
 * Finding a pixel's hue needs a divide by its chroma, and ARM946E-S has
 * no divide instruction -- the compiler would emit a call to
 * __aeabi_idiv for every pixel. Chroma is a byte difference, so 255
 * reciprocals cover every case and the per-pixel cost becomes one
 * multiply.
 */
static uint32_t s_recip[256];

/*
 * sin, as 8.8 fixed point, over a turn divided into 1536 -- the same
 * units hue is measured in, so a rotation indexes straight into it.
 *
 * Built here rather than taken from libm because this has to produce
 * byte-identical results on a camera with no libm at all, and because a
 * table built two different ways is a table that disagrees in the last
 * place. The Taylor series below IS the specification: tools/lookcheck.c
 * rebuilds it the same way, deliberately.
 */
static int16_t s_sin[1536];

/*
 * How much chroma survives at each luminance, 256 = all of it.
 *
 * This is the single most film-like thing in the file, and the most
 * obviously absent one. Real emulsion loses colour toward both ends: at
 * the top all three dye layers run out together, so a bright colour
 * desaturates on its way to white, and at the bottom there is too little
 * exposure left to hold a hue. Digital saturation does neither -- it
 * keeps a colour at full strength right up to clipping, which is exactly
 * what makes an oversaturated frame read as a slider rather than a film.
 *
 * Measured on a frame through VELVIA: foliage at luma 168 went from
 * chroma 47 to 100, a 2.1x boost held at full strength into the
 * highlights, and came back looking like neon.
 *
 * Flat through the midtones, where film is at its most colourful, and
 * rolling off above and below.
 */
static uint16_t s_chroma[256];

static void tables_init(void) {
  int i;
  if (s_recip[255])
    return; /* already built */
  for (i = 1; i < 256; i++)
    s_recip[i] = (uint32_t)(65536 / i);

  for (i = 0; i < 256; i++) {
    int w;
    if (i < 24)
      w = 150 + (i * 106) / 24; /* 150 -> 256  */
    else if (i <= 176)
      w = 256; /* midtones and upper midtones: Y 176 is ~70% luma */
    else
      w = 256 - ((i - 176) * 128) / 79; /* 256 -> 128 in the shoulder */
    s_chroma[i] = (uint16_t)(w < 0 ? 0 : (w > 256 ? 256 : w));
  }

  for (i = 0; i < 1536; i++) {
    /* fold into the first quarter turn, then Taylor to x^7 */
    int q = i & 1535, sign = 1;
    float x, x2, v;
    if (q >= 768) {
      q -= 768;
      sign = -1;
    }
    if (q > 384)
      q = 768 - q;
    x = (float)q * (3.14159265f / 768.0f);
    x2 = x * x;
    v = x * (1.0f -
             x2 * (1.0f / 6.0f) *
                 (1.0f - x2 * (1.0f / 20.0f) * (1.0f - x2 * (1.0f / 42.0f))));
    v *= (float)sign * 256.0f;
    s_sin[i] = (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
  }
}

static inline int16_t se_sin_fast(int a) {
  while (a < 0)
    a += 1536;
  while (a >= 1536)
    a -= 1536;
  return s_sin[a];
}

#define SE_SIN(a) se_sin_fast(a)
#define SE_COS(a) se_sin_fast((a) + 384)

/*
 * Which hue a pixel is, as 0..1535 -- 256 per sector, R Y G C B M.
 * Returns -1 for a pixel with no colour in it at all.
 */
static int hue1536(int r, int g, int b, int *chroma) {
  int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
  int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
  int d = mx - mn, h;

  *chroma = d;
  if (d <= 0)
    return -1;
  if (mx == r)
    h = ((g - b) * (int)s_recip[d]) >> 8;
  else if (mx == g)
    h = (((b - r) * (int)s_recip[d]) >> 8) + 512;
  else
    h = (((r - g) * (int)s_recip[d]) >> 8) + 1024;
  if (h < 0)
    h += 1536;
  if (h >= 1536)
    h -= 1536;
  return h;
}

/*
 * 256 * num / den without a divide, for den up to about 1020.
 * s_recip only covers 1..255, so halve both until it fits -- at most two
 * shifts, and the precision lost is far below one output level.
 */
/* (chroma * 256) / SE_HUE_FULL for every chroma: the hue fade is per pixel and
 * a divide costs ~100 cycles on these bodies. */
static uint16_t s_fade[SE_HUE_FULL + 1];
static int s_fade_ready;

static void fade_init(void) {
  if (s_fade_ready)
    return;
  for (int i = 0; i <= SE_HUE_FULL; i++)
    s_fade[i] = (uint16_t)((i * 256) / SE_HUE_FULL);
  s_fade_ready = 1;
}

static int fit_scale(int num, int den) {
  while (den > 255) {
    den >>= 1;
    num >>= 1;
  }
  if (den <= 0)
    return 256;
  return (num * (int)s_recip[den]) >> 8;
}

/*
 * Fit a colour back inside 0..255 by pulling its CHROMA in, not by
 * clipping its channels.
 *
 * This is the difference between a film look and a saturation slider.
 * Clipping takes a channel to 0 or 255 and leaves the others where they
 * are, which produces the purest, most electric version of that hue with
 * every trace of detail gone -- flat neon foliage, flat neon sky. Film
 * cannot do it: the three dye layers overlap, so a density of zero in one
 * layer while the others hold is not a state the material has.
 *
 * Measured on one frame through VELVIA: 57.5% of pixels had a channel at
 * the floor, against 27.8% with no look at all, and the greens came back
 * as a single flat colour.
 *
 * So find the largest scale that keeps every channel in range and apply
 * it to all three. Hue and luminance survive; only the saturation gives,
 * and only by as much as it must.
 */
static void gamut_fit(int *rr, int *gg, int *bb) {
  int r = *rr, g = *gg, b = *bb;
  int y, sc = 256, d;

  if (r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255)
    return; /* already fits: the common case */

  y = (77 * r + 150 * g + 29 * b) >> 8;
  if (y < 0)
    y = 0;
  if (y > 255)
    y = 255;

  d = r - y;
  if (r > 255 && d > 0) {
    int t = fit_scale(255 - y, d);
    if (t < sc)
      sc = t;
  }
  if (r < 0 && d < 0) {
    int t = fit_scale(y, -d);
    if (t < sc)
      sc = t;
  }
  d = g - y;
  if (g > 255 && d > 0) {
    int t = fit_scale(255 - y, d);
    if (t < sc)
      sc = t;
  }
  if (g < 0 && d < 0) {
    int t = fit_scale(y, -d);
    if (t < sc)
      sc = t;
  }
  d = b - y;
  if (b > 255 && d > 0) {
    int t = fit_scale(255 - y, d);
    if (t < sc)
      sc = t;
  }
  if (b < 0 && d < 0) {
    int t = fit_scale(y, -d);
    if (t < sc)
      sc = t;
  }

  if (sc < 0)
    sc = 0;
  if (sc > 256)
    sc = 256;
  *rr = y + (((r - y) * sc) >> 8);
  *gg = y + (((g - y) * sc) >> 8);
  *bb = y + (((b - y) * sc) >> 8);
}

/* ------------------------------------------------- stock -> look ----- */

void se_look_from_stock(const se_stock *s, int scale, se_look *lk) {
  int c;

  memset(lk, 0, sizeof *lk);
  tables_init();
  se_stock_bake_srgb8(s, lk->map);

  /* CURVE_Y: k(y) = curve(y) / y, in 1/256ths, so the pixel loop needs one
   * lookup and one multiply per channel. An identity curve gives k = 256 and
   * the stage is skipped by luma_any, so recipes without the key are untouched. */
  lk->luma_any = s->luma.n >= 2;
  if (lk->luma_any) {
    int i;
    for (i = 0; i < 256; i++) {
      float x = (float)i / 255.0f;
      float c = se_curve_eval(&s->luma, x);
      float k = (i > 0) ? (c / x) : 1.0f;
      int   v = (int)(k * 256.0f + 0.5f);
      if (v < 0)   v = 0;
      if (v > 512) v = 512;
      lk->luma_map[i] = (int16_t)v;
    }
  }

  for (c = 0; c < SE_HUE_SECTORS; c++) {
    lk->hue_sat[c] = (int)(s->hue_sat[c] * 2.56f + 0.5f);
    if (lk->hue_sat[c] < 0)
      lk->hue_sat[c] = 0;
    if (lk->hue_sat[c] > 1024)
      lk->hue_sat[c] = 1024;

    lk->hue_lum[c] = (int)(s->hue_lum[c] * 2.56f + 0.5f);
    if (lk->hue_lum[c] < 0)
      lk->hue_lum[c] = 0;
    if (lk->hue_lum[c] > 1024)
      lk->hue_lum[c] = 1024;

    /* authored in degrees; 1536 units to the turn */
    lk->hue_rot[c] = (int)(s->hue_rot[c] * (1536.0f / 360.0f) +
                           (s->hue_rot[c] < 0 ? -0.5f : 0.5f));
    if (lk->hue_rot[c] < -512)
      lk->hue_rot[c] = -512;
    if (lk->hue_rot[c] > 512)
      lk->hue_rot[c] = 512;

    if (lk->hue_sat[c] != 256 || lk->hue_lum[c] != 256 || lk->hue_rot[c])
      lk->hue_any = 1;
  }

  /* weak 128, strong 256: half and full. Clamped at the conversion because a
   * .fs is text a person can mistype: "COLOR_CHROME 9" used to become 1152,
   * four times Strong, and nothing said so. Fuji has three values. */
  lk->chrome  = s->color_chrome >= 2 ? 256
              : (s->color_chrome == 1 ? 128 : 0);
  lk->fx_blue = s->fx_blue >= 2 ? 256
              : (s->fx_blue == 1 ? 128 : 0);

  /* 3x3 colour matrix: copied and gated, so identity costs nothing. */
  {
    int i, any = 0, any_hi = 0;
    for (i = 0; i < 9; i++) {
      lk->mat[i] = s->matrix[i];
      lk->mat_hi[i] = s->matrix_hi[i];
      if (s->matrix_set && s->matrix[i] != ((i % 4 == 0) ? 256 : 0)) any = 1;
      if (s->matrix_hi_set && s->matrix_hi[i] != ((i % 4 == 0) ? 256 : 0)) any_hi = 1;
    }
    lk->mat_any = any;
    lk->mat_hi_any = any_hi && any;
  }

  lk->sat = s->sat_set ? (int)(s->sat * 2.56f + 0.5f) : 256;
  if (lk->sat < 0)
    lk->sat = 0;
  if (lk->sat > 1024)
    lk->sat = 1024;

  for (c = 0; c < 3; c++) {
    lk->sh[c] = (int)s->split_sh[c];
    lk->hi[c] = (int)s->split_hi[c];
  }

  lk->grain = (int)(s->grain + 0.5f);
  if (lk->grain < 0)
    lk->grain = 0;
  if (lk->grain > 100)
    lk->grain = 100;
  /*
   * size 0..100 -> a 1..9 pixel cell at full size, shrinking with the
   * decode scale so a half-size frame is not twice as grainy.
   *
   * The divisor was 25, giving five steps, and then the half-size divide
   * collapsed four of them onto 1: every stock from Velvia's fine grain
   * to a coarse 800-speed stock's clumps came out with a one-pixel
   * cell on the path the camera actually uses. Nine steps survive the
   * halving with a real spread -- 1 at the fine end, 4 or 5 at the
   * coarse end.
   */
  lk->grain_scale = scale;
  /* The base value, not the [RAW] override: se_look_from_stock() has no
   * source argument, so se_stock_grain(s, src) cannot be called here. The
   * amount above has the same limitation. Plumbing a source through this
   * function touches every caller including four host tools. */
  lk->grain_cell = 1 + (int)(s->grain_size / 12.0f);
  if (scale > 1)
    lk->grain_cell = (lk->grain_cell + scale - 1) / scale;
  if (lk->grain_cell < 1)
    lk->grain_cell = 1;

  /*
   * HALATION above 100 is a gain, and it has to be allowed.
   *
   * The glow is a box blur, so what reaches add_glow is the MEAN of the
   * mask over a (2r+1)^2 window, not its sum. A street light occupying
   * a hundred pixels inside a 53x53 window averages down to almost
   * nothing, and with the old cap of 100 there was no way to ask for it
   * back: the halo could never be brighter than the window mean. The
   * practical effect was that widening the radius made the bloom fainter
   * rather than larger, which is the opposite of what the control reads
   * as and the reason the halation stock's glow would not bloom however it was
   * tuned.
   *
   * 400 lets a stock say "this scatters hard" and still clip sanely --
   * add_glow runs every channel through clamp8. The reference in
   * tools/lookcheck.c takes hal_amount straight from the look, so it
   * needs no matching change; this converts a stock, and nothing else.
   */
  lk->hal_amount = (int)(s->hal_amount + 0.5f);
  if (lk->hal_amount < 0)
    lk->hal_amount = 0;
  if (lk->hal_amount > 400)
    lk->hal_amount = 400;
  lk->hal_thresh = (int)(s->hal_thresh + 0.5f);
  if (lk->hal_thresh < 0)
    lk->hal_thresh = 0;
  if (lk->hal_thresh > 255)
    lk->hal_thresh = 255;
  lk->hal_radius = (int)(s->hal_radius + 0.5f) / (scale > 0 ? scale : 1);
  if (lk->hal_radius < 1)
    lk->hal_radius = 1;
  if (lk->hal_radius > 48)
    lk->hal_radius = 48;
  for (c = 0; c < 3; c++)
    lk->hal_tint[c] = clamp8((int)s->hal_tint[c]);

  lk->strength = 100;
}

/* How much of the look to apply, 0..100. Called after se_look_from_stock, and
 * by both paths, so a developed file and the shot-time tables agree. */
void se_look_set_strength(se_look *lk, int strength) {
  if (!lk)
    return;
  if (strength < 0)
    strength = 0;
  if (strength > 100)
    strength = 100;
  lk->strength = strength;
  /* Grain and halation are part of the look, not something laid on top of it,
   * so they ride with the blend. Halation's 0..400 gain keeps its ceiling. */
  lk->grain = lk->grain * strength / 100;
  lk->hal_amount = lk->hal_amount * strength / 100;
  if (lk->hal_amount == 0)
    lk->hal_thresh = 255;   /* nothing glows: the delay line can be skipped */
}

/* Saturation on top of the stock's own, 100 = as written. One number for every
 * stock, because the per-hue stage only reaches full strength above a chroma
 * most of a real photograph never reaches -- see SE_HUE_FULL. */
void se_look_set_grain_size(se_look *lk, int size)
{
  if (!lk || size < 0)
    return;                     /* -1: keep whatever the stock asked for */
  lk->grain_cell = 1 + size / 12;
  if (lk->grain_scale > 1)
    lk->grain_cell = (lk->grain_cell + lk->grain_scale - 1) / lk->grain_scale;
  if (lk->grain_cell < 1)
    lk->grain_cell = 1;
}

void se_look_set_satboost(se_look *lk, int boost) {
  int c;
  if (!lk)
    return;
  if (boost < 20)
    boost = 20;
  if (boost > 200)
    boost = 200;
  if (boost == 100)
    return;
  lk->sat = lk->sat * boost / 100;
  if (lk->sat > 1024)
    lk->sat = 1024;
  for (c = 0; c < SE_HUE_SECTORS; c++) {
    lk->hue_sat[c] = lk->hue_sat[c] * boost / 100;
    if (lk->hue_sat[c] > 1024)
      lk->hue_sat[c] = 1024;
  }
}

/* --------------------------------------------------- shot-time tables -- */

static void grade_row(const se_look *lk, uint8_t *row, int w);

/*
 * Flatten the look into the tables a UYVY frame can be graded with in one
 * pass, by running the grade itself on synthetic pixels.
 *
 * Every number here comes out of grade_row(), so the per-channel curves, the
 * six hue axes, the chroma rolloff and the gamut fit are all inside it -- the
 * tables cannot drift from the develop because they are the develop, sampled.
 *
 * What the consumer reconstructs per pixel:
 *
 *   luma  = t.y[y] + t.dy[cell]
 *   U     = t.uoff[y] + u * cg[cell] * gain[y] / 4096
 *   V     = t.voff[y] + v * cg[cell] * gain[y] / 4096
 *
 * so `y` carries the tone curve, uoff/voff carry whatever cast a neutral pixel
 * picks up (a split tone, a hue rotation about grey), `gain` is how much
 * chroma survives at that luma, and cg/dy are the same two measurements per
 * (U,V) cell for the hue-dependent part.
 */
static void grade_px(const se_look *lk, int r, int g, int b, int *out);

static const se_lut3 *s_bake_cube;

void se_look_set_bake_cube(const se_lut3 *c) { s_bake_cube = c; }

/* the bake's sample path: cube first, then the look, which is the order
 * Develop uses (lut_apply_rows then se_look_rows) */
static void bake_px(const se_look *lk, int r, int g, int b, int *out)
{
    if (s_bake_cube) {
        uint16_t px[3];
        px[0] = (uint16_t)(clamp8(r) * 257);
        px[1] = (uint16_t)(clamp8(g) * 257);
        px[2] = (uint16_t)(clamp8(b) * 257);
        se_lut3_apply_row(s_bake_cube, px, 1);
        grade_px(lk, (int)(px[0] / 257), (int)(px[1] / 257),
                 (int)(px[2] / 257), out);
        return;
    }
    grade_px(lk, r, g, b, out);
}

static void grade_px(const se_look *lk, int r, int g, int b, int *out)
{
    uint8_t row[3];
    row[0] = (uint8_t)clamp8(r);
    row[1] = (uint8_t)clamp8(g);
    row[2] = (uint8_t)clamp8(b);
    grade_row(lk, row, 1);
    out[0] = row[0];
    out[1] = row[1];
    out[2] = row[2];
}

/* A pixel with this luma and this chroma, inside the 0..255 box.
 * u and v are Cb and Cr in standard BT.601 space (-128..127).
 * In BT.601:
 *   R = Y + 1.402 Cr              (359 / 256)
 *   B = Y + 1.772 Cb              (454 / 256)
 *   G = Y - 0.3441 Cb - 0.7141 Cr ((u * 88 + v * 183) >> 8)
 */
static int ycc_px(int y, int u, int v, int *r, int *g, int *b)
{
    int b_minus_y = (u * 454) >> 8;
    int r_minus_y = (v * 359) >> 8;
    *r = y + r_minus_y;
    *b = y + b_minus_y;
    *g = y - ((u * 88 + v * 183) >> 8);
    if (*r < 0 || *r > 255 || *g < 0 || *g > 255 || *b < 0 || *b > 255)
        return -1;
    return 0;
}

void se_look_bake_yuv(const se_look *lk, se_yuvtbl *t)
{
    static const int probe = 24;
    int y, cu, cv;
    int raw_gain[256];
    int mid_gain = 64;

    memset(t, 0, sizeof *t);
    t->strength = clamp8(lk->strength * 255 / 100) * 100 / 255;
    if (t->strength > 100) t->strength = 100;

    for (y = 0; y < 256; y++) {
        int o[3];

        /* Tone and cast: what a neutral pixel of this luma becomes. */
        bake_px(lk, y, y, y, o);
        t->y[y]     = (uint8_t)clamp8(luma8(o[0], o[1], o[2]));
        /* Convert output (B - Y) and (R - Y) to BT.601 Cb/Cr scale (* 144 / 256, * 183 / 256) */
        int uo = ((o[2] - t->y[y]) * 144) / 256;
        int vo = ((o[0] - t->y[y]) * 183) / 256;
        t->uoff[y]  = (int8_t)(uo < -128 ? -128 : (uo > 127 ? 127 : uo));
        t->voff[y]  = (int8_t)(vo < -128 ? -128 : (vo > 127 ? 127 : vo));

        /*
         * Chroma gain at this luma: measured across diagonal probe, subtracting
         * split-toning offset so constant color cast does not pollute gain.
         */
        {
            int acc = 0, n = 0, s;
            for (s = -1; s <= 1; s += 2) {
                int r, g, b;
                if (ycc_px(y, probe * s, probe * s, &r, &g, &b) != 0) continue;
                bake_px(lk, r, g, b, o);
                {
                    int oy = luma8(o[0], o[1], o[2]);
                    int ou = ((o[2] - oy) * 144) / 256 - t->uoff[y];
                    int ov = ((o[0] - oy) * 183) / 256 - t->voff[y];
                    acc += (int)(((long)(ou > 0 ? ou : -ou) +
                                  (long)(ov > 0 ? ov : -ov)) * 64 /
                                 (2L * probe));
                    n++;
                }
            }
            raw_gain[y] = n ? acc / n : 64;
        }
    }

    /*
     * Compute 2x2 chroma transformation matrix (hue rotation & saturation)
     * at mid-grey (luma 128) using two orthogonal probes:
     * Probe 1: (Cb = 32, Cr = 0)
     * Probe 2: (Cb = 0, Cr = 32)
     */
    {
        int r1, g1, b1, o1[3];
        int r2, g2, b2, o2[3];

        t->matrix[0] = 256;
        t->matrix[1] = 0;
        t->matrix[2] = 0;
        t->matrix[3] = 256;

        if (ycc_px(128, 32, 0, &r1, &g1, &b1) == 0 &&
            ycc_px(128, 0, 32, &r2, &g2, &b2) == 0) {
            bake_px(lk, r1, g1, b1, o1);
            bake_px(lk, r2, g2, b2, o2);

            int oy1 = luma8(o1[0], o1[1], o1[2]);
            int oy2 = luma8(o2[0], o2[1], o2[2]);

            int ou1 = (((o1[2] - oy1) * 144) / 256) - t->uoff[128];
            int ov1 = (((o1[0] - oy1) * 183) / 256) - t->voff[128];

            int ou2 = (((o2[2] - oy2) * 144) / 256) - t->uoff[128];
            int ov2 = (((o2[0] - oy2) * 183) / 256) - t->voff[128];

            /* Matrix entries scaled by 256 (probe was 32, so * 256 / 32 = * 8) */
            t->matrix[0] = (int16_t)(ou1 * 8);
            t->matrix[1] = (int16_t)(ou2 * 8);
            t->matrix[2] = (int16_t)(ov1 * 8);
            t->matrix[3] = (int16_t)(ov2 * 8);
        }
    }

    /*
     * Normalize luma-dependent chroma gain relative to mid-grey (luma 128)
     * so overall saturation is governed by the 2x2 matrix and not squared.
     */
    mid_gain = raw_gain[128] > 0 ? raw_gain[128] : 64;
    for (y = 0; y < 256; y++) {
        int g = (raw_gain[y] * 64) / mid_gain;
        if (g < 0) g = 0;
        if (g > 255) g = 255;
        t->gain[y] = (uint8_t)g;
    }

    /* Per hue cell, at mid grey: the hue-dependent gain and luma. */
    for (cv = 0; cv < SE_YUV_GRID; cv++) {
        for (cu = 0; cu < SE_YUV_GRID; cu++) {
            int u = cu * (256 / SE_YUV_GRID) + (128 / SE_YUV_GRID) - 128;
            int v = cv * (256 / SE_YUV_GRID) + (128 / SE_YUV_GRID) - 128;
            int cell = cv * SE_YUV_GRID + cu;
            int r, g, b, o[3], oy, ou, ov, mag, cg, dy;

            t->cg[cell] = 64;
            t->dy[cell] = 0;
            if (ycc_px(128, u, v, &r, &g, &b) != 0) continue;
            bake_px(lk, r, g, b, o);
            oy = luma8(o[0], o[1], o[2]);
            ou = (((o[2] - oy) * 144) / 256) - t->uoff[128];
            ov = (((o[0] - oy) * 183) / 256) - t->voff[128];

            /* Compare output chroma magnitude to matrix-transformed magnitude */
            int tu = (t->matrix[0] * u + t->matrix[1] * v) >> 8;
            int tv = (t->matrix[2] * u + t->matrix[3] * v) >> 8;
            mag = (tu < 0 ? -tu : tu) + (tv < 0 ? -tv : tv);
            if (mag >= 8) {
                int omag = (ou < 0 ? -ou : ou) + (ov < 0 ? -ov : ov);
                cg = (int)((long)omag * 64 / mag);
                if (cg < 0) cg = 0;
                if (cg > 255) cg = 255;
                t->cg[cell] = (uint8_t)cg;
            }
            dy = oy - t->y[128];
            if (dy > 127) dy = 127;
            if (dy < -128) dy = -128;
            t->dy[cell] = (int8_t)dy;
        }
    }
}

/*
 * Flatten the look into a joint (y,u,v) lattice plus a tone curve.
 *
 * The lattice is filled by running grade_px(), exactly as se_look_bake_yuv
 * does, so it is the develop sampled rather than an approximation of it. The
 * difference is the sampling grid: this one keeps luma as a third axis, which
 * is what lets chroma change with luma AND hue at the same time.
 */
static int div_round(int num, int den)
{
    return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
}

static int clamp_c(int v) { return v < -128 ? -128 : (v > 127 ? 127 : v); }

void se_look_bake_yuv3(const se_look *lk, se_yuv3 *t)
{
    const int NY = SE_YUV3_NY, NC = SE_YUV3_NC;
    int iy, iu, iv, y;

    for (y = 0; y < 256; y++) {
        int o[3];
        bake_px(lk, y, y, y, o);
        t->y[y] = (uint8_t)clamp8(luma8(o[0], o[1], o[2]));
    }

    for (iv = 0; iv < NC; iv++)
    for (iu = 0; iu < NC; iu++)
    for (iy = 0; iy < NY; iy++) {
        int yy = iy * 255 / (NY - 1);
        int u  = -128 + iu * 255 / (NC - 1);
        int vv = -128 + iv * 255 / (NC - 1);
        int r, g, b, o[3], y2;
        /* y innermost, so this MUST match fs_core_process_rows3's strides */
        size_t k = (((size_t)iv * NC + (size_t)iu) * NY + (size_t)iy) * 3;

        ycc_px(yy, u, vv, &r, &g, &b);
        /*
         * A node outside the RGB cube is not a colour the panel can show, but
         * it still takes part in the interpolation of nodes that are. Clamp it
         * into the cube and grade that, so a boundary node carries a nearby
         * real result instead of an identity that would pull the edge toward
         * grey.
         */
        bake_px(lk, clamp8(r), clamp8(g), clamp8(b), o);
        y2 = luma8(o[0], o[1], o[2]);
        t->v[k]     = (int16_t)(y2 - t->y[yy]);   /* vs grey of the same luma */
        t->v[k + 1] = (int16_t)clamp_c(div_round((o[2] - y2) * 256, 454));
        t->v[k + 2] = (int16_t)clamp_c(div_round((o[0] - y2) * 256, 359));
    }
}

/* ------------------------------------------------------- the run ----- */
struct se_look_run {
  se_look lk;
  int w;
  int y_out; /* rows emitted                            */

  int on;           /* halation enabled: a delay line is live  */
  int r;            /* halation radius, in rows                */
  int nring;        /* 2r + 1: the mask window                 */
  int ndelay;       /* r + 1: see se_look_begin                 */
  uint8_t *ring;    /* nring rows of horizontally blurred mask */
  uint8_t *delay;   /* ndelay rows of graded RGB               */
  int *colsum;      /* w running column sums over the ring     */
  int pushed;       /* mask rows pushed, priming included      */
  uint8_t *scratch; /* one row of mask, pre-blur               */

  uint8_t *flat; /* !on only: the one row we grade into     */
};

size_t se_look_bytes(const se_look *lk, int width) {
  size_t n;
  if (lk->hal_amount <= 0)
    return sizeof(struct se_look_run) + (size_t)width * 3 + 32;
  n = sizeof(struct se_look_run) + 16;
  n += (size_t)(2 * lk->hal_radius + 1) * (size_t)width; /* ring   */
  n += (size_t)(lk->hal_radius + 1) * (size_t)width * 3; /* delay  */
  n += (size_t)width * sizeof(int);                      /* colsum */
  n += (size_t)width;                                    /* scratch*/
  return n + 64;
}

size_t se_look_fit(se_look *lk, int width, size_t budget) {
  size_t n = se_look_bytes(lk, width);

  while (lk->hal_amount > 0 && lk->hal_radius > 1 && n > budget) {
    lk->hal_radius--;
    n = se_look_bytes(lk, width);
  }
  if (n > budget && lk->hal_amount > 0) {
    /* A radius of 1 is not a glow anyway, and with amount 0 the run
     * allocates no delay line at all -- so this always terminates at
     * something that fits. */
    lk->hal_amount = 0;
    n = se_look_bytes(lk, width);
  }
  return n;
}

se_err se_look_begin(se_look_run **out, const se_look *lk, int width,
                     se_arena *ar) {
  se_look_run *r;

  if (!out || !lk || width <= 0 || !ar)
    return SE_E_CORRUPT;
  fade_init();

  /*
   * Also here, not only in se_look_from_stock.
   *
   * A caller may fill se_look itself rather than resolving it from a
   * stock -- tools/lookcheck.c does exactly that -- and with the table
   * still zero every pixel reports hue 0 and the whole per-hue stage
   * silently collapses onto the red sector. It cost an afternoon
   * looking for an off-by-one in the reciprocal. Every consumer has to
   * come through here, so this is where it belongs.
   */
  tables_init();

  r = SE_NEW(ar, se_look_run, 1);
  if (!r)
    return SE_E_NOMEM;
  memset(r, 0, sizeof *r);
  r->lk = *lk;
  r->w = width;

  if (lk->hal_amount > 0) {
    r->r = lk->hal_radius;
    r->nring = 2 * r->r + 1;
    /*
     * The mask window spans 2r+1 rows, but the graded RGB does not.
     * A row is written at push index i and read back at push index
     * i+r, so it only has to survive r+1 pushes -- it is overwritten
     * at i+r+1, one push after it was needed. Sizing this ring like
     * the mask ring held r extra rows of RGB for nothing: 120 KB at
     * full width and radius 16, which is the difference between the
     * full-size develop getting radius 8 and radius 13.
     */
    r->ndelay = r->r + 1;
    r->ring = SE_NEW(ar, uint8_t, (size_t)r->nring *width);
    r->delay = SE_NEW(ar, uint8_t, (size_t)r->ndelay *width * 3);
    r->colsum = SE_NEW(ar, int, width);
    r->scratch = SE_NEW(ar, uint8_t, width);
    if (!r->ring || !r->delay || !r->colsum || !r->scratch)
      return SE_E_NOMEM;
    memset(r->ring, 0, (size_t)r->nring * width);
    memset(r->colsum, 0, (size_t)width * sizeof(int));
    r->on = 1;
  } else {
    r->flat = SE_NEW(ar, uint8_t, (size_t)width * 3);
    if (!r->flat)
      return SE_E_NOMEM;
  }
  *out = r;
  return SE_OK;
}

/* Curves, saturation and split tone: everything that needs only the pixel
 * itself. Writes in place. */

/*
 * Per-stage accounting for the look.
 *
 * The develop reports one "look" number, which says nothing about whether to
 * work on the curves, the halo or the grain. These four are the whole of it,
 * each timed where it is called -- once per row, so the clocks cost a few
 * thousand reads rather than millions.
 */
uint32_t se_lk_t_grade, se_lk_t_mask, se_lk_t_glow, se_lk_t_grain;
uint32_t se_lk_n_rows;

#ifdef SE_HOSTED
#include <sys/time.h>
uint32_t se_lk_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, 0);
  return (uint32_t)tv.tv_sec * 1000u + (uint32_t)tv.tv_usec / 1000u;
}
#else
#include <dryos.h>
uint32_t se_lk_ms(void) { return get_ms_clock(); }
#endif

static void grade_row(const se_look *lk, uint8_t *row, int w)
{
    const int st = lk->strength;
    const int st_scaled = st * 5243;
    int x;
    for (x = 0; x < w; x++) {
        const int o0 = row[x * 3 + 0];
        const int o1 = row[x * 3 + 1];
        const int o2 = row[x * 3 + 2];
        int rr = lk->map[0][o0];
        int gg = lk->map[1][o1];
        int bb = lk->map[2][o2];

    /*
     * CURVE_Y, right after the per-channel curves and before anything that
     * reads all three channels: scale each channel's distance from the pixel's
     * luma, leaving that luma where it is. This is a saturation control that
     * only acts on the brightness range the curve marks out -- it is how a
     * stock can lose chroma in foliage without going dark, which the master
     * curve cannot express because restoring the level restores the colour.
     */
    if (lk->luma_any) {
      int y  = luma8(clamp8(rr), clamp8(gg), clamp8(bb));
      int k  = lk->luma_map[clamp8(y)];
      if (k != 256) {
        rr = clamp8(y + ((rr - y) * k + 128) / 256);
        gg = clamp8(y + ((gg - y) * k + 128) / 256);
        bb = clamp8(y + ((bb - y) * k + 128) / 256);
      }
    }

    /*
     * Colour matrix: nine multiplies, on the curves' output, before every
     * stage that reads all three channels. This is the linear part of the
     * crosstalk a film LUT carries at full resolution.
     */
    if (lk->mat_any) {
      if (lk->mat_hi_any) {
        /* luma-weighted mix of the two matrices: 18 multiplies, no lattice */
        int t8 = luma8(clamp8(rr), clamp8(gg), clamp8(bb));
        int r0 = lk->mat[0] * rr + lk->mat[1] * gg + lk->mat[2] * bb;
        int g0 = lk->mat[3] * rr + lk->mat[4] * gg + lk->mat[5] * bb;
        int b0 = lk->mat[6] * rr + lk->mat[7] * gg + lk->mat[8] * bb;
        int r1 = lk->mat_hi[0] * rr + lk->mat_hi[1] * gg + lk->mat_hi[2] * bb;
        int g1 = lk->mat_hi[3] * rr + lk->mat_hi[4] * gg + lk->mat_hi[5] * bb;
        int b1 = lk->mat_hi[6] * rr + lk->mat_hi[7] * gg + lk->mat_hi[8] * bb;
        rr = clamp8((r0 * (256 - t8) + r1 * t8) >> 16);
        gg = clamp8((g0 * (256 - t8) + g1 * t8) >> 16);
        bb = clamp8((b0 * (256 - t8) + b1 * t8) >> 16);
      } else {
        int r2 = (lk->mat[0] * rr + lk->mat[1] * gg + lk->mat[2] * bb) >> 8;
        int g2 = (lk->mat[3] * rr + lk->mat[4] * gg + lk->mat[5] * bb) >> 8;
        int b2 = (lk->mat[6] * rr + lk->mat[7] * gg + lk->mat[8] * bb) >> 8;
        rr = clamp8(r2); gg = clamp8(g2); bb = clamp8(b2);
      }
    }

    /*
     * Color Chrome and FX Blue need the hue and the chroma but not the
     * per-hue tables, so they get their own gate: a recipe can ask for
     * Strong Color Chrome with no HUE_SAT line at all, and before this it
     * was ignored because the whole block was gated on hue_any.
     */
    if (lk->chrome || lk->fx_blue) {
      int chroma;
      /* Scene hue, like the per-hue stage below: a curve's own cast on
       * neutral grey is not a saturated colour, and probing the print made
       * Color Chrome react to the tone curve rather than to the subject. */
      int h = hue1536(o0, o1, o2, &chroma);
      if (h >= 0 && chroma > 8) {
        int y  = luma8(rr, gg, bb);
        int u  = bb - y, v = rr - y;
        int amt = lk->chrome;

        /*
         * The chroma weighting is the point. A pixel with 20 levels of
         * colour gets a fifth of the effect a pixel with 100 does, so muted
         * greens stay muted while a red flower deepens -- which is exactly
         * how Color Chrome is described and what a saturation slider cannot
         * do.
         */
        {
          int w = chroma > 96 ? 256 : (chroma * 256) / 96;
          amt = (lk->chrome * w) >> 8;
        }

        /* FX Blue is the same statement on blue hues only: the 12-sector
         * wheel puts blue at 8, so weight by distance from it. */
        if (lk->fx_blue) {
          int sec = h / (1536 / SE_HUE_SECTORS);
          int d = sec > 8 ? sec - 8 : 8 - sec;
          if (d > SE_HUE_SECTORS / 2) d = SE_HUE_SECTORS - d;
          /* full at blue, zero two sectors away */
          if (d < 2) {
            int bw = d == 0 ? 256 : 128;
            /* the same chroma weight as Color Chrome: a pale sky should not
             * be pushed as hard as a saturated one */
            int w = chroma > 96 ? 256 : (chroma * 256) / 96;
            amt += (((lk->fx_blue * bw) >> 8) * w) >> 8;
          }
        }

        if (amt) {
          int v2, u2;
          u = u + ((u * amt) >> 10);      /* up to +25% chroma at strong */
          v = v + ((v * amt) >> 10);
          /*
           * u and v here are RAW differences (B-Y, R-Y), not the scaled Cb/Cr
           * that ycc_px() takes -- so the inverse is y + difference, not
           * y + (454*diff)>>8. Using the scaled inverse multiplied the effect
           * by 454/256 and made a weak Color Chrome look like a strong one;
           * the host measurement caught it as muted colours gaining MORE
           * chroma than saturated ones, which is the opposite of the claim.
           */
          v2 = v; u2 = u;
          rr = clamp8(y + v2);
          bb = clamp8(y + u2);
          {
            int t2 = 77 * v2 + 29 * u2;
            int sg = t2 < 0 ? -1 : 1;
            if (t2 < 0) t2 = -t2;
            gg = clamp8(y - (sg * (int)(((long long)t2 * 27963) >> 22)));
          }
        }
      }
    }

    /*
     * The per-hue stage, on the colour the curves produced.
     *
     * Saturation, rotation and luminance all act on the same two
     * numbers, so this works in luma + chroma rather than in RGB:
     * u and v ARE the colour, rotating them IS a hue shift, scaling
     * them IS saturation, and y on its own IS how bright that hue
     * renders. Doing it in RGB would need three separate passes and
     * could not express a rotation at all.
     *
     * Everything is derived from luma8's own weights, so the inverse
     * below is exact up to luma8's truncation -- which is why the
     * whole block is skipped when a stock asks for nothing per-hue,
     * rather than relying on it round-tripping to the same bytes.
     */
    if (lk->hue_any) {
      int chroma;
      /*
       * The hue is the SCENE's, not the print's. Feeding the per-channel
       * curves' output here let a curve's own warm or cool cast on neutral
       * grey be read as scene colour: GOLD lifts R over G to 232/250 at the
       * top, the probe then called that yellow, HUE_LUM brightened it, and
       * the ramp went backwards near white. Measured across the library,
       * probing the input drops 108 luminance inversions to the count below.
       */
      int h = hue1536(o0, o1, o2, &chroma);
      if (h >= 0) {
        /*
         * 1536 hue units over SE_HUE_SECTORS sectors, so each sector is
         * 128 and `sec` is a shift, not a divide. With twelve sectors a
         * bead every 30 degrees sits ON a knot instead of halfway between
         * two, which is the whole reason for the count.
         */
        int sec = h / (1536 / SE_HUE_SECTORS);
        int f   = h % (1536 / SE_HUE_SECTORS);
        int nx  = (sec + 1) % SE_HUE_SECTORS;
        int msat = lk->hue_sat[sec] +
                   (((lk->hue_sat[nx] - lk->hue_sat[sec]) * f)
                    / (1536 / SE_HUE_SECTORS));
        int mlum = lk->hue_lum[sec] +
                   (((lk->hue_lum[nx] - lk->hue_lum[sec]) * f)
                    / (1536 / SE_HUE_SECTORS));
        int mrot = lk->hue_rot[sec] +
                   (((lk->hue_rot[nx] - lk->hue_rot[sec]) * f)
                    / (1536 / SE_HUE_SECTORS));
        int y = luma8(rr, gg, bb);
        int u = bb - y, v = rr - y;

        /*
         * Fade the whole stage out as the pixel approaches
         * neutral. See SE_HUE_FULL: the hue of a near-grey pixel
         * is meaningless, and acting on it tears flat areas into
         * blotches.
         */
        if (chroma < SE_HUE_FULL) {
          int wt = s_fade[chroma]; /* (chroma * 256) / SE_HUE_FULL */
          msat = 256 + (((msat - 256) * wt) >> 8);
          mlum = 256 + (((mlum - 256) * wt) >> 8);
          mrot = (mrot * wt) >> 8;
        }

        if (mrot) {
          int cs = SE_COS(mrot), sn = SE_SIN(mrot);
          int u2 = (u * cs - v * sn) >> 8;
          int v2 = (u * sn + v * cs) >> 8;
          u = u2;
          v = v2;
        }
        if (msat != 256) {
          u = (u * msat) >> 8;
          v = (v * msat) >> 8;
        }
        if (mlum != 256)
          y = (y * mlum) >> 8;

        rr = y + v;
        bb = y + u;
        /*
         * Same value as (77*v + 29*u) / 150 without the divide: these
         * bodies have no divide instruction, so every '/' is an
         * __aeabi_idiv call of about 100 cycles, once per pixel.
         * 27963/2^22 reproduces x/150 exactly for every |x| <= 27030
         * (77*255 + 29*255) -- the whole range here -- and the sign is
         * handled outside so truncation matches C for negative x.
         */
        {
          int t2 = 77 * v + 29 * u;
          int sg = 1;
          if (t2 < 0) {
            t2 = -t2;
            sg = -1;
          }
          gg = y - sg * ((t2 * 27963) >> 22);
        }
      }
    }


    /*
     * Global saturation, on top of the per-hue stage. Kept separate from
     * hue_sat because it has to work for a stock that sets no per-hue
     * controls at all, and skipped entirely at 256 so a stock that says
     * nothing costs nothing.
     */
    if (lk->sat != 256) {
      int y = luma8(clamp8(rr), clamp8(gg), clamp8(bb));
      rr = y + (((rr - y) * lk->sat) >> 8);
      gg = y + (((gg - y) * lk->sat) >> 8);
      bb = y + (((bb - y) * lk->sat) >> 8);
    }

    /*
     * Film's own chroma rolloff, applied to the scene colour before print
     * split-toning so SPLIT_HI's warm paper-base tint is preserved at the
     * highlight shoulder instead of collapsing to monochrome grey.
     */
    {
      int y = (77 * rr + 150 * gg + 29 * bb) >> 8;
      int w;
      if (y < 0)
        y = 0;
      if (y > 255)
        y = 255;
      w = s_chroma[y];
      if (w != 256) {
        rr = y + (((rr - y) * w) >> 8);
        gg = y + (((gg - y) * w) >> 8);
        bb = y + (((bb - y) * w) >> 8);
      }
    }

    if (lk->sh[0] | lk->sh[1] | lk->sh[2] | lk->hi[0] | lk->hi[1] | lk->hi[2]) {
      int y = luma8(clamp8(rr), clamp8(gg), clamp8(bb));
      int wl = 255 - y; /* shadow weight    */
      rr += (lk->sh[0] * wl + lk->hi[0] * y) >> 8;
      gg += (lk->sh[1] * wl + lk->hi[1] * y) >> 8;
      bb += (lk->sh[2] * wl + lk->hi[2] * y) >> 8;
    }

    gamut_fit(&rr, &gg, &bb);

    /*
     * Strength: halfway between the picture that came in and the one the
     * stock asks for. Last, so curves, hue, split tone and chroma rolloff all
     * scale together. Division by 100 via fixed point 5243 / 2^19 (exact for
     * |d*st| <= 25500).
     */
    if (st < 100) {
      int dr = rr - o0, sgr = 1; if (dr < 0) { dr = -dr; sgr = -1; }
      int dg = gg - o1, sgg = 1; if (dg < 0) { dg = -dg; sgg = -1; }
      int db = bb - o2, sgb = 1; if (db < 0) { db = -db; sgb = -1; }
      rr = o0 + sgr * ((dr * st_scaled) >> 19);
      gg = o1 + sgg * ((dg * st_scaled) >> 19);
      bb = o2 + sgb * ((db * st_scaled) >> 19);
    }

    row[x * 3 + 0] = (uint8_t)clamp8(rr);
    row[x * 3 + 1] = (uint8_t)clamp8(gg);
    row[x * 3 + 2] = (uint8_t)clamp8(bb);
  }
}

/*
 * The highlight mask for one graded row, blurred horizontally.
 *
 * A running sum, so the cost per pixel does not depend on the radius --
 * one add and one subtract however wide the glow is. The mask is how far
 * the pixel is above the threshold, scaled so that a pixel at 255 is 255:
 * a soft knee would be prettier, but this is what a threshold means and it
 * is one comparison.
 */

/*
 * Division without the library call.
 *
 * Every one of these five divisors is constant for the whole row (a window
 * width, a row count, a threshold span, 100, 255*255), and on an ARMv5 a
 * divide is a call to __aeabi_idiv -- while the develop's own profile says the
 * look costs 11 s for 2.8 Mpx, about twenty thousand times what the same code
 * costs on a desktop. Five divides per pixel over 2.8 Mpx is fourteen million
 * library calls, which is the right order of magnitude for that gap.
 *
 * q0 = (acc * r) >> 32 with r = floor((2^32 - 1) / n) + 1 lands within one of
 * the true quotient, either side, so two compares finish the job. The multiply
 * is one UMULL on this core. Checked exhaustively against the division for
 * every divisor and accumulator the five sites can produce -- 25 million
 * pairs, no mismatch.
 */
static uint32_t div_r(uint32_t acc, uint32_t n, uint32_t r) {
  uint32_t q = (uint32_t)(((uint64_t)acc * (uint64_t)r) >> 32);
  while ((uint64_t)(q + 1) * n <= acc)
    q++;
  while ((uint64_t)q * n > acc)
    q--; /* r can round the other way */
  return q;
}

static uint32_t recip32(uint32_t n) {
  return (uint32_t)((((uint64_t)1 << 32) - 1) / (n ? n : 1)) + 1;
}

static void mask_row(const se_look *lk, const uint8_t *row, int w,
                     uint8_t *scratch, uint8_t *dst) {
  int x, rad = lk->hal_radius, win = 2 * rad + 1;
  long sum = 0;
  int span = 255 - lk->hal_thresh;
  if (span < 1)
    span = 1;

  {
    const uint32_t rs = recip32((uint32_t)span);
    for (x = 0; x < w; x++) {
      int y = luma8(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]);
      int v = y - lk->hal_thresh;
      scratch[x] =
          (uint8_t)(v <= 0 ? 0
                           : div_r((uint32_t)(v * 255), (uint32_t)span, rs));
    }
  }

  /* prime the window with the left edge clamped */
  for (x = -rad; x <= rad; x++)
    sum += scratch[x < 0 ? 0 : (x >= w ? w - 1 : x)];
  {
    const uint32_t rw = recip32((uint32_t)win);
    for (x = 0; x < w; x++) {
      int add = x + rad + 1, sub = x - rad;
      dst[x] = (uint8_t)div_r((uint32_t)sum, (uint32_t)win, rw);
      sum += scratch[add >= w ? w - 1 : add];
      sum -= scratch[sub < 0 ? 0 : sub];
    }
  }
}

/* Add the glow to one row, given its vertical column sums. */
static void add_glow(const se_look *lk, uint8_t *row, int w, const int *colsum,
                     int nrows) {
  int x;
  if (nrows < 1)
    nrows = 1;
  {
    const uint32_t rn = recip32((uint32_t)nrows);
    const uint32_t ra = recip32(100u);
    const uint32_t rq = recip32(255u * 255u);
    for (x = 0; x < w; x++) {
      int g = (int)div_r((uint32_t)colsum[x], (uint32_t)nrows, rn); /* 0..255 */
      int a, room;
      if (g <= 0)
        continue;
      a = (int)div_r((uint32_t)(g * (int)lk->hal_amount), 100u, ra);

      /*
       * Fade the glow out where the frame is already bright.
       *
       * Halation is light that scattered off the base and exposed the
       * emulsion a second time. Where the emulsion is already near
       * saturation that second exposure changes nothing, so the halo is
       * only ever visible bleeding into something darker.
       *
       * Adding it everywhere is what made this unusable in daylight: a
       * white building lit the whole mask, the glow landed on top of
       * the building as well as beside it, and the frame came out
       * outlined in red and washed pink. Weighting by the headroom left
       * in the destination fixes both ends at once -- a street light
       * against a dark wall still blooms hard, and a white wall in
       * sunlight gets almost nothing, which is what film does.
       */
      room = 255 - luma8(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]);
      /*
       * Squared, not linear.
       *
       * With the mask taken from the ungraded row (see push_row) the glow
       * arrives at full strength instead of the ~30% the graded mask
       * allowed, and a linear headroom weight then pushed bright stocks
       * into clipping: CineStill 800T went from 0% to 25% of pixels with a
       * saturated red channel on a daylight frame. Headroom squared leaves
       * a glow bleeding into something dark almost untouched (room 200:
       * 0.61 instead of 0.78) while a glow landing on something already
       * bright is cut hard (room 65: 0.06 instead of 0.25), which is what
       * the film actually does.
       */
      a = (int)div_r((uint32_t)(a * room * room), 255u * 255u, rq);
      if (a <= 0)
        continue;

      row[x * 3 + 0] =
          (uint8_t)clamp8(row[x * 3 + 0] + ((a * lk->hal_tint[0]) >> 8));
      row[x * 3 + 1] =
          (uint8_t)clamp8(row[x * 3 + 1] + ((a * lk->hal_tint[1]) >> 8));
      row[x * 3 + 2] =
          (uint8_t)clamp8(row[x * 3 + 2] + ((a * lk->hal_tint[2]) >> 8));
    }
  }
}

/* one lattice point, signed: -32768..32767 */
static int grain_val(unsigned xc, unsigned yc, unsigned seed)
{
  return (int)(grain_hash(xc, yc, seed) & 0xFFFFu) - 32768;
}

static void grain_row(const se_look *lk, uint8_t *row, int w, int y) {
  int x, cell = lk->grain_cell;
  /* 0..100 -> about +-15 at full strength. This was 72/100, which put
   * +-28 on a midtone: at that amplitude a coarse stock stopped reading
   * as emulsion and started reading as sensor noise, which is the one
   * thing grain must not look like. */
  int amp = (lk->grain * SE_GRAIN_AMP) / 100;
  /* Keep grain whisper-fine so emulsions never read as digital luminance noise. */
  amp = amp * 2 / 7;
  if (amp <= 0)
    return;
  /*
   * The cell coordinates were divisions inside the pixel loop -- two per
   * pixel, a __aeabi_idiv call each on this CPU. y/cell is constant for the
   * row and x/cell only steps every `cell` pixels, so count instead of
   * dividing. Same values, no divide.
   */
  {
    /*
     * Two octaves, interpolated.
     *
     * The old grain was one hash per square cell: every cell a flat block, and
     * every block exactly on a grid, so at large sizes it read as tiles rather
     * than emulsion. Real grain is clumps of varying density.
     *
     * Octave 1 interpolates the hash lattice bilinearly, which turns the
     * squares into smooth blobs. The corners are hashed once per CELL, not per
     * pixel, so the per-pixel cost stays a few multiplies. Interpolation
     * flattens the variance, hence the 3/2 gain.
     *
     * Octave 2 is a plain half-size lattice at a quarter weight. On its own it
     * would be the same tiling problem; under octave 1 it breaks the blobs up
     * and gives the fine structure a fast film has.
     */
    const unsigned yc = (unsigned)(y / cell);
    const int fy  = (int)(((y - (int)yc * (long)cell) * 256) / cell);
    const int ify = 256 - fy;
    const int step = (cell > 0) ? 256 / cell : 256;
    const int fine = (cell > 1) ? (cell >> 1) : 1;
    const unsigned fyc = (unsigned)(y / fine);
    unsigned xc = 0, fxc = 0;
    int left = cell, fleft = fine, fx = 0, ifx = 256;
    int a = grain_val(xc, yc, 0x5EEDu), b = grain_val(xc + 1, yc, 0x5EEDu);
    int c = grain_val(xc, yc + 1, 0x5EEDu), d = grain_val(xc + 1, yc + 1, 0x5EEDu);
    /* octave 2 is constant across a fine cell: hash it once, not per pixel */
    int fn = grain_val(fxc, fyc, 0x9A31u) >> 2;
    for (x = 0; x < w; x++) {
      int n, top, bot;
      /* >>8 here, and >>8 again below: (a*ifx + b*fx) reaches 2^23, and
       * multiplying that by 256 to shift at the end overflows int32 at
       * the extreme lattice values. */
      top = (a * ifx + b * fx) >> 8;
      bot = (c * ifx + d * fx) >> 8;
      n = (((top * ify + bot * fy) >> 8) * 3) >> 1;   /* 1.5x: interpolation costs variance */
      n += fn;
      {
        /* +16384 rounds instead of flooring: >> on a negative value
         * floors, which biased every grained pixel down by ~1 level. */
        int dd = ((n * amp) + 16384) >> 15;
        int ch;
        for (ch = 0; ch < 3; ch++) {
          int v = row[x * 3 + ch];
          /* 4v(1-v), in eighths, so grain fades out at both ends */
          int wgt = (v * (255 - v)) >> 6; /* 0..254 */
          row[x * 3 + ch] = (uint8_t)clamp8(v + ((dd * wgt + 128) >> 8));
        }
      }
      /*
       * Advance AFTER the pixel, not before.
       *
       * Both counters used to be pre-decremented at the top of the body, so
       * x = 0 consumed a step: with cell = 4 the first cell covered three
       * pixels instead of four and every cell boundary in the row sat one
       * pixel left of where it should, and the four lattice hashes computed
       * before the loop were thrown away unused. Same for the fine octave.
       */
      if (--left <= 0) {
        left = cell;
        xc++;
        fx = 0;
        ifx = 256;
        a = b;
        c = d;
        b = grain_val(xc + 1, yc, 0x5EEDu);
        d = grain_val(xc + 1, yc + 1, 0x5EEDu);
      } else {
        fx += step;
        ifx = 256 - fx;
      }
      if (--fleft <= 0) {
        fleft = fine;
        fxc++;
        fn = grain_val(fxc, fyc, 0x9A31u) >> 2;
      }
    }
  }
}

/* One row, fully graded and already in `dst`, ready to go out. */
static se_err emit_one(se_look_run *r, uint8_t *dst, se_look_emit emit,
                       void *ctx) {
  if (r->lk.grain > 0) {
    uint32_t g0 = se_lk_ms();
    grain_row(&r->lk, dst, r->w, r->y_out);
    se_lk_t_grain += se_lk_ms() - g0;
  }
  r->y_out++;
  return emit(ctx, dst, r->w) == 0 ? SE_OK : SE_E_IO;
}

/*
 * Push one source row into the delay line, and emit whatever row that
 * completes.
 *
 * THE INDEXING, because it is the one thing here that is easy to get wrong
 * and impossible to see wrong in a photograph:
 *
 *   The ring is primed with `r` extra copies of row 0, so source row j
 *   lands at push index j + r. After push index p, the ring holds push
 *   indices p-2r .. p, which is exactly the vertical window centred on
 *   output row p - 2r. That row's RGB is at push index p - r, still in the
 *   ring because nring = 2r+1 > r.
 *
 *   Top and bottom edges are handled by replication -- the priming copies
 *   at the top, and `r` more copies of the last mask row in se_look_end at
 *   the bottom. Clamping the window instead would need a per-row divisor
 *   and would still be a guess about what is off the edge of the frame.
 *
 *   Total pushes are H + 2r and emits start at p = 2r, so exactly H rows
 *   come out however short the image is -- including H < r, where nothing
 *   at all is emitted until the flush.
 */
static se_err emit_delayed(se_look_run *r, int p, se_look_emit emit, void *ctx);

static se_err push_row(se_look_run *r, const uint8_t *src, se_look_emit emit,
                       void *ctx) {
  int w = r->w;
  int slot = r->pushed % r->nring;
  int dslot = r->pushed % r->ndelay;
  uint8_t *dly = r->delay + (size_t)dslot * w * 3;
  uint8_t *msk = r->ring + (size_t)slot * w;
  int x, p;

  /* The row leaving the window stops contributing to the column sums.
   * Only once the ring has been round at least once, or we would be
   * subtracting the zeros se_look_begin put there. */
  if (r->pushed >= r->nring)
    for (x = 0; x < w; x++)
      r->colsum[x] -= msk[x];

  memcpy(dly, src, (size_t)w * 3);
  {
    uint32_t q0 = se_lk_ms();
    grade_row(&r->lk, dly, w);
    se_lk_t_grade += se_lk_ms() - q0;
    se_lk_n_rows++;
  }

  /*
   * The mask comes from the UNGRADED row.
   *
   * Halation is light that scattered off the film base and exposed the
   * emulsion a second time, so how strongly it fires depends on how much
   * light hit the film -- the scene -- and not on the stock's print curve.
   *
   * Taking it after grade_row() put the threshold above the stock's own
   * white, because every curve pulls 255 down: Portra ends at 236, so a
   * fully blown highlight produced a mask of only (236-228)/(255-228) =
   * 30%, and after the box blur a bright frame measured +0.4 levels of red
   * in its shadows. That is why the glow could never be seen: it was being
   * measured against a white the stock never reaches.
   */
  {
    uint32_t m0 = se_lk_ms();
    mask_row(&r->lk, src, w, r->scratch, msk);
    se_lk_t_mask += se_lk_ms() - m0;
  }

  for (x = 0; x < w; x++)
    r->colsum[x] += msk[x];

  p = r->pushed++;
  if (p < 2 * r->r)
    return SE_OK; /* window not centred yet */
  return emit_delayed(r, p, emit, ctx);
}

/* Emit the output row whose window has just closed at push index p. */
static se_err emit_delayed(se_look_run *r, int p, se_look_emit emit,
                           void *ctx) {
  int slot = (p - r->r) % r->ndelay;
  uint8_t *dly = r->delay + (size_t)slot * r->w * 3;

  {
    uint32_t a0 = se_lk_ms();
    add_glow(&r->lk, dly, r->w, r->colsum, r->nring);
    se_lk_t_glow += se_lk_ms() - a0;
  }
  return emit_one(r, dly, emit, ctx);
}

se_err se_look_rows(se_look_run *r, const uint8_t *rgb, int nrows,
                    se_look_emit emit, void *ctx) {
  int i;
  se_err e;

  if (!r || !rgb || nrows < 0 || !emit)
    return SE_E_CORRUPT;

  for (i = 0; i < nrows; i++) {
    const uint8_t *src = rgb + (size_t)i * r->w * 3;

    if (!r->on) {
      /* No halation, so no neighbourhood and no delay: grade and go.
       * The caller's rows are const and may be the decoder's own
       * scratch, so the grade lands in our row, never in theirs. */
      memcpy(r->flat, src, (size_t)r->w * 3);
      grade_row(&r->lk, r->flat, r->w);
      e = emit_one(r, r->flat, emit, ctx);
      if (e != SE_OK)
        return e;
      continue;
    }

    /* First row of the frame: prime the top edge with r copies of it,
     * so the window is full from the first emit onwards. */
    if (r->pushed == 0) {
      int k;
      for (k = 0; k < r->r; k++) {
        e = push_row(r, src, emit, ctx);
        if (e != SE_OK)
          return e;
      }
    }

    e = push_row(r, src, emit, ctx);
    if (e != SE_OK)
      return e;
  }
  return SE_OK;
}

se_err se_look_end(se_look_run *r, se_look_emit emit, void *ctx) {
  int k, w, last;
  se_err e;

  if (!r || !emit)
    return SE_E_CORRUPT;
  if (!r->on || r->pushed == 0)
    return SE_OK;

  w = r->w;

  /* The bottom edge, replicated: copy the last mask row down `r` times.
   * The mask is copied rather than re-derived because the graded RGB it
   * came from has since had glow and grain written into it -- masking
   * that again would feed the glow back through itself. */
  for (k = 0; k < r->r; k++) {
    int slot, x;
    last = (r->pushed - 1) % r->nring;
    slot = r->pushed % r->nring;

    if (r->pushed >= r->nring)
      for (x = 0; x < w; x++)
        r->colsum[x] -= r->ring[(size_t)slot * w + x];
    memcpy(r->ring + (size_t)slot * w, r->ring + (size_t)last * w, (size_t)w);
    for (x = 0; x < w; x++)
      r->colsum[x] += r->ring[(size_t)slot * w + x];

    r->pushed++;
    if (r->pushed - 1 < 2 * r->r)
      continue;
    e = emit_delayed(r, r->pushed - 1, emit, ctx);
    if (e != SE_OK)
      return e;
  }
  return SE_OK;
}
