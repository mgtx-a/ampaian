// The application: when the line shows and hides, where screenshots come from,
// the tray icon, the hotkey and the main loop. A port of the C# AppController.
#include "anim.h"
#include "clip.h"
#include "dragdrop.h"
#include "flight.h"
#include "gfx.h"
#include "guide.h"
#include "layout.h"
#include "markup.h"
#include "model.h"
#include "panel.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>

namespace am {

namespace {

constexpr UINT WM_RUN = WM_APP + 1;
constexpr UINT WM_TRAY = WM_APP + 2;
constexpr UINT_PTR TIMER_MODAL = 99;
constexpr int HOTKEY_ID = 1;

constexpr double RevealDelay = 0.25;     // how long the pointer rests on the top edge
constexpr double RetractDelay = 0.5;     // how long it is away before the line tucks up
constexpr double CornerWidth = 200;      // top-right corner kept free for the window buttons, in points
constexpr double TopBand = 8;            // the hot strip along the top edge, in points
constexpr double OtherSourceWindow = 8;  // the same capture from the clipboard and from a file
constexpr double SameSourceWindow = 2;

HWND g_msg = nullptr;
std::mutex g_uiMutex;
std::vector<std::function<void()>> g_uiQueue;
int g_modalDepth = 0;

// ---------------------------------------------------------------- full screen detection, in the background

struct FullScreenWatcher {
    std::mutex m;
    std::vector<std::pair<HMONITOR, bool>> state;
    std::atomic<bool> stop{ false };
    std::thread th;
    bool lastAny = false;

    static wstring ProcessName(HWND w)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!p) return L"";
        wchar_t path[MAX_PATH] = L"";
        DWORD n = MAX_PATH;
        if (!QueryFullProcessImageNameW(p, 0, path, &n)) path[0] = 0;
        CloseHandle(p);
        wstring name = FileName(path);
        for (auto& c : name) c = wchar_t(towlower(c));
        return name;
    }

    /// Screenshot overlays cover the whole screen while you snip, and on Windows 10 the
    /// snipping overlay is often only cloaked afterwards, so it stays "in front" until
    /// something else is clicked. None of that is a full screen app.
    static bool IsCaptureOverlay(HWND w)
    {
        BOOL cloaked = FALSE;
        if (!IsWindowVisible(w) || IsIconic(w)) return true;
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return true;
        static const wchar_t* overlays[] = { L"screenclippinghost.exe", L"snippingtool.exe", L"screensketch.exe", L"shellexperiencehost.exe" };
        wstring name = ProcessName(w);
        for (auto* o : overlays) if (name == o) return true;
        return false;
    }

    void Poll()
    {
        int st = 0;
        bool busy = SHQueryUserNotificationState(reinterpret_cast<QUERY_USER_NOTIFICATION_STATE*>(&st)) == S_OK && (st == 2 || st == 3 || st == 4);   // busy, D3D full screen, presentation
        std::vector<std::pair<HMONITOR, bool>> now;
        HWND fg = GetForegroundWindow();
        wchar_t cls[64] = L"";
        if (fg) GetClassNameW(fg, cls, 64);
        bool shell = !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd");
        if (busy && fg && !shell && IsCaptureOverlay(fg)) busy = false;
        RECT wr{};
        bool haveRect = fg && GetWindowRect(fg, &wr);
        for (auto& mon : Monitors()) {
            bool on = false;
            if (busy && fg && !shell && haveRect && MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) == mon.handle) {
                // The app in front must really cover the whole screen: Windows reports "busy" in other situations too.
                on = wr.left <= mon.bounds.left + 1 && wr.top <= mon.bounds.top + 1 && wr.right >= mon.bounds.right - 1 && wr.bottom >= mon.bounds.bottom - 1;
            }
            now.push_back({ mon.handle, on });
        }
        bool any = false;
        for (auto& p : now) any = any || p.second;
        if (any != lastAny) {
            lastAny = any;
            // The top edge ignores the pointer while this is on, so say which app caused it.
            if (any) Log(L"full screen app detected: %s (%s)", ProcessName(fg).c_str(), cls);
            else Log(L"full screen app gone");
        }
        std::lock_guard<std::mutex> lock(m);
        state = std::move(now);
    }

    void Start()
    {
        Poll();
        th = std::thread([this] {
            while (!stop) { Sleep(250); Poll(); }
        });
    }

    bool Active(HMONITOR h)
    {
        std::lock_guard<std::mutex> lock(m);
        for (auto& p : state) if (p.first == h) return p.second;
        return false;
    }
};

