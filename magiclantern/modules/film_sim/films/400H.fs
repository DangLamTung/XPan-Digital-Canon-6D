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
CURVE   0,5 32,19 64,43 96,75 128,109 160,142 192,178 224,221 255,248
CURVE_R   0,45 32,46 64,64 96,95 128,127 160,159 192,183 224,222 255,227
CURVE_G   0,20 32,23 64,61 96,93 128,128 160,160 192,190 224,231 255,243
CURVE_B   0,21 32,32 64,65 96,96 128,128 160,161 192,189 224,237 255,244
SAT      110
HUE_SAT    96,90,84,95,106,104,102,100,98,99,100,98
HUE_ROT    -4,-5,-6,0,6,4,2,0,-2,-1,0,-2
HUE_LUM    102,103,104,99,94,96,98,97,96,99.5,103,102.5
SPLIT_SH  2,6,-2
SPLIT_HI  6,4,-2
HALATION  60 220 20
HAL_TINT  255,110,70
GRAIN     16
GRAIN_SIZE 30

