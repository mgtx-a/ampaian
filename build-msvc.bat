@echo off
rem Builds Ampaian (native) with Visual Studio's C++ compiler. Usage: build-msvc.bat [out-dir]
rem Needs Visual Studio 2019 or later (or the Build Tools) with the "Desktop development with C++" workload.
setlocal
set "HERE=%~dp0"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%HERE%."
set "OBJ=%TEMP%\ampaian-msvc-obj"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo Visual Studio not found. & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio has no C++ compiler installed. & exit /b 1)

rem vcvars looks for vswhere on PATH.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"

rc /nologo /fo "%OBJ%\app.res" "%HERE%res\app.rc" || exit /b 1
rem /MT (the default) puts the C runtime inside the exe, so nothing needs installing.
cl /nologo /std:c++20 /O2 /EHsc /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /W1 ^
   "%HERE%src\*.cpp" "%OBJ%\app.res" /Fo"%OBJ%\\" /Fe"%OUT%\Ampaian.exe" ^
   /link /SUBSYSTEM:WINDOWS ^
   d2d1.lib dwrite.lib windowscodecs.lib ole32.lib oleaut32.lib shell32.lib shlwapi.lib user32.lib gdi32.lib winmm.lib advapi32.lib uuid.lib psapi.lib dwmapi.lib ^
   || exit /b 1

for %%f in ("%OUT%\Ampaian.exe") do echo %%~zf bytes  %%~ff
