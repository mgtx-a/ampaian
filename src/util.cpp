#include "common.h"

#include <cstdarg>

namespace am {

double Now()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

wstring Widen(std::string_view s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(std::wstring_view w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool FileExists(const wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

wstring FileName(const wstring& p)
{
    size_t i = p.find_last_of(L"\\/");
    return i == wstring::npos ? p : p.substr(i + 1);
}

wstring Extension(const wstring& p)
{
    wstring n = FileName(p);
    size_t i = n.find_last_of(L'.');
    if (i == wstring::npos) return {};
    wstring e = n.substr(i);
    for (auto& c : e) c = wchar_t(towlower(c));
    return e;
}

wstring JoinPath(const wstring& a, const wstring& b)
{
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

wstring ParentDir(const wstring& p)
{
    size_t i = p.find_last_of(L"\\/");
    return i == wstring::npos ? wstring() : p.substr(0, i);
}

bool SamePath(const wstring& a, const wstring& b)
{
    return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

void EnsureDir(const wstring& p)
{
    if (p.empty() || DirExists(p)) return;
    EnsureDir(ParentDir(p));
    CreateDirectoryW(p.c_str(), nullptr);
}

bool ReadAllBytes(const wstring& path, std::vector<uint8_t>& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out.resize(size_t(size.QuadPart));
    DWORD got = 0;
    bool ok = out.empty() || ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr);
    CloseHandle(h);
    return ok && got == out.size();
}

bool WriteAllBytes(const wstring& path, const void* data, size_t size)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data, DWORD(size), &put, nullptr) && put == size;
    CloseHandle(h);
    return ok;
}

wstring EnvVar(const wchar_t* name)
{
    wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    return n > 0 && n < 2048 ? wstring(buf, n) : wstring();
}

bool TestMode() { static bool on = !EnvVar(L"AMPAIAN_DATA").empty(); return on; }
bool NoTray() { static bool on = EnvVar(L"AMPAIAN_NOTRAY") == L"1"; return on; }

wstring KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    return r;
}

wstring DataDir()
{
    static wstring d = [] {
        wstring t = EnvVar(L"AMPAIAN_DATA");
        return t.empty() ? JoinPath(KnownFolder(FOLDERID_RoamingAppData), L"Ampaian") : t;
    }();
    return d;
}

wstring InboxDir()
{
    static wstring d = [] {
        if (TestMode()) return JoinPath(DataDir(), L"inbox");
        return JoinPath(JoinPath(KnownFolder(FOLDERID_LocalAppData), L"Ampaian"), L"Screenshots");
    }();
    return d;
}

wstring WatchDir()
{
    static wstring d = [] {
        wstring forced = EnvVar(L"AMPAIAN_WATCH");
        if (!forced.empty()) { EnsureDir(forced); return forced; }
        // FOLDERID_Screenshots {b7bede81-df94-4682-a7d8-57a52620b86f}: where Win+PrtSc saves.
        static const GUID screenshots = { 0xb7bede81, 0xdf94, 0x4682, { 0xa7, 0xd8, 0x57, 0xa5, 0x26, 0x20, 0xb8, 0x6f } };
        wstring p = KnownFolder(screenshots);
        if (p.empty()) p = JoinPath(KnownFolder(FOLDERID_Pictures), L"Screenshots");
        EnsureDir(p);
        return p;
    }();
    return d;
}

double Scale()
{
    static double s = [] {
        using Fn = UINT(WINAPI*)();
        auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
        return fn ? fn() / 96.0 : 1.0;
    }();
    return s;
}

void Log(const wchar_t* fmt, ...)
{
    static double lastPurge = 0;
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(msg, 1023, fmt, ap);
    va_end(ap);
    msg[1023] = 0;

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1200];
    _snwprintf(line, 1199, L"%04d-%02d-%02dT%02d:%02d:%02d %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, msg);
    line[1199] = 0;
    std::string utf8 = Narrow(line);

    EnsureDir(DataDir());
    wstring file = JoinPath(DataDir(), L"log.txt");
    (void)lastPurge;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 200000) DeleteFileW(file.c_str());   // keep it small
    HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD put;
    WriteFile(h, utf8.data(), DWORD(utf8.size()), &put, nullptr);
    CloseHandle(h);
}

// ---------------------------------------------------------------- scheduler

namespace {
struct Job { double due; uint64_t seq; std::function<void()> fn; };
std::vector<Job>& Jobs() { static std::vector<Job> j; return j; }
uint64_t g_seq = 0;
}

void After(double seconds, std::function<void()> fn)
{
    Jobs().push_back({ Now() + seconds, g_seq++, std::move(fn) });
}

double NextDueIn()
{
    auto& j = Jobs();
    if (j.empty()) return -1;
    double best = j[0].due;
    for (auto& x : j) best = std::min(best, x.due);
    return std::max(0.0, best - Now());
}

void RunDue()
{
    auto& j = Jobs();
    for (;;) {
        double now = Now();
        size_t pick = j.size();
        for (size_t i = 0; i < j.size(); i++)
            if (j[i].due <= now && (pick == j.size() || j[i].due < j[pick].due || (j[i].due == j[pick].due && j[i].seq < j[pick].seq))) pick = i;
        if (pick == j.size()) return;
        auto fn = std::move(j[pick].fn);
        j.erase(j.begin() + pick);
        fn();   // may schedule more
    }
}

// ---------------------------------------------------------------- monitors

std::vector<Mon> Monitors()
{
    std::vector<Mon> out;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfoW(h, &mi)) {
                Mon m;
                m.handle = h; m.bounds = mi.rcMonitor; m.work = mi.rcWork;
                reinterpret_cast<std::vector<Mon>*>(lp)->push_back(m);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&out));
    return out;
}

