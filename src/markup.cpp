// The quick markup editor, opened by pressing and holding a photo: draw, highlight,
// point with arrows, add shapes and text, blur out private parts, crop. Done writes
// the result over the original file, like macOS Markup.
//
// Everything is drawn on a "stage" the size of the picture in pixels, so a mark lands
// on exactly the pixels it covers and what is saved is what was seen.
#include "markup.h"

#include "anim.h"
#include "gfx.h"

#include <optional>

namespace am {

namespace {

enum class Tool { Pen, Highlighter, Arrow, Rectangle, Oval, Text, Blur, Crop };

constexpr uint32_t kAccent = 0xFF3B6BE0;
constexpr uint32_t kInk = 0xFF2C2E38;
const uint32_t kPalette[7] = { 0xFFFF3B30, 0xFFFF9500, 0xFFFFCC00, 0xFF34C759, 0xFF0A84FF, 0xFF1C1C1E, 0xFFFFFFFF };

struct Annot {
    Tool kind = Tool::Pen;
    uint32_t color = 0;
    double thick = 0;
    std::vector<D2D1_POINT_2F> pts;     // pen, highlighter; arrow: start and end
    D2D1_RECT_F r{};                    // shapes, blur area
    wstring text;
    double fontSize = 0;
    PixelsPtr blurred;                  // the pixelated copy of the covered area
};

D2D1_RECT_F RectOf(D2D1_POINT_2F a, D2D1_POINT_2F b)
{
    return D2D1::RectF(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y));
}

Com<ID2D1StrokeStyle> RoundStroke()
{
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    Com<ID2D1StrokeStyle> s;
    D2D()->CreateStrokeStyle(sp, nullptr, 0, s.Put());
    return s;
}

Com<ID2D1StrokeStyle> SquareCapStroke()
{
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_SQUARE, D2D1_CAP_STYLE_SQUARE, D2D1_CAP_STYLE_SQUARE, D2D1_LINE_JOIN_ROUND);
    Com<ID2D1StrokeStyle> s;
    D2D()->CreateStrokeStyle(sp, nullptr, 0, s.Put());
    return s;
}

