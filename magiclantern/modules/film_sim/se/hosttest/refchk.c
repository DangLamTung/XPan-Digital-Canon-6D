/*
 * refchk - ColorChecker 24 through a .fs stock and through a reference .cube
 * film LUT, scored as CIE dE2000. The other host tools measure the engine
 * against itself; this is the only one that measures it against real film.
 *
 *   ./refchk stock.fs reference.cube
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

/* M_PI is not in ISO C: glibc hides it under -std=c99, macOS defines it
 * anyway. This only ever broke on Linux. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "se/engine/stock.h"
#include "se/engine/look.h"
#include "se/engine/lut.h"

static const int cc24[24][3] = {
  {115,82,68},{194,150,130},{98,122,157},{87,108,67},{133,128,177},{103,189,170},
  {214,126,44},{80,91,166},{193,90,99},{94,60,108},{157,188,64},{224,163,46},
  {56,61,150},{70,148,73},{175,54,60},{231,199,31},{187,86,149},{8,133,161},
  {243,243,242},{200,200,200},{160,160,160},{122,122,121},{85,85,85},{52,52,52}
};

static double linear_to_srgb(double v)
{
  return (v <= 0.0031308) ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

static double srgb_to_linear(double v)
{
  return (v > 0.04045) ? pow((v + 0.055) / 1.055, 2.4) : v / 12.92;
}

static void rgb2lab(double r, double g, double b, double *L, double *A, double *B)
{
  double v[3]; int i;
  double x, y, z;
  v[0] = r / 255.0; v[1] = g / 255.0; v[2] = b / 255.0;
  for (i = 0; i < 3; i++)
    v[i] = (v[i] > 0.04045) ? pow((v[i] + 0.055) / 1.055, 2.4) : v[i] / 12.92;
  x = (v[0]*0.4124 + v[1]*0.3576 + v[2]*0.1805) / 0.95047;
  y = (v[0]*0.2126 + v[1]*0.7152 + v[2]*0.0722);
  z = (v[0]*0.0193 + v[1]*0.1192 + v[2]*0.9505) / 1.08883;
  for (i = 0; i < 3; i++) { double *p = (i==0)?&x:(i==1)?&y:&z;
    *p = (*p > 0.008856) ? pow(*p, 1.0/3.0) : (7.787 * *p + 16.0/116.0); }
  *L = 116.0*y - 16.0; *A = 500.0*(x-y); *B = 200.0*(y-z);
}

static double de00(const double *l1, const double *l2)
{
  double dL = l1[0]-l2[0], C1 = hypot(l1[1],l1[2]), C2 = hypot(l2[1],l2[2]);
  double Cb = (C1+C2)/2, G = 0.5*(1-sqrt(pow(Cb,7)/(pow(Cb,7)+pow(25.0,7))));
  double a1 = l1[1]*(1+G), a2 = l2[1]*(1+G);
  double Cp1 = hypot(a1,l1[2]), Cp2 = hypot(a2,l2[2]);
  double h1 = atan2(l1[2],a1)*180/M_PI, h2 = atan2(l2[2],a2)*180/M_PI;
  double dCp = Cp1-Cp2, dhp, dHp, Lb = (l1[0]+l2[0])/2, Cpb = (Cp1+Cp2)/2;
  double hb;
  if (h1 < 0) h1 += 360; if (h2 < 0) h2 += 360;
  dhp = h2-h1;
  if (Cp1*Cp2 == 0) dhp = 0;
  else if (dhp > 180) dhp -= 360; else if (dhp < -180) dhp += 360;
  dHp = 2*sqrt(Cp1*Cp2)*sin(dhp*M_PI/360);
  hb = (fabs(h1-h2) > 180) ? (h1+h2+360)/2 : (h1+h2)/2;
  {
    double T = 1 - 0.17*cos((hb-30)*M_PI/180) + 0.24*cos(2*hb*M_PI/180)
                 + 0.32*cos((3*hb+6)*M_PI/180) - 0.20*cos((4*hb-63)*M_PI/180);
    double SL = 1 + 0.015*pow(Lb-50,2)/sqrt(20+pow(Lb-50,2));
    double SC = 1 + 0.045*Cpb, SH = 1 + 0.015*Cpb*T;
    double RT = -2*sqrt(pow(Cpb,7)/(pow(Cpb,7)+pow(25.0,7)))
                * sin(60*exp(-pow((hb-275)/25,2))*M_PI/180);
    return sqrt(pow(dL/SL,2) + pow(dCp/SC,2) + pow(dHp/SH,2)
                + RT*(dCp/SC)*(dHp/SH));
  }
}


static uint8_t g_out[3 * 24];
static int emit_cb(void *ctx, const uint8_t *row, int w)
{
  int *n = (int *)ctx;
  memcpy(g_out + (size_t)3 * (*n), row, (size_t)w * 3);
  *n += w;
  return 0;
}

int main(int argc, char **argv)
{
  FILE *f;
  char text[65536];
  size_t got;
  se_stock st;
  se_look lk;
  se_look_run *run = NULL;
  static uint8_t arena_mem[SE_LOOK_BUDGET];
  se_arena ar;
  uint8_t row[3 * 24];
  se_lut3 cube = { 0, NULL };
  static uint8_t cube_arena_mem[512 * 1024];
  se_arena ar2;
  uint16_t cube_data[65 * 65 * 65 * 3];
  char cube_text[1 << 20];
  int i, n = 0;
  double sum = 0, worst = 0;
  int worst_i = 0;

  /* The reference cube's input domain decides everything. abpy's Fuji
   * profiles want LINEAR scene values; GlassMeter's say "input/output:
   * sRGB encoded" outright. Mixing the two produced numbers that were
   * wrong by 15 levels, so the caller now has to say which it is. */
  int linear_in = 1;
  if (argc >= 4 && !strcmp(argv[3], "display")) linear_in = 0;
  if (argc < 3) {
    fprintf(stderr, "usage: %s stock.fs reference.cube [display]\n", argv[0]);
    return 2;
  }

  f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 1; }
  got = fread(text, 1, sizeof text, f);
  fclose(f);
  if (se_stock_parse(text, got, &st) != SE_OK) { fprintf(stderr, "parse .fs failed\n"); return 1; }
  se_look_from_stock(&st, 1, &lk);
  /* These 24 patches are measured as a single row, so a horizontal halation
   * blur would bleed the white patch into its neighbours and grain would be
   * frozen at y = 0: both would be baked into the numbers. */
  lk.grain = 0;
  lk.hal_amount = 0;
  se_arena_init(&ar, arena_mem, sizeof arena_mem);
  if (se_look_begin(&run, &lk, 24, &ar) != SE_OK) { fprintf(stderr, "look begin failed\n"); return 1; }

  for (i = 0; i < 24; i++) {
    row[i * 3 + 0] = (uint8_t)cc24[i][0];
    row[i * 3 + 1] = (uint8_t)cc24[i][1];
    row[i * 3 + 2] = (uint8_t)cc24[i][2];
  }
  if (se_look_rows(run, row, 1, emit_cb, &n) != SE_OK) { fprintf(stderr, "rows failed\n"); return 1; }
  se_look_end(run, emit_cb, &n);

  f = fopen(argv[2], "rb");
  if (!f) { perror(argv[2]); return 1; }
  got = fread(cube_text, 1, sizeof cube_text, f);
  fclose(f);
  se_arena_init(&ar2, cube_arena_mem, sizeof cube_arena_mem);
  if (se_lut3_parse_cube(cube_text, got, &cube, &ar2) != SE_OK) {
    fprintf(stderr, "parse .cube failed\n"); return 1;
  }
  (void)cube_data;

  printf("%s vs %s\n", st.name, argv[2]);
  for (i = 0; i < 24; i++) {
    double l1[3], l2[3], d;
    uint16_t p[3];
    /*
     * The reference cubes expect LINEAR input: abpy's FujifilmCameraProfiles
     * README says "the cube LUTs are intended to be applied to an image with
     * linear contrast. A linear camera profile is required for a correct
     * result." Feeding display-encoded sRGB made every number this tool
     * printed meaningless -- and black/white still looked right, because
     * both ends clamp, which is why a bad test passed.
     */
    if (linear_in) {
      p[0] = (uint16_t)(srgb_to_linear(cc24[i][0] / 255.0) * 65535.0);
      p[1] = (uint16_t)(srgb_to_linear(cc24[i][1] / 255.0) * 65535.0);
      p[2] = (uint16_t)(srgb_to_linear(cc24[i][2] / 255.0) * 65535.0);
    } else {
      p[0] = (uint16_t)(cc24[i][0] * 257);
      p[1] = (uint16_t)(cc24[i][1] * 257);
      p[2] = (uint16_t)(cc24[i][2] * 257);
    }
    se_lut3_apply_row(&cube, p, 1);
    rgb2lab(g_out[i * 3 + 0], g_out[i * 3 + 1], g_out[i * 3 + 2], &l1[0], &l1[1], &l1[2]);
    /* In linear mode the cube hands back LINEAR light; rgb2lab expects
     * display-encoded values and would linearise a second time. That double
     * conversion is what made the abpy numbers read 16.85 instead of 10.16. */
    if (linear_in) {
      double r = linear_to_srgb(p[0] / 65535.0) * 255.0;
      double g = linear_to_srgb(p[1] / 65535.0) * 255.0;
      double b = linear_to_srgb(p[2] / 65535.0) * 255.0;
      rgb2lab(r, g, b, &l2[0], &l2[1], &l2[2]);
    } else {
      rgb2lab(p[0] / 257.0, p[1] / 257.0, p[2] / 257.0, &l2[0], &l2[1], &l2[2]);
    }
    d = de00(l1, l2);
    sum += d;
    if (d > worst) { worst = d; worst_i = i; }
  }
  printf("  CC24 mean dE00 %5.2f   worst %5.2f (patch %d)\n", sum / 24, worst, worst_i);
  return 0;
}
