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
CURVE     0,0 32,29 64,63 96,95 128,127 160,160 192,192 224,224 255,255
CURVE_R   0,0 32,30 64,62 96,95 128,128 160,160 192,192 224,224 255,255
CURVE_G   0,0 32,29 64,63 96,95 128,128 160,160 192,192 224,224 255,255
CURVE_B   0,0 32,32 64,64 96,96 128,128 160,160 192,192 224,224 255,255
SAT       100
HUE_SAT   100,100,100,100,100,100,100,100,100,100,100,100
HUE_ROT   -0.1,-0.1,-0.1,-0.1,-0.1,-0.1,-0.1,-0.1,-0.1,-0.1,-0.1,-0.1
HUE_LUM   100,100,100,100,100,100,100,100,100,100,100,100
SPLIT_SH  -2,2,-3
SPLIT_MID 1,1,1
SPLIT_HI  0,0,0
HALATION  36 232 13
HAL_TINT  255,80,44
GRAIN     16
GRAIN_SIZE 20