// ---------------------------------------------------------------- watching the screenshots folder

struct FolderWatcher {
    HANDLE dir = INVALID_HANDLE_VALUE;
    HANDLE ev = nullptr;
    OVERLAPPED ov{};
    alignas(DWORD) uint8_t buf[4096]{};
    wstring folder;
    std::unordered_set<wstring> known;     // lower-case paths already seen
    FILETIME launch{};
    int retries = 0;
    uint32_t scanToken = 0;
    std::function<void(const wstring&)> onNew;
    std::function<void()> onChange;

    static wstring Lower(wstring s) { for (auto& c : s) c = wchar_t(towlower(c)); return s; }

    static bool IsImage(const wstring& name)
    {
        static const wchar_t* exts[] = { L".png", L".jpg", L".jpeg", L".bmp", L".tif", L".tiff", L".gif", L".webp", L".heic" };
        wstring e = Extension(name);
        for (auto* x : exts) if (e == x) return true;
        return false;
    }

    struct Entry { wstring path; FILETIME created; };

    std::vector<Entry> Listing() const
    {
        std::vector<Entry> out;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(JoinPath(folder, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;
        do {
            if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) || !IsImage(fd.cFileName)) continue;
            out.push_back({ JoinPath(folder, fd.cFileName), fd.ftCreationTime });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return CompareFileTime(&a.created, &b.created) < 0; });
        return out;
    }

    static bool Ready(const wstring& path)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0;
        CloseHandle(h);
        return ok;
    }

    bool Offer(const wstring& path)
    {
        if (!Ready(path)) return false;
        known.insert(Lower(path));
        if (onNew) onNew(path);
        return true;
    }

    void Arm()
    {
        ResetEvent(ev);
        memset(&ov, 0, sizeof ov);
        ov.hEvent = ev;
        ReadDirectoryChangesW(dir, buf, sizeof buf, FALSE,
                              FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE, nullptr, &ov, nullptr);
    }

    void Start(const wstring& path)
    {
        folder = path;
        GetSystemTimeAsFileTime(&launch);
        // Anything created after the app launched counts as new, even if it landed before the watcher was ready.
        for (auto& e : Listing()) {
            if (CompareFileTime(&e.created, &launch) < 0) known.insert(Lower(e.path));
        }
        for (auto& e : Listing()) if (!known.count(Lower(e.path))) Offer(e.path);

        dir = CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (dir == INVALID_HANDLE_VALUE) { Log(L"cannot watch %s", folder.c_str()); return; }
        ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Arm();
    }

    void OnSignal()
    {
        DWORD got = 0;
        GetOverlappedResult(dir, &ov, &got, FALSE);
        Arm();
        ScheduleScan(0.2);   // Windows writes the file in steps: give it a moment
    }

    void ScheduleScan(double delay)
    {
        uint32_t token = ++scanToken;
        After(delay, [this, token] { if (token == scanToken) Scan(); });
    }

    void Scan()
    {
        bool waiting = false;
        for (auto& e : Listing()) {
            if (known.count(Lower(e.path))) continue;
            if (!Offer(e.path)) waiting = true;
        }
        // A file Windows is still writing is offered again in a moment.
        if (waiting && retries++ < 12) ScheduleScan(0.35);
        else retries = 0;
        if (onChange) onChange();
    }
};

struct RecentCapture {
    double at;
    int w, h;
    std::vector<uint8_t> sig;
    bool fromClipboard;
};

// ---------------------------------------------------------------- the app

struct App {
    Settings settings;
    Line line;
    Panel panel;
    HINSTANCE inst = nullptr;
    FolderWatcher watcher;
    FullScreenWatcher fullscreen;
    NOTIFYICONDATAW nid{};
    bool trayAdded = false;

    // When the line shows and hides.
    bool isPresent = false;     // the panel window is up; it can still be tucked above the top edge
    bool isRevealed = false;    // the line has slid down into view
    bool pinned = false;        // opened on purpose: stays down until the pointer has visited and left
    bool wanted = false;        // the line should be up, if nothing prevents it
    bool keepOpen = false;      // opened on purpose, so it stays up while empty
    bool userOpened = false;    // shown even over a full screen app
    bool pendingPeek = false;   // a photo arrived while the line could not be shown
    bool lastBlocked = false;
    double peekUntil = 0, hotZoneSince = -1, awaySince = -1, lastInBand = -1e9, lastRecheck = 0;
    int lastLiveCount = 0;
    std::optional<Mon> pendingScreen;
    bool tracking = false;
    double nextTick = 0, nextFrame = 0, lastFrame = 0;
    std::vector<Mon> monCache;
    double monCacheAt = -1e9;

