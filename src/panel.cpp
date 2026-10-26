#include "panel.h"

namespace am {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::mt19937& Rng()
{
    static std::mt19937 rng{ std::random_device{}() };
    return rng;
}
double Rand(double a, double b) { return std::uniform_real_distribution<double>(a, b)(Rng()); }

uint32_t WithAlpha(uint32_t argb, double factor)
{
    uint32_t a = uint32_t(std::clamp(((argb >> 24) & 255) * factor, 0.0, 255.0) + 0.5);
    return (argb & 0x00FFFFFF) | (a << 24);
}

Com<ID2D1LinearGradientBrush> Gradient(ID2D1RenderTarget* rt, D2D1_POINT_2F a, D2D1_POINT_2F b,
                                       std::initializer_list<std::pair<float, uint32_t>> stops, float opacity = 1.0f)
{
    std::vector<D2D1_GRADIENT_STOP> gs;
    for (auto& s : stops) gs.push_back({ s.first, Argb(s.second) });
    Com<ID2D1GradientStopCollection> coll;
    Com<ID2D1LinearGradientBrush> br;
    if (FAILED(rt->CreateGradientStopCollection(gs.data(), UINT32(gs.size()), D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, coll.Put()))) return br;
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), D2D1::BrushProperties(opacity), coll.Get(), br.Put());
    return br;
}

constexpr uint32_t kGlass = 0xB8F7F7F7;   // the frame: a pale pane, 72% opaque

void DrawEdge(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float radius, float width, float opacity)
{
    // A thin specular edge, lit from above.
    float h = width / 2;
    D2D1_RECT_F inner = D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h);
    auto br = Gradient(rt, D2D1::Point2F(0, r.top), D2D1::Point2F(0, r.bottom), { { 0.0f, 0x8CFFFFFF }, { 1.0f, 0x1FFFFFFF } }, opacity);
    if (br) rt->DrawRoundedRectangle(D2D1::RoundedRect(inner, radius - h, radius - h), br.Get(), width);
}

}  // namespace

// ---------------------------------------------------------------- window

Panel::Panel(Line& line) : line_(line) {}

Panel::~Panel()
{
    if (hwnd_) DestroyWindow(hwnd_);
}

bool Panel::Create(HINSTANCE inst)
{
    WNDCLASSW wc{};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AmpaianLine";
    RegisterClassW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, wc.lpszClassName, L"Ampaian",
                            WS_POPUP, -10000, -10000, 100, 100, nullptr, nullptr, inst, this);
    slide_.Set(-(L::PanelHeight + 12));
    return hwnd_ != nullptr;
}

LRESULT CALLBACK Panel::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE) {
        auto* self = reinterpret_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;   // the handle is needed during creation, before CreateWindowEx returns
    }
    auto* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) return self->Handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT Panel::Handle(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONDOWN: OnMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false); return 0;
        case WM_LBUTTONDBLCLK: OnMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true); return 0;
        case WM_MOUSEMOVE: OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_LBUTTONUP: OnMouseUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_RBUTTONUP: OnRightClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_CAPTURECHANGED:
            if (downCard_ && !startedDrag_) EndPress();
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void Panel::PlaceOnScreen(const Mon& mon)
{
    mon_ = mon;
    double s = Scale();
    int w = mon.work.right - mon.work.left;
    int h = int(std::lround(L::PanelHeight * s));
    RECT nf{ mon.work.left, mon.work.top, mon.work.left + w, mon.work.top + h };
    bool changed = nf.left != frame_.left || nf.top != frame_.top || nf.right != frame_.right || nf.bottom != frame_.bottom;
    frame_ = nf;
    widthDip_ = w / s;
    if (shown_) {
        surf_.Resize(w, h);
        if (changed) Sync(true);
        dirty_ = true;
    }
}

