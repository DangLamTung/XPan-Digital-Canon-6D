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
HUE_SAT    101,101,102,102,102,102,102,102,102,100,98,95,93,90,88,86,83,84,84,84,84,85,85,85,85,86,86,86,87,88,88,89,90,90,90,90,90,90,90,90,90,91,93,94,96,96,96,96
HUE_ROT    +4.0,+3.8,+3.5,+3.2,+3.0,+2.8,+2.5,+2.2,+2.0,-6.0,-8.0,-10.0,-12.0,-14.0,-16.0,-18.0,-20.0,-19.2,-18.5,-17.8,-17.0,-16.2,-15.5,-14.8,+0.0,+0.9,+1.8,+2.6,+3.5,+4.4,+5.2,+6.1,+7.0,+6.8,+6.5,+6.2,+4.0,+3.8,+3.5,+3.2,+3.0,+3.9,+4.8,+5.6,+6.5,+6.5,+6.5,+6.5
HUE_LUM    98,98,99,99,99,100,100,101,101,108,107,105,103,101,99,98,96,96,96,96,97,97,97,98,98,97,96,95,95,94,93,92,92,93,95,97,93,95,96,98,100,100,100,100,100,100,100,100
SPLIT_SH  -8,-2,10
SPLIT_HI 0,0,3
HALATION  24 238 10
HAL_TINT  255,80,40
GRAIN     8
GRAIN_SIZE 10

