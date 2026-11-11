#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "engine/stock.h"
#include "engine/look.h"

#define N_TARGETS 27

static const int p160_in[N_TARGETS][3] = {
  {61, 61, 61},
  {87, 87, 87},
  {114, 114, 114},
  {140, 140, 140},
  {167, 167, 167},
  {193, 193, 193},
  {78, 72, 0},
  {44, 89, 0},
  {0, 89, 22},
  {0, 89, 89},
  {0, 22, 89},
  {33, 5, 89},
  {145, 134, 0},
  {82, 165, 0},
  {0, 165, 41},
  {0, 165, 165},
  {0, 41, 165},
  {62, 10, 165},
  {213, 196, 0},
  {121, 242, 0},
  {0, 242, 60},
  {0, 242, 242},
  {0, 60, 242},
  {92, 15, 242},
  {97, 82, 75},    // 24. Portrait Skin
  {48, 134, 137},  // 25. Mountain Deep Water
  {62, 67, 52}     // 26. Foliage Greenery
};

static const int p160_deh[N_TARGETS][3] = {
  {37, 36, 38},
  {59, 61, 63},
  {88, 93, 95},
  {112, 120, 123},
  {134, 145, 147},
  {157, 171, 173},
  {50, 41, 4},
  {30, 54, 0},
  {12, 56, 9},
  {14, 65, 62},
  {0, 19, 65},
  {14, 12, 64},
  {117, 95, 10},
  {72, 124, 0},
  {40, 133, 21},
  {37, 158, 155},
  {0, 42, 155},
  {25, 30, 155},
  {197, 154, 20},
  {117, 211, 0},
  {83, 222, 25},
  {65, 255, 254},
  {0, 75, 255},
  {33, 59, 255},
  {101, 89, 83},   // 24. Target Portrait Skin
  {71, 138, 164},  // 25. Target Mountain Deep Water
  {64, 74, 61}     // 26. Target Foliage Greenery
};

static const char *patch_names[N_TARGETS] = {
  "Gray 1 (Dark)", "Gray 2", "Gray 3 (Mid)", "Gray 4", "Gray 5", "Gray 6 (Light)",
  "Dark Yellow", "Dark Green", "Dark Cyan", "Dark Slate", "Dark Blue", "Dark Violet",
  "Mid Yellow", "Mid Green", "Mid Teal", "Mid Cyan", "Mid Blue", "Mid Violet",
  "Bright Yellow", "Bright Green", "Bright Teal", "Bright Cyan", "Bright Blue", "Bright Violet",
  "Portrait Skin", "Mountain Water", "Foliage Green"
};

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

static uint8_t g_out[3 * N_TARGETS];
static int emit_cb(void *ctx, const uint8_t *row, int w)
{
  int *n = (int *)ctx;
  memcpy(g_out + (size_t)3 * (*n), row, (size_t)w * 3);
  *n += w;
  return 0;
}