void Panel::Show(bool on)
{
    if (on == shown_) return;
    shown_ = on;
    if (on) {
        surf_.Resize(frame_.right - frame_.left, frame_.bottom - frame_.top);
        dirty_ = true;
        Draw(true);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        ShowWindow(hwnd_, SW_HIDE);
        surf_.Release();   // a hidden line holds no memory
    }
}

void Panel::SetClickThrough(bool on)
{
    if (!hwnd_ || on == clickThrough_) return;
    clickThrough_ = on;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    ex = on ? (ex | WS_EX_TRANSPARENT) : (ex & ~LONG_PTR(WS_EX_TRANSPARENT));
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
}

void Panel::SetRevealed(bool on)
{
    // Tucked away, the whole line waits above the top edge and slides out from
    // under it, the way an auto-hiding taskbar does.
    if (on) {
        slide_.Spring(0, 0.42, 0.82);
        // Overlays such as a screenshot tool's can end up above the line: take the top again.
        if (shown_) SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    else slide_.Ease(-(L::PanelHeight + 12), 0.22, Curve::EaseIn);
    dirty_ = true;
}

// ---------------------------------------------------------------- cards

Card* Panel::FindCard(uint32_t id)
{
    for (auto& c : cards_) if (c->id == id) return c.get();
    return nullptr;
}

void Panel::Nudge(Card& c, double degrees)
{
    c.swing.Ease(degrees, 0.3, Curve::EaseOut);
    uint32_t id = c.id;
    After(0.3, [this, id] { if (Card* k = FindCard(id)) k->swing.SpringRaw(0, 38, 2.4); });
}

void Panel::Breeze()
{
    for (auto& c : cards_) {
        uint32_t id = c->id;
        double deg = Rand(1.6, 3.4);
        After(Rand(0, 0.35), [this, id, deg] { if (Card* k = FindCard(id)) Nudge(*k, deg); });
    }
}

void Panel::RefreshCard(Card& c)
{
    L::Size cs = L::CardSize(c.item->thumb.width(), c.item->thumb.height());
    c.cardW = cs.w; c.cardH = cs.h;
    if (c.item->falling != c.falling) {
        c.falling = c.item->falling;
        // The fall itself is drawn over the whole screen, so the card here steps aside at once.
        if (c.falling) c.visible.Set(0);
    }
    if (c.item->flying != c.flying) {
        bool was = c.flying;
        c.flying = c.item->flying;
        if (c.flying) c.visible.Set(0);
        else if (!c.falling) c.visible.Ease(1, 0.16, Curve::EaseOut);
        if (was && !c.flying) Nudge(c, 2.2);   // landing: a small sway from rest
    }
    dirty_ = true;
}

void Panel::Sync(bool snap)
{
    // Cards whose photo is gone leave.
    cards_.erase(std::remove_if(cards_.begin(), cards_.end(), [&](auto& c) { return line_.Find(c->id) == nullptr; }), cards_.end());

    int n = int(line_.items.size());
    for (int i = 0; i < n; i++) {
        Pegged* it = line_.items[size_t(i)].get();
        double x = L::X(i, n, widthDip_);
        Card* c = FindCard(it->id);
        if (!c) {
            auto card = std::make_unique<Card>();
            card->id = it->id;
            card->item = it;
            c = card.get();
            cards_.push_back(std::move(card));
            c->X.Set(x);
            // Arrive: a capture that flew in is already in place; the flight did the arriving.
            c->scale.Set(1);
            L::Size cs = L::CardSize(it->thumb.width(), it->thumb.height());
            c->cardW = cs.w; c->cardH = cs.h;
            if (it->flying) {
                c->arrived.Set(1);
                c->visible.Set(0);
                c->flying = true;
            } else {
                c->visible.Set(1);
                c->swing.Set(16);
                c->arrived.Set(0);
                c->arrived.Spring(1, 0.42, 0.72);
                c->swing.SpringRaw(0, 46, 2.6);
            }
        } else if (snap) {
            c->X.Set(x);
        } else {
            c->X.Spring(x, 0.55, 0.78);
        }
        RefreshCard(*c);
    }
    // Cards draw in line order.
    std::stable_sort(cards_.begin(), cards_.end(), [&](auto& a, auto& b) {
        auto idx = [&](uint32_t id) { for (size_t i = 0; i < line_.items.size(); i++) if (line_.items[i]->id == id) return i; return size_t(0); };
        return idx(a->id) < idx(b->id);
    });
    hintOpacity_.Ease(n == 0 ? 1 : 0, 0.3, Curve::EaseInOut);
    dirty_ = true;
}

void Panel::OnLineState()
{
    for (auto& cp : cards_) {
        Card& c = *cp;
        bool p = line_.pressedId == c.id, d = line_.draggingId == c.id, cop = line_.copiedId == c.id;
        if (p != c.pressed) {
            c.pressed = p;
            // Holding presses the photo in slowly, so a long press feels like it is building up to something.
            if (p) c.scale.Ease(0.95, 0.45, Curve::EaseInOut);
            else c.scale.Spring(c.hovering ? 1.035 : 1, 0.3, 0.6);
        }
        if (cop != c.copied) {
            c.copied = cop;
            c.copiedT.Ease(cop ? 1 : 0, 0.2, Curve::EaseOut);
            if (cop) Nudge(c, 3);
        }
        c.dragging = d;
    }
    dirty_ = true;
}

bool Panel::CardFrame(uint32_t id, RectD* out) const
{
    int n = int(line_.items.size());
    for (int i = 0; i < n; i++) {
        Pegged* it = line_.items[size_t(i)].get();
        if (it->id != id) continue;
        double x = L::X(i, n, widthDip_);
        double viewTop = L::RopeY(x, widthDip_) - L::PinAbove;
        double cardTop = viewTop + L::CardOffsetBelowTop;
        L::Size cs = L::CardSize(it->thumb.width(), it->thumb.height());
        double s = Scale();
        *out = { frame_.left + (x - cs.w / 2) * s, frame_.top + cardTop * s, cs.w * s, cs.h * s };
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- drawing

void Panel::Draw(bool animating)
{
    if (!shown_ || !hwnd_) return;
    if (!dirty_ && !animating) return;
    dirty_ = false;
    surf_.Resize(frame_.right - frame_.left, frame_.bottom - frame_.top);
    rt_ = surf_.Begin();
    if (!rt_) return;
    const float s = float(Scale());
    stage_ = D2D1::Matrix3x2F::Translation(0, float(slide_.value)) * D2D1::Matrix3x2F::Scale(s, s);
    if (!slidFullyUp()) {
        rt_->SetTransform(stage_);
        DrawRope(widthDip_);
        if (hintOpacity_.value > 0.01) DrawHint(widthDip_);
        for (auto& c : cards_) DrawCard(*c, widthDip_);
    }
    rt_ = nullptr;
    surf_.End();
    surf_.Present(hwnd_, frame_.left, frame_.top);
}

/// A thin, neutral line: a mid gray core with a faint highlight and a soft shadow,
/// so it reads on light and dark backgrounds alike. It fades out at both ends so it
/// seems to come from beyond the screen.
void Panel::DrawRope(double width)
{
    double top = L::RopeTop, sag = L::Sag(width);
    if (!ropeGeo_ || ropeW_ != width) {
        ropeGeo_.Reset();
        D2D()->CreatePathGeometry(ropeGeo_.Put());
        Com<ID2D1GeometrySink> sink;
        ropeGeo_->Open(sink.Put());
        sink->BeginFigure(D2D1::Point2F(-20, float(top)), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(float(width / 2), float(top + 2 * sag)), D2D1::Point2F(float(width + 20), float(top))));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        ropeW_ = width;
    }
    auto mask = Gradient(rt_, D2D1::Point2F(0, 0), D2D1::Point2F(float(width), 0),
                         { { 0.0f, 0x00000000 }, { 0.08f, 0xFF000000 }, { 0.92f, 0xFF000000 }, { 1.0f, 0x00000000 } });
    rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f, mask.Get()), surf_.Layer());

    auto stroke = [&](uint32_t argb, float w, float dy) {
        rt_->SetTransform(D2D1::Matrix3x2F::Translation(0, dy) * stage_);
        rt_->DrawGeometry(ropeGeo_.Get(), surf_.Solid(argb), w);
    };
    stroke(0x16000000, 3.6f, 1.2f);   // soft shadow, as a few widening strokes
    stroke(0x1E000000, 2.6f, 1.2f);
    stroke(0x2A000000, 1.6f, 1.2f);
    stroke(0xFF8C8C8C, 1.2f, 0);      // the core
    stroke(0x73FFFFFF, 0.4f, -0.35f); // a faint highlight
    rt_->SetTransform(stage_);
    rt_->PopLayer();
}

