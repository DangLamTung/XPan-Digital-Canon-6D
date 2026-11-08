#!/bin/sh
#
# Build Magic Lantern for one camera on macOS.
#
#   ./build.sh              # Canon 6D, firmware 1.1.6 (default)
#   ./build.sh 6D.116       # same, explicit
#   ./build.sh 5D3.123
#   ./build.sh 6D.116 clean # also clean modules first (needed after module changes)
#
# Output: magiclantern/platform/<CAM>/build/magiclantern.zip
#
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
# Found rather than named: GitHub's macOS runners are arm64 or x86_64 depending
# on the image, and each needs its own toolchain build.
TC=$(ls -d "$ROOT"/toolchain/arm-gnu-toolchain-*/bin 2>/dev/null | head -1)
VENV="$ROOT/toolchain/venv/bin"
SRC="$ROOT/magiclantern"

CAM=${1:-6D.116}
MODE=${2:-}

if [ -z "$TC" ] || [ ! -x "$TC/arm-none-eabi-gcc" ]; then
    echo "ARM toolchain not found under $ROOT/toolchain" >&2
    echo "Download arm-gnu-toolchain-13.3.rel1-darwin-{arm64,x86_64}-arm-none-eabi.tar.xz" >&2
    echo "(whichever matches uname -m) and extract it into $ROOT/toolchain" >&2
    exit 1
fi

if [ ! -x "$VENV/rst2html" ]; then
    echo "docutils venv not found in $VENV (python3 -m venv venv && venv/bin/pip install docutils)" >&2
    exit 1
fi

PATH="$VENV:$TC:$PATH"
export PATH

# ML asks for GNU stat and a plain "readelf", and sets them with "=" in its
# makefiles, so they must be passed on the make command line (env is ignored).
MAKEARGS="ARM_BINPATH=$TC READELF=$TC/arm-none-eabi-readelf"

if [ "$MODE" = "clean" ]; then
    # the platform build does not notice module changes by itself
    make -C "$SRC/modules" clean
fi

# The platform build copies each module's .mo out of modules/build, and only
# `make -C modules` refreshes that directory.  Skipping this step silently
# packages the previous binary - it already shipped once, so it is now part of
# every build.
# shellcheck disable=SC2086
make -C "$SRC/modules" $MAKEARGS > /dev/null

cd "$SRC/platform/$CAM"

# Ship the film recipes in the zip.
#
# Nothing in the build produced ML/FILM. The folder that the v1.2.0 release
# carried had been left in build/zip by an earlier manual copy, which meant two
# things: a clean checkout shipped no recipes at all -- every stock fell back to
# a generated look -- and the copies that did ship were older than
# modules/film_sim/films/. Measured before fixing: films/soup800.fs was
# af497d4d, the zip and the card carried 67e7bf11, and four recipes were
# different (classicneg, portra, soup800, velvia). The staging tree is what the
# platform make zips, so it is synced here, before make runs.
FILMS="$SRC/modules/film_sim/films"
ZIPFILM="build/zip/ML/FILM"
if [ -d "$FILMS" ]; then
    mkdir -p "$ZIPFILM"
    # Only the recipes: prune ones that no longer exist in films/, copy the rest.
    for f in "$ZIPFILM"/*.fs; do
        [ -e "$f" ] || continue
        [ -e "$FILMS/$(basename "$f")" ] || rm -f "$f"
    done
    cp -f "$FILMS"/*.fs "$ZIPFILM"/
    echo "  films: $(ls "$FILMS"/*.fs | wc -l | tr -d ' ') recipes -> $ZIPFILM"
else
    echo "  ERROR $FILMS is missing; the zip would ship no film recipes" >&2
    exit 1
fi

# shellcheck disable=SC2086
make -j"$(sysctl -n hw.ncpu)" $MAKEARGS "STAT_CMD=stat -f %N:%z"

# A module that imports a symbol the camera does not export does not fail at
# build time - it simply refuses to load on the camera, with "module load
# error" and no log, because none of its code ever runs. That cost hours:
# a strncasecmp() call was enough to make film_sim unloadable.
SYM="$SRC/platform/$CAM/build/$(echo "$CAM" | tr "." "_").sym"
NM=$(command -v "${ARM_BINPATH:-}arm-none-eabi-nm" 2>/dev/null) \
  || NM="$ROOT/toolchain/arm-gnu-toolchain-13.3.rel1-darwin-arm64-arm-none-eabi/bin/arm-none-eabi-nm"
if [ ! -x "$NM" ]; then
    echo "  ERROR cannot find arm-none-eabi-nm; refusing to build an unverified module" >&2
    exit 1
fi
if [ -f "$SYM" ]; then
    awk '{print $NF}' "$SYM" | sort -u > /tmp/ml_sym.$$
    BAD=0
    # Only the modules this project ships. Bundled third-party modules import
    # symbols the 6D build does not export (mlv_play, mlv_snd, pic_view, yolo);
    # they are not ours to fix and must not block the build.
    # mlwifi is a probe that ships outside the zip, so a missing import would
    # not break an install -- it would break the only diagnostic that exists
    # for the WiFi reversing: the module loads with "module load error" and
    # writes nothing to the log at all. Gate it like the shipped two.
    for mo in "$SRC"/modules/build/film_sim.mo "$SRC"/modules/build/film_lv.mo \
              "$SRC"/modules/build/mlwifi.mo; do
        [ -f "$mo" ] || continue
        "$NM" -u "$mo" | awk '{print $2}' | sort -u > /tmp/ml_undef.$$
        MISSING=$(comm -23 /tmp/ml_undef.$$ /tmp/ml_sym.$$)
        if [ -n "$MISSING" ]; then
            echo "  ERROR $(basename "$mo") imports symbols the camera does not export:" >&2
            echo "$MISSING" | sed 's/^/          /' >&2
            BAD=1
        fi
    done
    rm -f /tmp/ml_sym.$$ /tmp/ml_undef.$$
    [ "$BAD" = 0 ] || { echo "build FAILED: unusable module" >&2; exit 1; }
    echo "  modules: every import resolves against $CAM.sym"
fi

echo
echo "done: $SRC/platform/$CAM/build/magiclantern.zip"
