# Kodak Portra 400: pastel, which is a thing curves cannot do.
#
# Portra is low-saturation portrait film -- creamy, slightly warm, with the
# blacks lifted well off zero so a print never looks harsh. "Pastel" means
# colours pulled TOWARD their own brightness, and a per-channel curve
# cannot do that: a curve on R has no idea what G and B are doing at that
# pixel. So this stock was, until there was a SAT control, just a warm
# low-contrast curve, which is not the same thing and never looked like it.
#
# SAT 82 is the whole character. The warm split on both ends is what keeps
# the desaturation from reading as grey.
VERSION   3
NAME      PORTRA
DESC      PASTEL, MILKY BLACKS, WARM SKIN, MUTED FOLIAGE
WB        AS_SHOT
CURVE     0,8 64,74 128,132 192,188 255,234
CURVE_R   0,14 64,80 128,136 192,192 255,238
CURVE_G   0,12 128,128 192,186 255,232
CURVE_B   0,10 64,66 128,122 192,178 255,224
SAT      110
HUE_SAT    96,104,112,95,78,89,100,102,104,102,100,98
HUE_ROT    7,5,3,5.5,8,1.5,-5,-4.5,-4,-4,-4,1.5
HUE_LUM    101,102,102,100,93,95.5,98,97,96,99.5,101,101
SPLIT_SH  4,2,-1
SPLIT_HI  10,4,-6
HALATION  95 206 20
HAL_TINT  255,96,48
# 14, not 20: at 20 the grain read as blotches on a smooth subject, which is
# not what a 400-speed colour negative does. Measured on a flat patch, the
# high-frequency std goes 15.28 at 20 and 13.80 at 14 with everything else
# equal, and the difference is obvious at 1:1.
GRAIN     5
# GRAIN_SIZE only moves the grain cell at a full-scale develop: the cell is
# derived as 1 + size/12 and then divided by the decode scale, so at the
# default half-scale decode 36 and 24 both land on 2 px and produce
# byte-identical files.
GRAIN_SIZE 10