void Panel::DrawHint(double width)
{
    const wchar_t* text = L"Take a screenshot and it will hang here";
    float tw = surf_.MeasureText(text, 12, DWRITE_FONT_WEIGHT_MEDIUM);
    float w = tw + 24, h = 28;
    float cx = float(width / 2), cy = float(L::RopeY(width / 2, width) + 34);
    double o = std::clamp(hintOpacity_.value, 0.0, 1.0);
    D2D1_RECT_F r = D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, h / 2, h / 2), surf_.Solid(WithAlpha(0xD6F5F5F5, o)));
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, h / 2, h / 2), surf_.Solid(WithAlpha(0x46000000, o)), 0.5f);
    surf_.DrawText(text, 12, DWRITE_FONT_WEIGHT_MEDIUM, D2D1::RectF(r.left + 12, r.top + 5.5f, r.right, r.bottom), WithAlpha(0xFF5A5A5A, o));
}

/// One photo with its clothespin. All the charm lives here: it drops onto the line,
/// swings, sways with the breeze and falls when you pull it off.
void Panel::DrawCard(Card& c, double width)
{
    if (!c.item) return;
    double opacity = std::clamp(c.visible.value, 0.0, 1.0) * std::clamp(c.arrived.value, 0.0, 1.0);
    if (opacity < 0.002) return;

    double x = c.X.value;
    double left = x - L::CardWidth / 2, top = L::RopeY(x, width) - L::PinAbove;
    double angle = c.swing.value + c.item->tilt;
    double off = -46 * (1 - std::clamp(c.arrived.value, 0.0, 1.5));
    const float s = float(Scale());
    D2D1::Matrix3x2F root = D2D1::Matrix3x2F::Rotation(float(angle), D2D1::Point2F(float(L::CardWidth / 2), 0)) *
                            D2D1::Matrix3x2F::Translation(0, float(off)) * D2D1::Matrix3x2F::Translation(float(left), float(top)) * stage_;
    rt_->SetTransform(root);

    bool layered = opacity < 0.998;
    if (layered)
        rt_->PushLayer(D2D1::LayerParameters(D2D1::RectF(-30, -70, float(L::CardWidth + 30), float(L::PanelHeight + 40)), nullptr,
                                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), float(opacity)), surf_.Layer());

    // ---- the card
    const double cw = c.cardW, ch = c.cardH;
    const float cx0 = float((L::CardWidth - cw) / 2), cy0 = float(L::CardOffsetBelowTop);
    const float h = float(std::clamp(c.hover.value, 0.0, 1.0));
    D2D1_RECT_F card = D2D1::RectF(cx0, cy0, cx0 + float(cw), cy0 + float(ch));

    bool dim = c.dragging;   // while it is being dragged away, the card on the line pales
    if (dim)
        rt_->PushLayer(D2D1::LayerParameters(D2D1::RectF(-30, -70, float(L::CardWidth + 30), float(L::PanelHeight + 40)), nullptr,
                                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 0.45f), surf_.Layer());

    rt_->SetTransform(D2D1::Matrix3x2F::Scale(float(c.scale.value), float(c.scale.value), D2D1::Point2F(cx0 + float(cw) / 2, cy0)) * root);

    surf_.DrawShadow(card, float(L::Radius), 16 + 8 * h, 0, 5 + 3 * h, 0.18f + 0.08f * h);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(card, float(L::Radius), float(L::Radius)), surf_.Solid(kGlass));

    D2D1_RECT_F photo = D2D1::RectF(card.left + float(L::Inset), card.top + float(L::Inset), card.right - float(L::Inset), card.bottom - float(L::Inset));
    float pr = float(L::Radius - L::Inset);
    surf_.FillMips(c.item->thumb, photo, pr);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(photo.left + 0.25f, photo.top + 0.25f, photo.right - 0.25f, photo.bottom - 0.25f), pr, pr),
                              surf_.Solid(0x2EFFFFFF), 0.5f);
    DrawEdge(rt_, card, float(L::Radius), 0.75f, 1.0f);

    // The corner cross that appears on hover; it is clicked through the grab area.
    if (h > 0.01f && !c.dragging) {
        float ccx = card.left + 3 + 10, ccy = card.top + 3 + 10;
        float k = 0.6f + 0.4f * h;
        rt_->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(ccx, ccy)) *
                          D2D1::Matrix3x2F::Scale(float(c.scale.value), float(c.scale.value), D2D1::Point2F(cx0 + float(cw) / 2, cy0)) * root);
        D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(ccx, ccy), 10, 10);
        rt_->FillEllipse(e, surf_.Solid(WithAlpha(kGlass, h)));
        auto br = Gradient(rt_, D2D1::Point2F(0, ccy - 10), D2D1::Point2F(0, ccy + 10), { { 0.0f, 0x8CFFFFFF }, { 1.0f, 0x1FFFFFFF } }, h);
        if (br) rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ccx, ccy), 9.6f, 9.6f), br.Get(), 0.75f);
        static Com<ID2D1StrokeStyle> round;
        if (!round) {
            D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
            D2D()->CreateStrokeStyle(sp, nullptr, 0, round.Put());
        }
        ID2D1SolidColorBrush* ink = surf_.Solid(WithAlpha(0xFF282828, h));
        rt_->DrawLine(D2D1::Point2F(ccx - 2.6f, ccy - 2.6f), D2D1::Point2F(ccx + 2.6f, ccy + 2.6f), ink, 1.4f, round.Get());
        rt_->DrawLine(D2D1::Point2F(ccx + 2.6f, ccy - 2.6f), D2D1::Point2F(ccx - 2.6f, ccy + 2.6f), ink, 1.4f, round.Get());
    }
    if (dim) { rt_->SetTransform(root); rt_->PopLayer(); }
    rt_->SetTransform(root);

    // ---- the clothespin: a minimal aluminium clip with a slot where it grips the line
    D2D1_RECT_F pin = D2D1::RectF(float((L::CardWidth - 9) / 2), 0, float((L::CardWidth + 9) / 2), 26);
    surf_.DrawShadow(pin, 3.5f, 4, 0, 1.5f, 0.30f);
    auto metal = Gradient(rt_, D2D1::Point2F(pin.left, 0), D2D1::Point2F(pin.right, 0),
                          { { 0.00f, 0xFFB3B3B3 }, { 0.35f, 0xFFEDEDED }, { 0.65f, 0xFFD1D1D1 }, { 1.00f, 0xFF9E9E9E } });
    if (metal) rt_->FillRoundedRectangle(D2D1::RoundedRect(pin, 3.5f, 3.5f), metal.Get());
    auto rim = Gradient(rt_, D2D1::Point2F(0, 0), D2D1::Point2F(0, 26), { { 0.0f, 0xE6FFFFFF }, { 1.0f, 0x2E000000 } });
    if (rim) rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(pin.left + 0.3f, 0.3f, pin.right - 0.3f, 25.7f), 3.2f, 3.2f), rim.Get(), 0.6f);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(pin.left + 2, 8.5f, pin.right - 2, 9.9f), 0.7f, 0.7f), surf_.Solid(0x52000000));

    // ---- the "Copied" pill under the card
    double cp = std::clamp(c.copiedT.value, 0.0, 1.0);
    if (cp > 0.01) {
        const wchar_t* text = L"✓  Copied";
        float tw = surf_.MeasureText(text, 11, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float pw = tw + 20, ph = 24;
        float px = float((L::CardWidth - pw) / 2), py = float(cy0 + ch + 16 - 24 - 4 * (1 - cp));
        D2D1_RECT_F pr2 = D2D1::RectF(px, py, px + pw, py + ph);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(pr2, 12, 12), surf_.Solid(WithAlpha(kGlass, cp)));
        DrawEdge(rt_, pr2, 12, 0.75f, float(cp));
        surf_.DrawText(text, 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(px + 10, py + 4.4f, px + pw, py + ph), WithAlpha(0xFF282828, cp));
    }

    if (layered) rt_->PopLayer();
    rt_->SetTransform(stage_);
}

