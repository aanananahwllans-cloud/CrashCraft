#!/bin/bash
# Build c1 with CrashCraft (c1/c1.exe) inside MSYS2's 32-bit MinGW toolchain.
#   bash build_c1.sh [make args...]          e.g.  bash build_c1.sh clean
# MSYS2_ROOT: where MSYS2 is installed (default C:/msys64; local.env can set it). Run setup.sh first.
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -f "$HERE/local.env" ] && . "$HERE/local.env"
MSYS2_ROOT="${MSYS2_ROOT:-C:/msys64}"
ROOT="$(cd "$HERE" && (pwd -W 2>/dev/null || pwd))"
mkdir -p "$HERE/_tmp"
export MSYSTEM=MINGW32 TMP="$ROOT/_tmp" TEMP="$ROOT/_tmp" TMPDIR="$ROOT/_tmp"
exec "$MSYS2_ROOT/usr/bin/bash.exe" -lc "
set -e
export PATH=/mingw32/bin:/usr/bin:\$PATH TMP='$ROOT/_tmp' TEMP='$ROOT/_tmp'
cd '$ROOT/c1'
make -C src/ext/lib/cimgui -j\$(nproc) CXX=g++ CXXFLAGS='-g -O1 -m32 -I/mingw32/include/SDL2'
make -j\$(nproc) CC='gcc -std=gnu11' OUT=c1.exe LDLIBS='-lmingw32 -lSDL2main -lSDL2 -lopengl32 -lfluidsynth-3 -lm -lstdc++ -lws2_32' $*
"
