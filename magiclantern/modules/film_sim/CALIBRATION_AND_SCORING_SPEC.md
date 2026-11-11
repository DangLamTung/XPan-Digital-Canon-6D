# CineStill 50D & Film Simulation Calibration Specification
## Objective CIEDE2000 Metric & Direct-in-the-Loop C Engine Optimization

---

## 1. Executive Summary & Objective

The objective of this calibration system is to bring the embedded Magic Lantern C film simulation engine (`film_sim` SE engine) to exact perceptual and photometric parity with Dehancer Desktop reference profiles (specifically CineStill 50D).

Previous naive RGB Euclidean distance ($L_2$) or simple channel-difference metrics fail because:
1. **Human Perceptual Non-uniformity:** The human eye is vastly more sensitive to small hue shifts in skin dermis and cyan-blue tones than to identical Euclidean deviations in deep shadows or saturated primaries.
2. **Skin Tone Sunburn / Oversaturation:** Standard RGB losses fail to penalize excessive saturation when luminance is slightly lower, resulting in unnatural ruddy/orange skin.
3. **Cyan-Blue Drift:** The transition between daylight sky and oceanic cyan is prone to magenta or turquoise drift unless constrained by hue angle rotation in perceptual space.
4. **Foliage Green Balance:** Cine green must balance $G > R$ without turning into radioactive neon yellow-green or muddy olive-brown.
5. **Specular Highlight Rolloff & Daylight Tint:** CineStill 50D features a characteristic gentle highlight rolloff with a subtle daylight blue bias ($B > R$ above $Y > 200$) and soft halation on specular transitions.

To solve this, our calibration pipeline operates **directly on the compiled C engine binary** (`magiclantern/modules/film_sim/se/hosttest/grade_jpeg`) using **CIEDE2000 ($\Delta E_{00}$)** over segmented spatial color domains.

---

## 2. Loss Metric Formulation: CIEDE2000 ($\Delta E_{00}$)

We employ the ISO/CIE 11664-6:2014 (CIEDE2000) standard. Unlike CIE76 ($\Delta E^*_{ab}$) or CIE94, CIEDE2000 corrects for:
- Non-uniformity of the CIELAB color space.
- The blue-region rotation factor ($R_T$) addressing the non-linear ellipse tilt around $h \approx 275^\circ$.
- Chroma-dependent weighting functions ($S_C, S_H$).
- Neutral lightness-dependent compensation ($S_L$).

### 2.1 Mathematical Formulation

Given reference pixel $L^*_1, a^*_1, b^*_1$ (Dehancer ground truth) and engine graded pixel $L^*_2, a^*_2, b^*_2$:

