#include "install.h"

namespace am {

static const wchar_t* RunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static wstring ExePath()
{
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    return wstring(exe, n);
}

/// Started through a short path (C:\Users\MEGAT~1.AFH\...) the same folder reads differently.
static wstring LongPath(const wstring& p)
{
    wchar_t b[MAX_PATH * 2];
    DWORD n = GetLongPathNameW(p.c_str(), b, MAX_PATH * 2);
    return n && n < MAX_PATH * 2 ? wstring(b, n) : p;
}

static wstring StartMenuLink()
{
    wstring p = KnownFolder(FOLDERID_Programs);
    return p.empty() ? wstring() : JoinPath(p, L"Ampaian.lnk");
}

wstring InstallDir()
{
    wstring p = KnownFolder(FOLDERID_UserProgramFiles);
    return p.empty() ? wstring() : JoinPath(p, L"Ampaian");
}

bool RunningInstalled()
{
    wstring dir = InstallDir();
    return !dir.empty() && SamePath(LongPath(ParentDir(ExePath())), LongPath(dir));
}

bool AutoStartOn()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(k, L"Ampaian", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

static void SetAutoStart(bool on, const wstring& exe)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RunKey, 0, KEY_READ | KEY_WRITE, &k) != ERROR_SUCCESS) return;
    if (!on) {
        RegDeleteValueW(k, L"Ampaian");
    } else {
        wstring v = L"\"" + exe + L"\"";
        RegSetValueExW(k, L"Ampaian", 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(k);
}

void SetAutoStart(bool on) { SetAutoStart(on, ExePath()); }

static void MakeShortcut(const wstring& link, const wstring& target)
{
    Com<IShellLinkW> sl;
    Com<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(sl.Put())))) return;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(ParentDir(target).c_str());
    sl->SetDescription(L"Screenshots hang on a line at the top of the screen");
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(file.Put())))) file->Save(link.c_str(), TRUE);
}

/// Copies this exe into the apps folder, adds the shortcut and the start at sign-in.
/// Returns the installed exe, or empty when the copy failed.
static wstring CopyIn()
{
    wstring dir = InstallDir();
    if (dir.empty()) return L"";
    wstring dest = JoinPath(dir, L"Ampaian.exe");
    EnsureDir(dir);
    if (!CopyFileW(ExePath().c_str(), dest.c_str(), FALSE)) {
        Log(L"could not install to %s (error %lu)", dest.c_str(), GetLastError());
        MessageBoxW(nullptr, L"Ampaian could not be copied to your apps folder, so it keeps running from here.", L"Ampaian", MB_ICONWARNING);
        return L"";
    }
    wstring link = StartMenuLink();
    if (!link.empty()) MakeShortcut(link, dest);
    SetAutoStart(true, dest);
    Log(L"installed to %s", dest.c_str());
    return dest;
}

static bool AskInstall()
{
    return MessageBoxW(nullptr,
        L"Keep Ampaian on this PC?\n\n"
        L"It will be copied to your apps folder, added to the Start menu and opened each time you sign in "
        L"(you can turn that off in the tray menu). No admin rights are needed.\n\n"
        L"Choose No to just run it from here.",
        L"Ampaian", MB_YESNO | MB_ICONQUESTION) == IDYES;
}

wstring OfferInstall()
{
    if (TestMode() || RunningInstalled()) return L"";
    Settings s = Settings::Load();
    if (s.installAsked) return L"";   // asked once; the tray menu still offers it
    s.installAsked = true;
    s.Save();
    return AskInstall() ? CopyIn() : L"";
}

wstring Install()
{
    return AskInstall() ? CopyIn() : L"";
}

bool Uninstall()
{
    wstring dir = InstallDir();
    if (!RunningInstalled()) return false;   // never removes a folder this exe is not in
    if (MessageBoxW(nullptr,
            L"Remove Ampaian from this PC?\n\n"
            L"The app, its Start menu shortcut and the start at sign-in are removed. "
            L"Your screenshots and settings are kept.",
            L"Ampaian", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return false;
    SetAutoStart(false);
    wstring link = StartMenuLink();
    if (!link.empty()) DeleteFileW(link.c_str());
    // A running exe cannot delete itself: a hidden cmd removes the folder once this one has quit.
    wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + dir + L"\"";
    wstring cwd = ParentDir(dir);
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    Log(L"uninstalled from %s", dir.c_str());
    return true;
}

}  // namespace am