/// Icons are small SVG-like paths on a 24 x 24 grid (M L H V A Z, absolute).
Com<ID2D1PathGeometry> ParseIcon(const char* d)
{
    Com<ID2D1PathGeometry> geo;
    D2D()->CreatePathGeometry(geo.Put());
    Com<ID2D1GeometrySink> sink;
    geo->Open(sink.Put());
    bool open = false;
    D2D1_POINT_2F cur{ 0, 0 };
    const char* p = d;
    auto skip = [&] { while (*p == ' ' || *p == ',') p++; };
    auto num = [&]() { skip(); char* e; float v = strtof(p, &e); p = e; return v; };
    char cmd = 0;
    while (*p) {
        skip();
        if (!*p) break;
        if (isalpha(static_cast<unsigned char>(*p))) cmd = *p++;
        switch (cmd) {
            case 'M': {
                if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
                cur = { num(), num() };
                sink->BeginFigure(cur, D2D1_FIGURE_BEGIN_HOLLOW);
                open = true;
                cmd = 'L';
                break;
            }
            case 'L': cur = { num(), num() }; sink->AddLine(cur); break;
            case 'H': cur.x = num(); sink->AddLine(cur); break;
            case 'V': cur.y = num(); sink->AddLine(cur); break;
            case 'A': {
                float rx = num(), ry = num(), rot = num(), large = num(), sweep = num();
                cur = { num(), num() };
                sink->AddArc(D2D1::ArcSegment(cur, D2D1::SizeF(rx, ry), rot, sweep > 0.5f ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                              large > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                break;
            }
            case 'Z':
                if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                open = false;
                break;
            default: p++; break;
        }
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    return geo;
}

struct Button {
    enum Kind { ToolBtn, Swatch, Undo, Cancel, Done } kind;
    D2D1_RECT_F r;
    Tool tool = Tool::Pen;
    uint32_t color = 0;
    Com<ID2D1PathGeometry> icon;
    wstring tip;
};

constexpr UINT_PTR kTimer = 1;

class Editor {
public:
    Editor(const wstring& path, std::function<void(const wstring&)> saved) : path_(path), saved_(std::move(saved)) {}
    ~Editor();
    bool Open();
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    HWND hwnd = nullptr;
    bool closed = false;

private:
    LRESULT Handle(UINT, WPARAM, LPARAM);
    void Layout();
    void Render();
    void DrawScene(ID2D1RenderTarget* rt, bool forExport);
    void DrawAnnot(ID2D1RenderTarget* rt, const Annot& a);
    void DrawToolbar(ID2D1RenderTarget* rt);
    ID2D1Bitmap* BitmapFor(ID2D1RenderTarget* rt, const Pixels& px);
    std::unordered_map<uint64_t, Com<ID2D1Bitmap>> bmpCache_;

    // input
    void OnDown(double x, double y);
    void OnMove(double x, double y);
    void OnUp(double x, double y);
    void OnKey(WPARAM vk);
    void OnChar(wchar_t c);
    D2D1_POINT_2F ToStage(double x, double y) const;
    const Button* ButtonAt(double x, double y) const;
    void SelectTool(Tool t);
    void Undo();
    void Finish(bool save);
    bool Save();
    PixelsPtr Pixelate(D2D1_RECT_F r);

    // text
    void BeginText(D2D1_POINT_2F p);
    void CommitText();
    void CancelText();
    Com<IDWriteTextLayout> TextLayoutFor(const wstring& text, double fontSize);

    double Unit() const { return unit_; }
    double Thickness() const { return std::clamp(4 * unit_, 3.0, 30.0); }

    wstring path_;
    std::function<void(const wstring&)> saved_;
    Mips image_;
    int pw_ = 0, ph_ = 0;
    double dispW_ = 0, dispH_ = 0, unit_ = 1;       // unit: stage pixels per DIP
    double winW_ = 0, winH_ = 0;                    // DIPs
    D2D1_RECT_F frame_{}, toolbar_{};               // DIPs, window space
    std::vector<Button> buttons_;
    Surface surf_;
    POINT winPos_{};                                // screen pixels
    Tool tool_ = Tool::Pen;
    uint32_t color_ = kPalette[0];
    std::vector<Annot> annots_;
    struct UndoOp { bool crop; std::optional<D2D1_RECT_F> prevCrop; };
    std::vector<UndoOp> undo_;
    std::optional<D2D1_RECT_F> crop_, cropAtStart_;
    bool drawing_ = false;
    Annot active_;
    D2D1_POINT_2F start_{};
    // text editing
    bool editing_ = false;
    Annot textAnnot_;
    size_t caret_ = 0;
    bool caretOn_ = true;
    double caretFlip_ = 0;
    // chrome
    const Button* hover_ = nullptr;
    Anim appear_, fade_;
    double drawnAppear_ = -1, drawnFade_ = -1;
    bool closing_ = false;
    Com<ID2D1StrokeStyle> round_, square_;
};

Editor* g_editor = nullptr;

Editor::~Editor()
{
    if (hwnd) { KillTimer(hwnd, kTimer); DestroyWindow(hwnd); }
}

bool Editor::Open()
{
    int ow = 0, oh = 0;
    // A single level at full size, plus the smaller copies for drawing it small.
    int w0 = 0, h0 = 0;
    if (!ImageSize(path_, &w0, &h0)) return false;
    image_ = LoadMips(path_, std::max(w0, h0), &ow, &oh);
    if (image_.empty()) return false;
    pw_ = image_.width(); ph_ = image_.height();
    round_ = RoundStroke();
    square_ = SquareCapStroke();

    // Fit the picture to the screen it will appear on.
    Mon mon = UnderPointer();
    double s = Scale();
    double workW = (mon.work.right - mon.work.left) / s, workH = (mon.work.bottom - mon.work.top) / s;
    double nw = pw_ / s, nh = ph_ / s;
    double fit = std::min(1.0, std::min((workW * 0.88 - 64) / nw, (workH * 0.9 - 64 - 90) / nh));
    dispW_ = nw * fit; dispH_ = nh * fit;
    unit_ = pw_ / dispW_;

    // Toolbar: tools, colours, undo, cancel, done.
    struct Def { Tool t; const char* tip; const char* icon; };
    static const Def defs[] = {
        { Tool::Pen, "Pen (P)", "M4,20 L5,15.5 L16.5,4 L20,7.5 L8.5,19 Z M14.5,6 L18,9.5" },
        { Tool::Highlighter, "Highlighter (H)", "M9,14 L16,4 L21,9 L12,17 Z M9,14 L7,19 L12,17 M4,21 H9" },
        { Tool::Arrow, "Arrow (A)", "M5,19 L19,5 M10,5 H19 V14" },
        { Tool::Rectangle, "Rectangle (R)", "M4,6 H20 V18 H4 Z" },
        { Tool::Oval, "Oval (O)", "M3,12 A9,7 0 1 0 21,12 A9,7 0 1 0 3,12 Z" },
        { Tool::Text, "Text (T)", "M5,6 H19 M12,6 V19 M9,19 H15" },
        { Tool::Blur, "Blur out (B)", "M4,4 H10 V10 H4 Z M14,4 H20 V10 H14 Z M4,14 H10 V20 H4 Z M14,14 H20 V20 H14 Z" },
        { Tool::Crop, "Crop (C)", "M7,3 V17 H21 M3,7 H17 V21" },
    };
    double x = 10;
    auto add = [&](Button b, double w) { b.r = D2D1::RectF(float(x), 8, float(x + w), 44); buttons_.push_back(std::move(b)); x += w; };
    auto sep = [&] { x += 17; };
    for (auto& d : defs) {
        Button b{ Button::ToolBtn, {} };
        b.tool = d.t; b.icon = ParseIcon(d.icon);
        x += 1; add(std::move(b), 38); x += 1;
    }
    sep();
    for (uint32_t c : kPalette) { Button b{ Button::Swatch, {} }; b.color = c; x += 2; add(std::move(b), 22); x += 2; }
    sep();
    { Button b{ Button::Undo, {} }; b.icon = ParseIcon("M8,8 L4,12 L8,16 M4,12 H14 A5,5 0 0 1 14,22 H9"); x += 1; add(std::move(b), 38); x += 1; }
    sep();
    // The text buttons are as wide as their labels.
    {
        Surface tmp;
        tmp.Resize(8, 8);
        double cancelW = 0, doneW = 0;
        if (tmp.Begin()) {
            cancelW = tmp.MeasureText(L"Cancel", 14, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 32;
            doneW = tmp.MeasureText(L"Done", 14, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 32;
            tmp.End();
        }
        Button c{ Button::Cancel, {} }; x += 4; add(std::move(c), cancelW);
        Button d{ Button::Done, {} }; x += 4; add(std::move(d), doneW);
    }
    double tbW = x + 10, tbH = 52;

    winW_ = std::ceil(std::max(dispW_, tbW) + 72);
    winH_ = std::ceil(28 + dispH_ + 22 + tbH + 40);
    frame_ = D2D1::RectF(float((winW_ - dispW_) / 2), 28, float((winW_ + dispW_) / 2), float(28 + dispH_));
    toolbar_ = D2D1::RectF(float((winW_ - tbW) / 2), float(28 + dispH_ + 22), float((winW_ + tbW) / 2), float(28 + dispH_ + 22 + tbH));
    for (auto& b : buttons_) {   // buttons were laid out relative to the toolbar: move them into window space
        b.r.left += toolbar_.left; b.r.right += toolbar_.left;
        b.r.top += toolbar_.top; b.r.bottom += toolbar_.top;
    }
    for (auto& b : buttons_) if (b.kind == Button::Swatch || b.kind == Button::Cancel || b.kind == Button::Done) { b.r.top = toolbar_.top + 8 + (b.kind == Button::Swatch ? 7 : 2); b.r.bottom = toolbar_.top + 44 - (b.kind == Button::Swatch ? 7 : 2); }

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"AmpaianMarkup";
        RegisterClassW(&wc);
        registered = true;
    }
    int pxW = int(std::lround(winW_ * s)), pxH = int(std::lround(winH_ * s));
    winPos_.x = mon.work.left + ((mon.work.right - mon.work.left) - pxW) / 2;
    winPos_.y = mon.work.top + ((mon.work.bottom - mon.work.top) - pxH) / 2;
    hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"AmpaianMarkup", L"Ampaian Markup", WS_POPUP, winPos_.x, winPos_.y, pxW, pxH,
                           nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd) return false;
    surf_.Resize(pxW, pxH);
    appear_.Set(0);
    appear_.Ease(1, 0.2, Curve::EaseOut);
    SelectTool(Tool::Pen);
    Render();
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    SetTimer(hwnd, kTimer, 16, nullptr);
    return true;
}

LRESULT CALLBACK Editor::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE) {
        auto* self = reinterpret_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd = h;
    }
    auto* self = reinterpret_cast<Editor*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT Editor::Handle(UINT msg, WPARAM wp, LPARAM lp)
{
    double s = Scale();
    switch (msg) {
        case WM_LBUTTONDOWN: SetCapture(hwnd); OnDown(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s); return 0;
        case WM_MOUSEMOVE: OnMove(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s); return 0;
        case WM_LBUTTONUP: ReleaseCapture(); OnUp(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s); return 0;
        case WM_KEYDOWN: OnKey(wp); return 0;
        case WM_CHAR: OnChar(wchar_t(wp)); return 0;
        case WM_SETCURSOR: {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            double x = p.x / s, y = p.y / s;
            if (ButtonAt(x, y)) SetCursor(LoadCursorW(nullptr, IDC_HAND));
            else if (x >= frame_.left && x <= frame_.right && y >= frame_.top && y <= frame_.bottom)
                SetCursor(LoadCursorW(nullptr, tool_ == Tool::Text ? IDC_IBEAM : IDC_CROSS));
            else SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        case WM_TIMER:
            if (wp == kTimer) {
                bool blink = false;
                if (editing_ && Now() - caretFlip_ > 0.5) { caretFlip_ = Now(); caretOn_ = !caretOn_; blink = true; }
                // Redraw whenever the opacity moved since the last frame: the final step of an
                // animation lands after it stops being "active", and must be drawn too.
                if (appear_.value != drawnAppear_ || fade_.value != drawnFade_ || blink) Render();
                if (closing_ && !fade_.Active() && fade_.value >= 1) {
                    closed = true;
                    HWND h = hwnd;
                    hwnd = nullptr;
                    DestroyWindow(h);
                    PostToUi([] { if (g_editor && g_editor->closed) { delete g_editor; g_editor = nullptr; } });   // after this call returns
                }
            }
            return 0;
        case WM_KILLFOCUS: return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- geometry helpers

D2D1_POINT_2F Editor::ToStage(double x, double y) const
{
    double sx = (x - frame_.left) * unit_, sy = (y - frame_.top) * unit_;
    return D2D1::Point2F(float(std::clamp(sx, 0.0, double(pw_))), float(std::clamp(sy, 0.0, double(ph_))));
}

const Button* Editor::ButtonAt(double x, double y) const
{
    for (auto& b : buttons_)
        if (x >= b.r.left && x <= b.r.right && y >= b.r.top && y <= b.r.bottom) return &b;
    return nullptr;
}

ID2D1Bitmap* Editor::BitmapFor(ID2D1RenderTarget* rt, const Pixels& px)
{
    // Bitmaps belong to the render target that made them: the key has both.
    uint64_t key = px.uid * 1000003ull ^ uint64_t(reinterpret_cast<uintptr_t>(rt));
    auto it = bmpCache_.find(key);
    if (it != bmpCache_.end()) return it->second.Get();
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    Com<ID2D1Bitmap> bmp;
    if (FAILED(rt->CreateBitmap(D2D1::SizeU(UINT(px.w), UINT(px.h)), px.bgra.data(), UINT(px.stride()), &bp, bmp.Put()))) return nullptr;
    ID2D1Bitmap* raw = bmp.Get();
    bmpCache_[key] = bmp;
    return raw;
}

// ---------------------------------------------------------------- the marks

/// The arrow: a shaft and a filled head, as geometry.
void ArrowGeometries(D2D1_POINT_2F a, D2D1_POINT_2F b, double t, Com<ID2D1PathGeometry>& shaft, Com<ID2D1PathGeometry>& head)
{
    double dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
    D2D()->CreatePathGeometry(shaft.Put());
    D2D()->CreatePathGeometry(head.Put());
    Com<ID2D1GeometrySink> s1, s2;
    shaft->Open(s1.Put());
    head->Open(s2.Put());
    if (len < 1) {
        s1->BeginFigure(a, D2D1_FIGURE_BEGIN_HOLLOW); s1->AddLine(a); s1->EndFigure(D2D1_FIGURE_END_OPEN);
        s2->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED); s2->AddLine(a); s2->EndFigure(D2D1_FIGURE_END_CLOSED);
    } else {
        double ux = dx / len, uy = dy / len, nx = -uy, ny = ux;
        double h = std::min(t * 4.8, len * 0.6);
        D2D1_POINT_2F joint = D2D1::Point2F(float(b.x - ux * h * 0.8), float(b.y - uy * h * 0.8));
        s1->BeginFigure(a, D2D1_FIGURE_BEGIN_HOLLOW);
        s1->AddLine(joint);
        s1->EndFigure(D2D1_FIGURE_END_OPEN);
        s2->BeginFigure(b, D2D1_FIGURE_BEGIN_FILLED);
        s2->AddLine(D2D1::Point2F(float(joint.x + nx * h * 0.5), float(joint.y + ny * h * 0.5)));
        s2->AddLine(D2D1::Point2F(float(joint.x - nx * h * 0.5), float(joint.y - ny * h * 0.5)));
        s2->EndFigure(D2D1_FIGURE_END_CLOSED);
    }
    s1->Close();
    s2->Close();
}

Com<ID2D1PathGeometry> PolylineGeometry(const std::vector<D2D1_POINT_2F>& pts)
{
    Com<ID2D1PathGeometry> geo;
    D2D()->CreatePathGeometry(geo.Put());
    Com<ID2D1GeometrySink> sink;
    geo->Open(sink.Put());
    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
    if (pts.size() == 1) sink->AddLine(pts[0]);
    for (size_t i = 1; i < pts.size(); i++) sink->AddLine(pts[i]);
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    return geo;
}

Com<IDWriteTextLayout> Editor::TextLayoutFor(const wstring& text, double fontSize)
{
    Com<IDWriteTextFormat> fmt;
    DWrite()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, float(fontSize), L"en-us", fmt.Put());
    Com<IDWriteTextLayout> layout;
    DWrite()->CreateTextLayout(text.c_str(), UINT32(text.size()), fmt.Get(), 100000, 100000, layout.Put());
    return layout;
}

void Editor::DrawAnnot(ID2D1RenderTarget* rt, const Annot& a)
{
    Com<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(Argb(a.color), br.Put());
    if (!br) return;
    switch (a.kind) {
        case Tool::Pen:
        case Tool::Highlighter: {
            if (a.pts.empty()) return;
            auto geo = PolylineGeometry(a.pts);
            rt->DrawGeometry(geo.Get(), br.Get(), float(a.thick), a.kind == Tool::Pen ? round_.Get() : square_.Get());
            break;
        }
        case Tool::Arrow: {
            if (a.pts.size() < 2) return;
            Com<ID2D1PathGeometry> shaft, head;
            ArrowGeometries(a.pts[0], a.pts[1], a.thick, shaft, head);
            rt->DrawGeometry(shaft.Get(), br.Get(), float(a.thick), round_.Get());
            rt->FillGeometry(head.Get(), br.Get());
            rt->DrawGeometry(head.Get(), br.Get(), float(a.thick), round_.Get());
            break;
        }
        case Tool::Rectangle:
            rt->DrawRoundedRectangle(D2D1::RoundedRect(a.r, float(a.thick * 0.7), float(a.thick * 0.7)), br.Get(), float(a.thick), round_.Get());
            break;
        case Tool::Oval:
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((a.r.left + a.r.right) / 2, (a.r.top + a.r.bottom) / 2), (a.r.right - a.r.left) / 2, (a.r.bottom - a.r.top) / 2),
                            br.Get(), float(a.thick), round_.Get());
            break;
        case Tool::Blur:
            if (a.blurred) {
                if (auto* bmp = BitmapFor(rt, *a.blurred)) rt->DrawBitmap(bmp, a.r, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            } else {   // the area being chosen
                Com<ID2D1SolidColorBrush> fill, line;
                rt->CreateSolidColorBrush(Argb(0x3CFFFFFF), fill.Put());
                rt->CreateSolidColorBrush(Argb(0xFFFFFFFF), line.Put());
                rt->FillRectangle(a.r, fill.Get());
                Com<ID2D1StrokeStyle> dash;
                D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_DASH);
                D2D()->CreateStrokeStyle(sp, nullptr, 0, dash.Put());
                rt->DrawRectangle(a.r, line.Get(), float(1.5 * unit_), dash.Get());
            }
            break;
        case Tool::Text: {
            if (a.text.empty()) return;
            auto layout = TextLayoutFor(a.text, a.fontSize);
            if (!layout) return;
            Com<ID2D1SolidColorBrush> shadow;
            rt->CreateSolidColorBrush(Argb(0x59000000), shadow.Put());
            rt->DrawTextLayout(D2D1::Point2F(a.r.left + float(unit_), a.r.top + float(unit_)), layout.Get(), shadow.Get());
            rt->DrawTextLayout(D2D1::Point2F(a.r.left, a.r.top), layout.Get(), br.Get());
            break;
        }
        default: break;
    }
}

void Editor::DrawScene(ID2D1RenderTarget* rt, bool forExport)
{
    // The picture.
    if (forExport) {
        if (auto* bmp = BitmapFor(rt, *image_.levels[0])) rt->DrawBitmap(bmp, D2D1::RectF(0, 0, float(pw_), float(ph_)), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    for (auto& a : annots_) DrawAnnot(rt, a);
    if (!forExport) {
        if (drawing_ && tool_ != Tool::Crop) DrawAnnot(rt, active_);
        if (editing_) {
            DrawAnnot(rt, textAnnot_);
            // box and caret
            auto layout = TextLayoutFor(textAnnot_.text, textAnnot_.fontSize);
            DWRITE_TEXT_METRICS tm{};
            float cx = 0, cy = 0;
            DWRITE_HIT_TEST_METRICS hm{};
            if (layout) {
                layout->GetMetrics(&tm);
                layout->HitTestTextPosition(UINT32(caret_), FALSE, &cx, &cy, &hm);
            }
            float h = hm.height > 0 ? hm.height : float(textAnnot_.fontSize * 1.3);
            float w = std::max(tm.widthIncludingTrailingWhitespace, float(60 * unit_));
            Com<ID2D1SolidColorBrush> box, ink;
            rt->CreateSolidColorBrush(Argb(0xC8FFFFFF), box.Put());
            rt->CreateSolidColorBrush(Argb(textAnnot_.color), ink.Put());
            Com<ID2D1StrokeStyle> dash;
            D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_DASH);
            D2D()->CreateStrokeStyle(sp, nullptr, 0, dash.Put());
            float pad = float(3 * unit_);
            rt->DrawRectangle(D2D1::RectF(textAnnot_.r.left - pad, textAnnot_.r.top - pad, textAnnot_.r.left + w + pad, textAnnot_.r.top + std::max(h, tm.height) + pad), box.Get(), float(unit_), dash.Get());
            if (caretOn_) rt->DrawLine(D2D1::Point2F(textAnnot_.r.left + cx, textAnnot_.r.top + cy), D2D1::Point2F(textAnnot_.r.left + cx, textAnnot_.r.top + cy + h), ink.Get(), float(1.5 * unit_));
        }
        // The crop: what will be cut away is dimmed.
        if (crop_) {
            Com<ID2D1SolidColorBrush> dim, white;
            rt->CreateSolidColorBrush(Argb(0x96000000), dim.Put());
            rt->CreateSolidColorBrush(Argb(0xFFFFFFFF), white.Put());
            const D2D1_RECT_F c = *crop_;
            rt->FillRectangle(D2D1::RectF(0, 0, float(pw_), c.top), dim.Get());
            rt->FillRectangle(D2D1::RectF(0, c.bottom, float(pw_), float(ph_)), dim.Get());
            rt->FillRectangle(D2D1::RectF(0, c.top, c.left, c.bottom), dim.Get());
            rt->FillRectangle(D2D1::RectF(c.right, c.top, float(pw_), c.bottom), dim.Get());
            rt->DrawRectangle(c, white.Get(), float(2 * unit_));
        }
    }
}

// ---------------------------------------------------------------- the window's picture

void Editor::DrawToolbar(ID2D1RenderTarget* rt)
{
    auto solid = [&](uint32_t argb) { return surf_.Solid(argb); };
    D2D1_RECT_F tb = toolbar_;
    surf_.DrawShadow(tb, 26, 24, 0, 8, 0.28f);
    rt->FillRoundedRectangle(D2D1::RoundedRect(tb, 26, 26), solid(0xF0F8F9FC));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tb.left + 0.4f, tb.top + 0.4f, tb.right - 0.4f, tb.bottom - 0.4f), 26, 26), solid(0x46000000), 0.75f);

    // separators sit between the groups
    auto sepAt = [&](float x) { rt->FillRectangle(D2D1::RectF(x, tb.top + 15, x + 1, tb.top + 37), solid(0x28000000)); };
    {
        float lastTool = 0, firstSwatch = 0, lastSwatch = 0, undoLeft = 0, undoRight = 0, cancelLeft = 0;
        for (auto& b : buttons_) {
            if (b.kind == Button::ToolBtn) lastTool = b.r.right + 1;
            if (b.kind == Button::Swatch) { if (!firstSwatch) firstSwatch = b.r.left; lastSwatch = b.r.right; }
            if (b.kind == Button::Undo) { undoLeft = b.r.left; undoRight = b.r.right; }
            if (b.kind == Button::Cancel) cancelLeft = b.r.left;
        }
        sepAt(lastTool + 8);
        sepAt(lastSwatch + 2 + 8);
        sepAt(undoRight + 1 + 8);
        (void)undoLeft; (void)firstSwatch; (void)cancelLeft;
    }

    for (auto& b : buttons_) {
        bool over = hover_ == &b;
        switch (b.kind) {
            case Button::ToolBtn:
            case Button::Undo: {
                bool selected = b.kind == Button::ToolBtn && b.tool == tool_;
                if (selected) rt->FillRoundedRectangle(D2D1::RoundedRect(b.r, 10, 10), solid(kAccent));
                else if (over) rt->FillRoundedRectangle(D2D1::RoundedRect(b.r, 10, 10), solid(0x16000000));
                D2D1::Matrix3x2F old;
                rt->GetTransform(&old);
                float ox = (b.r.left + b.r.right) / 2 - 12, oy = (b.r.top + b.r.bottom) / 2 - 12;
                rt->SetTransform(D2D1::Matrix3x2F::Translation(ox, oy) * old);
                if (b.icon) rt->DrawGeometry(b.icon.Get(), solid(selected ? 0xFFFFFFFF : kInk), 1.8f, round_.Get());
                rt->SetTransform(old);
                break;
            }
            case Button::Swatch: {
                float cx = (b.r.left + b.r.right) / 2, cy = (b.r.top + b.r.bottom) / 2;
                if (b.color == color_) rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 10, 10), solid(kAccent), 2);
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 8, 8), solid(b.color));
                rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 8, 8), solid(0x46000000), 0.75f);
                break;
            }
            case Button::Cancel:
            case Button::Done: {
                bool primary = b.kind == Button::Done;
                float r = (b.r.bottom - b.r.top) / 2;
                if (primary) rt->FillRoundedRectangle(D2D1::RoundedRect(b.r, r, r), solid(over ? 0xD93B6BE0 : kAccent));
                else if (over) rt->FillRoundedRectangle(D2D1::RoundedRect(b.r, r, r), solid(0x14000000));
                surf_.DrawText(primary ? L"Done" : L"Cancel", 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(b.r.left + 16, b.r.top + 5.2f, b.r.right, b.r.bottom), primary ? 0xFFFFFFFF : kInk);
                break;
            }
        }
    }
}

