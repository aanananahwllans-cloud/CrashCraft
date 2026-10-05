#!/bin/bash
# CrashCraft setup: fetches c1 (the Crash Bandicoot port to C) at the tested commit, applies the
# CrashCraft patch, adds CrashCraft's host sources and, if you give it your disc image, copies the
# level files from it.
#
#   bash setup.sh ["path/to/your/Crash Bandicoot disc.(cue|bin|zip)"]
#
# Needs git and Python 3 (py or python3). Building needs MSYS2, see README.md.
set -e
cd "$(dirname "$0")"
C1_REPO=https://github.com/wurlyfox/c1
C1_COMMIT=256fdcef59f15a190290cc19db3fa9a707843b69

if [ ! -d c1/.git ]; then
	echo "== cloning c1"
	git clone "$C1_REPO" c1
fi
echo "== c1 at $C1_COMMIT + CrashCraft patch"
git -C c1 checkout -q -f "$C1_COMMIT"
git -C c1 clean -q -fd src
git -C c1 apply --ignore-whitespace --whitespace=nowarn ../patches/c1-crashcraft.patch
cp -r host/src/. c1/src/
cp protocol/crashcraft_protocol.h c1/src/pc/crashcraft/

if [ -n "$1" ]; then
	echo "== level files from your disc"
	PY=$(command -v py >/dev/null && echo "py -3" || echo python3)
	$PY tools/extract_disc.py "$1"
fi
echo "== done. Next: bash build_c1.sh   (and see README.md for the Minecraft side)"
