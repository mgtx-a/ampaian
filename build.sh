#!/usr/bin/env bash
# Builds Ampaian (native) with the portable Zig toolchain on D:. Usage: ./build.sh [out-dir]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ZIG=/d/native-build/toolchain/zig-x86_64-windows-0.17.0/zig.exe
OUT="${1:-/d/native-build/out}"
mkdir -p "$OUT"
cd "$HERE"
"$ZIG" c++ -O2 -std=c++20 -target x86_64-windows-gnu -fms-extensions -Wno-deprecated-declarations \
  -Wl,--subsystem,windows -s \
  src/*.cpp res/app.rc -o "$OUT/Ampaian.exe" \
  -ld2d1 -ldwrite -lwindowscodecs -lole32 -loleaut32 -lshell32 -lshlwapi -luser32 -lgdi32 -lwinmm -ladvapi32 -luuid -lpsapi -ldwmapi
ls -la "$OUT/Ampaian.exe" | awk '{printf "%.0f KB  %s\n", $5/1024, $9}'