void Editor::Render()
{
    if (!hwnd || !surf_.ready()) return;
    ID2D1DCRenderTarget* rt = surf_.Begin();
    if (!rt) return;
    const float s = float(Scale());
    double a = std::clamp(appear_.value, 0.0, 1.0);
    // appearing: a small zoom from the middle of the window
    float zoom = float(0.96 + 0.04 * a);
    D2D1::Matrix3x2F base = D2D1::Matrix3x2F::Scale(zoom, zoom, D2D1::Point2F(float(winW_ / 2), float(winH_ / 2))) * D2D1::Matrix3x2F::Scale(s, s);
    rt->SetTransform(base);

    // The picture's frame, with its shadow.
    surf_.DrawShadow(frame_, 12, 36, 0, 12, 0.35f);
    rt->FillRoundedRectangle(D2D1::RoundedRect(frame_, 12, 12), surf_.Solid(0xFF1E1E22));

    // Everything inside the frame is drawn in stage pixels, clipped to the rounded frame.
    Com<ID2D1RoundedRectangleGeometry> clip;
    D2D()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(frame_, 12, 12), clip.Put());
    rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), surf_.Layer());
    {
        D2D1::Matrix3x2F stage = D2D1::Matrix3x2F::Scale(float(1.0 / unit_), float(1.0 / unit_)) * D2D1::Matrix3x2F::Translation(frame_.left, frame_.top) * base;
        surf_.DrawMips(image_, frame_, 1.0f);
        rt->SetTransform(stage);
        DrawScene(rt, false);
        rt->SetTransform(base);
    }
    rt->PopLayer();

    DrawToolbar(rt);
    surf_.End();
    drawnAppear_ = appear_.value;
    drawnFade_ = fade_.value;
    BYTE alpha = BYTE(std::clamp(a * (1 - std::clamp(fade_.value, 0.0, 1.0)) * 255.0 + 0.5, 0.0, 255.0));
    surf_.Present(hwnd, winPos_.x, winPos_.y, alpha);
}

