# Ampaian (native)

A tiny native Windows app: screenshots hang from a clothesline at the top of the screen. Same behaviour and animations
as the C# version, but written in C++20 against plain Win32 + Direct2D. No runtime, no framework.

## Download
**[Download Ampaian.exe](https://github.com/mgtx-a/ampaian/releases/latest/download/Ampaian.exe)** (latest version, Windows 10 / 11, 64-bit).
No installer: just run it. Release notes and older versions are on the [Releases page](https://github.com/mgtx-a/ampaian/releases).

Windows SmartScreen may warn that the app is unrecognised because the exe is not signed: choose **More info → Run anyway**.

## Details
- **Size:** ~0.5 MB exe (C# self-contained: 145 MB)
- **Memory (private working set):** idle ~1 MB, one 4K photo ~11 MB, nine 4K photos ~22 MB (C#: 42 / 86 / 133 MB)
- Settings are compatible with the C# build (`%APPDATA%\Ampaian\settings.json`).

## Build
Needs a C++20 MinGW-style toolchain; the script uses Zig:

    ./build.sh [outdir]
