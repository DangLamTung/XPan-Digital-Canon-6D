# Kodak Gold 200: the gold tint, and the blue underneath it.
#
# What everyone remembers about Gold is the warmth. What makes it Gold
# rather than just "warm" is that the warmth is in the HIGHLIGHTS while the
# shadows go blue -- the two ends pull in opposite directions, and the
# picture reads as golden hour because of the contrast between them, not
# because everything is yellow.
#
# That is split toning, and it is not something per-channel curves can
# express: the tint has to depend on how bright the pixel is, which needs
# all three channels at once. CURVE_B is deliberately gentler than it was:
# with SPLIT_HI also pulling blue out of the highlights, the old curve
# doubled up and the whole frame went sepia instead of golden. The old version warmed the whole frame with a
# red lift and a blue pull, and came out looking like a tungsten white
# balance error. SPLIT_SH and SPLIT_HI are doing the work now.
#
# The shoulder then had to come down. SPLIT_HI adds to R, so a CURVE_R that
# ended at 255 meant every highlight above ~236 landed on 255: on a bright
# frame 37.9% of the image was pure white with no separation left. Both tops
# now stop short of 255 by more than SPLIT_HI can add -- 240 and 232 with a
# +18 split -- so the last stop of highlight detail survives while the tint
# and the overall brightness stay where they were.
VERSION   3
NAME      GOLD
DESC      GOLD HIGHLIGHTS, DARK BLUE SHADOWS, WARM MIDS
WB        CLOUDY
CURVE     0,8 32,19 64,38 96,64 128,96 160,129 192,160 224,193 255,226
CURVE_R   0,8 32,33 64,63 96,93 128,123 160,154 192,187 224,219 255,255
CURVE_G   0,4 32,30 64,61 96,94 128,128 160,160 192,193 224,230 255,255
CURVE_B   0,8 32,33 64,67 96,99 128,131 160,162 192,198 224,233 255,255
SAT       110
HUE_SAT   109,109,110,109,108,106,103,98,93,89,88,90,91,92,90,86,80,76,74,75,77,80,83,86,89,91,94,98,102,106,110,113,115,115,113,110,107,104,103,102,103,104,104,105,106,107,107,108
HUE_ROT   +3.2,+2.6,+2.1,+1.5,+0.7,-0.1,-0.7,-1.0,-1.2,-1.4,-1.8,-2.4,-3.2,-4.1,-5.6,-7.6,-10.0,-11.8,-12.6,-12.4,-11.7,-11.0,-10.2,-9.3,-8.2,-7.0,-5.5,-3.5,-1.2,+1.1,+3.2,+5.1,+6.8,+7.7,+7.7,+6.9,+5.8,+4.7,+3.8,+3.1,+2.7,+2.5,+2.8,+3.4,+4.1,+4.5,+4.5,+4.0
HUE_LUM   102,102,102,102,102,102,102,102,99,99,99,99,100,100,100,100,86,86,86,86,88,88,88,88,91,91,91,91,84,84,84,84,78,78,78,78,90,90,90,90,103,103,103,103,104,104,104,104
SPLIT_SH  -2,0,3
SPLIT_HI  0,0,0
HALATION  50 226 16
HAL_TINT  255,84,30
GRAIN     24
GRAIN_SIZE 30
