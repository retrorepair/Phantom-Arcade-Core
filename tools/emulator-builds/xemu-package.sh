#!/bin/bash
# Package xemu the way build.sh's package_windows does (exe + the DLLs it needs), but strip the debug info first.
SRC=/c/Users/joelw/Documents/groovy-forks/xemu
cd "$SRC" || exit 1
rm -rf dist
mkdir -p dist
cp build/qemu-system-i386w.exe dist/xemu.exe
strip -s dist/xemu.exe
python3 "$SRC/get_deps.py" dist/xemu.exe dist
python3 ./scripts/gen-license.py --platform windows > dist/LICENSE.txt 2>/dev/null || echo "(no license generated)"
ls -l dist | awk '{print $5, $9}'