// ---------------------------------------------------------------- input

void Editor::SelectTool(Tool t)
{
    CommitText();
    tool_ = t;
    Render();
}

void Editor::Undo()
{
    CommitText();
    if (undo_.empty()) return;
    UndoOp op = undo_.back();
    undo_.pop_back();
    if (op.crop) crop_ = op.prevCrop;
    else if (!annots_.empty()) annots_.pop_back();
    Render();
}

void Editor::OnDown(double x, double y)
{
    if (const Button* b = ButtonAt(x, y)) {
        CommitText();
        switch (b->kind) {
            case Button::ToolBtn: SelectTool(b->tool); break;
            case Button::Swatch: color_ = b->color; Render(); break;
            case Button::Undo: Undo(); break;
            case Button::Cancel: Finish(false); break;
            case Button::Done: Finish(true); break;
        }
        return;
    }
    if (!(x >= frame_.left && x <= frame_.right && y >= frame_.top && y <= frame_.bottom)) { CommitText(); return; }

    auto p = ToStage(x, y);
    if (editing_) {
        // A click inside the text box would move the caret; one outside commits it.
        CommitText();
    }
    start_ = p;
    if (tool_ == Tool::Text) { BeginText(p); return; }
    drawing_ = true;
    active_ = Annot{};
    active_.kind = tool_;
    active_.color = color_;
    double t = Thickness();
    active_.thick = t;
    switch (tool_) {
        case Tool::Pen: active_.pts = { p, p }; break;
        case Tool::Highlighter:
            active_.pts = { p, p };
            active_.color = (color_ & 0x00FFFFFF) | 0x6E000000;
            active_.thick = t * 4.5;
            break;
        case Tool::Arrow: active_.pts = { p, p }; break;
        case Tool::Rectangle:
        case Tool::Oval:
        case Tool::Blur: active_.r = RectOf(p, p); break;
        case Tool::Crop: cropAtStart_ = crop_; break;
        default: break;
    }
    Render();
}

