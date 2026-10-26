#include "flight.h"

#include "anim.h"
#include "layout.h"

namespace am {

namespace {

constexpr double kFlightSeconds = 0.65;
constexpr double kFallSeconds = 0.55;
constexpr double kArc = 30;          // how high the gentle arc rises halfway, in points
constexpr double kMargin = 70;
constexpr uint32_t kGlass = 0xB8F7F7F7;

double EaseInOutCubic(double x) { return x < 0.5 ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3) / 2; }

double Smooth(double x, double a, double b)
{
    double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

LRESULT CALLBACK FlightProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

HWND NewOverlayWindow()
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = FlightProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"AmpaianFlight";
        RegisterClassW(&wc);
        registered = true;
    }
    return CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"AmpaianFlight", L"",
                           WS_POPUP, -10000, -10000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

struct Flight {
    HWND hwnd = nullptr;
    Surface surf;
    Mips image;
    RectD from, to;          // DIPs, screen space
    double tilt = 0;
    bool falling = false;
    double duration = kFlightSeconds, elapsed = 0;
    double originX = 0, originY = 0;   // the window's top left, DIPs
    std::function<void()> arrived;
    bool finishedMotion = false;
    Anim fade;
    bool fading = false, done = false;

    void Create()
    {
        RectD b = { std::min(from.x, to.x), std::min(from.y, to.y), 0, 0 };
        double r = std::max(from.right(), to.right()), bt = std::max(from.bottom(), to.bottom());
        b.w = r - b.x; b.h = bt - b.y;
        b.x -= kMargin; b.y -= kMargin; b.w += 2 * kMargin; b.h += 2 * kMargin;
        if (falling) b.h += 520 + kMargin;
        originX = b.x; originY = b.y;
        hwnd = NewOverlayWindow();
        double s = Scale();
        surf.Resize(int(std::lround(b.w * s)), int(std::lround(b.h * s)));
        posX = int(std::lround(b.x * s)); posY = int(std::lround(b.y * s));
        if (falling) { Update(1); UpdateFall(0); } else Update(0);
        Render();
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }

    int posX = 0, posY = 0;
    double chrome = 0;
    // The state the last update left, drawn by Render().
    double cw = 0, ch = 0, topX = 0, topY = 0, angle = 0, inset = 0, radius = 0, containerOpacity = 1;

    void Update(double raw)
    {
        double k = EaseInOutCubic(raw);
        chrome = Smooth(k, 0.35, 1);
        auto lerp = [&](double a, double b) { return a + (b - a) * k; };
        cw = lerp(from.w, to.w); ch = lerp(from.h, to.h);
        topX = lerp(from.midX(), to.midX()) - originX;
        topY = lerp(from.y, to.y) - originY - std::sin(3.14159265358979 * k) * kArc;
        angle = tilt * k;
        inset = L::Inset * k;
        radius = lerp(0, L::Radius);
        containerOpacity = 1;
    }

    void UpdateFall(double raw)
    {
        double e = raw * raw * raw;
        cw = to.w; ch = to.h;
        topX = to.midX() - originX;
        topY = to.y - originY + 520 * e;
        angle = tilt + (tilt * 7 + 20) * e;
        containerOpacity = 1 - e;
        chrome = 1;
        inset = L::Inset;
        radius = L::Radius;
    }