// ---------------------------------------------------------------- input

Card* Panel::HitCard(double dx, double dy, double* lx, double* ly)
{
    double sy = dy - slide_.value;   // into the line's own space
    for (size_t i = cards_.size(); i-- > 0;) {
        Card& c = *cards_[i];
        if (!c.item || c.falling || c.flying) continue;
        if (c.visible.value < 0.2 || c.arrived.value < 0.2) continue;
        double x = c.X.value;
        double left = x - L::CardWidth / 2, top = L::RopeY(x, widthDip_) - L::PinAbove;
        double angle = (c.swing.value + c.item->tilt) * kPi / 180.0;
        double off = -46 * (1 - std::clamp(c.arrived.value, 0.0, 1.5));
        double px = dx - left, py = sy - top - off;
        double rx = px - L::CardWidth / 2, ry = py;
        double cs = std::cos(-angle), sn = std::sin(-angle);
        double ux = rx * cs - ry * sn + L::CardWidth / 2, uy = rx * sn + ry * cs;
        double cx0 = (L::CardWidth - c.cardW) / 2, mx = cx0 + c.cardW / 2, sc = std::max(0.2, c.scale.value);
        double lxx = (ux - mx) / sc + mx, lyy = (uy - L::CardOffsetBelowTop) / sc + L::CardOffsetBelowTop;
        if (lxx >= cx0 && lxx <= cx0 + c.cardW && lyy >= L::CardOffsetBelowTop && lyy <= L::CardOffsetBelowTop + c.cardH) {
            if (lx) *lx = lxx - cx0;
            if (ly) *ly = lyy - L::CardOffsetBelowTop;
            return &c;
        }
    }
    return nullptr;
}

