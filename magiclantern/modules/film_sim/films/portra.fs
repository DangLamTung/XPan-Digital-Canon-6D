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
CURVE   0,2 32,21 64,43 96,71 128,102 160,133 192,164 224,200 255,228
CURVE_R   0,12 32,30 64,62 96,93 128,124 160,155 192,201 224,202 255,216
CURVE_G   0,10 32,33 64,64 96,96 128,129 160,160 192,198 224,203 255,234
CURVE_B   0,17 32,34 64,70 96,99 128,129 160,158 192,192 224,218 255,219
SAT      112
HUE_SAT   107,110,113,115,115,113,110,105,99,94,92,91,92,93,91,89,86,84,84,85,88,91,95,98,102,105,108,110,113,116,119,121,123,123,121,117,113,108,105,104,103,102,102,101,101,101,102,104
HUE_ROT   +0.9,+1.1,+1.1,+0.8,+0.3,-0.3,-0.7,-1.0,-1.1,-1.0,-0.8,-0.3,+0.4,+1.1,+1.8,+2.4,+3.0,+3.4,+3.4,+3.0,+2.4,+1.8,+1.1,+0.2,-0.5,-1.0,-1.0,-0.3,+0.6,+1.5,+2.2,+2.8,+3.2,+3.3,+3.0,+2.4,+1.5,+0.7,+0.1,-0.3,-0.5,-0.7,-0.6,-0.2,+0.2,+0.6,+0.8,+0.8
HUE_LUM   102,102,102,102,102,102,102,102,100,100,100,100,100,100,100,100,94,94,94,94,96,96,96,96,102,102,102,102,104,104,104,104,102,102,102,102,100,100,100,100,101,101,101,101,101,101,101,101
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
