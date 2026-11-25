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
CURVE     0,3 32,37 64,69 96,99 128,130 160,161 192,191 224,226 255,250
CURVE_R   0,1 32,29 64,60 96,93 128,121 160,154 192,186 224,216 255,255
CURVE_G   0,3 32,31 64,63 96,95 128,128 160,160 192,195 224,233 255,255
CURVE_B   0,7 32,34 64,66 96,97 128,127 160,156 192,191 224,230 255,254
SAT       106
HUE_SAT   83,87,92,97,100,100,99,96,91,85,82,81,81,82,81,80,79,76,77,79,82,87,90,93,97,99,102,104,107,110,112,114,116,117,115,110,105,101,97,96,95,93,91,89,88,84,81,80
HUE_ROT   -1.3,-2.5,-3.9,-5.4,-6.3,-7.0,-7.5,-8.3,-9.4,-10.3,-11.4,-12.3,-12.5,-12.6,-12.2,-12.1,-11.7,-11.3,-10.7,-9.9,-8.8,-7.6,-6.6,-6.3,-6.3,-6.6,-6.6,-6.1,-5.3,-4.5,-3.9,-3.1,-2.4,-1.9,-1.6,-1.5,-1.4,-1.1,-0.3,+0.9,+2.2,+3.4,+4.5,+5.1,+4.8,+3.7,+2.2,+0.3
HUE_LUM   108,105,103,100,101,99,98,98,95,94,94,94,94,96,97,99,95,96,96,97,97,97,94,94,97,97,97,98,101,102,103,104,102,102,102,102,104,104,104,104,106,106,105,105,107,108,110,110
SPLIT_SH  1,3,-2
SPLIT_HI  0,0,0
HALATION  95 200 36
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
