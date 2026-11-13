# Kodak Tri-X 400: contrasty b&w, deep blacks, visible grain.
#
# Tri-X at box speed: 48,26 puts the shadows down fast, 176,196 keeps a long
# straight middle, and the shoulder at 251 means the highlights still hold.
# All three per-channel curves are the identity because SAT 0 has already
# thrown the colour away -- a coloured curve here would only reintone the
# grey.
#
VERSION   3
NAME PLUS-X PAN 125
DESC Classic fine-grain black and white.
WB        AS_SHOT
CURVE     0,0 48,24 112,101 176,179 232,220 255,230
CURVE_R     0,0 128,128 255,255
CURVE_G   0,0 128,128 255,255
CURVE_B     0,0 128,128 255,255
SAT      0
HALATION  44 226 16
HAL_TINT  255,255,255
GRAIN     34
GRAIN_SIZE 24