void Panel::Hover(Card* target)
{
    for (auto& cp : cards_) {
        Card& c = *cp;
        bool on = &c == target;
        if (c.hovering == on) continue;
        c.hovering = on;
        c.hover.Ease(on ? 1 : 0, 0.18, Curve::EaseOut);
        if (!c.pressed) c.scale.Ease(on ? 1.035 : 1, 0.18, Curve::EaseOut);
    }
    dirty_ = true;
}

void Panel::UpdateHover()
{
    if (!shown_ || GrabDragging()) return;
    POINT p = CursorPos();
    double s = Scale();
    bool inside = p.x >= frame_.left && p.x < frame_.right && p.y >= frame_.top && p.y < frame_.bottom;
    Card* hit = nullptr;
    if (inside && !clickThrough_) hit = HitCard((p.x - frame_.left) / s, (p.y - frame_.top) / s, nullptr, nullptr);
    Hover(hit);
}

void Panel::SetPressed(Card* c, bool on)
{
    line_.pressedId = on && c ? c->id : 0;
    line_.RaiseState();
}

void Panel::EndPress()
{
    holdToken_++;
    SetPressed(nullptr, false);
}

void Panel::OnMouseDown(int px, int py, bool dbl)
{
    double s = Scale();
    double lx = 0, ly = 0;
    Card* c = HitCard(px / s, py / s, &lx, &ly);
    if (!c) return;
    const double cross = 26;
    if (lx <= cross && ly <= cross) {   // the discard cross in the card's corner
        downCard_ = nullptr;
        line_.Discard(c->id);
        return;
    }
    if (dbl) {
        downCard_ = nullptr;
        ignoreNextUp_ = true;
        line_.Open(c->id);
        return;
    }
    downCard_ = c;
    downPx_ = CursorPos();
    startedDrag_ = false;
    didLongPress_ = false;
    ignoreNextUp_ = false;
    SetPressed(c, true);
    SetCapture(hwnd_);
    uint32_t token = ++holdToken_;
    uint32_t id = c->id;
    After(0.45, [this, token, id] {
        // Long press: the markup editor.
        if (token != holdToken_ || !downCard_ || startedDrag_) return;
        didLongPress_ = true;
        SetPressed(nullptr, false);
        ReleaseCapture();
        line_.Markup(id);
    });
}

