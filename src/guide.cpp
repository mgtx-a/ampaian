// The tour card is a topmost window that never takes focus, so the screenshot tool and
// everything else keep working while it sits at the bottom of the screen.
#include "guide.h"

#include "anim.h"
#include "gfx.h"

namespace am {

namespace {

enum class Wait { Shot, Hover, Copy, Markup, Button };

struct Step {
    const wchar_t* title;
    const wchar_t* line1;
    const wchar_t* line2;
    Wait wait;
};

const Step kSteps[] = {
    { L"Take a screenshot", L"Press Win + Shift + S, then drag a box over anything on your screen.", L"It will fly up and hang on a clothesline.", Wait::Shot },
    { L"Bring the line back", L"Move your mouse to the very top edge of the screen and wait a moment.", L"(Ctrl + Alt + T also shows and hides it.)", Wait::Hover },
    { L"Copy it", L"Click the photo. It is copied, so you can paste it anywhere with Ctrl + V.", L"Drag it into a chat or folder instead to drop a copy there.", Wait::Copy },
    { L"Mark it up", L"Press and hold the photo for half a second. Enter saves it, Esc cancels.", L"Keys: P pen, H highlighter, A arrow, R box, O oval, T text, B blur, C crop.", Wait::Markup },
    { L"You're all set", L"Right-click a photo for more: open, show in Explorer, take down.", L"The tray icon has settings and \"Take the tour\" to see this again.", Wait::Button },
};
constexpr int kStepCount = int(sizeof(kSteps) / sizeof(kSteps[0]));

constexpr double kW = 560, kH = 176;
constexpr uint32_t kAccent = 0xFF3B6BE0, kInk = 0xFF2C2E38, kMuted = 0xFF6B6F80, kGreen = 0xFF2FA866;
constexpr UINT_PTR kTimer = 1;

class Tour {
public:
    TourHooks hooks;
    HWND hwnd = nullptr;
    bool Open();
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);

private:
    LRESULT Handle(UINT, WPARAM, LPARAM);
    void Poll();
    void Advance();
    void Render();
    int ShownKey() const { return step_ * 10 + (doneAt_ >= 0 ? 1 : 0); }

    Surface surf_;
    POINT pos_{};
    int step_ = 0;
    int startCount_ = 0;
    bool sawHidden_ = false;
    bool editorSeen_ = false;
    double doneAt_ = -1;            // a short "done" tick is shown before the next step
    double stepSince_ = 0;
    int lastShown_ = -1;
    D2D1_RECT_F skip_{}, next_{};
    int hover_ = 0;                 // 1 = end tour, 2 = skip / finish
    Anim appear_;
};

Tour* g_tour = nullptr;

bool Tour::Open()
{
    double s = Scale();
    Mon mon = UnderPointer();
    int w = int(kW * s + 0.5), h = int(kH * s + 0.5);
    pos_.x = mon.work.left + ((mon.work.right - mon.work.left) - w) / 2;
    pos_.y = mon.work.bottom - h - int(28 * s);
    skip_ = D2D1::RectF(24, float(kH - 24 - 32), 24 + 92, float(kH - 24));
    next_ = D2D1::RectF(float(kW - 24 - 112), float(kH - 24 - 34), float(kW - 24), float(kH - 24));

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"AmpaianTour";
        RegisterClassW(&wc);
        registered = true;
    }
    hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"AmpaianTour", L"Ampaian tour", WS_POPUP,
                           pos_.x, pos_.y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd) return false;
    surf_.Resize(w, h);
    startCount_ = hooks.liveCount ? hooks.liveCount() : 0;
    stepSince_ = Now();
    appear_.Set(0);
    appear_.Ease(1, 0.25, Curve::EaseOut);
    Render();
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, kTimer, 50, nullptr);
    return true;
}

void Tour::Advance()
{
    step_++;
    doneAt_ = -1;
    stepSince_ = Now();
    sawHidden_ = false;
    editorSeen_ = false;
    if (step_ >= kStepCount) { DestroyWindow(hwnd); return; }
    startCount_ = hooks.liveCount ? hooks.liveCount() : 0;
}

void Tour::Poll()
{
    if (doneAt_ >= 0) {                      // showing the tick, then moving on
        if (Now() - doneAt_ > 0.9) Advance();
        return;
    }
    bool met = false;
    switch (kSteps[step_].wait) {
        case Wait::Shot: met = hooks.liveCount && hooks.liveCount() > startCount_; break;
        case Wait::Hover:
            if (hooks.revealed && !hooks.revealed()) sawHidden_ = true;   // after the little peek the line tucks away
            met = sawHidden_ && hooks.revealed && hooks.revealed();
            break;
        case Wait::Copy: met = hooks.copied && hooks.copied(); break;
        case Wait::Markup: {
            // The editor takes the middle of the screen, so the card steps aside while it is open.
            bool open = hooks.markupOpen && hooks.markupOpen();
            if (open && !editorSeen_) { editorSeen_ = true; ShowWindow(hwnd, SW_HIDE); }
            if (!open && editorSeen_) { ShowWindow(hwnd, SW_SHOWNOACTIVATE); met = true; }
            break;
        }
        case Wait::Button: break;
    }
    if (met) doneAt_ = Now();
}

