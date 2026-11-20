# Kodak Portra 800: pastel, warm, more contrast and grain than 400.
#
# Portra 800 keeps the pastel character but the speed costs grain and buys
# contrast: the shoulder ends at 236, the mid point rides two levels higher
# than portra.fs, and GRAIN 26 at size 34 is roughly double.  The halation is
# stronger too -- a fast emulsion blooms more around a specular highlight, and
# that bloom is a visible part of what 800 looks like.
#
VERSION   3
NAME      PORTRA 800
DESC      PASTEL, WARM, MORE CONTRAST AND GRAIN THAN 400
WB        AS_SHOT
CURVE     0,0 32,23 64,46 96,75 128,108 160,141 192,175 224,216 255,246
CURVE_R   0,14 32,18 64,57 96,91 128,126 160,160 192,190 224,232 255,238
CURVE_G   0,19 32,28 64,64 96,96 128,127 160,159 192,186 224,227 255,241
CURVE_B   0,24 32,33 64,67 96,97 128,127 160,159 192,183 224,241 255,250
SAT       95
HUE_SAT   99,98,91,98,86,91,99,100,100,102,100,97
HUE_ROT   -2.3,-3.7,-5.8,-12.0,-13.5,-6.2,-1.5,-0.7,-0.1,-1.7,-6.8,-5.9
HUE_LUM   106,96,94,96,93,97,99,99,99,100,101,102
SPLIT_SH  10,5,-2
SPLIT_HI  4,4,-1
HALATION  104 209 32
HAL_TINT  255,98,48
GRAIN     26
GRAIN_SIZE 34