void Panel::OnMouseMove(int px, int py)
{
    if (downCard_ && !startedDrag_ && (GetKeyState(VK_LBUTTON) & 0x8000)) {
        POINT now = CursorPos();
        double dist = std::hypot(double(now.x - downPx_.x), double(now.y - downPx_.y));
        if (dist > 4 * Scale() && !didLongPress_) {
            startedDrag_ = true;
            Card* c = downCard_;
            EndPress();
            ReleaseCapture();
            if (beginDrag) beginDrag(*c, downPx_);
            startedDrag_ = false;
            downCard_ = nullptr;
            return;
        }
    }
    double s = Scale();
    if (!GrabDragging()) Hover(HitCard(px / s, py / s, nullptr, nullptr));
}

void Panel::OnMouseUp(int, int)
{
    ReleaseCapture();
    bool wasDown = downCard_ != nullptr;
    Card* c = downCard_;
    EndPress();
    if (ignoreNextUp_) { ignoreNextUp_ = false; downCard_ = nullptr; didLongPress_ = false; return; }
    if (wasDown && c && !startedDrag_ && !didLongPress_) line_.Copy(c->id);
    downCard_ = nullptr;
    didLongPress_ = false;
}

void Panel::OnRightClick(int px, int py)
{
    double s = Scale();
    Card* c = HitCard(px / s, py / s, nullptr, nullptr);
    if (!c) return;
    POINT sp{ frame_.left + px, frame_.top + py };
    ContextMenu(*c, sp);
}

