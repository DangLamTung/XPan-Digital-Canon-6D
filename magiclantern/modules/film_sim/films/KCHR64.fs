# Kodak Kodachrome 64: deep reds, dark blue shadows, rich mids.
#
# Kodachrome's look is mostly its cyan dye: blue is pulled hard (B curve
# 64,56 and 128,118) while red sits 6-8 levels high through the middle.  That
# is the "deep reds, dark blue shadows" everyone describes, and it is a pair of
# curves, not a global temperature.  ROT +8 on red toward orange keeps skin
# from going pink against all that red.
#
VERSION   3
NAME      KODACHROME 64
DESC      DEEP REDS, DARK BLUE SHADOWS, RICH MIDS
WB        DAYLIGHT
CURVE     0,5 32,15 64,35 96,63 128,101 160,142 192,181 224,217 255,251
CURVE_R   0,13 32,38 64,70 96,102 128,134 160,167 192,200 224,234 255,254
CURVE_G   0,1 32,26 64,57 96,89 128,123 160,156 192,189 224,221 255,251
CURVE_B   0,13 32,38 64,69 96,99 128,127 160,157 192,186 224,219 255,250
SAT       112
HUE_SAT    114,115,116,105,94,95,96,99,102,102,102,108
HUE_ROT    8,7,6,-2,-10,-7,-4,-0.5,3,2,1,4.5
HUE_LUM    101,102.5,104,97,90,91,92,89,86,93,100,100.5
SPLIT_SH  -8,-2,10
SPLIT_HI  10,6,-10
HALATION  24 238 10
HAL_TINT  255,80,40
GRAIN     8
GRAIN_SIZE 10

