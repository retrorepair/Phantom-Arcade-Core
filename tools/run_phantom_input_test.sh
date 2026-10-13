#!/bin/bash
# Build and run tools/phantom_input_test.cpp: the real launcher (phantom.cpp) with its
# hardware hooks stubbed, against a fake host on loopback.
#
#   wsl -- bash tools/run_phantom_input_test.sh
#
# Any Linux with g++ works; WSL is just what Windows machines have.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$here/hps_linux/src"
grv="$src/support/groovy"
# user_io.h includes file_io.h from upstream Main_MiSTer, which this repo only overlays.
# build_phantom_hps.sh keeps a checkout here; point MAIN_MISTER elsewhere if yours differs.
upstream="${MAIN_MISTER:-/tmp/phantom_hps_build/Main_MiSTer}"
[ -f "$upstream/file_io.h" ] || { echo "no Main_MiSTer checkout at $upstream - run tools/build_phantom_hps.sh once, or set MAIN_MISTER" >&2; exit 2; }
phantom_cpp="${PHANTOM_CPP:-$grv/phantom.cpp}"   # override to compare against another revision
out="${TMPDIR:-/tmp}/phantom_input_test"
mkdir -p "$out"

gcc -std=c11 -O1 -c "$grv/phantom_ui.c" -I "$grv" -o "$out/phantom_ui.o"
g++ -std=c++17 -O1 -Wall -c "$phantom_cpp" -I "$src" -I "$grv" -I "$upstream" \
    -DPH_INI="\"$out/phantom.ini\"" -DPH_CACHE="\"$out/cache.tsv\"" -o "$out/phantom.o"
g++ -std=c++17 -O1 -Wall -pthread "$here/tools/phantom_input_test.cpp" \
    "$out/phantom.o" "$out/phantom_ui.o" -I "$src" -I "$grv" -I "$upstream" -DTEST_DIR="\"$out\"" \
    -o "$out/phantom_input_test"

exec "$out/phantom_input_test"
