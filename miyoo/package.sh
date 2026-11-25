#!/bin/sh
# MY-FAM by David Amo: build for the Miyoo Mini Plus and package dist/MY-FAM-vN.zip, one zip for its three systems:
#   Apps/MY-FAM.pak   Allium (its big icon: Allium scales it)
#   App/MY-FAM        OnionOS and the Miyoo firmware (a 74x74 icon: they show it as it is)
# The same program and the same launch.sh in both (it tells the system apart by itself); each system reads its folder.
#
#   TOOLCHAIN=/path/to/gcc-linaro-arm-linux-gnueabihf-raspbian-x64 \
#   SDL_INCLUDE=/path/to/include  SDL_LIB=/path/to/libSDL-1.2.so.0  miyoo/package.sh
#
# TOOLCHAIN: a GCC for arm-linux-gnueabihf with an old glibc (the one in github.com/raspberrypi/tools,
#   arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian-x64, works). SDL_INCLUDE: a folder holding SDL/SDL.h
#   (SDL 1.2's headers, github.com/libsdl-org/SDL-1.2, with an SDL_config.h). SDL_LIB: a libSDL-1.2.so.0 for the
#   Miyoo to link against (Allium's own; not shipped: the app uses the one on the console).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
: "${TOOLCHAIN:?set TOOLCHAIN}" "${SDL_INCLUDE:?set SDL_INCLUDE}" "${SDL_LIB:?set SDL_LIB}"
B="$TOOLCHAIN/bin/arm-linux-gnueabihf"
V=$(sed -n 's/^#define MYFAM_VERSION "\([0-9]*\)".*/\1/p' fam/app.h)

# the tests first, on this machine
python3 tools/build.py fam --miyoo-test
T=$(mktemp -d)
for t in miyoo/tests/*.txt; do   # (each from a fresh start: no save file; WAVs into the temporary folder)
    rm -rf "$T/run" && mkdir -p "$T/run"
    (cd "$T/run" && MYFAM_WAV_ROOT="$T/run" "$ROOT/fam/build/miyoo/my-fam_test" "$ROOT/$t" "$T/run" | grep -v '^said\|^my-fam:' | tail -1) | tee "$T/result"
    grep -q ' 0 failure' "$T/result" || { echo "package: $t failed" >&2; exit 1; }
done

# the app
CC="$B-gcc" \
MIYOO_CFLAGS="-O3 -I$SDL_INCLUDE -march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm" \
MIYOO_LIBS="$SDL_LIB -lpthread -lrt -lm -Wl,--allow-shlib-undefined" \
    python3 tools/build.py fam --miyoo
if "$B-objdump" -T fam/build/miyoo/my-fam | grep -q 'GLIBC_2\.[5-9]\|GLIBC_2\.[1-9][0-9]'; then
    echo "package: the program asks for a newer glibc than GLIBC_2.4 (scanf?)" >&2
    exit 1
fi

# the package: both folders, the same program and launch.sh, the icon each menu wants
"$B-strip" -o "$T/my-fam.elf" fam/build/miyoo/my-fam
for D in "$T/pkg/Apps/MY-FAM.pak" "$T/pkg/App/MY-FAM"; do
    mkdir -p "$D"
    cp "$T/my-fam.elf" "$D/my-fam.elf"
    cp miyoo/pak/launch.sh miyoo/pak/config.json "$D/"
    case "$D" in */Apps/*) cp miyoo/pak/icon.png "$D/icon.png" ;; *) cp miyoo/pak/icon-74.png "$D/icon.png" ;; esac
    cp README.md "$D/README.md"
    cp LICENSE "$D/LICENSE.txt"
    cp assets/fonts/Barlow-OFL.txt assets/fonts/Terminus-LICENSE.txt "$D/"
    chmod +x "$D/my-fam.elf" "$D/launch.sh"
    (cd "$D" && md5sum my-fam.elf > my-fam.md5)
done
mkdir -p dist
rm -f "dist/MY-FAM-v$V.zip"
(cd "$T/pkg" && zip -qr "$ROOT/dist/MY-FAM-v$V.zip" Apps App)
echo "package: dist/MY-FAM-v$V.zip"
rm -rf "$T"
