// Ampaian (native): shared helpers. No framework: plain Win32 + Direct2D.
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <objbase.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace am {

using std::wstring;

/// Minimal COM smart pointer.
template <class T>
class Com {
    T* p_ = nullptr;
public:
    Com() = default;
    Com(T* p) : p_(p) {}
    Com(const Com& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~Com() { Reset(); }
    Com& operator=(Com o) { std::swap(p_, o.p_); return *this; }
    T* operator->() const { return p_; }
    T* Get() const { return p_; }
    T** Put() { Reset(); return &p_; }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    explicit operator bool() const { return p_ != nullptr; }
};

// ---- time, strings, files
double Now();                                   // seconds, monotonic
wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view wide);
bool FileExists(const wstring& path);
bool DirExists(const wstring& path);
wstring FileName(const wstring& path);
wstring Extension(const wstring& path);         // lower case, with the dot
wstring JoinPath(const wstring& a, const wstring& b);
wstring ParentDir(const wstring& path);
bool SamePath(const wstring& a, const wstring& b);   // case-insensitive
void EnsureDir(const wstring& path);
bool ReadAllBytes(const wstring& path, std::vector<uint8_t>& out);
bool WriteAllBytes(const wstring& path, const void* data, size_t size);

// ---- log, environment, folders
void Log(const wchar_t* fmt, ...);
wstring EnvVar(const wchar_t* name);
bool TestMode();                                // AMPAIAN_DATA is set: isolated copy
bool NoTray();                                  // AMPAIAN_NOTRAY=1
wstring DataDir();                              // %APPDATA%\Ampaian (or AMPAIAN_DATA)
wstring InboxDir();                             // where clipboard-only captures are kept
wstring WatchDir();                             // the Screenshots folder (or AMPAIAN_WATCH)
double Scale();                                 // system DPI / 96
wstring KnownFolder(REFKNOWNFOLDERID id);        // empty when Windows has none

// ---- monitors
struct Mon {
    HMONITOR handle = nullptr;
    RECT bounds{}, work{};
};
std::vector<Mon> Monitors();
POINT CursorPos();
const Mon* MonitorAt(const std::vector<Mon>& all, POINT p);
Mon UnderPointer();

// ---- settings: the same settings.json the C# version writes
struct Settings {
    std::vector<wstring> paths;
    bool soundOff = false, clipboardOff = false, welcomed = false;
    bool installAsked = false;   // the downloaded exe offered to install itself
    bool cornerFree = true;   // the top corners (window buttons, menus, tabs) do not bring the line down
    static Settings Load();
    void Save() const;
};

// ---- application services used across modules (defined in app.cpp / dragdrop.cpp)
HWND MessageWindow();        // the hidden window that owns menus, the tray icon and the hotkey
void BeginModal();           // keep animating inside menus and drags (their loops bypass ours)
void EndModal();
bool GrabDragging();         // an OS drag is in progress

// ---- run something on the UI thread from any thread
void PostToUi(std::function<void()> fn);

// ---- a tiny scheduler: do something later, on the UI thread
void After(double seconds, std::function<void()> fn);
double NextDueIn();          // seconds until the next scheduled job, or -1
void RunDue();               // run what is due (call from the message loop)

struct RectD {
    double x = 0, y = 0, w = 0, h = 0;
    double right() const { return x + w; }
    double bottom() const { return y + h; }
    double midX() const { return x + w / 2; }
    double midY() const { return y + h / 2; }
};

}  // namespace am