    // Captures.
    double ignoreClipboardUntil = 0;
    uint32_t clipToken = 0;
    std::vector<RecentCapture> recent;
    RecentCapture lastClip{};   // the picture the clipboard last held, while nothing else was copied
    std::unordered_map<uint32_t, Mips> pendingFlight;

    App() : line(settings), panel(line) {}

    // ------------------------------------------------------------ start

    bool Start(HINSTANCE hInst)
    {
        inst = hInst;
        settings = Settings::Load();
        panel.Create(inst);
        panel.PlaceOnScreen(UnderPointer());
        UpdateCapacity();

        line.onItemsChanged = [this](bool added) { panel.Sync(false); ItemsChanged(added); };
        line.onState = [this] { panel.OnLineState(); };
        line.onGust = [this] { panel.Breeze(); };
        line.onFall = [this](Pegged& p) { Fall(p); };
        line.onEdited = [this] { if (isPresent) Reveal(false, 2.5); };
        line.onOwnClipboardWrite = [this] { ignoreClipboardUntil = Now() + 4; };
        panel.beginDrag = [this](Card& c, POINT) { BeginDrag(c); };

        fullscreen.Start();
        watcher.onNew = [this](const wstring& p) { HangCapture(p, false); };
        watcher.onChange = [this] { line.Prune(); };
        watcher.Start(WatchDir());

        if (!settings.clipboardOff || true) AddClipboardFormatListener(g_msg);
        if (!TestMode()) {
            if (!RegisterHotKey(g_msg, HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'T'))
                Log(L"Ctrl+Alt+T is taken by another app (the tray icon and the top edge still work)");
        }
        if (!NoTray()) SetUpTray();
        Log(L"started (native): Windows, scale %.2f, %d screen(s), screenshots folder %s, clipboard capture %s", Scale(), int(Monitors().size()),
            watcher.folder.c_str(), settings.clipboardOff ? L"off" : L"on");
        if (TestMode()) WatchMemory();

        // Photos restored from last time hang again, quietly.
        RestorePhotos(line.SavedPaths());

        if (!settings.welcomed) {
            settings.welcomed = true;
            settings.Save();
            if (!TestMode() || _wgetenv(L"AMPAIAN_GUIDE")) BeginTour();
        }
        return true;
    }

    void BeginTour()
    {
        TourHooks h;
        h.liveCount = [this] { return line.LiveCount(); };
        h.revealed = [this] { return isRevealed; };
        h.copied = [this] { return line.copiedId != 0; };
        h.markupOpen = [] { return MarkupIsOpen(); };
        StartTour(std::move(h));
    }