    void Render()
    {
        ID2D1DCRenderTarget* rt = surf.Begin();
        if (!rt) return;
        const float s = float(Scale());
        D2D1::Matrix3x2F base = D2D1::Matrix3x2F::Scale(s, s);
        float x0 = float(topX - cw / 2), y0 = float(topY);
        rt->SetTransform(D2D1::Matrix3x2F::Rotation(float(angle), D2D1::Point2F(float(topX), float(topY))) * base);

        bool layered = containerOpacity < 0.998;
        if (layered)
            rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), float(std::max(0.0, containerOpacity))), surf.Layer());

        D2D1_RECT_F card = D2D1::RectF(x0, y0, x0 + float(cw), y0 + float(ch));
        surf.DrawShadow(card, float(radius), 18, 0, 5, 0.24f);

        auto withAlpha = [&](uint32_t argb, double f) {
            uint32_t a = uint32_t(std::clamp(((argb >> 24) & 255) * f, 0.0, 255.0) + 0.5);
            return (argb & 0x00FFFFFF) | (a << 24);
        };
        if (chrome > 0.01)
            rt->FillRoundedRectangle(D2D1::RoundedRect(card, float(radius), float(radius)), surf.Solid(withAlpha(kGlass, chrome)));

        float in = float(inset);
        D2D1_RECT_F photo = D2D1::RectF(card.left + in, card.top + in, std::max(card.left + in, card.right - in), std::max(card.top + in, card.bottom - in));
        surf.FillMips(image, photo, float(std::max(0.0, radius - inset)), 1.0f, true);

        if (chrome > 0.01) {
            // The edge: a thin specular line, lit from above.
            Com<ID2D1GradientStopCollection> coll;
            D2D1_GRADIENT_STOP gs[2] = { { 0.0f, Argb(0xE6FFFFFF) }, { 1.0f, Argb(0x40FFFFFF) } };
            Com<ID2D1LinearGradientBrush> br;
            if (SUCCEEDED(rt->CreateGradientStopCollection(gs, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, coll.Put())) &&
                SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, card.top), D2D1::Point2F(0, card.bottom)),
                                                        D2D1::BrushProperties(float(chrome)), coll.Get(), br.Put()))) {
                float h = 0.75f;
                rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(card.left + h, card.top + h, card.right - h, card.bottom - h), float(radius) - h, float(radius) - h), br.Get(), 1.5f);
            }
            // The clip grips the top edge: 26 points tall, 12 of them over the card.
            D2D1_RECT_F pin = D2D1::RectF(float(topX) - 4.5f, y0 - 14, float(topX) + 4.5f, y0 + 12);
            Com<ID2D1GradientStopCollection> mc;
            D2D1_GRADIENT_STOP ms[4] = { { 0.00f, Argb(0xFFB3B3B3) }, { 0.35f, Argb(0xFFEDEDED) }, { 0.65f, Argb(0xFFD1D1D1) }, { 1.00f, Argb(0xFF9E9E9E) } };
            Com<ID2D1LinearGradientBrush> metal;
            if (SUCCEEDED(rt->CreateGradientStopCollection(ms, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, mc.Put())) &&
                SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(pin.left, 0), D2D1::Point2F(pin.right, 0)),
                                                        D2D1::BrushProperties(float(chrome)), mc.Get(), metal.Put())))
                rt->FillRoundedRectangle(D2D1::RoundedRect(pin, 3.5f, 3.5f), metal.Get());
            rt->DrawRoundedRectangle(D2D1::RoundedRect(pin, 3.5f, 3.5f), surf.Solid(withAlpha(0xB2FFFFFF, chrome)), 0.6f);
        }
        if (layered) rt->PopLayer();
        surf.End();
        surf.Present(hwnd, posX, posY, fading ? BYTE(std::clamp((1 - fade.value) * 255.0, 0.0, 255.0)) : BYTE(255));
    }

    /// Returns false when this flight is over and can be destroyed.
    bool Step(double dt)
    {
        if (!finishedMotion) {
            elapsed += dt;
            double k = std::min(1.0, elapsed / duration);
            if (falling) UpdateFall(k); else Update(k);
            Render();
            if (k >= 1) {
                finishedMotion = true;
                if (arrived) { auto fn = std::move(arrived); fn(); }
                if (falling) return false;
                // The real card fades in underneath; this one fades out over it.
                fading = true;
                fade.Set(0);
                fade.Ease(1, 0.16, Curve::EaseOut);
            }
            return true;
        }
        // fading out
        Render();
        return fade.Active() || fade.value < 1;
    }

    ~Flight()
    {
        if (hwnd) DestroyWindow(hwnd);
    }
};

std::vector<std::unique_ptr<Flight>>& Flights()
{
    static std::vector<std::unique_ptr<Flight>> f;
    return f;
}

RectD ToDip(const RectD& px)
{
    double s = Scale();
    return { px.x / s, px.y / s, px.w / s, px.h / s };
}

}  // namespace

void FlightFly(Mips image, RectD fromPx, RectD toPx, double tilt, std::function<void()> arrived)
{
    auto f = std::make_unique<Flight>();
    f->image = std::move(image);
    f->from = ToDip(fromPx);
    f->to = ToDip(toPx);
    f->tilt = tilt;
    f->arrived = std::move(arrived);
    f->Create();
    Flights().push_back(std::move(f));
}

void FlightFall(Mips image, RectD cardPx, double tilt)
{
    auto f = std::make_unique<Flight>();
    f->image = std::move(image);
    f->from = f->to = ToDip(cardPx);
    f->tilt = tilt;
    f->falling = true;
    f->duration = kFallSeconds;
    f->Create();
    Flights().push_back(std::move(f));
}

bool FlightsActive() { return !Flights().empty(); }

bool FlightsFrame(double dt)
{
    auto& list = Flights();
    for (size_t i = 0; i < list.size();) {
        if (!list[i]->Step(dt)) list.erase(list.begin() + long(i));
        else i++;
    }
    return !list.empty();
}

}  // namespace am
