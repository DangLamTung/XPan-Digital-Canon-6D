# Fujifilm Pro 400H: pastel, minty greens, light cyan, soft highlights.
#
# 400H's party trick is that it barely clips: the shoulder sits at 234 on a
# curve that is flat across the top, so a wedding dress keeps its folds.  The
# mint cast is not a global shift -- HUE_SAT 84 on the yellow-green sector with
# -6 degrees of rotation -- and the blue curve starts at 16, which is where the
# pale cyan in the shadows comes from.
#
VERSION   3
NAME      PRO 400H
DESC      PASTEL, MINTY GREENS, LIGHT CYAN, SOFT HIGHLIGHTS
WB        AS_SHOT
CURVE     0,6 32,19 64,43 96,74 128,108 160,141 192,177 224,218 255,248
CURVE_R   0,2 32,31 64,63 96,96 128,127 160,158 192,189 224,225 255,238
CURVE_G   0,6 32,31 64,62 96,94 128,126 160,158 192,192 224,231 255,252
CURVE_B   0,9 32,33 64,66 96,97 128,128 160,160 192,195 224,236 255,253
SAT      110
HUE_SAT    96,90,84,95,106,104,102,100,98,99,100,98
HUE_ROT    -4,-5,-6,0,6,4,2,0,-2,-1,0,-2
HUE_LUM    102,103,104,99,94,96,98,97,96,99.5,103,102.5
SPLIT_SH  2,6,-2
SPLIT_HI  6,4,-2
HALATION  120 205 38
HAL_TINT  255,110,70
GRAIN     16
GRAIN_SIZE 30