void Tour::Render()
{
    ID2D1DCRenderTarget* rt = surf_.Begin();
    if (!rt) return;
    float s = float(Scale());
    double a = std::clamp(appear_.value, 0.0, 1.0);
    float lift = float((1 - a) * 14);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, lift) * D2D1::Matrix3x2F::Scale(s, s));

    D2D1_RECT_F card = D2D1::RectF(0.5f, 0.5f, float(kW) - 0.5f, float(kH) - 0.5f);
    surf_.DrawShadow(card, 18, 18, 0, 6, 0.22f);
    rt->FillRoundedRectangle(D2D1::RoundedRect(card, 18, 18), surf_.Solid(0xFFF8F9FC));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(card, 18, 18), surf_.Solid(0x46000000), 1);

    for (int i = 0; i < kStepCount; i++) {   // progress dots
        float cx = float(kW - 24 - (kStepCount - 1 - i) * 14), cy = 26;
        float r = i == step_ ? 4.5f : 3.5f;
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), surf_.Solid(i < step_ ? kGreen : i == step_ ? kAccent : 0x33000000));
    }
    const Step& st = kSteps[std::min(step_, kStepCount - 1)];
    wchar_t head[64];
    swprintf(head, 64, L"STEP %d OF %d", step_ + 1, kStepCount);
    surf_.DrawText(head, 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(24, 16, 300, 36), kAccent);
    surf_.DrawText(st.title, 20, DWRITE_FONT_WEIGHT_BOLD, D2D1::RectF(24, 34, float(kW - 24), 64), kInk);
    surf_.DrawText(st.line1, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::RectF(24, 68, float(kW - 12), 90), kInk);
    surf_.DrawText(st.line2, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, D2D1::RectF(24, 90, float(kW - 12), 112), kMuted);

    bool last = kSteps[step_].wait == Wait::Button;
    if (doneAt_ >= 0) {   // that step is done
        rt->FillRoundedRectangle(D2D1::RoundedRect(next_, 17, 17), surf_.Solid(kGreen));
        surf_.DrawText(L"\x2713  Nice!", 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(next_.left + 28, next_.top + 6, next_.right, next_.bottom), 0xFFFFFFFF);
    } else {
        if (hover_ == 1) rt->FillRoundedRectangle(D2D1::RoundedRect(skip_, 16, 16), surf_.Solid(0x14000000));
        surf_.DrawText(L"End tour", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(skip_.left + 16, skip_.top + 6, skip_.right, skip_.bottom), kMuted);
        if (last) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(next_, 17, 17), surf_.Solid(hover_ == 2 ? 0xD93B6BE0 : kAccent));
            surf_.DrawText(L"Finish", 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(next_.left + 34, next_.top + 6, next_.right, next_.bottom), 0xFFFFFFFF);
        } else {
            if (hover_ == 2) rt->FillRoundedRectangle(D2D1::RoundedRect(next_, 17, 17), surf_.Solid(0x14000000));
            surf_.DrawText(L"Skip step  \x203A", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(next_.left + 16, next_.top + 7, next_.right, next_.bottom), kAccent);
        }
    }
    surf_.End();
    lastShown_ = ShownKey();
    surf_.Present(hwnd, pos_.x, pos_.y, BYTE(std::clamp(a * 255.0 + 0.5, 0.0, 255.0)));
}

LRESULT CALLBACK Tour::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE) {
        auto* self = reinterpret_cast<Tour*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd = h;
    }
    auto* self = reinterpret_cast<Tour*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, m, w, l);
    if (m == WM_NCDESTROY) {
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        g_tour = nullptr;
        delete self;
        return DefWindowProcW(h, m, w, l);
    }
    return self->Handle(m, w, l);
}

LRESULT Tour::Handle(UINT msg, WPARAM wp, LPARAM lp)
{
    double s = Scale();
    auto inside = [](const D2D1_RECT_F& r, double x, double y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; };
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            double x = GET_X_LPARAM(lp) / s, y = GET_Y_LPARAM(lp) / s;
            int hv = inside(skip_, x, y) ? 1 : inside(next_, x, y) ? 2 : 0;
            if (hv != hover_) { hover_ = hv; Render(); }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            double x = GET_X_LPARAM(lp) / s, y = GET_Y_LPARAM(lp) / s;
            if (doneAt_ >= 0) return 0;
            if (inside(skip_, x, y)) DestroyWindow(hwnd);
            else if (inside(next_, x, y)) { Advance(); if (g_tour) Render(); }
            return 0;
        }
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, hover_ ? IDC_HAND : IDC_ARROW));
            return TRUE;
        case WM_TIMER:
            if (wp == kTimer) {
                Poll();
                if (g_tour && (appear_.value < 1 || lastShown_ != ShownKey())) Render();
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void StartTour(TourHooks hooks)
{
    if (g_tour) return;
    g_tour = new Tour();
    g_tour->hooks = std::move(hooks);
    if (!g_tour->Open()) { delete g_tour; g_tour = nullptr; }
}

bool TourActive() { return g_tour != nullptr; }

}  // namespace am
