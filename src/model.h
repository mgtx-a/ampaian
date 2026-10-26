// The line: what hangs on it and what you can do with each photo.
// The files never move. The line is only a view onto them.
#pragma once
#include "common.h"
#include "gfx.h"

namespace am {

/// One screenshot hanging on the line.
struct Pegged {
    uint32_t id = 0;
    wstring path;
    Mips thumb;
    int origW = 0, origH = 0;
    double tilt = 0;          // every photo hangs a little crooked
    bool falling = false;
    bool flying = false;      // still flying in: the card waits hidden
};

namespace Inbox {
bool Contains(const wstring& path);
wstring NewPath();                                   // a fresh file name in Ampaian's own folder
wstring Unique(const wstring& folder, const wstring& name);
}

namespace Sound {
void Tink(bool on);
void Pop(bool on);
}

class Line {
public:
    explicit Line(Settings& settings);

    std::vector<std::unique_ptr<Pegged>> items;
    int maxItems = 8;
    uint32_t copiedId = 0, draggingId = 0, pressedId = 0;
    bool menuOpen = false;

    // Events (set by the app).
    std::function<void(bool added)> onItemsChanged;
    std::function<void()> onState;
    std::function<void()> onGust;
    std::function<void(Pegged&)> onFall;
    std::function<void()> onEdited;
    std::function<void()> onOwnClipboardWrite;

    Settings& settings;

    int LiveCount() const;
    Pegged* Find(uint32_t id);
    Pegged* FindByPath(const wstring& path);
    bool SoundOn() const { return !settings.soundOff; }

    /// Hangs a prepared picture. Returns null if it is already on the line.
    Pegged* Hang(const wstring& path, Mips thumb, int origW, int origH, bool flying, bool quietly);
    void Land(uint32_t id);
    void Drop(uint32_t id, bool quietly = false);
    void Clear();
    void Prune();                       // pictures whose file is gone fall by themselves

    void Copy(uint32_t id);
    void Open(uint32_t id);
    void Trash(uint32_t id);            // to the Recycle Bin
    bool IsInInbox(uint32_t id);
    void Discard(uint32_t id);          // the cross and "Take down"
    void SaveToDesktop(uint32_t id);
    void Reveal(uint32_t id);           // show in Explorer
    void Markup(uint32_t id);
    void ReloadThumb(const wstring& path, Mips thumb, int w, int h);

    void RaiseState() { if (onState) onState(); }
    void Save();
    /// Paths to restore at start (those whose files still exist).
    std::vector<wstring> SavedPaths() const;

private:
    uint32_t nextId_ = 1;
    void ScheduleGust();
};

}  // namespace am
