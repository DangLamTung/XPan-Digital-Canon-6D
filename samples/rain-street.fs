# CLASSIC CHROME -- the one that made people care about film simulations.
#
# It is a documentary look: muted, a little cold, with real tonal contrast
# underneath. The mistake is to read "muted" as "turn the saturation down",
# which gives you a flat grey picture. What Classic Chrome actually does is
# selective -- it drains the greens and yellows almost to nothing while
# letting reds keep their weight, so a red coat in a dull street still
# reads as red and the street stays dull.
#
# That is HUE_SAT, and it is the whole stock. A global SAT cannot say it
# (it moves every hue together) and per-channel curves cannot either (a
# curve on R has no idea what G and B are doing at that pixel). Greens at
# 52 against reds at 108 is a ratio of better than two to one.
VERSION   3
NAME      RAIN STREET
DESC      WARM OLIVE, DESATURATED, CREAM HIGHLIGHTS
WB        DAYLIGHT
CURVE     0,4 32,22 96,88 160,181 224,240 255,250
CURVE_R   0,5 64,66 128,132 192,199 255,251
CURVE_G   0,6 128,127 255,245
CURVE_B   0,9 64,62 128,118 192,178 255,232
SAT       80
HUE_SAT    112,83,54,48,42,57,72,79,86,93,100,106
HUE_ROT    7,-2.5,-12,-21,-30,-27,-24,-21,-18,-11.5,-5,1
HUE_LUM    103,96.5,90,84,78,83,88,87,86,92,98,100.5
SPLIT_SH  0,-1,3
SPLIT_HI  12,5,-14
HALATION  36 232 13
HAL_TINT  255,80,44
GRAIN     16
GRAIN_SIZE 20