static double eval_stock(const se_stock *st, double *out_des, uint8_t out_rgb[N_TARGETS][3])
{
  se_look lk;
  se_look_run *run = NULL;
  static uint8_t arena_mem[SE_LOOK_BUDGET];
  se_arena ar;
  uint8_t row[3 * N_TARGETS];
  int i, n = 0;
  double sum_de = 0.0;
  double worst_de = 0.0;

  se_look_from_stock(st, 1, &lk);
  se_arena_init(&ar, arena_mem, sizeof arena_mem);
  if (se_look_begin(&run, &lk, N_TARGETS, &ar) != SE_OK) return 999.0;

  for (i = 0; i < N_TARGETS; i++) {
    row[i * 3 + 0] = (uint8_t)p160_in[i][0];
    row[i * 3 + 1] = (uint8_t)p160_in[i][1];
    row[i * 3 + 2] = (uint8_t)p160_in[i][2];
  }
  se_look_rows(run, row, 1, emit_cb, &n);
  se_look_end(run, emit_cb, &n);

  for (i = 0; i < N_TARGETS; i++) {
    double L1, A1, B1, L2, A2, B2;
    double lab1[3], lab2[3];
    uint8_t r = g_out[i * 3 + 0];
    uint8_t g = g_out[i * 3 + 1];
    uint8_t b = g_out[i * 3 + 2];
    if (out_rgb) {
      out_rgb[i][0] = r; out_rgb[i][1] = g; out_rgb[i][2] = b;
    }
    rgb2lab(p160_deh[i][0], p160_deh[i][1], p160_deh[i][2], &L1, &A1, &B1);
    rgb2lab(r, g, b, &L2, &A2, &B2);
    lab1[0] = L1; lab1[1] = A1; lab1[2] = B1;
    lab2[0] = L2; lab2[1] = A2; lab2[2] = B2;
    double d = de00(lab1, lab2);
    if (out_des) out_des[i] = d;
    
    // Balanced weighting
    double weight = 1.0;
    if (i == 24) weight = 5.0; // Skin
    if (i == 25) weight = 4.5; // Water
    if (i == 26) weight = 3.5; // Foliage
    sum_de += d * weight;
    if (d > worst_de) worst_de = d;
  }

  // Smoothness regularization to prevent gradient banding between adjacent sectors
  double smoothness = 0.0;
  for (int s = 0; s < SE_HUE_SECTORS; s++) {
    int nx = (s + 1) % SE_HUE_SECTORS;
    double d_sat = (st->hue_sat[nx] - st->hue_sat[s]) / 8.0;
    double d_lum = (st->hue_lum[nx] - st->hue_lum[s]) / 6.0;
    double d_rot = (st->hue_rot[nx] - st->hue_rot[s]) / 1.5;
    smoothness += (d_sat * d_sat + d_lum * d_lum + d_rot * d_rot);
  }

  return (sum_de / (N_TARGETS + 10.0)) + 0.15 * worst_de + 0.006 * smoothness;
}