void Editor::OnMove(double x, double y)
{
    const Button* over = ButtonAt(x, y);
    if (over != hover_) { hover_ = over; Render(); }
    if (!drawing_) return;
    auto p = ToStage(x, y);
    switch (tool_) {
        case Tool::Pen:
        case Tool::Highlighter: {
            auto last = active_.pts.back();
            if (std::hypot(p.x - last.x, p.y - last.y) >= 0.6 * unit_) active_.pts.push_back(p);
            break;
        }
        case Tool::Arrow: active_.pts[1] = p; break;
        case Tool::Rectangle:
        case Tool::Oval:
        case Tool::Blur: active_.r = RectOf(start_, p); break;
        case Tool::Crop: crop_ = RectOf(start_, p); break;
        default: break;
    }
    Render();
}

PixelsPtr Editor::Pixelate(D2D1_RECT_F r)
{
    int x = int(std::lround(r.left)), y = int(std::lround(r.top));
    int w = std::min(int(std::lround(r.right - r.left)), pw_ - x), h = std::min(int(std::lround(r.bottom - r.top)), ph_ - y);
    if (w < 6 || h < 6) return nullptr;
    int block = int(std::clamp(std::round(14 * unit_), 4.0, std::min(w, h) / 2.0));
    const Pixels& src = *image_.levels[0];
    auto out = std::make_shared<Pixels>();
    out->w = w; out->h = h;
    out->bgra.resize(size_t(w) * h * 4);
    for (int by = 0; by < h; by += block)
        for (int bx = 0; bx < w; bx += block) {
            int bw = std::min(block, w - bx), bh = std::min(block, h - by);
            uint64_t sum[4] = { 0, 0, 0, 0 };
            for (int j = 0; j < bh; j++)
                for (int i = 0; i < bw; i++) {
                    const uint8_t* p = &src.bgra[(size_t(y + by + j) * src.w + (x + bx + i)) * 4];
                    for (int k = 0; k < 4; k++) sum[k] += p[k];
                }
            uint8_t avg[4];
            for (int k = 0; k < 4; k++) avg[k] = uint8_t(sum[k] / uint64_t(bw * bh));
            for (int j = 0; j < bh; j++)
                for (int i = 0; i < bw; i++) memcpy(&out->bgra[(size_t(by + j) * w + (bx + i)) * 4], avg, 4);
        }
    return out;
}

