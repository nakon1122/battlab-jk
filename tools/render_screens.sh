#!/bin/bash
# Build the UI on the PC and render preview PNGs.  usage: tools/render_screens.sh <LovyanGFX src dir> <out dir>
set -e
LG=${1:?LovyanGFX src dir}; OUT=${2:-firmware/screens}
HERE=$(cd "$(dirname "$0")/.." && pwd); B=$(mktemp -d)
mkdir -p "$OUT"
SRCS="$(find $LG/lgfx/v1 -maxdepth 1 -name '*.cpp') $(find $LG/lgfx/v1/misc $LG/lgfx/v1/panel $LG/lgfx/v1/touch $LG/lgfx/v1/platforms/framebuffer -name '*.cpp') $(find $LG/lgfx/utility -name '*.c*')"
i=0; for f in $SRCS; do g++ -std=c++17 -O1 -w -I$LG -c $f -o $B/l$i.o & i=$((i+1)); done
for f in $(find $LG/lgfx/Fonts -name '*.c'); do gcc -O0 -w -c $f -o $B/f$i.o & i=$((i+1)); done
wait
ar rcs $B/liblgfx.a $B/*.o
cat > $B/stubs.cpp <<'S'
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
namespace lgfx { inline namespace v1 { void delay(unsigned long){} void gpio_hi(uint32_t){} void gpio_lo(uint32_t){} void pinMode(int_fast16_t, pin_mode_t){} }}
S
g++ -std=c++17 -O1 -w -I$LG -I$HERE/src/screen $B/stubs.cpp $HERE/src/screen/ui.cpp $HERE/tools/render_screens.cpp $B/liblgfx.a -o $B/render -lpthread
$B/render "$OUT"
python3 - "$OUT" <<'P'
import sys, glob, os
from PIL import Image
for f in glob.glob(sys.argv[1] + '/*.ppm'):
    Image.open(f).save(f[:-4] + '.png'); os.remove(f)
P
ls "$OUT"
