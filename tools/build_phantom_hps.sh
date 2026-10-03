#!/bin/bash
# build_phantom_hps.sh - build the Phantom Arcade core's HPS binary from a clean tree.
#
# The core's ARM-side program is Main_MiSTer with this repo's hps_linux/src overlaid on
# top: the repo carries only the files it changes, not a whole fork of Main_MiSTer. This
# script does that overlay reproducibly.
#
#   ./tools/build_phantom_hps.sh          # MiSTer_phantom (UDP)
#   ./tools/build_phantom_hps.sh xdp      # also MiSTer_phantom_XDP (AF_XDP)
#
# Toolchain: ARM's 10.2-2020.11 arm-none-linux-gnueabihf, downloaded on first run to
# ~/xtools. The version is pinned, not incidental - 10.3 miscompiles Main_MiSTer's
# fpga_io.cpp, where the 32-byte RBF copy loop clobbers r0-r7 while asking for four
# more "+r" operands and the allocator runs out of registers:
#   fpga_io.cpp:242: error: 'asm' operand has impossible constraints
#
# Outputs land in build_output/.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WANT_XDP="${1:-}"

TC_VER="10.2-2020.11"
TC_NAME="gcc-arm-${TC_VER}-x86_64-arm-none-linux-gnueabihf"
TC_URL="https://developer.arm.com/-/media/Files/downloads/gnu-a/${TC_VER}/binrel/${TC_NAME}.tar.xz"
TC_DIR="${XTOOLS:-$HOME/xtools}/${TC_NAME}"

# Main_MiSTer commit this overlay is written against. Bump deliberately: the overlay
# replaces whole files (menu.cpp, user_io.cpp, input.cpp, cfg.cpp), so a newer upstream
# silently loses its own changes to them.
BASE_REPO="https://github.com/MiSTer-devel/Main_MiSTer.git"
BASE_COMMIT="cab156339e523adce5f4126322a47735b7bb67bc"

WORK="${WORK:-/tmp/phantom_hps_build}"

say() { printf '\n== %s ==\n' "$*"; }

say "toolchain"
if [ ! -x "$TC_DIR/bin/arm-none-linux-gnueabihf-g++" ]; then
	mkdir -p "$(dirname "$TC_DIR")"
	echo "downloading $TC_NAME (~100MB, once)"
	curl -fL --retry 3 -o "$(dirname "$TC_DIR")/tc.tar.xz" "$TC_URL"
	tar xf "$(dirname "$TC_DIR")/tc.tar.xz" -C "$(dirname "$TC_DIR")"
	rm -f "$(dirname "$TC_DIR")/tc.tar.xz"
fi
export PATH="$TC_DIR/bin:$PATH"
arm-none-linux-gnueabihf-g++ --version | head -1

say "base checkout"
mkdir -p "$WORK"
if [ ! -d "$WORK/Main_MiSTer/.git" ]; then
	git clone -q "$BASE_REPO" "$WORK/Main_MiSTer"
fi
git -C "$WORK/Main_MiSTer" fetch -q --all
git -C "$WORK/Main_MiSTer" checkout -q "$BASE_COMMIT"
echo "Main_MiSTer @ $(git -C "$WORK/Main_MiSTer" rev-parse --short HEAD)"

say "overlay"
rm -rf "$WORK/tree"
cp -r "$WORK/Main_MiSTer" "$WORK/tree"
rm -rf "$WORK/tree/.git"
cp -r "$REPO/hps_linux/src/"* "$WORK/tree/"

say "build (UDP)"
cd "$WORK/tree"
make _AF_XDP=0
mkdir -p "$REPO/build_output"
cp MiSTer_groovy "$REPO/build_output/MiSTer_phantom"
echo "-> build_output/MiSTer_phantom"

if [ "$WANT_XDP" = "xdp" ]; then
	say "build (AF_XDP)"
	# Pre-existing upstream gap, not something the launcher introduced: the Makefile
	# adds -I./support/groovy/kernel/usr/include for the XDP variant, but that
	# directory is not in the repository (the kernel headers under
	# support/groovy/kernel/lib/xdp-tools/headers are a different layout). Without it
	# libxdp's xsk.h compiles against no uapi/linux/if_xdp.h and fails on
	# XSK_UNALIGNED_BUF_ADDR_MASK and XDP_USE_NEED_WAKEUP.
	#
	# XDP is the optional high-throughput path and needs a patched kernel, a BPF
	# object and libelf on the MiSTer anyway; the UDP binary above is what the install
	# instructions use.
	if [ ! -d support/groovy/kernel/usr/include ]; then
		echo "SKIPPED: support/groovy/kernel/usr/include is absent from this repo, so the"
		echo "         AF_XDP variant cannot build here. This is true of upstream too and"
		echo "         is unrelated to the launcher. Supply those kernel uapi headers to"
		echo "         build it."
	else
		# the groovy object has to go: the two variants compile it with different flags
		rm -f support/groovy/groovy.cpp.o support/groovy/phantom.cpp.o
		make _AF_XDP=1
		cp MiSTer_groovy_XDP "$REPO/build_output/MiSTer_phantom_XDP"
		echo "-> build_output/MiSTer_phantom_XDP"
	fi
fi

say "check"
for f in "$REPO"/build_output/MiSTer_phantom*; do
	printf '%s\n' "$f"
	file "$f" | sed 's/^/    /'
	# The launcher is the point of this build; if it is not linked in, say so loudly.
	# grep reads the binary directly: piping strings into grep -q makes grep exit first,
	# strings take SIGPIPE, and pipefail turn a passing check into a failing one.
	if grep -qa "DISCOVER_PHANTOM" "$f"; then
		echo "    launcher: present"
	else
		echo "    launcher: MISSING - the build produced a stock Groovy binary"
		exit 1
	fi
done