void Editor::OnUp(double x, double y)
{
    if (!drawing_) return;
    drawing_ = false;
    auto p = ToStage(x, y);
    double moved = std::hypot(p.x - start_.x, p.y - start_.y);

    if (tool_ == Tool::Crop) {
        D2D1_RECT_F r = RectOf(start_, p);
        if (r.right - r.left < 20 || r.bottom - r.top < 20) { crop_ = cropAtStart_; Render(); return; }   // a click, not a frame
        undo_.push_back({ true, cropAtStart_ });
        Render();
        return;
    }
    bool freehand = tool_ == Tool::Pen || tool_ == Tool::Highlighter;
    if (!freehand && moved < 3 * unit_) { Render(); return; }   // a click, not a drag

    Annot a = active_;
    if (tool_ == Tool::Blur) {
        a.blurred = Pixelate(a.r);
        if (!a.blurred) { Render(); return; }
    }
    annots_.push_back(std::move(a));
    undo_.push_back({ false, std::nullopt });
    Render();
}

void Editor::OnKey(WPARAM vk)
{
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (editing_) {
        switch (vk) {
            case VK_ESCAPE: CancelText(); return;
            case VK_RETURN:
                if (shift) { textAnnot_.text.insert(caret_, L"\n"); caret_++; }
                else CommitText();
                Render();
                return;
            case VK_BACK: if (caret_ > 0) { textAnnot_.text.erase(caret_ - 1, 1); caret_--; } break;
            case VK_DELETE: if (caret_ < textAnnot_.text.size()) textAnnot_.text.erase(caret_, 1); break;
            case VK_LEFT: if (caret_ > 0) caret_--; break;
            case VK_RIGHT: if (caret_ < textAnnot_.text.size()) caret_++; break;
            case VK_HOME: caret_ = 0; break;
            case VK_END: caret_ = textAnnot_.text.size(); break;
            default: return;
        }
        caretOn_ = true; caretFlip_ = Now();
        Render();
        return;
    }
    if (ctrl) { if (vk == 'Z') Undo(); return; }
    switch (vk) {
        case VK_ESCAPE: Finish(false); break;
        case VK_RETURN: Finish(true); break;
        case 'P': SelectTool(Tool::Pen); break;
        case 'H': SelectTool(Tool::Highlighter); break;
        case 'A': SelectTool(Tool::Arrow); break;
        case 'R': SelectTool(Tool::Rectangle); break;
        case 'O': SelectTool(Tool::Oval); break;
        case 'T': SelectTool(Tool::Text); break;
        case 'B': SelectTool(Tool::Blur); break;
        case 'C': SelectTool(Tool::Crop); break;
    }
}

