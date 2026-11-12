# Builds se/se_cam.c plus the Soup Engine for a desktop, so the camera
# develop path can be run and checked against the engine's own reference.
#
#   make            build ./se_cam_test
#   make check CR2=photo.CR2 STOCK=velvia.fs REF=reference.jpg
#
# "check" compares the bytes the camera glue produces with a reference
# render; they must match exactly, because the camera must produce the same
# image the desktop does.

SE_DIR   = ..
ENGINE   = $(SE_DIR)/engine
ENG_SRC  = $(wildcard $(ENGINE)/*.c)
SRC      = main.c stubs.c $(SE_DIR)/se_cam.c $(SE_DIR)/frame.c $(SE_DIR)/se_libgcc.c $(ENG_SRC)

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -DSE_HOSTED
# -I. comes first so the stub ML headers win over anything else
CPPFLAGS += -I. -I$(SE_DIR)/..
LDLIBS  += -lm

BIN = se_cam_test

# lookmatch answers a different question: given the same frame and the same
# stock, does the LiveView path (RGB -> UYVY 4:2:2 -> look -> RGB) produce the
# same pixels as the develop path (RGB -> look)?
ENG_SRC  = $(wildcard $(ENGINE)/*.c)
LOOK_SRC = lookmatch.c $(SE_DIR)/se_libgcc.c $(SE_DIR)/../fs_yuvlook.c $(ENG_SRC)

all: $(BIN) lookmatch grade_jpeg rawdev lvgrade tabledump

# The develop colour path applied to a plain JPEG, for evaluating recipes on
# real photographs when no raw file is at hand.
rawdev: rawdev.c $(SE_DIR)/se_cam.c $(SE_DIR)/frame.c $(SE_DIR)/se_libgcc.c $(ENG_SRC) stubs.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

# lvgrade: the shot-time look applied to a LiveView buffer the camera dumped.
# tabledump: the tables a recipe bakes into, as text, for plotting.
tabledump: tabledump.c $(SE_DIR)/se_libgcc.c $(SE_DIR)/../fs_yuvlook.c $(ENG_SRC) stubs.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

lvgrade: lvgrade.c $(SE_DIR)/se_libgcc.c $(SE_DIR)/../fs_yuvlook.c $(ENG_SRC) stubs.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

# tableerr: how far the shot-time tables fall from the full grade, by luma
# and by chroma. Answers "what does the separable form lose" with numbers.
# lutchk: does an exported cube still hold the recipe it came from?
lutchk: lutchk.c $(SE_DIR)/se_libgcc.c $(ENG_SRC) stubs.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

# fs2cube: a .fs exported as a 3D .cube, for handing a look to anything else.
fs2cube: fs2cube.c $(SE_DIR)/se_libgcc.c $(ENG_SRC) stubs.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

# springtest: the shelf's spring, at the loop's real rate. No engine needed.
springtest: springtest.c
	$(CC) $(CFLAGS) -o $@ $^

# bench_grade: the preview grade and the shot grade, on one LiveView frame.
bench_grade: bench_grade.c $(SE_DIR)/se_libgcc.c $(SE_DIR)/../fs_yuvlook.c $(ENG_SRC) stubs.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(SE_DIR)/.. -o $@ $^ $(LDLIBS)

# the drawing half needs a backend for fsg_fill/fsg_text; the desktop one
# goes here so the writer test links without a panel
fujiwrite: fujiwrite.c ../../gui/fs_fuji.c $(SE_DIR)/se_libgcc.c $(ENG_SRC) stubs.c
	$(CC) -I../../.. $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

creatorwrite: creatorwrite.c ../../gui/fsg_creator.c ../../gui/fsg_host.c \
              $(SE_DIR)/se_libgcc.c $(ENG_SRC) stubs.c
	$(CC) -I../../.. $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

# The same test, but every snprintf goes through a stand-in for the camera's
# formatter: no precision, no '-' flag, both printed literally. macOS printf is
# correct, so the plain build above passes while the camera writes ".8s.fs" or
# "-9s 80,100,...". This build is the one that would have caught all three.
creatorwrite_ml: creatorwrite.c ../../gui/fsg_creator.c ../../gui/fsg_host.c \
                 $(SE_DIR)/se_libgcc.c $(ENG_SRC) stubs.c mlprintf.c
	$(CC) -I../../.. -D_FORTIFY_SOURCE=0 \
	      $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

lv3d: lv3d.c $(SE_DIR)/se_libgcc.c $(SE_DIR)/../fs_yuvlook.c $(ENG_SRC) stubs.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

tableerr: tableerr.c $(SE_DIR)/se_libgcc.c $(SE_DIR)/../fs_yuvlook.c $(ENG_SRC) stubs.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

# rampchk needs the engine like every other test. Without this rule make falls
# back to its built-in "cc rampchk.c -lm", which links nothing and fails on the
# first se_stock_* symbol.
rampchk: rampchk.c $(SE_DIR)/se_libgcc.c $(ENG_SRC) stubs.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

grade_jpeg: grade_jpeg.c $(SE_DIR)/se_cam.c $(SE_DIR)/frame.c $(SE_DIR)/se_libgcc.c $(ENG_SRC) stubs.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRC) $(LDLIBS)

lookmatch: $(LOOK_SRC)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(LOOK_SRC) $(LDLIBS)

CR2   ?=
STOCK ?=
REF   ?=
OUT   ?= /tmp/se_cam_test.jpg

check: $(BIN)
	@test -n "$(CR2)"   || { echo "set CR2=file.CR2" >&2; exit 2; }
	@test -n "$(STOCK)" || { echo "set STOCK=stock.fs" >&2; exit 2; }
	./$(BIN) "$(CR2)" "$(STOCK)" "$(OUT)"
	@if [ -n "$(REF)" ]; then \
	    cmp "$(OUT)" "$(REF)" && echo "PASS: identical to $(REF)"; \
	else \
	    echo "wrote $(OUT)"; \
	fi

match: lookmatch
	@test -n "$(CR2)"   || { echo "set CR2=file.CR2" >&2; exit 2; }
	@test -n "$(STOCK)" || { echo "set STOCK=stock.fs" >&2; exit 2; }
	./lookmatch "$(CR2)" "$(STOCK)"

clean:
	rm -f $(BIN) lookmatch rawdev grade_jpeg lvgrade tabledump tableerr bench_grade lv3d creatorwrite fujiwrite springtest fs2cube lutchk

.PHONY: all check match clean

# se_cam_keep_yuv(): does the copy taken inside the hook encode identically to
# the frame it was copied from?
keeptest: keeptest.c $(SE_DIR)/se_cam.c $(SE_DIR)/frame.c $(SE_DIR)/se_libgcc.c $(ENG_SRC) stubs.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(SE_DIR) -o $@ $^ $(LDLIBS)

# se_cam_save_yuv(): UYVY frame -> JPEG file, the shot path's exit
yuvsave_test: yuvsave_test.c $(SE_DIR)/se_cam.c $(SE_DIR)/frame.c $(SE_DIR)/se_libgcc.c $(ENG_SRC) stubs.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(SE_DIR) -o $@ $^ $(LDLIBS)

# The whole shot-path exit, end to end: bake -> apply -> save
savechain: savechain.c $(SE_DIR)/se_cam.c $(SE_DIR)/frame.c $(SE_DIR)/se_libgcc.c $(SE_DIR)/../fs_yuvlook.c $(ENG_SRC) stubs.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(SE_DIR) -o $@ $^ $(LDLIBS)
