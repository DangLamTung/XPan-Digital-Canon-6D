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
CURVE     0,4 32,21 64,43 96,70 128,101 160,133 192,164 224,201 255,228
CURVE_R   0,2 32,31 64,62 96,94 128,124 160,156 192,187 224,216 255,255
CURVE_G   0,3 32,32 64,63 96,95 128,128 160,160 192,195 224,233 255,255
CURVE_B   0,7 32,34 64,67 96,98 128,128 160,159 192,193 224,232 255,255
SAT      112
HUE_SAT   101,103,106,108,108,106,103,99,93,88,86,86,86,87,86,84,81,79,79,80,83,86,89,92,96,99,102,103,106,109,112,114,116,116,114,110,106,102,99,98,97,96,96,95,95,95,96,98
HUE_ROT   -1.1,-0.9,-0.9,-1.2,-1.7,-2.3,-2.7,-3.0,-3.1,-3.0,-2.8,-2.3,-1.6,-0.9,-0.2,+0.4,+1.0,+1.4,+1.4,+1.0,+0.4,-0.2,-0.9,-1.8,-2.5,-3.0,-3.0,-2.3,-1.4,-0.5,+0.2,+0.8,+1.2,+1.3,+1.0,+0.4,-0.5,-1.3,-1.9,-2.3,-2.5,-2.7,-2.6,-2.2,-1.8,-1.4,-1.2,-1.2
HUE_LUM   102,102,102,102,102,102,102,102,100,100,100,100,100,100,100,100,94,94,94,94,96,96,96,96,99,99,99,99,101,101,101,101,99,99,99,99,103,103,103,103,104,104,104,104,104,104,104,104
SPLIT_SH  4,2,-1
SPLIT_HI  0,0,0
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
