#!/bin/sh
#
# Install the current 6D.116 build to the camera's SD card.
#
#   ./install-sd.sh              # find the card automatically
#   ./install-sd.sh /Volumes/X   # explicit mount point
#   ./install-sd.sh --dry-run    # only report what would happen
#
# The card is never formatted: ML/SETTINGS and ML/LOGS are preserved, so
# configs, the module enable flags and the slot backups survive a reinstall.
# Every copied file is verified against the build tree by SHA-256.
#
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD="$ROOT/magiclantern/platform/6D.116/build"
ZIP="$BUILD/zip"
FIRMWARE=autoexec.bin

DRY=0
VOL=""
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY=1 ;;
        -*) echo "unknown option: $arg" >&2; exit 2 ;;
        *) VOL=$arg ;;
    esac
done

[ -f "$ZIP/$FIRMWARE" ] || { echo "no build in $ZIP - run ./build.sh 6D.116 first" >&2; exit 1; }

if [ -z "$VOL" ]; then
    for v in /Volumes/*; do
        [ "$v" = "/Volumes/Macintosh HD" ] && continue
        [ -d "$v/DCIM" ] || continue
        VOL=$v
        break
    done
fi
if [ -z "$VOL" ] || [ ! -d "$VOL" ]; then
    if [ "$DRY" = 1 ]; then
        echo "card : (not mounted)"
        echo "build: $ZIP"
        echo "--- would copy ---"
        (cd "$ZIP" && find . -type f | sort)
        exit 0
    fi
    echo "SD card not found - insert it, or pass the mount point" >&2
    exit 1
fi

echo "card : $VOL"
echo "build: $ZIP"

FSTYPE=$(diskutil info "$VOL" | awk -F: '/File System Personality/{gsub(/^ +/,"",$2);print $2}')
echo "fs   : ${FSTYPE:-unknown}"
case "$FSTYPE" in
    *FAT*) ;;
    "") ;;
    *) echo "warning: ML needs FAT32/exFAT, this looks like '$FSTYPE'" >&2 ;;
esac

if [ "$DRY" = 1 ]; then
    echo "--- would copy ---"
    (cd "$ZIP" && find . -type f | sort)
    exit 0
fi

# 1. files at the card root
for f in "$FIRMWARE" ML-SETUP.FIR; do
    cp -f "$ZIP/$f" "$VOL/$f"
    echo "  root  $f"
done

# 2. ML tree, without clobbering settings or logs
mkdir -p "$VOL/ML"
(cd "$ZIP" && find ML -type d) | while read -r d; do
    mkdir -p "$VOL/$d"
done
# .mo files that exist in modules/build come from there, never from the zip: the
# platform make stages them while the module sub-make can still be writing, so
# the zip regularly holds one build older than modules/build.  Modules that only
# exist in the zip (prebuilt ones) are still copied.
MODOUT="$ROOT/magiclantern/modules/build"
N=0
(cd "$ZIP" && find ML -type f) | while read -r f; do
    case "$f" in
        ML/modules/*.mo) [ -f "$MODOUT/$(basename "$f")" ] && continue ;;
    esac
    cp -f "$ZIP/$f" "$VOL/$f"
done
N=$(cd "$ZIP" && find ML -type f | wc -l | tr -d ' ')
echo "  tree  ML/ ($N files)"

NM=0
for mo in "$MODOUT"/*.mo; do
    [ -f "$mo" ] || continue
    cp -f "$mo" "$VOL/ML/modules/$(basename "$mo")"
    NM=$((NM + 1))
done
echo "  mods  ML/modules/ ($NM fresh .mo from modules/build)"

# 3. modules must be enabled at boot; only a zero-byte flag loads one
mkdir -p "$VOL/ML/SETTINGS" "$VOL/ML/LOGS"
# film_lv is deliberately not enabled: ML runs exactly ONE display filter (the
# first module that claims it) and film_lv claims it unconditionally, so while
# it is loaded film_sim's LiveView grade is never called at all.
for m in film_sim; do
    : > "$VOL/ML/SETTINGS/$m.en"
    echo "  flag  ML/SETTINGS/$m.en"
done

# Park the rival module so it cannot take the display-filter slot back, and
# drop its boot flag if an older install left one.
mkdir -p "$VOL/ML/modules.disabled"
if [ -f "$VOL/ML/modules/film_lv.mo" ]; then
    mv -f "$VOL/ML/modules/film_lv.mo" "$VOL/ML/modules.disabled/film_lv.mo"
    echo "  park  ML/modules.disabled/film_lv.mo (rival display filter)"
fi
rm -f "$VOL/ML/SETTINGS/film_lv.en"

# 3b. the runtime style library: real blobs extracted from the ROM dump
#     (tools/extract_styles.py), so "Style on Card" has entries on first boot
mkdir -p "$VOL/ML/FILMSIM/STYLES"
# converted .pf2 -> camera blobs (tools/pf2_to_style.py, Canon EdsCFParse);
# STYLE01 is the black-and-white one so it is the first thing Load Style shows
# Only bodies the CAMERA produced render: the payload is whitened, so a blob
# we derived ourselves keeps its base/sliders but the body is ignored.  These
# three were read back out of the camera's own User 1/2/3 slots, and every one
# is checked for the camera payload marker below.
LIB="$ROOT/magiclantern/modules/film_sim/tools/styles_camera"
LIB2="$ROOT/magiclantern/modules/film_sim/tools/styles_film"
# purge logs and per-boot debug dumps (keep the ROM/FW dumps, they are analysis data)
mkdir -p "$VOL/ML/LOGS"
NLOG=$(ls "$VOL/ML/LOGS"/*.LOG 2>/dev/null | wc -l | tr -d ' ')
[ -f "$VOL/ML/LOGS/FILMSIM.LOG" ] && mv -f "$VOL/ML/LOGS/FILMSIM.LOG" "$VOL/ML/LOGS/FILMSIM.PREV.LOG"
# Keep the rotated copy: "*.LOG" also matches FILMSIM.PREV.LOG, so a blanket
# rm deleted the very log the line above had just preserved, and with it the
# record of which stock was written to which style slot for which shot.
for lg in "$VOL/ML/LOGS"/*.LOG; do
    [ -f "$lg" ] || continue
    [ "$(basename "$lg")" = "FILMSIM.PREV.LOG" ] && continue
    rm -f "$lg"
done
rm -f "$VOL/ML/LOGS"/PSTYLE.TXT \
      "$VOL/ML/LOGS"/RELOAD.BIN "$VOL/ML/LOGS"/SLOT1.BIN "$VOL/ML/LOGS"/SLOT2.BIN
echo "  purge ML/LOGS ($NLOG old .LOG -> FILMSIM.PREV.LOG, PSTYLE.TXT, RELOAD/SLOT)"
rm -f "$VOL/ML/FILMSIM/STYLES/STYLE"*.BIN
# macOS leaves AppleDouble "._name" files on exFAT, and an old derived
# USER<nn>.BIN is indistinguishable from a real archive by name - the module
# re-mirrors the slots on the next boot, so clearing them is safe.
rm -f "$VOL/ML/FILMSIM/STYLES/"._* "$VOL/ML/FILMSIM/STYLES/USER"*.BIN 2>/dev/null
NLIB=0
if [ -d "$LIB" ]; then
    for dir in "$LIB" "$LIB2"; do
        [ -d "$dir" ] || continue
        for b in "$dir"/STYLE*.BIN; do
            [ -f "$b" ] || continue
            # payload starts with the 4-byte marker 73d14af6; byte 4 of the
            # payload varies per style (0x4d, 0x0a, 0x06 ...), so compare the
            # marker only - requiring the whole 8 bytes silently dropped 10 of
            # the 13 styles.
            if [ "$(stat -f %z "$b")" != 16704 ] || \
               [ "$(dd if="$b" bs=1 skip=72 count=4 2>/dev/null | xxd -p)" != "73d14af6" ]; then
                echo "  skip  $(basename "$b"): not a 16,704-byte style body" >&2
                continue
            fi
            cp -f "$b" "$VOL/ML/FILMSIM/STYLES/$(basename "$b")"
            NLIB=$((NLIB + 1))
        done
    done
fi
# macOS writes an AppleDouble "._name" companion for every file it copies to
# exFAT, so they have to be swept after the copy, not before
[ "$NLIB" -gt 0 ] && echo "  lib   ML/FILMSIM/STYLES/ ($NLIB style bodies)"

# 3bb. canister artwork: the user's FILMCAN.DAT plus the stocks it lacks.
#      Our drawn records come from tools/make_can_art.py (in this repo); the
#      manufacturer artwork is only ever read from the card, never committed.
if [ -f "$VOL/ML/data/FILMCAN.DAT" ] && command -v python3 >/dev/null 2>&1; then
    ARTDIR=$(mktemp -d)
    if python3 "$ROOT/tools/make_can_art.py" --base "$VOL/ML/data/FILMCAN.DAT" \
            --out "$ARTDIR" >/dev/null 2>&1 && \
       python3 "$ROOT/tools/merge_cans.py" --base "$VOL/ML/data/FILMCAN.DAT" \
            --art "$ARTDIR" --out "$VOL/ML/FILMSIM/CANS.DAT" >/dev/null 2>&1; then
        NCAN=$(python3 - "$VOL/ML/FILMSIM/CANS.DAT" <<'EOF'
import struct, sys
d = open(sys.argv[1], "rb").read()
print(struct.unpack("<6H", d[4:16])[0])
EOF
)
        echo "  art   ML/FILMSIM/CANS.DAT ($NCAN canisters)"
    else
        rm -f "$VOL/ML/FILMSIM/CANS.DAT"
        echo "  art   CANS.DAT not built (module falls back to FILMCAN.DAT)" >&2
    fi
    rm -rf "$ARTDIR"
fi

# 3c. Canon .pf2 picture styles, copied under ASCII names: the card is exFAT and
#     ML's directory listing does not return usable names for non-ASCII
#     filenames, so a Chinese-named .pf2 is invisible to the module.
mkdir -p "$VOL/ML/FILMSIM/PF2"
# purge first: leftovers from older installs were duplicates and one file whose
# name had been truncated to "RDP" (no extension)
rm -f "$VOL/ML/FILMSIM/PF2"/* 2>/dev/null
PF2SRC="$ROOT/magiclantern/modules/film_sim/tools/pf2"
NPF2=0
if [ -d "$PF2SRC" ]; then
    while IFS='|' read -r src dst; do
        [ -n "$src" ] || continue
        [ -f "$PF2SRC/$src" ] || { echo "  warn  missing $src" >&2; continue; }
        cp -f "$PF2SRC/$src" "$VOL/ML/FILMSIM/PF2/$dst"
        NPF2=$((NPF2 + 1))
    done <<'LIST'
佳能官方_怀旧_NOSTALGIA.pf2|canon-nostalgia.pf2
佳能官方_清晰_CLEAR.pf2|canon-clear.pf2
佳能官方_黎明和黄昏_TWILIGHT.pf2|canon-twilight.pf2
佳能官方_翠绿色_EMERALD.pf2|canon-emerald.pf2
佳能官方_秋天色调_AUTUMN_HUES.pf2|canon-autumn.pf2
佳能官方_影楼人像_P-STUDIO.pf2|canon-studio.pf2
佳能官方_快拍人像_P-SNAPSHOT.pf2|canon-snapshot.pf2
富士RVP-100F底片风格.pf2|fuji-rvp100f-base.pf2
富士RVP-100F浓色风格.pf2|fuji-rvp100f-vivid.pf2
富士RVP-100F淡色风格.pf2|fuji-rvp100f-mild.pf2
富士RVP.pf2|fuji-rvp.pf2
富士正片(自然)_RVP-100F(Neutral).pf2|fuji-rvp100f-neutral.pf2
富士正片_RDP III.pf2|fuji-rdp3.pf2
velvia50.pf2|fuji-velvia50.pf2
RVP.pf2|fuji-rvp-b.pf2
RDP III.pf2|fuji-rdp3-b.pf2
Eyer_黑白风格_EyerBW99.pf2|bw-eyer99.pf2
LIST
fi
[ "$NPF2" -gt 0 ] && echo "  pf2   ML/FILMSIM/PF2/ ($NPF2 Canon style files, ASCII names)"

# 3d. Soup Engine stock recipes. These are the looks themselves: a stock with no
#     .fs silently develops through a generated single-curve fallback, so the
#     card must carry exactly the recipes in the repo or the colour drifts from
#     whatever was tested on the host.
FSSRC="$ROOT/magiclantern/modules/film_sim/films"
mkdir -p "$VOL/ML/FILM"
NFSTOCK=0
if [ -d "$FSSRC" ]; then
    # Refresh the staging tree too. Nothing in the platform build copies the
    # recipes into $ZIP, so without this the zip keeps whatever was placed
    # there by hand and the byte-for-byte check below compares the card
    # against a stale copy.
    mkdir -p "$ZIP/ML/FILM"
    for f in "$FSSRC"/*.fs; do
        [ -f "$f" ] || continue
        cp -f "$f" "$ZIP/ML/FILM/$(basename "$f")"
        cp -f "$f" "$VOL/ML/FILM/$(basename "$f")"
        NFSTOCK=$((NFSTOCK + 1))
    done
    # stale recipes from an older install would shadow nothing but confuse the
    # shelf, so report any that are no longer in the repo
    for f in "$VOL/ML/FILM"/*.fs; do
        [ -f "$f" ] || continue
        [ -f "$FSSRC/$(basename "$f")" ] || echo "  warn  stale ML/FILM/$(basename "$f")"
    done
fi
[ "$NFSTOCK" -gt 0 ] && echo "  fs    ML/FILM/ ($NFSTOCK stock recipes)"

SEEDED=0
[ "$NLIB" -gt 0 ] && SEEDED=1
for src in "ML/LOGS/SLOT1_ORIG.BIN" "ML/LOGS/SLOT1.BIN" "ML/LOGS/SLOT2.BIN"; do
    [ "$SEEDED" = 1 ] && break
    [ -f "$VOL/$src" ] || continue
    if [ "$(stat -f %z "$VOL/$src")" != 16704 ]; then continue; fi
    if [ -f "$VOL/ML/FILMSIM/STYLES/STYLE01.BIN" ]; then SEEDED=1; break; fi
    cp -f "$VOL/$src" "$VOL/ML/FILMSIM/STYLES/STYLE01.BIN"
    echo "  seed  ML/FILMSIM/STYLES/STYLE01.BIN <- $src"
    SEEDED=1
    break
done
[ "$SEEDED" = 1 ] || echo "  note  no style blob on the card yet; use Save User 1/2 on the camera"

# 4. verify every byte against the tree it came from: .mo files come from
#    modules/build (the authoritative output), everything else from the zip
BAD=0
LIST=$(mktemp)
{ printf '%s\n' "$FIRMWARE" ML-SETUP.FIR; (cd "$ZIP" && find ML -type f); } > "$LIST"
while read -r f; do
    case "$f" in
        ML/modules/*.mo) SRC="$MODOUT/$(basename "$f")" ;;
        *)               SRC="$ZIP/$f" ;;
    esac
    [ -f "$SRC" ] || continue
    # a module parked in modules.disabled is deliberately absent from modules/
    [ -f "$VOL/$f" ] || case "$f" in
        ML/modules/*.mo) [ -f "$VOL/ML/modules.disabled/$(basename "$f")" ] && continue ;;
    esac
    a=$(shasum -a 256 "$SRC"        | cut -d' ' -f1)
    b=$(shasum -a 256 "$VOL/$f" 2>/dev/null | cut -d' ' -f1)
    [ "$a" = "$b" ] || { echo "  MISMATCH $f" >&2; BAD=1; }
done < "$LIST"
rm -f "$LIST"
[ "$BAD" = 0 ] && echo "  all files verified"

# 5. the film_sim strings that must be present in the installed module
echo "--- installed modules ---"
for m in film_lv film_sim; do
    if [ -f "$VOL/ML/modules/$m.mo" ]; then
        ls -l "$VOL/ML/modules/$m.mo"
        if [ -f "$MODOUT/$m.mo" ] && ! cmp -s "$MODOUT/$m.mo" "$VOL/ML/modules/$m.mo"; then
            echo "  MISMATCH $m.mo is not the freshly built module" >&2
            BAD=1
        else
            echo "  ok   matches $(basename "$MODOUT")/$m.mo"
        fi
    else
        echo "  parked $m.mo (absent from ML/modules on purpose)"
    fi
done
# two modules now: the preview one and the picture-style one, checked separately
if [ -f "$VOL/ML/modules/film_lv.mo" ]; then
    for s in "Simulation" "Preset" "Strength" "Grain" "Frame rate" "TEST band"; do
        strings "$VOL/ML/modules/film_lv.mo" | grep -q "$s" \
            && echo "  ok   film_lv  '$s'" || { echo "  MISSING film_lv '$s'" >&2; BAD=1; }
    done
fi
# The menu itself, not just the module: a build that loads but whose rows are
# missing is the failure this catches. Mark and Marked Set are gone on purpose
# (the root entry ticked one photo at a time with no visible state); Film
# Settings and Cinemascope are the new rows.
for s in "Level" "Stock" "Photo" "Strength" "Grain" "Develop" "Capture" "LiveView Filter" "Debug" \
         "Current Photo" "Auto Develop" "Film Shelf" "Film Settings" "Darkroom" "Comparison x4" \
         "Cinemascope" "Pad to 3:2"; do
    strings "$VOL/ML/modules/film_sim.mo" | grep -q "$s" \
        && echo "  ok   film_sim '$s'" || { echo "  MISSING film_sim '$s'" >&2; BAD=1; }
done

[ "$BAD" = 0 ] || { echo "install FAILED verification" >&2; exit 1; }

# Every file copied to ExFAT gets an AppleDouble "._name" companion; sweep them
# last, once nothing else will be written.
#
# Scoped to ML/ and DCIM/ on purpose: a sweep of the volume root hits .Trashes
# and .Spotlight-V100, where macOS refuses access ("Operation not permitted"),
# and under `set -e` that non-zero `find -delete` killed the script *after* the
# copy had already succeeded -- with 2>/dev/null hiding the reason, so the
# installer exited 1 on every successful install and never printed "installed OK".
NSWEEP=$(find "$VOL/ML" "$VOL/DCIM" -name "._*" 2>/dev/null | wc -l | tr -d ' ')
find "$VOL/ML" "$VOL/DCIM" -name "._*" -delete 2>/dev/null \
    || echo "  warn: some ._* files could not be removed" >&2
if [ "$NSWEEP" != 0 ]; then
    echo "  sweep $NSWEEP AppleDouble files (.\_*)"
fi

sync
echo "installed OK"
