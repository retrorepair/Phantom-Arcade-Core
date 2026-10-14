#!/bin/bash
# Build and run tools/phantom_art_test.cpp: the cabinet's picture client (phantom_art.c) against a fake host on
# loopback, under the address and undefined-behaviour sanitizers.
#
#   wsl -- bash tools/run_phantom_art_test.sh
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
grv="$here/hps_linux/src/support/groovy"
out="${TMPDIR:-/tmp}/phantom_art_test"
mkdir -p "$out"

gcc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -c "$grv/phantom_art.c" -I "$grv" -o "$out/phantom_art.o"
g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -pthread \
    "$here/tools/phantom_art_test.cpp" "$out/phantom_art.o" -I "$grv" -o "$out/phantom_art_test"

exec "$out/phantom_art_test"
