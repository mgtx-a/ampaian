#include "model.h"

#include "clip.h"
#include "markup.h"

namespace am {

// ---------------------------------------------------------------- inbox

namespace Inbox {

bool Contains(const wstring& path)
{
    wstring dir = InboxDir();
    wchar_t full[MAX_PATH * 2], fullDir[MAX_PATH * 2];
    if (!GetFullPathNameW(path.c_str(), MAX_PATH * 2, full, nullptr)) return false;
    if (!GetFullPathNameW(dir.c_str(), MAX_PATH * 2, fullDir, nullptr)) return false;
    wstring prefix = wstring(fullDir);
    while (!prefix.empty() && prefix.back() == L'\\') prefix.pop_back();
    prefix += L'\\';
    wstring f(full);
    return f.size() > prefix.size() && CompareStringOrdinal(f.c_str(), int(prefix.size()), prefix.c_str(), int(prefix.size()), TRUE) == CSTR_EQUAL;
}

wstring Unique(const wstring& folder, const wstring& name)
{
    wstring base = name, ext;
    size_t dot = name.find_last_of(L'.');
    if (dot != wstring::npos) { base = name.substr(0, dot); ext = name.substr(dot); }
    wstring candidate = JoinPath(folder, name);
    for (int n = 2; FileExists(candidate); n++) candidate = JoinPath(folder, base + L" " + std::to_wstring(n) + ext);
    return candidate;
}

wstring NewPath()
{
    EnsureDir(InboxDir());
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[64];
    swprintf(stamp, 64, L"Screenshot %04d-%02d-%02d at %02d.%02d.%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return Unique(InboxDir(), stamp);
}

}  // namespace Inbox

// ---------------------------------------------------------------- sounds: short synthesized tones, no audio files

namespace Sound {
namespace {

std::vector<uint8_t> Build(double volume, double seconds, const std::function<double(double)>& wave)
{
    const int rate = 22050;
    int n = int(rate * seconds);
    std::vector<uint8_t> b(44 + n * 2);
    auto put32 = [&](size_t at, uint32_t v) { memcpy(&b[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { memcpy(&b[at], &v, 2); };
    memcpy(&b[0], "RIFF", 4); put32(4, 36 + n * 2); memcpy(&b[8], "WAVEfmt ", 8);
    put32(16, 16); put16(20, 1); put16(22, 1); put32(24, rate); put32(28, rate * 2); put16(32, 2); put16(34, 16);
    memcpy(&b[36], "data", 4); put32(40, n * 2);
    for (int i = 0; i < n; i++) {
        double v = std::clamp(wave(i / double(rate)) * volume * 0.6, -1.0, 1.0);
        put16(44 + i * 2, uint16_t(int16_t(v * 32767)));
    }
    return b;
}

const std::vector<uint8_t>& TinkData()
{
    static auto d = Build(0.35, 0.16, [](double t) {
        return (std::sin(2 * M_PI * 2300 * t) + 0.5 * std::sin(2 * M_PI * 3450 * t)) * std::exp(-t * 34);
    });
    return d;
}

const std::vector<uint8_t>& PopData()
{
    static auto d = Build(0.25, 0.10, [](double t) { return std::sin(2 * M_PI * (520 - 2200 * t) * t) * std::exp(-t * 38); });
    return d;
}

}  // namespace

void Tink(bool on) { if (on) PlaySoundW(reinterpret_cast<LPCWSTR>(TinkData().data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT); }
void Pop(bool on) { if (on) PlaySoundW(reinterpret_cast<LPCWSTR>(PopData().data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT); }

}  // namespace Sound

// ---------------------------------------------------------------- line

Line::Line(Settings& s) : settings(s)
{
    ScheduleGust();
}

int Line::LiveCount() const
{
    int n = 0;
    for (auto& i : items) if (!i->falling) n++;
    return n;
}

Pegged* Line::Find(uint32_t id)
{
    for (auto& i : items) if (i->id == id) return i.get();
    return nullptr;
}

Pegged* Line::FindByPath(const wstring& path)
{
    for (auto& i : items) if (!i->falling && SamePath(i->path, path)) return i.get();
    return nullptr;
}

Pegged* Line::Hang(const wstring& path, Mips thumb, int origW, int origH, bool flying, bool quietly)
{
    if (FindByPath(path) || thumb.empty()) return nullptr;
    static std::mt19937 rng{ std::random_device{}() };
    auto item = std::make_unique<Pegged>();
    item->id = nextId_++;
    item->path = path;
    item->thumb = std::move(thumb);
    item->origW = origW; item->origH = origH;
    item->tilt = std::uniform_real_distribution<double>(-2.5, 2.5)(rng);
    item->flying = flying;
    Pegged* raw = item.get();
    items.push_back(std::move(item));
    if (onItemsChanged) onItemsChanged(true);

    // A full line lets the oldest photo fall off the far end.
    while (LiveCount() > maxItems) {
        Pegged* oldest = nullptr;
        for (auto& i : items) if (!i->falling) { oldest = i.get(); break; }
        if (!oldest || oldest == raw) break;
        Drop(oldest->id, true);
    }
    Save();
    if (!quietly) Sound::Tink(SoundOn());
    return raw;
}

void Line::Land(uint32_t id)
{
    if (Pegged* p = Find(id)) {
        p->flying = false;
        if (onItemsChanged) onItemsChanged(false);
    }
}

void Line::Drop(uint32_t id, bool quietly)
{
    Pegged* p = Find(id);
    if (!p || p->falling) return;
    if (onFall) onFall(*p);
    p->falling = true;
    Save();
    if (!quietly) Sound::Pop(SoundOn());
    if (onItemsChanged) onItemsChanged(false);
    After(0.6, [this, id] {
        items.erase(std::remove_if(items.begin(), items.end(), [id](auto& i) { return i->id == id; }), items.end());
        if (onItemsChanged) onItemsChanged(false);
    });
}

void Line::Clear()
{
    std::vector<uint32_t> live;
    for (auto& i : items) if (!i->falling) live.push_back(i->id);
    for (size_t n = 0; n < live.size(); n++) {
        uint32_t id = live[n];
        if (n == 0) Drop(id);
        else After(0.06 * double(n), [this, id] { Drop(id, true); });
    }
}

void Line::Prune()
{
    std::vector<uint32_t> gone;
    for (auto& i : items) if (!i->falling && !FileExists(i->path)) gone.push_back(i->id);
    for (uint32_t id : gone) Drop(id, true);
}

void Line::Copy(uint32_t id)
{
    Pegged* p = Find(id);
    if (!p) return;
    wstring path = p->path;
    copiedId = id;
    RaiseState();
    After(1.2, [this, id] { if (copiedId == id) { copiedId = 0; RaiseState(); } });
    if (onOwnClipboardWrite) onOwnClipboardWrite();
    // Decoding a full screenshot takes a moment: off the UI thread, so the line keeps moving.
    WriteClipboardAsync(path, [this](bool ok) {
        if (onOwnClipboardWrite) onOwnClipboardWrite();
        if (!ok) { Log(L"copy failed: the clipboard stayed busy"); MessageBeep(MB_ICONASTERISK); }
    });
}

void Line::Open(uint32_t id)
{
    if (Pegged* p = Find(id)) ShellExecuteW(nullptr, L"open", p->path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Line::Trash(uint32_t id)
{
    Pegged* p = Find(id);
    if (!p) return;
    wstring from = p->path;
    from.push_back(L'\0');   // the shell wants a double-null-terminated list
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) {
        Drop(id, true);
        Sound::Pop(SoundOn());
    } else {
        Log(L"could not recycle %s", p->path.c_str());
        MessageBeep(MB_ICONASTERISK);
    }
}

bool Line::IsInInbox(uint32_t id)
{
    Pegged* p = Find(id);
    return p && Inbox::Contains(p->path);
}

void Line::Discard(uint32_t id)
{
    if (IsInInbox(id)) Trash(id); else Drop(id);
}

void Line::SaveToDesktop(uint32_t id)
{
    Pegged* p = Find(id);
    if (!p) return;
    PWSTR desk = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk)) || !desk) return;
    wstring target = Inbox::Unique(desk, FileName(p->path));
    CoTaskMemFree(desk);
    if (MoveFileW(p->path.c_str(), target.c_str())) Drop(id, true);
    else { Log(L"could not save to Desktop"); MessageBeep(MB_ICONASTERISK); }
}

void Line::Reveal(uint32_t id)
{
    Pegged* p = Find(id);
    if (!p) return;
    wstring args = L"/select,\"" + p->path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void Line::Markup(uint32_t id)
{
    Pegged* p = Find(id);
    if (!p) return;
    OpenMarkup(p->path, [this](const wstring& path) {
        // The picture was written: load its new thumbnail, then show it.
        int w = 0, h = 0;
        Mips m = LoadMips(path, 480, &w, &h);
        ReloadThumb(path, std::move(m), w, h);
        if (onEdited) onEdited();
    });
}

void Line::ReloadThumb(const wstring& path, Mips thumb, int w, int h)
{
    Pegged* p = FindByPath(path);
    if (!p || thumb.empty()) return;
    p->thumb = std::move(thumb);
    p->origW = w; p->origH = h;
    if (onItemsChanged) onItemsChanged(false);
}

void Line::ScheduleGust()
{
    // Every so often a little wind moves the line: the detail that makes it feel
    // like an object and not a widget.
    static std::mt19937 rng{ std::random_device{}() };
    After(std::uniform_real_distribution<double>(7, 16)(rng), [this] {
        if (!items.empty() && draggingId == 0 && onGust) onGust();
        ScheduleGust();
    });
}

void Line::Save()
{
    settings.paths.clear();
    for (auto& i : items) if (!i->falling) settings.paths.push_back(i->path);
    settings.Save();
}

std::vector<wstring> Line::SavedPaths() const
{
    std::vector<wstring> out;
    for (auto& p : settings.paths) if (FileExists(p)) out.push_back(p);
    return out;
}

}  // namespace am
