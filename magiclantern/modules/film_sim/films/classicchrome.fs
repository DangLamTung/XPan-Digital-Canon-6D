# CLASSIC CHROME -- the one that made people care about film simulations.
#
# It is a documentary look: muted, a little cold, with real tonal contrast
# underneath. The mistake is to read "muted" as "turn the saturation down",
# which gives you a flat grey picture. What Classic Chrome actually does is
# selective -- it drains the greens and yellows almost to nothing while
# letting reds keep their weight, so a red coat in a dull street still
# reads as red and the street stays dull.
#
# That is HUE_SAT, and it is the whole stock. A global SAT cannot say it
# (it moves every hue together) and per-channel curves cannot either (a
# curve on R has no idea what G and B are doing at that pixel). Greens at
# 52 against reds at 108 is a ratio of better than two to one.
VERSION   3
NAME      CLASSIC CHROME
DESC      OLIVE GREENS, CYAN BLUES, DENSE MUTED MIDS
WB        DAYLIGHT
CURVE     0,0 32,26 64,61 96,97 128,132 160,165 192,197 224,228 255,255
CURVE_R   0,3 32,33 64,63 96,95 128,128 160,161 192,195 224,231 255,255
CURVE_G   0,0 32,32 64,64 96,96 128,128 160,161 192,193 224,224 255,255
CURVE_B   0,0 32,32 64,65 96,96 128,127 160,160 192,192 224,224 255,255
SAT       79
HUE_SAT   107,99,92,94,119,100,104,119,89,91,93,99
HUE_ROT   +1.6,-4.1,-2.2,-1.9,+16.6,+3.0,-1.7,-7.2,-10.4,-4.6,+6.4,+4.1
HUE_LUM   100,100,101,100,91,102,100,99,98,99,100,99
SPLIT_SH  0,2,0
SPLIT_MID 0,0,1
SPLIT_HI  1,2,0
HALATION  36 232 13
HAL_TINT  255,80,44
GRAIN     16
GRAIN_SIZE 20