1. Compute modified $a'$ values:
   $$\bar{C} = \frac{C^*_1 + C^*_2}{2}$$
   $$G = \frac{1}{2} \left(1 - \sqrt{\frac{\bar{C}^7}{\bar{C}^7 + 25^7}}\right)$$
   $$a'_i = (1 + G) a^*_i, \quad C'_i = \sqrt{{a'_i}^2 + {b^*_i}^2}, \quad h'_i = \text{atan2}(b^*_i, a'_i) \pmod{360^\circ}$$

2. Compute metric differences $\Delta L'$, $\Delta C'$, $\Delta H'$:
   $$\Delta L' = L^*_2 - L^*_1, \quad \Delta C' = C'_2 - C'_1$$
   $$\Delta H' = 2 \sqrt{C'_1 C'_2} \sin\left(\frac{\Delta h'}{2}\right)$$

3. Apply weighting factors and cross-term rotation:
   $$\Delta E_{00} = \sqrt{\left(\frac{\Delta L'}{k_L S_L}\right)^2 + \left(\frac{\Delta C'}{k_C S_C}\right)^2 + \left(\frac{\Delta H'}{k_H S_H}\right)^2 + R_T \left(\frac{\Delta C'}{k_C S_C}\right)\left(\frac{\Delta H'}{k_H S_H}\right)}$$

Where $k_L = k_C = k_H = 1.0$ for standard photographic reproduction evaluation.

---

## 3. Segmented Color Evaluation Domains

Evaluation is performed across dedicated physical photographic sub-domains extracted from canonical 35mm benchmark frames:

| Domain | Key Frames | Physical Subject | Critical Visual Target | Weight ($w_i$) |
| :--- | :--- | :--- | :--- | :--- |
| **Skin Dermis (Midtones)** | `001_IMG_6883`, `015_IMG_6924` | Natural human face & neck skin | Peach dermis, $\Delta E_{00} \le 1.8$, eliminate sunburn ($R-G \approx 8..28$) | 3.5 |
| **Lips & Hair Boundary** | `015_IMG_6924` | Vermilion border & dark melanin | Preserved lip chroma without bleeding into dermis | 1.5 |
| **Cyan / Coastal Water** | `018_IMG_6938` | Ocean surface & sea waves | Deep aquatic cyan ($B > G > R$, Sectors 23..26 rotation) | 3.0 |
| **Foliage Greens** | `008_IMG_6896` | Natural trees, grass, leaves | Organic cine green ($G > R + 15$, Sectors 15..18) | 2.5 |
| **Specular Highlights** | `001`, `018`, Chart P19 | Sky highlights & specular glints | Daylight cyan-blue bias ($B > R$ at $Y > 200$), un-choked rolloff | 2.5 |
| **Grayscale Tone Curve** | ColorChecker Row 4 (19-24) | 6-step neutral grayscale ladder | Perfect luma slope, strict neutral gray ($\|R - B\| \le 1.0$) | 2.0 |

### 3.1 Composite Loss Function

$$\mathcal{L}_{\text{total}} = \sum_{k \in \text{Domains}} w_k \cdot \overline{\Delta E_{00}^{(k)}} + \lambda_{\text{gray}} \cdot \mathcal{P}_{\text{gray\_balance}} + \lambda_{\text{hi}} \cdot \mathcal{P}_{\text{hi\_tint}}$$

Where:
- $\overline{\Delta E_{00}^{(k)}}$ is the spatial mean CIEDE2000 over domain mask $M_k$.
- $\mathcal{P}_{\text{gray\_balance}} = \frac{1}{N_{\text{gray}}} \sum_{p \in \text{Row 4}} (R_p - B_p)^2$ enforces strict neutral balance in grays.
- $\mathcal{P}_{\text{hi\_tint}} = \max(0, (B - R)_{\text{target}} - (B - R)_{\text{actual}})^2$ ensures highlight cyan-blue daylight tint is maintained.

---

## 4. Parameter Space in Film Simulation Engine

The C engine (`look.c`) exposes the following tuneable parameter axes:

1. **Hue-Specific Color Matrix (48 Sectors):**
   - Hexagonal hue angle mapping: $h \in [0, 1536)$, step size = 32 units ($7.5^\circ$).
   - `HUE_ROT[48]`: Hue angle rotation $\pm 30.0^\circ$.
   - `HUE_SAT[48]`: Per-sector saturation scaling $[50.0\%, 200.0\%]$.
   - `HUE_LUM[48]`: Per-sector luminance scaling $[70.0\%, 130.0\%]$.
2. **Channel Tone Curves:**
   - 9 control knots per channel at input nodes $x \in \{0, 32, 64, 96, 128, 160, 192, 224, 255\}$.
   - `CURVE_R`, `CURVE_G`, `CURVE_B` control shoulder rolloff, highlight daylight tint, and toe contrast.
3. **Split Toning:**
   - `SPLIT_SH`: Shadow $R, G, B$ bias offset.
   - `SPLIT_MID`: Midtone $R, G, B$ neutrality anchor ($0, 0, 0$).
   - `SPLIT_HI`: Highlight $R, G, B$ daylight tint offset.

---

## 5. Direct-in-the-Loop C Execution Architecture

```
+-------------------------------------------------------------------+
|                        Optimizer (Python)                         |
|   1. Proposes parameter vector theta                              |
|   2. Formats temporary recipe: /tmp/eval_50d.fs                   |
+---------------------------------+---------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|             Host C Engine: grade_jpeg (15ms execution)            |
|   Inputs:                                                         |
|     - /tmp/eval_canvas.jpg (Composite source patches)             |
|     - /tmp/eval_50d.fs (Proposed candidate recipe)                |
|   Output:                                                         |
|     - /tmp/eval_out.jpg (True hardware-equivalent grade)          |
+---------------------------------+---------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|                 CIEDE2000 Evaluator (ISO 11664-6)                 |
|   1. Converts /tmp/eval_out.jpg & Ground Truth to CIE L*a*b*      |
|   2. Calculates domain-masked delta E00:                          |
|      - Skin midtones, lips, cyan water, cine greens, grays        |
|   3. Computes weighted scalar loss J(theta)                       |
+---------------------------------+---------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|                   Nelder-Mead / Powell Simplex                    |
|   Updates theta -> Repeats until delta E00 <= target threshold    |
+-------------------------------------------------------------------+
```

---

## 6. Verification and Acceptance Criteria

A recipe is certified for production deployment when:
1. **Skin Mean $\Delta E_{00} \le 2.0$** across both fair and olive portraits (`001` and `015`).
2. **Cyan Water $\Delta E_{00} \le 2.5$** on coastal marine frames (`018`).
3. **Green Foliage $\Delta E_{00} \le 2.5$** on landscape foliage (`008`).
4. **Grayscale Row 4 Neutrality:** $\|R - B\| \le 1.0$ across all mid-to-high neutral patches.
5. **No Specular Highlight Flattening:** Specular peak reaches $Y \ge 245$ without flat-topping or unnatural yellow shifts.

