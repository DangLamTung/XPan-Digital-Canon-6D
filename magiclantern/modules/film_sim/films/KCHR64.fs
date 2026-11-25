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
CURVE     0,1 32,37 64,68 96,99 128,133 160,167 192,197 224,225 255,251
CURVE_R   0,13 32,38 64,69 96,100 128,133 160,166 192,201 224,235 255,255
CURVE_G   0,1 32,26 64,57 96,89 128,122 160,155 192,188 224,220 255,251
CURVE_B   0,13 32,38 64,69 96,99 128,127 160,157 192,185 224,219 255,251
SAT       98
HUE_SAT   100,100,100,100,100,101,101,99,99,93,89,85,82,78,76,75,73,75,76,78,80,81,83,84,86,86,85,84,83,82,80,81,81,81,83,84,85,87,86,86,87,86,89,90,93,93,94,94
HUE_ROT   +9.1,+6.2,+2.6,-0.2,-1.1,-0.4,+0.6,+1.6,+2.7,-4.1,-4.8,-5.7,-6.7,-8.2,-10.2,-12.6,-15.3,-15.2,-15.1,-14.6,-14.1,-13.3,-12.3,-11.5,+3.5,+4.3,+4.8,+5.1,+5.2,+5.4,+5.4,+5.8,+6.3,+6.0,+5.7,+5.6,+3.7,+3.9,+4.2,+4.4,+4.6,+6.1,+7.4,+8.6,+10.1,+11.1,+12.3,+12.7
HUE_LUM   103,103,103,103,99,98,95,95,96,103,104,104,102,103,103,103,103,105,106,104,104,104,102,103,102,101,101,99,100,101,100,99,98,99,100,102,95,97,96,97,98,97,96,95,96,98,101,103
SPLIT_MID -1,0,2
SPLIT_HI  0,0,3
SPLIT_HI 0,0,3
HALATION  24 238 10
HAL_TINT  255,80,40
GRAIN     8
GRAIN_SIZE 10

