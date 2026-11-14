# Fujifilm C200: clean, slightly green-neutral, mild contrast.
#
# C200 is superia400.fs with the volume turned down: the same green direction
# in the shadows, half the saturation push, and a cleaner black.  It is the
# film people buy when they want plain colour, so the recipe is deliberately
# the least opinionated in the set.
#
VERSION   3
NAME      C200
DESC      CLEAN, SLIGHTLY GREEN-NEUTRAL, MILD CONTRAST
WB        AS_SHOT
CURVE     0,1 64,15 128,92 192,175 255,249
CURVE_R     0,10 128,137 255,244
CURVE_G   0,10 128,132 255,244
CURVE_B     0,9 64,68 128,131 192,196 255,250
SAT      88
SPLIT_MID 0,0,5
HUE_SAT    81,98,94,70,71,73,130,126,70,69,68,72
HUE_ROT    4,-9,-22,-18,-15,24,-4,-17,-17,-19,0,-4
HUE_LUM    104,98,100,98,94,93,100,94,92,97,104,122
SPLIT_SH -4,6,6
SPLIT_HI 6,4,-2
HALATION  44 228 15
HAL_TINT  255,94,44
GRAIN     13
GRAIN_SIZE 24

