#!/bin/bash
# Build and run tools/phantom_cat_test.cpp: the cabinet's paged library (phantom_cat.c) against the daemon's
# real reply code (host/game_index.h), under the address and undefined-behaviour sanitizers.
#
#   wsl -- bash tools/run_phantom_cat_test.sh
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
grv="$here/hps_linux/src/support/groovy"
out="${TMPDIR:-/tmp}/phantom_cat_test"
mkdir -p "$out"

gcc -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -c "$grv/phantom_ui.c"  -I "$grv" -o "$out/phantom_ui.o"
gcc -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -c "$grv/phantom_cat.c" -I "$grv" -o "$out/phantom_cat.o"
g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    "$here/tools/phantom_cat_test.cpp" "$out/phantom_cat.o" "$out/phantom_ui.o" -I "$grv" -o "$out/phantom_cat_test"

exec "$out/phantom_cat_test"
