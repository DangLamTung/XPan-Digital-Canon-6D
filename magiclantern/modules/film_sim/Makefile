# define the module name - make sure name is max 8 characters
MODULE_NAME = film_sim

# The Soup Engine port (se/engine/, vendored verbatim - see se/README.md).
#
# Objects are kept flat in $(BUILD_DIR) with a "se_" prefix because the
# shared pattern rule writes its .d file next to the object and never
# creates subdirectories, and because a distinct prefix makes these rules
# win over the generic $(BUILD_DIR)/%.o: %.c (GNU make picks the shortest
# stem).
SE_ENGINE_SRC = se cr2 jpeg_dec jpeg_enc look stock color lut wb grade debayer pipeline

SE_OBJS = $(addprefix $(BUILD_DIR)/se_,$(addsuffix .o,$(SE_ENGINE_SRC))) \
          $(BUILD_DIR)/se_frame.o \
          $(BUILD_DIR)/se_se_cam.o \
          $(BUILD_DIR)/se_libgcc.o

GUI_OBJS = $(BUILD_DIR)/gui_fsg.o $(BUILD_DIR)/gui_fsg_dat.o \
           $(BUILD_DIR)/gui_fsg_ml.o $(BUILD_DIR)/gui_fsg_creator.o \
           $(BUILD_DIR)/gui_fsg_wb.o $(BUILD_DIR)/gui_fs_fuji.o

MODULE_OBJS = $(BUILD_DIR)/film_sim.o $(BUILD_DIR)/fs_yuvhook.o \
              $(BUILD_DIR)/fs_yuvlook.o $(SE_OBJS) $(GUI_OBJS)
TOP_DIR = ../..

include $(TOP_DIR)/modules/Makefile

# The engine is vendored so that it stays diffable against upstream. Magic
# Lantern builds with -fsingle-precision-constant and -Wdouble-promotion,
# which together flag every unsuffixed float literal in it; that is a
# soft-float performance hint, not a correctness bug, so it is silenced here
# rather than patched in a hundred places in code we do not own.
SE_ENGINE_CFLAGS = -Wno-double-promotion -Wno-inline

# ML builds modules with -Os. This one spends ~38 of its ~50 seconds per photo
# inside the JPEG decoder -- integer Huffman/IDCT byte-shuffling -- and -O2
# measured ~25% faster with byte-identical output (checked against a file the
# camera itself produced). MODULE_CFLAGS is appended after the -Os, so -O2 wins.
MODULE_CFLAGS = -O2

$(BUILD_DIR)/se_%.o: se/engine/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CDEPS_FLAGS) $(SE_ENGINE_CFLAGS) -c -o $@ $<

# The screens: one implementation of the drawing, two of the primitives.
$(BUILD_DIR)/gui_%.o: gui/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CDEPS_FLAGS) -c -o $@ $<

$(BUILD_DIR)/se_frame.o: se/frame.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CDEPS_FLAGS) -c -o $@ $<

$(BUILD_DIR)/se_se_cam.o: se/se_cam.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CDEPS_FLAGS) -c -o $@ $<

$(BUILD_DIR)/se_libgcc.o: se/se_libgcc.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CDEPS_FLAGS) -c -o $@ $<

# The shot-time YUV probe and look need the patch API, which the engine does not.
$(BUILD_DIR)/fs_yuvhook.o: fs_yuvhook.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CDEPS_FLAGS) -c -o $@ $<

$(BUILD_DIR)/fs_yuvlook.o: fs_yuvlook.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CDEPS_FLAGS) $(SE_ENGINE_CFLAGS) -c -o $@ $<

.DEFAULT_GOAL := $(BUILD_DIR)/module_complete
