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
HUE_SAT    107,107,108,108,108,108,109,109,109,106,104,101,99,96,94,91,88,89,89,89,89,90,90,90,90,91,92,92,93,94,94,95,96,96,96,96,96,96,96,96,96,97,99,100,102,102,102,102
HUE_ROT    +6.0,+5.8,+5.5,+5.2,+5.0,+4.8,+4.5,+4.2,+4.0,-4.0,-6.0,-8.0,-10.0,-12.0,-14.0,-16.0,-18.0,-17.2,-16.5,-15.8,-15.0,-14.2,-13.5,-12.8,-2.0,-1.1,-0.2,+0.6,+1.5,+2.4,+3.2,+4.1,+5.0,+4.8,+4.5,+4.2,+4.0,+3.8,+3.5,+3.2,+3.0,+3.9,+4.8,+5.6,+6.5,+6.5,+6.5,+6.5
HUE_LUM    98,98,99,99,99,100,100,101,101,105,104,102,100,98,96,95,93,93,93,93,94,94,94,95,95,94,93,92,92,91,90,89,89,90,92,94,93,95,96,98,100,100,100,100,100,100,100,100
SPLIT_SH  -8,-2,10
SPLIT_HI  10,6,-10
HALATION  24 238 10
HAL_TINT  255,80,40
GRAIN     8
GRAIN_SIZE 10

