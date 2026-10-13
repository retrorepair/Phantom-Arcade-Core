#!/bin/bash
# Continue xemu's build after configure. meson's custom command runs a bare `bash`, which CreateProcess resolves to
# WSL's C:\Windows\System32\bash.exe (System32 is searched before PATH), and that cannot open C:/ paths. Generate the one
# header that needs it with MSYS2's bash, exactly as meson would (capturing stdout), then let ninja do the rest.
SRC=/c/Users/joelw/Documents/groovy-forks/xemu
cd "$SRC/build" || exit 1
echo "bash is: $(command -v bash)"
if [ ! -s xemu-version-macro.h ]; then
  bash "$SRC/scripts/xemu-version.sh" "$SRC" > xemu-version-macro.h || { echo "xemu-version.sh failed"; exit 1; }
fi
echo "--- xemu-version-macro.h:"; cat xemu-version-macro.h
ninja -j6
