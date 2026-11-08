CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra

all: filmsim_sim devsim look_host

filmsim_sim: filmsim_sim.c ../../fs_core.h
	$(CC) $(CFLAGS) -o $@ filmsim_sim.c

# raw develop harness: same core, but it starts from Bayer data
devsim: devsim.c ../../fs_core.h
	$(CC) $(CFLAGS) -o $@ devsim.c -lm

# look-only harness: a real photograph in, the same core the camera runs, a
# photograph out. This is the form that can be compared against another grading
# engine on the same pixels, with no synthetic Bayer round trip in the way.
look_host: look_host.c ../../fs_core.h
	$(CC) $(CFLAGS) -o $@ look_host.c -lm

run: filmsim_sim
	mkdir -p out
	./filmsim_sim out
	python3 ppm2png.py out

# raw develop path on a real photograph: PPM in, one TIFF per look out
devrun: devsim
	mkdir -p dev_out
	./devsim input.ppm dev_out

# one look on a real photograph: PPM in, PPM out
#   make lookrun IN=photo.ppm PRESET=10
lookrun: look_host
	./look_host $(IN) look_$(PRESET).ppm $(PRESET) 100 20

clean:
	rm -rf filmsim_sim devsim look_host out dev_out *.ppm *.o

.PHONY: all run devrun lookrun clean