void Panel::ContextMenu(Card& c, POINT sp)
{
    uint32_t id = c.id;
    bool inbox = line_.IsInInbox(id);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Copy");
    AppendMenuW(m, MF_STRING, 2, L"Open");
    AppendMenuW(m, MF_STRING, 3, L"Markup");
    AppendMenuW(m, MF_STRING, 4, L"Show in Explorer");
    if (inbox) AppendMenuW(m, MF_STRING, 5, L"Save to Desktop");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (inbox) {
        AppendMenuW(m, MF_STRING, 6, L"Discard");
    } else {
        AppendMenuW(m, MF_STRING, 7, L"Take down");
        AppendMenuW(m, MF_STRING, 8, L"Move to Recycle Bin");
    }
    line_.menuOpen = true;
    BeginModal();
    SetForegroundWindow(MessageWindow());
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, sp.x, sp.y, 0, MessageWindow(), nullptr);
    EndModal();
    line_.menuOpen = false;
    DestroyMenu(m);
    switch (cmd) {
        case 1: line_.Copy(id); break;
        case 2: line_.Open(id); break;
        case 3: line_.Markup(id); break;
        case 4: line_.Reveal(id); break;
        case 5: line_.SaveToDesktop(id); break;
        case 6: line_.Discard(id); break;
        case 7: line_.Discard(id); break;
        case 8: line_.Trash(id); break;
    }
}

}  // namespace am
