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
CURVE     0,8 32,34 64,65 96,97 128,132 160,164 192,193 224,222 255,250
CURVE_R   0,8 32,33 64,62 96,91 128,122 160,154 192,187 224,219 255,255
CURVE_G   0,4 32,31 64,62 96,95 128,128 160,160 192,193 224,229 255,255
CURVE_B   0,7 32,32 64,66 96,98 128,129 160,161 192,197 224,233 255,255
SAT       109
HUE_SAT   89,93,97,100,101,101,98,94,90,88,90,93,95,95,93,88,81,74,72,71,74,77,81,83,86,88,90,93,97,101,103,105,106,106,103,99,96,92,90,89,89,89,90,91,92,91,88,90
HUE_ROT   -1.4,-2.3,-3.1,-4.0,-5.4,-7.0,-8.7,-10.0,-11.3,-12.9,-14.8,-16.5,-17.8,-18.2,-18.2,-18.0,-18.0,-17.6,-16.7,-15.1,-13.2,-11.5,-9.8,-8.3,-6.9,-5.8,-4.8,-3.8,-2.7,-1.9,-0.9,+0.2,+1.4,+2.2,+2.5,+2.3,+2.0,+1.6,+1.5,+1.3,+1.5,+1.7,+2.1,+2.6,+2.7,+2.3,+1.3,-0.2
HUE_LUM   109,108,105,103,101,98,97,96,93,95,97,100,101,102,104,106,94,96,97,97,99,100,101,102,105,106,107,107,100,99,98,98,90,89,88,86,98,96,96,95,107,107,107,107,108,109,113,114
SPLIT_SH  -4,0,1
SPLIT_HI  -4,-1,-1
HALATION  100 211 30
HAL_TINT  255,84,30
GRAIN     24
GRAIN_SIZE 30