POINT CursorPos()
{
    POINT p{};
    GetCursorPos(&p);
    return p;
}

const Mon* MonitorAt(const std::vector<Mon>& all, POINT p)
{
    for (auto& m : all)
        if (p.x >= m.bounds.left && p.x < m.bounds.right && p.y >= m.bounds.top && p.y < m.bounds.bottom) return &m;
    return nullptr;
}

Mon UnderPointer()
{
    auto all = Monitors();
    if (auto* m = MonitorAt(all, CursorPos())) return *m;
    for (auto& m : all) if (m.bounds.left == 0 && m.bounds.top == 0) return m;
    return all.empty() ? Mon{} : all[0];
}

// ---------------------------------------------------------------- settings.json (same file as the C# version)

static void AppendJsonString(std::string& out, const wstring& s)
{
    out += '"';
    for (wchar_t c : s) {
        if (c == L'\\') out += "\\\\";
        else if (c == L'"') out += "\\\"";
        else if (c < 0x20 || c > 0x7e) { char b[8]; snprintf(b, sizeof b, "\\u%04x", unsigned(c)); out += b; }
        else out += char(c);
    }
    out += '"';
}

void Settings::Save() const
{
    std::string j = "{\"Paths\":[";
    for (size_t i = 0; i < paths.size(); i++) {
        if (i) j += ',';
        AppendJsonString(j, paths[i]);
    }
    j += "],\"SoundOff\":";      j += soundOff ? "true" : "false";
    j += ",\"ClipboardOff\":";   j += clipboardOff ? "true" : "false";
    j += ",\"CornerFree\":";   j += cornerFree ? "true" : "false";
    j += ",\"Welcomed\":";       j += welcomed ? "true" : "false";
    j += ",\"InstallAsked\":";   j += installAsked ? "true" : "false";
    j += "}";
    EnsureDir(DataDir());
    WriteAllBytes(JoinPath(DataDir(), L"settings.json"), j.data(), j.size());
}

static bool ParseBool(const std::string& j, const char* key, bool fallback)
{
    size_t k = j.find(std::string("\"") + key + "\"");
    if (k == std::string::npos) return fallback;
    size_t colon = j.find(':', k);
    if (colon == std::string::npos) return fallback;
    size_t v = j.find_first_not_of(" \t\r\n", colon + 1);
    if (v == std::string::npos) return fallback;
    return j.compare(v, 4, "true") == 0 ? true : (j.compare(v, 5, "false") == 0 ? false : fallback);
}

Settings Settings::Load()
{
    Settings s;
    std::vector<uint8_t> bytes;
    if (!ReadAllBytes(JoinPath(DataDir(), L"settings.json"), bytes) || bytes.empty()) return s;
    size_t skip = bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? 3 : 0;   // BOM
    std::string j(reinterpret_cast<const char*>(bytes.data()) + skip, bytes.size() - skip);
    s.soundOff = ParseBool(j, "SoundOff", false);
    s.clipboardOff = ParseBool(j, "ClipboardOff", false);
    s.cornerFree = ParseBool(j, "CornerFree", true);
    s.welcomed = ParseBool(j, "Welcomed", false);
    s.installAsked = ParseBool(j, "InstallAsked", false);

    size_t k = j.find("\"Paths\"");
    if (k == std::string::npos) return s;
    size_t open = j.find('[', k);
    if (open == std::string::npos) return s;
    size_t i = open + 1;
    while (i < j.size()) {
        while (i < j.size() && (j[i] == ' ' || j[i] == ',' || j[i] == '\n' || j[i] == '\r' || j[i] == '\t')) i++;
        if (i >= j.size() || j[i] == ']') break;
        if (j[i] != '"') break;
        i++;
        wstring cur;
        while (i < j.size() && j[i] != '"') {
            if (j[i] == '\\' && i + 1 < j.size()) {
                char e = j[i + 1];
                if (e == 'u' && i + 5 < j.size()) { cur += wchar_t(strtoul(j.substr(i + 2, 4).c_str(), nullptr, 16)); i += 6; continue; }
                cur += wchar_t(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e);
                i += 2;
            } else {
                // raw UTF-8 byte sequences: collect and widen
                size_t st = i;
                while (i < j.size() && j[i] != '"' && j[i] != '\\') i++;
                cur += Widen(std::string_view(j).substr(st, i - st));
            }
        }
        i++;
        s.paths.push_back(cur);
    }
    return s;
}

}  // namespace am