    /// Photos hang in the order they were saved, so they are decoded one after the other.
    void RestorePhotos(std::vector<wstring> paths)
    {
        if (paths.empty()) return;
        std::thread([this, paths = std::move(paths)] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            struct Restored { wstring path; Mips m; int w = 0, h = 0; };
            std::vector<Restored> out;
            for (auto& p : paths) {
                Restored r;
                r.path = p;
                r.m = LoadMips(p, 480, &r.w, &r.h);
                out.push_back(std::move(r));
            }
            CoUninitialize();
            PostToUi([this, out = std::move(out)] {
                for (auto& r : out) if (!r.m.empty()) line.Hang(r.path, r.m, r.w, r.h, false, true);
            });
        }).detach();
    }

    void Shutdown()
    {
        fullscreen.stop = true;
        if (fullscreen.th.joinable()) fullscreen.th.join();
        if (trayAdded) Shell_NotifyIconW(NIM_DELETE, &nid);
        UnregisterHotKey(g_msg, HOTKEY_ID);
        RemoveClipboardFormatListener(g_msg);
    }

    void WatchMemory()
    {
        // Test mode only: a memory line in the log every two seconds.
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
        Log(L"mem private=%llu ws=%llu animating=%d", (unsigned long long)(pmc.PrivateUsage >> 20), (unsigned long long)(pmc.WorkingSetSize >> 20), AnyAnimating() ? 1 : 0);
        After(2, [this] { WatchMemory(); });
    }

    // ------------------------------------------------------------ screens

    const std::vector<Mon>& CachedScreens(double now)
    {
        if (now - monCacheAt > 1) { monCache = Monitors(); monCacheAt = now; }
        return monCache;
    }

    void UpdateCapacity()
    {
        double usable = panel.widthDip() - 200;
        line.maxItems = std::max(3, std::min(12, int(usable / L::Spacing)));
    }

    // ------------------------------------------------------------ showing and hiding

    void ItemsChanged(bool added)
    {
        int live = line.LiveCount();
        if (added || live > lastLiveCount) {
            panel.PlaceOnScreen(pendingScreen ? *pendingScreen : UnderPointer());
            pendingScreen.reset();
            UpdateCapacity();
            panel.Sync(true);
            wanted = true;
            Refresh();
            if (isPresent) Reveal(false, 2.5); else pendingPeek = true;
        } else if (live == 0 && !keepOpen) {
            After(0.7, [this] {
                if (line.LiveCount() != 0 || keepOpen) return;
                wanted = false;
                Refresh();
            });
        }
        lastLiveCount = live;
    }

    /// Decides whether the panel is shown at all: something to show, and no full screen app on that screen.
    void Refresh()
    {
        Mon screen = panel.screen().handle ? panel.screen() : UnderPointer();
        bool blocked = !userOpened && fullscreen.Active(screen.handle);
        if (blocked != lastBlocked) {
            lastBlocked = blocked;
            Log(blocked ? L"a full screen app is in front: the line waits" : L"no full screen app in front");
        }
        if (wanted && !blocked) Present(); else Dismiss();
        // The pointer is watched while there is a line, even tucked away, to notice it pushing against the top edge.
        tracking = wanted;
    }

    void Present()
    {
        if (isPresent) return;
        isPresent = true;
        panel.PlaceOnScreen(panel.screen());
        panel.Sync(true);
        panel.Show(true);
        panel.SetClickThrough(true);
    }

    void Dismiss()
    {
        if (!isPresent) return;
        isPresent = false;
        SetRevealed(false);
        After(0.4, [this] { if (!isPresent) panel.Show(false); });
    }

    void Reveal(bool pin = false, double peekFor = 0)
    {
        if (!isPresent) return;
        if (pin) pinned = true;
        if (peekFor > 0) peekUntil = Now() + peekFor;
        awaySince = -1;
        SetRevealed(true);
    }

    void SetRevealed(bool on)
    {
        if (on == isRevealed) return;
        isRevealed = on;
        panel.SetRevealed(on);
        panel.SetClickThrough(!on);
        if (!on) {
            pinned = false;
            userOpened = false;
            peekUntil = 0;
        }
    }

    void Toggle()
    {
        // An empty line has nothing to show, so it stays hidden.
        if (!isRevealed && line.LiveCount() == 0) return;
        if (isRevealed) {
            SetRevealed(false);
            if (line.LiveCount() == 0) {
                keepOpen = false;
                wanted = false;
                Refresh();
            }
        } else {
            keepOpen = true;
            wanted = true;
            userOpened = true;
            panel.PlaceOnScreen(UnderPointer());
            UpdateCapacity();
            Refresh();
            Reveal(true);
        }
    }

    // ------------------------------------------------------------ the pointer

    void Tick()
    {
        double now = Now();
        POINT mouse = CursorPos();
        // While the line could not be shown (a full screen app), look again now and then.
        if (wanted && !isPresent && now - lastRecheck >= 0.5) {
            lastRecheck = now;
            Refresh();
            if (isPresent && pendingPeek) { pendingPeek = false; Reveal(false, 2.5); }
        }
        const auto& screens = CachedScreens(now);
        const Mon* under = MonitorAt(screens, mouse);
        // Windows has no menu bar: the hot strip is the top edge of the screen.
        bool atTopEdge = under && mouse.y < under->bounds.top + LONG(std::lround(TopBand * Scale()));
        // Minimise / maximise / close live in the top-right corner: reaching for them must not drop the line.
        if (atTopEdge && settings.cornerFree && mouse.x >= under->bounds.right - LONG(std::lround(CornerWidth * Scale()))) atTopEdge = false;
        bool buttonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

        if (!isRevealed) {
            // Resting on the top edge brings the line down on that screen. Pushing against the edge is
            // part of it. Dragging a window up there does not count.
            bool inBand = under && atTopEdge && !buttonDown && line.LiveCount() > 0 && !fullscreen.Active(under->handle);
            if (inBand) {
                lastInBand = now;
                if (hotZoneSince < 0) hotZoneSince = now;
                if (now - hotZoneSince >= RevealDelay) {
                    hotZoneSince = -1;
                    if (panel.screen().handle != under->handle) {
                        panel.PlaceOnScreen(*under);
                        UpdateCapacity();
                    }
                    Refresh();
                    Reveal();
                }
            } else if (now - lastInBand > 0.15) {
                hotZoneSince = -1;   // a hand is never perfectly still: a short slip does not start the wait over
            }
            return;
        }

        // The line's zone runs from its lowest point up to the top of the screen, so moving up never hides it.
        RECT f = panel.pixelFrame();
        RECT zone{ f.left, panel.screen().bounds.top, f.right, f.bottom };
        bool inside = PtInRect(&zone, mouse) != 0;
        if (inside && pinned) pinned = false;

        bool busy = pinned || GrabDragging() || line.pressedId != 0 || line.menuOpen || now < peekUntil;
        if (inside || busy) {
            awaySince = -1;
        } else {
            if (awaySince < 0) awaySince = now;
            if (now - awaySince >= RetractDelay) { awaySince = -1; SetRevealed(false); }
        }
        panel.UpdateHover();
    }

    // ------------------------------------------------------------ captures

    /// Windows keeps no record of where on screen a capture was taken, so it is guessed:
    /// a capture the size of a screen flies up from that whole screen; anything smaller
    /// from just above and left of the pointer, where a snip drag usually ends.
    std::optional<RectD> GuessCaptureRect(int w, int h)
    {
        auto screens = Monitors();
        for (auto& m : screens) {
            if (std::abs((m.bounds.right - m.bounds.left) - w) < 2 && std::abs((m.bounds.bottom - m.bounds.top) - h) < 2)
                return RectD{ double(m.bounds.left), double(m.bounds.top), double(m.bounds.right - m.bounds.left), double(m.bounds.bottom - m.bounds.top) };
        }
        if (screens.size() > 1) {
            RECT all{ INT_MAX, INT_MAX, INT_MIN, INT_MIN };
            for (auto& m : screens) {
                all.left = std::min(all.left, m.bounds.left); all.top = std::min(all.top, m.bounds.top);
                all.right = std::max(all.right, m.bounds.right); all.bottom = std::max(all.bottom, m.bounds.bottom);
            }
            if (std::abs((all.right - all.left) - w) < 2 && std::abs((all.bottom - all.top) - h) < 2)
                return RectD{ double(all.left), double(all.top), double(all.right - all.left), double(all.bottom - all.top) };
        }
        POINT cursor = CursorPos();
        const Mon* screen = MonitorAt(screens, cursor);
        if (!screen) return std::nullopt;
        double sw = screen->bounds.right - screen->bounds.left, sh = screen->bounds.bottom - screen->bounds.top;
        double rw = std::min<double>(w, sw), rh = std::min<double>(h, sh);
        double x = std::clamp<double>(cursor.x - rw, screen->bounds.left, screen->bounds.right - rw);
        double y = std::clamp<double>(cursor.y - rh, screen->bounds.top, screen->bounds.bottom - rh);
        return RectD{ x, y, rw, rh };
    }

    /// The same capture arriving from two sources (Windows 11's Snipping Tool saves a file
    /// and also puts the picture on the clipboard) is one screenshot, hung once. From the
    /// same source only an instant repeat counts, so taking the same screen twice still hangs twice.
    bool IsDuplicate(const std::vector<uint8_t>& sig, int w, int h, bool fromClipboard)
    {
        double now = Now();
        recent.erase(std::remove_if(recent.begin(), recent.end(), [&](auto& r) { return now - r.at > OtherSourceWindow; }), recent.end());
        for (auto& r : recent) {
            if (r.w != w || r.h != h || r.sig.size() != sig.size()) continue;
            if (r.fromClipboard == fromClipboard && now - r.at > SameSourceWindow) continue;
            long total = 0;
            int worst = 0, count = 0;
            for (size_t i = 0; i + 3 < sig.size(); i += 4) {
                for (int k = 0; k < 3; k++) {   // colour only
                    int d = std::abs(int(r.sig[i + size_t(k)]) - int(sig[i + size_t(k)]));
                    total += d;
                    worst = std::max(worst, d);
                    count++;
                }
            }
            if (count && worst <= 10 && double(total) / count < 1.5) {
                Log(L"same screenshot arrived twice, hung once");
                return true;
            }
        }
        recent.push_back({ now, w, h, sig, fromClipboard });
        return false;
    }

    /// A capture tool that quits hands its clipboard over to Windows, which announces the
    /// same picture again, sometimes minutes later. Still the same clipboard: not a new screenshot.
    bool IsClipboardRepeat(const std::vector<uint8_t>& sig, int w, int h)
    {
        bool same = lastClip.w == w && lastClip.h == h && lastClip.sig.size() == sig.size();
        for (size_t i = 0; same && i < sig.size(); i++)
            if (i % 4 != 3 && std::abs(int(lastClip.sig[i]) - int(sig[i])) > 2) same = false;
        lastClip = { Now(), w, h, sig, true };
        if (same) Log(L"the clipboard announced the same picture again, not hung");
        return same;
    }

    void HangCapture(const wstring& path, bool fromClipboard)
    {
        int w = 0, h = 0;
        if (!ImageSize(path, &w, &h)) { Log(L"could not read the picture, so it was not hung: %s", FileName(path).c_str()); return; }
        std::optional<RectD> from = GuessCaptureRect(w, h);
        std::optional<Mon> screenOfCapture;
        if (from) {
            POINT center{ LONG(from->midX()), LONG(from->midY()) };
            auto all = Monitors();
            if (const Mon* m = MonitorAt(all, center)) screenOfCapture = *m;
        }
        int flightPx = from ? int(std::clamp(std::max(from->w, from->h), 400.0, 3000.0)) : 0;

        // The decoding runs off the UI thread, so the line never stutters.
        std::thread([this, path, fromClipboard, from, screenOfCapture, flightPx, w, h] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::vector<uint8_t> sig;
            int sw = w, sh = h;
            bool haveSig = LoadSignature(path, sig, &sw, &sh);
            Mips thumb = LoadMips(path, 480);
            Mips flight;
            if (flightPx > 0 && !thumb.empty()) flight = LoadMips(path, flightPx);
            CoUninitialize();
            PostToUi([=, this] {
                if (thumb.empty()) { Log(L"could not read the picture, so it was not hung: %s", FileName(path).c_str()); return; }
                if (haveSig && ((fromClipboard && IsClipboardRepeat(sig, w, h)) || IsDuplicate(sig, w, h, fromClipboard))) {
                    // The copy we saved ourselves is not needed; a file in the Screenshots folder is left alone.
                    if (Inbox::Contains(path)) DeleteFileW(path.c_str());
                    return;
                }
                if (screenOfCapture) pendingScreen = *screenOfCapture;
                Pegged* item = line.Hang(path, thumb, w, h, from.has_value(), false);
                if (!item || !from) return;
                uint32_t id = item->id;
                pendingFlight[id] = flight;
                RectD fromRect = *from;
                // Let the line come down and lay out before measuring the landing spot.
                After(0.03, [this, id, fromRect] { Fly(id, fromRect); });
            });
        }).detach();
    }

    void Fly(uint32_t id, RectD from)
    {
        Pegged* item = line.Find(id);
        RectD to;
        if (!isPresent || !isRevealed || !item || !panel.CardFrame(id, &to)) {
            line.Land(id);
            pendingFlight.erase(id);
            return;
        }
        Mips img = pendingFlight[id];
        pendingFlight.erase(id);
        if (img.empty()) img = item->thumb;
        FlightFly(img, from, to, item->tilt, [this, id] { line.Land(id); });
    }

    /// A discarded card falls over the whole screen, from where it hangs.
    void Fall(Pegged& p)
    {
        RectD frame;
        if (!isPresent || !isRevealed || p.flying || !panel.CardFrame(p.id, &frame)) return;
        FlightFall(p.thumb, frame, p.tilt);
    }

    void BeginDrag(Card& c)
    {
        Pegged* it = c.item;
        if (!it) return;
        RectD frame;
        if (!panel.CardFrame(it->id, &frame)) return;
        L::Size ps = L::PhotoSize(it->thumb.width(), it->thumb.height());
        DragStart d;
        d.path = it->path;
        d.thumb = it->thumb;
        d.photoW = ps.w; d.photoH = ps.h;
        d.photoCenterPx = { LONG(frame.midX()), LONG(frame.midY()) };
        uint32_t id = it->id;
        line.draggingId = id;
        line.RaiseState();
        auto finish = [this, id](DragEffect eff) {
            line.draggingId = 0;
            line.RaiseState();
            line.Prune();
            // The shell finishes a move a moment later. Check again then, so a photo saved into a folder leaves the line.
            if (eff == DragEffect::Move) After(0.6, [this] { line.Prune(); });
            (void)id;
        };
        RunFileDrag(d, finish);
    }

    // ------------------------------------------------------------ clipboard

    void OnClipboardUpdate()
    {
        if (settings.clipboardOff) return;
        if (Now() < ignoreClipboardUntil) { lastClip = {}; return; }   // our own copy replaced the clipboard
        uint32_t token = ++clipToken;
        // A capture tool often updates the clipboard several times in a row: the last update is read, once.
        After(0.45, [this, token] { if (token == clipToken) GrabClipboard(0); });
    }

    void GrabClipboard(int attempt)
    {
        ClipImage ci = InspectClipboard();
        if (ci == ClipImage::Busy) {
            if (attempt < 6) After(0.12, [this, attempt] { GrabClipboard(attempt + 1); });
            return;
        }
        if (ci != ClipImage::Bare) {
            lastClip = {};   // something else was copied in between
            return;
        }
        wstring path = Inbox::NewPath();
        if (!SaveClipboardImage(path)) {
            DeleteFileW(path.c_str());
            if (attempt < 6) After(0.12, [this, attempt] { GrabClipboard(attempt + 1); });
            else Log(L"clipboard: could not read the picture");
            return;
        }
        int w = 0, h = 0;
        if (!ImageSize(path, &w, &h) || w < 8 || h < 8) { DeleteFileW(path.c_str()); return; }
        HangCapture(path, true);
    }

    // ------------------------------------------------------------ tray and menus

    static bool AutoStartOn()
    {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
        bool on = RegQueryValueExW(k, L"Ampaian", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        RegCloseKey(k);
        return on;
    }

    static void ToggleAutoStart()
    {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ | KEY_WRITE, &k) != ERROR_SUCCESS) return;
        if (AutoStartOn()) {
            RegDeleteValueW(k, L"Ampaian");
        } else {
            wchar_t exe[MAX_PATH * 2];
            GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
            wstring v = wstring(L"\"") + exe + L"\"";
            RegSetValueExW(k, L"Ampaian", 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(k);
    }

    void SetUpTray()
    {
        nid.cbSize = sizeof nid;
        nid.hWnd = g_msg;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy(nid.szTip, L"Ampaian");
        trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    }

    void TrayMenu()
    {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, isRevealed ? L"Hide line\tCtrl+Alt+T" : L"Show line\tCtrl+Alt+T");
        AppendMenuW(m, MF_STRING | (line.LiveCount() > 0 ? 0 : MF_GRAYED), 2, L"Take everything down");
        AppendMenuW(m, MF_STRING | (settings.clipboardOff ? 0 : MF_CHECKED), 3, L"Hang clipboard screenshots");
        AppendMenuW(m, MF_STRING, 4, L"Open screenshots folder");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (line.SoundOn() ? MF_CHECKED : 0), 5, L"Sounds");
        AppendMenuW(m, MF_STRING | (AutoStartOn() ? MF_CHECKED : 0), 6, L"Open at login");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (settings.cornerFree ? MF_CHECKED : 0), 9, L"Keep top-right corner free");
        AppendMenuW(m, MF_STRING, 8, L"Take the tour");
        AppendMenuW(m, MF_STRING, 7, L"Quit Ampaian");
        POINT p = CursorPos();
        BeginModal();
        SetForegroundWindow(g_msg);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_BOTTOMALIGN, p.x, p.y, 0, g_msg, nullptr);
        EndModal();
        DestroyMenu(m);
        switch (cmd) {
            case 1: Toggle(); break;
            case 2: line.Clear(); break;
            case 3: settings.clipboardOff = !settings.clipboardOff; settings.Save(); break;
            case 4: ShellExecuteW(nullptr, L"open", watcher.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
            case 5: settings.soundOff = !settings.soundOff; settings.Save(); break;
            case 6: ToggleAutoStart(); break;
            case 7: PostQuitMessage(0); break;
            case 8: BeginTour(); break;
            case 9: settings.cornerFree = !settings.cornerFree; settings.Save(); break;
        }
    }

    // ------------------------------------------------------------ frames and the loop

    bool NeedFrame() const { return AnyAnimating() || FlightsActive() || DragGhostsActive() || panel.dirty(); }

    void Frame()
    {
        double now = Now();
        double dt = lastFrame > 0 ? std::min(0.05, now - lastFrame) : 1.0 / 60;
        lastFrame = now;
        bool moved = false;
        StepAll(dt, &moved);
        FlightsFrame(dt);
        DragGhostsFrame(dt);
        panel.Draw(moved);
    }

    // Frame statistics: logged for animations that ran at least a second (test mode: always).
    int statFrames = 0;
    double statStart = 0, statLastReport = 0;

    void Pump()
    {
        RunDue();
        double now = Now();
        if (tracking && now >= nextTick) { nextTick = now + 1.0 / 30; Tick(); }
        if (NeedFrame()) {
            if (now >= nextFrame) {
                if (statFrames == 0) statStart = now;
                statFrames++;
                nextFrame = now + 1.0 / 62;
                Frame();
            }
        } else {
            lastFrame = 0;
            if (statFrames > 0) {
                double secs = now - statStart, fps = statFrames / std::max(secs, 0.001);
                if (secs >= 1.0 && (TestMode() || (fps < 45 && now - statLastReport > 60))) {
                    statLastReport = now;
                    Log(L"animation ran at %.0f frames per second (%d frames in %.1f s)", fps, statFrames, secs);
                }
                statFrames = 0;
            }
        }
    }

    int Run()
    {
        timeBeginPeriod(1);
        MSG msg;
        for (;;) {
            double timeout = -1;
            double now = Now();
            auto want = [&](double secs) { timeout = timeout < 0 ? secs : std::min(timeout, secs); };
            if (NeedFrame()) want(std::max(0.0, nextFrame - now));
            if (tracking) want(std::max(0.0, nextTick - now));
            double due = NextDueIn();
            if (due >= 0) want(due);

            HANDLE handles[1] = { watcher.ev };
            DWORD n = watcher.ev ? 1 : 0;
            DWORD ms = timeout < 0 ? INFINITE : DWORD(std::ceil(timeout * 1000));
            DWORD r = MsgWaitForMultipleObjectsEx(n, handles, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (n && r == WAIT_OBJECT_0) watcher.OnSignal();

            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { timeEndPeriod(1); return int(msg.wParam); }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Pump();
        }
    }
};