void Editor::OnChar(wchar_t c)
{
    if (!editing_ || c < 32 || c == 127) return;
    textAnnot_.text.insert(caret_, 1, c);
    caret_++;
    caretOn_ = true; caretFlip_ = Now();
    Render();
}

// ---------------------------------------------------------------- text

void Editor::BeginText(D2D1_POINT_2F p)
{
    editing_ = true;
    textAnnot_ = Annot{};
    textAnnot_.kind = Tool::Text;
    textAnnot_.color = color_;
    textAnnot_.fontSize = std::clamp(19 * unit_, 14.0, 200.0);
    textAnnot_.r = D2D1::RectF(p.x, float(p.y - textAnnot_.fontSize * 0.8), p.x + 10, p.y);
    caret_ = 0;
    caretOn_ = true;
    caretFlip_ = Now();
    Render();
}

void Editor::CancelText()
{
    editing_ = false;
    textAnnot_ = Annot{};
    Render();
}

void Editor::CommitText()
{
    if (!editing_) return;
    editing_ = false;
    bool blank = true;
    for (wchar_t c : textAnnot_.text) if (!iswspace(c)) blank = false;
    if (!blank) {
        while (!textAnnot_.text.empty() && iswspace(textAnnot_.text.back())) textAnnot_.text.pop_back();
        annots_.push_back(textAnnot_);
        undo_.push_back({ false, std::nullopt });
    }
    textAnnot_ = Annot{};
    Render();
}

