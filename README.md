# Ampaian (native)

A tiny native Windows app: screenshots hang from a clothesline at the top of the screen. Same behaviour and animations
as the C# version, but written in C++20 against plain Win32 + Direct2D. No runtime, no framework.

## Download
**[Download Ampaian.exe](https://github.com/mgtx-a/ampaian/releases/latest/download/Ampaian.exe)** (latest version, Windows 10 / 11, 64-bit).
No installer: just run it. Release notes and older versions are on the [Releases page](https://github.com/mgtx-a/ampaian/releases).

Windows SmartScreen may warn that the app is unrecognised because the exe is not signed: choose **More info → Run anyway**.

## Privacy & permissions
Ampaian makes **no network connections**: no telemetry, no update checks, nothing is uploaded anywhere.
Everything it touches stays on your PC:

- **Clipboard:** watched for new screenshots (Win+Shift+S, PrtSc). Ordinary copies of text, files or pictures from apps are ignored. Can be turned off in the tray menu.
- **Your Screenshots folder:** watched for new pictures (Win+PrtSc).
- **`%LOCALAPPDATA%\Ampaian\Screenshots`:** screenshots that only came from the clipboard are saved here so they can hang on the line.
- **`%APPDATA%\Ampaian`:** `settings.json` and a small `log.txt` (kept under 200 KB) for troubleshooting.
- **Registry:** one value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, only when you tick "Open at login" (untick it to remove it).
- **Hotkey:** Ctrl+Alt+T, to show and hide the line.

Files are only moved, saved to the Desktop or sent to the Recycle Bin when you choose that from a photo's menu.

## Verify the download
Release builds are made by [GitHub Actions](.github/workflows/build.yml) from the public source, not on a personal PC,
and come with a signed build provenance attestation. With the [GitHub CLI](https://cli.github.com) you can check
that your `Ampaian.exe` was built by this repository's workflow:

    gh attestation verify Ampaian.exe --repo mgtx-a/ampaian

Each release also lists the exe's SHA-256 checksum (`Get-FileHash Ampaian.exe` in PowerShell).

## Details
- **Size:** ~0.5 MB exe (C# self-contained: 145 MB)
- **Memory (private working set):** idle ~1 MB, one 4K photo ~11 MB, nine 4K photos ~22 MB (C#: 42 / 86 / 133 MB)
- Settings are compatible with the C# build (`%APPDATA%\Ampaian\settings.json`).

## Build
Needs a C++20 MinGW-style toolchain; the script uses Zig:

    ./build.sh [outdir]

Or with Visual Studio 2019+ (the "Desktop development with C++" workload); the exe lands next to the script by default:

    build-msvc.bat [outdir]