App* g_app = nullptr;

LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
        case WM_RUN: {
            std::vector<std::function<void()>> q;
            { std::lock_guard<std::mutex> lock(g_uiMutex); q.swap(g_uiQueue); }
            for (auto& f : q) f();
            return 0;
        }
        case WM_HOTKEY:
            if (g_app && w == HOTKEY_ID) g_app->Toggle();
            return 0;
        case WM_CLIPBOARDUPDATE:
            if (g_app) g_app->OnClipboardUpdate();
            return 0;
        case WM_TRAY:
            if (g_app) {
                if (LOWORD(l) == WM_LBUTTONUP) g_app->Toggle();
                else if (LOWORD(l) == WM_RBUTTONUP) g_app->TrayMenu();
            }
            return 0;
        case WM_TIMER:
            if (w == TIMER_MODAL && g_app) g_app->Pump();   // menus and drags run their own loops
            return 0;
        case WM_DISPLAYCHANGE:
            if (g_app) {
                g_app->monCacheAt = -1e9;
                g_app->panel.PlaceOnScreen(UnderPointer());
                g_app->UpdateCapacity();
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

HWND MessageWindow() { return g_msg; }

void BeginModal()
{
    if (g_modalDepth++ == 0) SetTimer(g_msg, TIMER_MODAL, 8, nullptr);
}

void EndModal()
{
    if (--g_modalDepth == 0) KillTimer(g_msg, TIMER_MODAL);
}

void PostToUi(std::function<void()> fn)
{
    { std::lock_guard<std::mutex> lock(g_uiMutex); g_uiQueue.push_back(std::move(fn)); }
    if (g_msg) PostMessageW(g_msg, WM_RUN, 0, 0);
}

}  // namespace am

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
    using namespace am;
    wstring lockName = L"Ampaian.SingleInstance";
    if (TestMode()) lockName += L".Test." + std::to_wstring(std::hash<wstring>{}(DataDir()) % 1000000);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, lockName.c_str());
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    OleInitialize(nullptr);   // apartment-threaded COM, and drag and drop
    if (!GfxInit()) { MessageBoxW(nullptr, L"Ampaian could not start its graphics.", L"Ampaian", MB_ICONERROR); return 1; }

    WNDCLASSW wc{};
    wc.lpfnWndProc = MsgProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"AmpaianMessages";
    RegisterClassW(&wc);
    g_msg = CreateWindowExW(0, wc.lpszClassName, L"Ampaian", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);

    App app;
    g_app = &app;
    app.Start(inst);
    int rc = app.Run();
    app.Shutdown();
    g_app = nullptr;
    OleUninitialize();
    return rc;
}
