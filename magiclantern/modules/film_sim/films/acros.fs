# ACROS -- black and white, and the reason SAT exists.
#
# This stock was impossible here until the look stage could do saturation.
# Desaturation is inherently cross-channel: you cannot reach grey with
# three independent curves, because none of them can see the other two.
# SAT 0 is the entire conversion.
#
# The per-channel curves still matter, and they are doing the job a
# coloured filter does on real monochrome film: they run BEFORE the
# desaturation, so pulling blue down and lifting red darkens a sky and
# opens skin, exactly like screwing a yellow filter onto the lens. That is
# also why this stock cannot work on the raw path -- there is no "the
# other two channels" before demosaic.
#
# Acros is famously fine-grained for a 100 speed film, with long, soft
# shadow detail under a hard shoulder.
VERSION   3
NAME      ACROS
DESC      BLACK AND WHITE, BLACK SHADOWS, HARD SHOULDER
WB        DAYLIGHT
CURVE     0,8 32,16 64,35 96,65 128,107 160,152 192,190 224,223 255,251
CURVE_R   0,10 32,33 64,63 96,93 128,123 160,158 192,191 224,223 255,250
CURVE_G   0,6 32,31 64,63 96,95 128,127 160,161 192,194 224,225 255,252
CURVE_B   0,1 32,30 64,71 96,102 128,126 160,157 192,189 224,217 255,244
SAT       0
HALATION  101 210 33
HAL_TINT  255,255,255
GRAIN      9
GRAIN_SIZE 12

SPLIT_SH 0,2,-6

SPLIT_MID 0,2,-6

SPLIT_HI 0,2,-6