int main(int argc, char **argv)
{
  char text[16384];
  se_stock best_st, cur_st;
  FILE *f;
  size_t got;
  const char *path = argc > 1 ? argv[1] : "../../films/P160.fs";
  double des[N_TARGETS];
  uint8_t rgbs[N_TARGETS][3];
  int s, iter, k;

  f = fopen(path, "rb");
  if (!f) { perror(path); return 1; }
  got = fread(text, 1, sizeof text - 1, f);
  text[got] = '\0';
  fclose(f);

  if (se_stock_parse(text, got, &best_st) != SE_OK) {
    fprintf(stderr, "se_stock_parse failed\n");
    return 1;
  }

  // Split Toning: cool cyan shadows
  best_st.split_sh[0] = -3.0f; // Red: reduce shadow warmth
  best_st.split_sh[1] = +1.0f; // Green: gentle neutral
  best_st.split_sh[2] = +12.0f; // Blue: cool cyan shadow base
  best_st.split_hi[0] = 0.0f;
  best_st.split_hi[1] = 0.0f;
  best_st.split_hi[2] = 0.0f;

  // Master Curve based on Dehancer grayscale transfer
  best_st.master.n = 5;
  best_st.master.x[0] = 0;   best_st.master.y[0] = 16;
  best_st.master.x[1] = 64;  best_st.master.y[1] = 42;
  best_st.master.x[2] = 128; best_st.master.y[2] = 104;
  best_st.master.x[3] = 192; best_st.master.y[3] = 170;
  best_st.master.x[4] = 255; best_st.master.y[4] = 246;

  // Channel Curves
  best_st.r.n = 3;
  best_st.r.x[0] = 0;   best_st.r.y[0] = 6;
  best_st.r.x[1] = 128; best_st.r.y[1] = 124;
  best_st.r.x[2] = 255; best_st.r.y[2] = 246;

  best_st.g.n = 3;
  best_st.g.x[0] = 0;   best_st.g.y[0] = 12;
  best_st.g.x[1] = 128; best_st.g.y[1] = 132;
  best_st.g.x[2] = 255; best_st.g.y[2] = 252;

  best_st.b.n = 5;
  best_st.b.x[0] = 0;   best_st.b.y[0] = 24;
  best_st.b.x[1] = 64;  best_st.b.y[1] = 78;
  best_st.b.x[2] = 128; best_st.b.y[2] = 144;
  best_st.b.x[3] = 192; best_st.b.y[3] = 198;
  best_st.b.x[4] = 255; best_st.b.y[4] = 248;

  double best_score = eval_stock(&best_st, des, rgbs);
  printf("Starting Score: %f\n", best_score);

  cur_st = best_st;

  // Optimization loop over sectors, curves, and split toning
  for (iter = 0; iter < 180; iter++) {
    int improved = 0;

    // 1. Optimize HUE sectors (48 sectors)
    for (s = 0; s < SE_HUE_SECTORS; s++) {
      // HUE_ROT
      float rot_orig = cur_st.hue_rot[s];
      float rot_steps[] = { -3.0f, -1.5f, -0.6f, -0.2f, 0.2f, 0.6f, 1.5f, 3.0f };
      for (int step = 0; step < 8; step++) {
        cur_st.hue_rot[s] = rot_orig + rot_steps[step];
        double sc = eval_stock(&cur_st, NULL, NULL);
        if (sc < best_score - 1e-4) {
          best_score = sc;
          best_st = cur_st;
          improved = 1;
        } else {
          cur_st.hue_rot[s] = best_st.hue_rot[s];
        }
      }

      // HUE_SAT
      float sat_orig = cur_st.hue_sat[s];
      float sat_steps[] = { -8.0f, -4.0f, -2.0f, -1.0f, 1.0f, 2.0f, 4.0f, 8.0f };
      for (int step = 0; step < 8; step++) {
        cur_st.hue_sat[s] = sat_orig + sat_steps[step];
        if (cur_st.hue_sat[s] < 30.0f) cur_st.hue_sat[s] = 30.0f;
        if (cur_st.hue_sat[s] > 180.0f) cur_st.hue_sat[s] = 180.0f;
        double sc = eval_stock(&cur_st, NULL, NULL);
        if (sc < best_score - 1e-4) {
          best_score = sc;
          best_st = cur_st;
          improved = 1;
        } else {
          cur_st.hue_sat[s] = best_st.hue_sat[s];
        }
      }

      // HUE_LUM
      float lum_orig = cur_st.hue_lum[s];
      float lum_steps[] = { -8.0f, -4.0f, -2.0f, -1.0f, 1.0f, 2.0f, 4.0f, 8.0f };
      for (int step = 0; step < 8; step++) {
        cur_st.hue_lum[s] = lum_orig + lum_steps[step];
        if (cur_st.hue_lum[s] < 40.0f) cur_st.hue_lum[s] = 40.0f;
        if (cur_st.hue_lum[s] > 160.0f) cur_st.hue_lum[s] = 160.0f;
        double sc = eval_stock(&cur_st, NULL, NULL);
        if (sc < best_score - 1e-4) {
          best_score = sc;
          best_st = cur_st;
          improved = 1;
        } else {
          cur_st.hue_lum[s] = best_st.hue_lum[s];
        }
      }
    }

    // 2. Fine-tune master curve interior knots
    for (k = 1; k < cur_st.master.n - 1; k++) {
      uint8_t orig_y = cur_st.master.y[k];
      int y_steps[] = { -3, -2, -1, 1, 2, 3 };
      for (int step = 0; step < 6; step++) {
        int ny = (int)orig_y + y_steps[step];
        if (ny > cur_st.master.y[k-1] && ny < cur_st.master.y[k+1]) {
          cur_st.master.y[k] = (uint8_t)ny;
          double sc = eval_stock(&cur_st, NULL, NULL);
          if (sc < best_score - 1e-4) {
            best_score = sc;
            best_st = cur_st;
            improved = 1;
          } else {
            cur_st.master.y[k] = best_st.master.y[k];
          }
        }
      }
    }

    // 3. Fine-tune blue curve knots
    for (k = 1; k < cur_st.b.n - 1; k++) {
      uint8_t orig_y = cur_st.b.y[k];
      int y_steps[] = { -3, -2, -1, 1, 2, 3 };
      for (int step = 0; step < 6; step++) {
        int ny = (int)orig_y + y_steps[step];
        if (ny > cur_st.b.y[k-1] && ny < cur_st.b.y[k+1]) {
          cur_st.b.y[k] = (uint8_t)ny;
          double sc = eval_stock(&cur_st, NULL, NULL);
          if (sc < best_score - 1e-4) {
            best_score = sc;
            best_st = cur_st;
            improved = 1;
          } else {
            cur_st.b.y[k] = best_st.b.y[k];
          }
        }
      }
    }

    // 4. Fine-tune green curve knots
    for (k = 1; k < cur_st.g.n - 1; k++) {
      uint8_t orig_y = cur_st.g.y[k];
      int y_steps[] = { -3, -2, -1, 1, 2, 3 };
      for (int step = 0; step < 6; step++) {
        int ny = (int)orig_y + y_steps[step];
        if (ny > cur_st.g.y[k-1] && ny < cur_st.g.y[k+1]) {
          cur_st.g.y[k] = (uint8_t)ny;
          double sc = eval_stock(&cur_st, NULL, NULL);
          if (sc < best_score - 1e-4) {
            best_score = sc;
            best_st = cur_st;
            improved = 1;
          } else {
            cur_st.g.y[k] = best_st.g.y[k];
          }
        }
      }
    }

    // 5. Fine-tune split toning
    for (int ch = 0; ch < 3; ch++) {
      float orig_sh = cur_st.split_sh[ch];
      float orig_mid = cur_st.split_mid[ch];
      float orig_hi = cur_st.split_hi[ch];
      float steps[] = { -2.0f, -1.0f, 1.0f, 2.0f };

      // shadow
      for (int step = 0; step < 4; step++) {
        cur_st.split_sh[ch] = orig_sh + steps[step];
        double sc = eval_stock(&cur_st, NULL, NULL);
        if (sc < best_score - 1e-4) {
          best_score = sc; best_st = cur_st; improved = 1;
        } else {
          cur_st.split_sh[ch] = best_st.split_sh[ch];
        }
      }
      // mid
      for (int step = 0; step < 4; step++) {
        cur_st.split_mid[ch] = orig_mid + steps[step];
        double sc = eval_stock(&cur_st, NULL, NULL);
        if (sc < best_score - 1e-4) {
          best_score = sc; best_st = cur_st; improved = 1;
        } else {
          cur_st.split_mid[ch] = best_st.split_mid[ch];
        }
      }
      // hi
      for (int step = 0; step < 4; step++) {
        cur_st.split_hi[ch] = orig_hi + steps[step];
        double sc = eval_stock(&cur_st, NULL, NULL);
        if (sc < best_score - 1e-4) {
          best_score = sc; best_st = cur_st; improved = 1;
        } else {
          cur_st.split_hi[ch] = best_st.split_hi[ch];
        }
      }
    }

    if (!improved) break;
  }

  printf("\n--- Final Fully Optimized Results (iter %d) ---\n", iter);
  eval_stock(&best_st, des, rgbs);
  double sum_chart_de = 0.0;
  for (int i = 0; i < N_TARGETS; i++) {
    if (i < 24) sum_chart_de += des[i];
    printf("%2d. %-18s Deh=(%3d,%3d,%3d) ML=(%3d,%3d,%3d) dE00=%.2f\n",
           i+1, patch_names[i], p160_deh[i][0], p160_deh[i][1], p160_deh[i][2],
           rgbs[i][0], rgbs[i][1], rgbs[i][2], des[i]);
  }
  printf("Target Chart Mean dE00: %.2f\n", sum_chart_de / 24.0);
  printf("Portrait Skin dE00: %.2f | Mountain Water dE00: %.2f | Foliage dE00: %.2f\n",
         des[24], des[25], des[26]);

  // Write new recipe to P160.fs
  FILE *out_f = fopen("../../films/P160.fs", "w");
  if (out_f) {
    fprintf(out_f, "# Kodak Portra 160: pale, fine grain, cool cyan shadows, radiant pastel skin.\n");
    fprintf(out_f, "#\n");
    fprintf(out_f, "# Calibrated against Dehancer Desktop Kodak Portra 160 reference renders.\n");
    fprintf(out_f, "# Balanced 48-sector non-banding matrix, cool shadow split toning.\n");
    fprintf(out_f, "VERSION   3\n");
    fprintf(out_f, "NAME      PORTRA 160\n");
    fprintf(out_f, "DESC      PALE, FINE GRAIN, PASTEL SKIN, COOL SHADOWS, 48-SEC\n");
    fprintf(out_f, "WB        AS_SHOT\n");
    
    // Master curve
    fprintf(out_f, "CURVE    ");
    for (int p = 0; p < best_st.master.n; p++) {
      fprintf(out_f, " %d,%d", best_st.master.x[p], best_st.master.y[p]);
    }
    fprintf(out_f, "\n");

    // R G B curves
    if (best_st.r.n > 0) {
      fprintf(out_f, "CURVE_R  ");
      for (int p = 0; p < best_st.r.n; p++) fprintf(out_f, " %d,%d", best_st.r.x[p], best_st.r.y[p]);
      fprintf(out_f, "\n");
    }
    if (best_st.g.n > 0) {
      fprintf(out_f, "CURVE_G  ");
      for (int p = 0; p < best_st.g.n; p++) fprintf(out_f, " %d,%d", best_st.g.x[p], best_st.g.y[p]);
      fprintf(out_f, "\n");
    }
    if (best_st.b.n > 0) {
      fprintf(out_f, "CURVE_B  ");
      for (int p = 0; p < best_st.b.n; p++) fprintf(out_f, " %d,%d", best_st.b.x[p], best_st.b.y[p]);
      fprintf(out_f, "\n");
    }

    fprintf(out_f, "SAT       %.0f\n", best_st.sat);

    // HUE_SAT (48 values)
    fprintf(out_f, "HUE_SAT   ");
    for (s = 0; s < SE_HUE_SECTORS; s++) {
      fprintf(out_f, "%s%.0f", (s > 0 ? "," : ""), best_st.hue_sat[s]);
    }
    fprintf(out_f, "\n");

    // HUE_ROT (48 values)
    fprintf(out_f, "HUE_ROT   ");
    for (s = 0; s < SE_HUE_SECTORS; s++) {
      fprintf(out_f, "%s%+.1f", (s > 0 ? "," : ""), best_st.hue_rot[s]);
    }
    fprintf(out_f, "\n");

    // HUE_LUM (48 values)
    fprintf(out_f, "HUE_LUM   ");
    for (s = 0; s < SE_HUE_SECTORS; s++) {
      fprintf(out_f, "%s%.0f", (s > 0 ? "," : ""), best_st.hue_lum[s]);
    }
    fprintf(out_f, "\n");

    // SPLIT
    fprintf(out_f, "SPLIT_SH  %.0f,%.0f,%.0f\n", best_st.split_sh[0], best_st.split_sh[1], best_st.split_sh[2]);
    fprintf(out_f, "SPLIT_MID %.0f,%.0f,%.0f\n", best_st.split_mid[0], best_st.split_mid[1], best_st.split_mid[2]);
    fprintf(out_f, "SPLIT_HI  %.0f,%.0f,%.0f\n", best_st.split_hi[0], best_st.split_hi[1], best_st.split_hi[2]);

    fprintf(out_f, "HALATION  %.0f %.0f %.0f\n", best_st.hal_amount, best_st.hal_thresh, best_st.hal_radius);
    fprintf(out_f, "HAL_TINT  %.0f,%.0f,%.0f\n", best_st.hal_tint[0], best_st.hal_tint[1], best_st.hal_tint[2]);
    fprintf(out_f, "GRAIN     %.0f\n", best_st.grain);
    fprintf(out_f, "GRAIN_SIZE %.0f\n", best_st.grain_size);

    fclose(out_f);
    printf("Successfully updated magiclantern/modules/film_sim/films/P160.fs\n");
  }

  return 0;
}
