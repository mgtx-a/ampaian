#include "dragdrop.h"

#include "anim.h"
#include "layout.h"

#include <shobjidl.h>

namespace am {

namespace {

bool g_dragging = false;

LRESULT CALLBACK GhostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

/// The photo that follows the pointer while dragging, since the system drag shows only a cursor.
struct Ghost {
    HWND hwnd = nullptr;
    Surface surf;
    Mips thumb;
    double w = 0, h = 0;                // photo, DIPs
    POINT grabOffset{};                 // pointer minus the ghost's top left, px
    POINT origin{};                     // top left when the drag started, px
    Anim t;
    bool flyingBack = false, done = false;
    POINT backFrom{};
    std::function<void()> finished;

    static constexpr double kPad = 14;  // room for the shadow, DIPs

    bool Create(const DragStart& d)
    {
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = GhostProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"AmpaianGhost";
            RegisterClassW(&wc);
            registered = true;
        }
        thumb = d.thumb;
        w = d.photoW; h = d.photoH;
        double s = Scale();
        origin.x = LONG(std::lround(d.photoCenterPx.x - w * s / 2));
        origin.y = LONG(std::lround(d.photoCenterPx.y - h * s / 2));
        POINT cur = CursorPos();
        grabOffset = { cur.x - origin.x, cur.y - origin.y };
        hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"AmpaianGhost", L"", WS_POPUP,
                               -10000, -10000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) return false;
        surf.Resize(int(std::lround((w + 2 * kPad) * s)), int(std::lround((h + 2 * kPad) * s)));
        MoveTo(origin);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        return true;
    }

    void MoveTo(POINT topLeftPx)
    {
        ID2D1DCRenderTarget* rt = surf.Begin();
        if (!rt) return;
        D2D1_RECT_F r = D2D1::RectF(float(kPad), float(kPad), float(kPad + w), float(kPad + h));
        surf.DrawShadow(r, 12, 16, 0, 6, 0.30f);
        surf.FillMips(thumb, r, 12, 0.92f);
        surf.End();
        double s = Scale();
        surf.Present(hwnd, int(topLeftPx.x - std::lround(kPad * s)), int(topLeftPx.y - std::lround(kPad * s)));
    }

    void Follow()
    {
        POINT c = CursorPos();
        MoveTo({ c.x - grabOffset.x, c.y - grabOffset.y });
    }

    void FlyBack(std::function<void()> done)
    {
        POINT c = CursorPos();
        backFrom = { c.x - grabOffset.x, c.y - grabOffset.y };
        flyingBack = true;
        finished = std::move(done);
        t.Set(0);
        t.Ease(1, 0.25, Curve::EaseOut);
    }

    bool Step()
    {
        if (!flyingBack) return true;
        double k = t.value;
        MoveTo({ LONG(backFrom.x + (origin.x - backFrom.x) * k), LONG(backFrom.y + (origin.y - backFrom.y) * k) });
        if (!t.Active() && k >= 1) {
            if (finished) { auto f = std::move(finished); f(); }
            return false;
        }
        return true;
    }

    ~Ghost() { if (hwnd) DestroyWindow(hwnd); }
};

std::vector<std::unique_ptr<Ghost>>& Ghosts()
{
    static std::vector<std::unique_ptr<Ghost>> g;
    return g;
}

class DropSource : public IDropSource {
    LONG ref_ = 1;
public:
    Ghost* ghost = nullptr;
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropSource) { *ppv = static_cast<IDropSource*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ULONG(InterlockedIncrement(&ref_)); }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = ULONG(InterlockedDecrement(&ref_)); if (!r) delete this; return r; }
    STDMETHODIMP QueryContinueDrag(BOOL esc, DWORD keys) override
    {
        if (esc) return DRAGDROP_S_CANCEL;
        if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD) override
    {
        if (ghost) ghost->Follow();
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }
};

}  // namespace

bool GrabDragging() { return g_dragging; }

void RunFileDrag(const DragStart& start, std::function<void(DragEffect)> finished)
{
    // The shell's own data object for the file: every kind of destination understands it.
    Com<IShellItem> item;
    Com<IDataObject> data;
    if (FAILED(SHCreateItemFromParsingName(start.path.c_str(), nullptr, IID_PPV_ARGS(item.Put()))) ||
        FAILED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(data.Put())))) {
        Log(L"drag: no data object for %s", start.path.c_str());
        finished(DragEffect::None);
        return;
    }

    auto ghostOwner = std::make_unique<Ghost>();
    Ghost* ghost = ghostOwner->Create(start) ? ghostOwner.get() : nullptr;
    auto* src = new DropSource();
    src->ghost = ghost;

    g_dragging = true;
    BeginModal();
    DWORD effect = DROPEFFECT_NONE;
    HRESULT hr = DoDragDrop(data.Get(), src, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
    EndModal();
    g_dragging = false;
    src->Release();

    DragEffect result = DragEffect::None;
    if (hr == DRAGDROP_S_DROP) {
        if (effect & DROPEFFECT_MOVE) result = DragEffect::Move;
        else if (effect & DROPEFFECT_COPY) result = DragEffect::Copy;
    }

    if (result == DragEffect::None && ghost) {
        // Released where nothing accepts it: it flies back to the line.
        ghost->FlyBack([finished] { finished(DragEffect::None); });
        Ghosts().push_back(std::move(ghostOwner));
        return;
    }
    ghostOwner.reset();
    finished(result);
}

bool DragGhostsFrame(double)
{
    auto& list = Ghosts();
    for (size_t i = 0; i < list.size();) {
        if (!list[i]->Step()) list.erase(list.begin() + long(i));
        else i++;
    }
    return !list.empty();
}

bool DragGhostsActive() { return !Ghosts().empty(); }

}  // namespace am
