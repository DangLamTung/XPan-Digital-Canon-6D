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
HUE_SAT    81,99,89,70,74,73,138,121,70,69,68,74
HUE_ROT    4,-12,-22,-18,-13,22,-7,-18,-17,-19,0,-4
HUE_LUM    102,102,102,98,94,95,96,94,92,97,102,102
SPLIT_SH  -2,6,4
SPLIT_HI  4,4,0
HALATION  44 228 15
HAL_TINT  255,94,44
GRAIN     13
GRAIN_SIZE 24