// ---------------------------------------------------------------- finishing

void Editor::Finish(bool save)
{
    CommitText();
    if (save && !Save()) return;
    // A short fade, then the window goes.
    closing_ = true;
    fade_.Set(0);
    fade_.Ease(1, 0.12, Curve::EaseIn);
}

bool Editor::Save()
{
    int outX = 0, outY = 0, outW = pw_, outH = ph_;
    if (crop_) {
        int x = int(std::lround(crop_->left)), y = int(std::lround(crop_->top));
        int w = std::min(int(std::lround(crop_->right - crop_->left)), pw_ - x), h = std::min(int(std::lround(crop_->bottom - crop_->top)), ph_ - y);
        if (w >= 8 && h >= 8) { outX = x; outY = y; outW = w; outH = h; }
    }
    Com<IWICBitmap> target;
    if (FAILED(Wic()->CreateBitmap(UINT(outW), UINT(outH), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, target.Put()))) return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    Com<ID2D1RenderTarget> rt;
    if (FAILED(D2D()->CreateWicBitmapRenderTarget(target.Get(), props, rt.Put()))) return false;
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0.0f));
    rt->SetTransform(D2D1::Matrix3x2F::Translation(float(-outX), float(-outY)));
    DrawScene(rt.Get(), true);
    bool drawn = SUCCEEDED(rt->EndDraw());
    bmpCache_.clear();   // the full-size bitmap made for this render target is not needed any more
    rt.Reset();
    if (!drawn) return false;

    wstring ext = Extension(path_);
    GUID container = GUID_ContainerFormatPng;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (ext == L".jpg" || ext == L".jpeg") { container = GUID_ContainerFormatJpeg; fmt = GUID_WICPixelFormat24bppBGR; }
    else if (ext == L".bmp") { container = GUID_ContainerFormatBmp; fmt = GUID_WICPixelFormat24bppBGR; }
    else if (ext == L".tif" || ext == L".tiff") { container = GUID_ContainerFormatTiff; fmt = GUID_WICPixelFormat32bppBGRA; }

    // Written beside the original and swapped in, so a failure never leaves half a picture.
    wstring temp = path_ + L".ampaian-tmp";
    {
        Com<IStream> stream;
        if (FAILED(SHCreateStreamOnFileEx(temp.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, stream.Put()))) return false;
        Com<IWICBitmapEncoder> enc;
        if (FAILED(Wic()->CreateEncoder(container, nullptr, enc.Put())) || FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
        Com<IWICBitmapFrameEncode> frame;
        Com<IPropertyBag2> bag;
        if (FAILED(enc->CreateNewFrame(frame.Put(), bag.Put()))) return false;
        if (container == GUID_ContainerFormatJpeg && bag) {
            PROPBAG2 opt{};
            opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_R4; v.fltVal = 0.95f;
            bag->Write(1, &opt, &v);
        }
        if (FAILED(frame->Initialize(bag.Get())) || FAILED(frame->SetSize(UINT(outW), UINT(outH))) || FAILED(frame->SetPixelFormat(&fmt))) return false;
        Com<IWICFormatConverter> conv;
        if (FAILED(Wic()->CreateFormatConverter(conv.Put())) ||
            FAILED(conv->Initialize(target.Get(), fmt, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
        if (FAILED(frame->WriteSource(conv.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(enc->Commit())) return false;
    }
    if (!MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Log(L"markup: could not save: error %lu", GetLastError());
        DeleteFileW(temp.c_str());
        MessageBeep(MB_ICONASTERISK);
        return false;
    }
    if (saved_) saved_(path_);
    return true;
}

}  // namespace

// ---------------------------------------------------------------- public

void OpenMarkup(const wstring& path, std::function<void(const wstring&)> saved)
{
    if (g_editor && !g_editor->closed) { SetForegroundWindow(g_editor->hwnd); return; }
    wstring ext = Extension(path);
    if (!(ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp" || ext == L".tif" || ext == L".tiff")) {
        Log(L"markup: cannot edit %s", FileName(path).c_str());
        MessageBeep(MB_ICONASTERISK);
        return;
    }
    delete g_editor;
    g_editor = new Editor(path, std::move(saved));
    if (!g_editor->Open()) {
        Log(L"markup: could not open %s", FileName(path).c_str());
        delete g_editor;
        g_editor = nullptr;
        MessageBeep(MB_ICONASTERISK);
    }
}

bool MarkupIsOpen() { return g_editor && !g_editor->closed; }

}  // namespace am
