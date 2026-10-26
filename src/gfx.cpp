#include "gfx.h"

#include <atomic>

namespace am {

static Com<ID2D1Factory> g_d2d;
static Com<IDWriteFactory> g_dw;
static Com<IWICImagingFactory> g_wic;

ID2D1Factory* D2D() { return g_d2d.Get(); }
IDWriteFactory* DWrite() { return g_dw.Get(); }
IWICImagingFactory* Wic() { return g_wic.Get(); }

bool GfxInit()
{
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.Put()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(g_dw.Put())))) return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(g_wic.Put())))) return false;
    return true;
}

// ---------------------------------------------------------------- pictures

static std::atomic<uint64_t> g_uid{ 1 };
Pixels::Pixels() : uid(g_uid++) {}

const Pixels* Mips::Pick(double pixelsWide) const
{
    if (levels.empty()) return nullptr;
    const Pixels* best = levels[0].get();
    for (auto& l : levels)
        if (l->w >= pixelsWide) best = l.get();   // levels shrink: the last one still wide enough
    return best;
}

namespace {

struct Decoded {
    Com<IStream> stream;
    Com<IWICBitmapDecoder> decoder;
    Com<IWICBitmapFrameDecode> frame;
    UINT w = 0, h = 0;
};

bool Decode(const wstring& path, Decoded& d)
{
    if (FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, d.stream.Put()))) return false;
    if (FAILED(Wic()->CreateDecoderFromStream(d.stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, d.decoder.Put()))) return false;
    if (FAILED(d.decoder->GetFrame(0, d.frame.Put()))) return false;
    return SUCCEEDED(d.frame->GetSize(&d.w, &d.h)) && d.w > 0 && d.h > 0;
}

/// Scales (Fant) and converts to the given pixel format; copies out the pixels.
bool ScaleTo(IWICBitmapSource* src, UINT srcW, UINT srcH, UINT tw, UINT th, REFWICPixelFormatGUID fmt, std::vector<uint8_t>& out)
{
    Com<IWICBitmapSource> cur(src);
    src->AddRef();   // Com() takes ownership; keep the caller's reference
    Com<IWICBitmapScaler> scaler;
    if (tw != srcW || th != srcH) {
        if (FAILED(Wic()->CreateBitmapScaler(scaler.Put()))) return false;
        if (FAILED(scaler->Initialize(src, tw, th, WICBitmapInterpolationModeFant))) return false;
        cur = Com<IWICBitmapSource>(scaler.Get());
        scaler->AddRef();
    }
    Com<IWICFormatConverter> conv;
    if (FAILED(Wic()->CreateFormatConverter(conv.Put()))) return false;
    if (FAILED(conv->Initialize(cur.Get(), fmt, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    out.resize(size_t(tw) * th * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, tw * 4, UINT(out.size()), out.data()));
}

}  // namespace

bool ImageSize(const wstring& path, int* w, int* h)
{
    Decoded d;
    if (!Decode(path, d)) return false;
    if (w) *w = int(d.w);
    if (h) *h = int(d.h);
    return true;
}

Mips LoadMips(const wstring& path, int maxPixels, int* origW, int* origH)
{
    Mips mips;
    Decoded d;
    if (!Decode(path, d)) return mips;
    if (origW) *origW = int(d.w);
    if (origH) *origH = int(d.h);

    UINT tw = d.w, th = d.h;
    int longest = int(std::max(d.w, d.h));
    if (longest > maxPixels) {
        double k = double(maxPixels) / longest;
        tw = UINT(std::max(1.0, std::round(d.w * k)));
        th = UINT(std::max(1.0, std::round(d.h * k)));
    }
    auto top = std::make_shared<Pixels>();
    top->w = int(tw); top->h = int(th);
    if (!ScaleTo(d.frame.Get(), d.w, d.h, tw, th, GUID_WICPixelFormat32bppPBGRA, top->bgra)) return mips;
    mips.levels.push_back(top);

    // Halve until small. Each level is made from the one above, so the steps stay cheap.
    PixelsPtr prev = top;
    while (std::max(prev->w, prev->h) > 48) {
        int nw = std::max(1, prev->w / 2), nh = std::max(1, prev->h / 2);
        Com<IWICBitmap> bm;
        if (FAILED(Wic()->CreateBitmapFromMemory(UINT(prev->w), UINT(prev->h), GUID_WICPixelFormat32bppPBGRA, UINT(prev->stride()),
                                                 UINT(prev->bgra.size()), prev->bgra.data(), bm.Put()))) break;
        auto next = std::make_shared<Pixels>();
        next->w = nw; next->h = nh;
        if (!ScaleTo(bm.Get(), UINT(prev->w), UINT(prev->h), UINT(nw), UINT(nh), GUID_WICPixelFormat32bppPBGRA, next->bgra)) break;
        mips.levels.push_back(next);
        prev = next;
    }
    return mips;
}

bool LoadSignature(const wstring& path, std::vector<uint8_t>& out, int* origW, int* origH)
{
    Decoded d;
    if (!Decode(path, d)) return false;
    if (origW) *origW = int(d.w);
    if (origH) *origH = int(d.h);
    UINT tw = d.w, th = d.h;
    int longest = int(std::max(d.w, d.h));
    if (longest > 64) {
        double k = 64.0 / longest;
        tw = UINT(std::max(1.0, std::round(d.w * k)));
        th = UINT(std::max(1.0, std::round(d.h * k)));
    }
    return ScaleTo(d.frame.Get(), d.w, d.h, tw, th, GUID_WICPixelFormat32bppBGR, out);
}

// ---------------------------------------------------------------- shadows (CPU gaussian of a rounded rectangle)

namespace {

void BoxBlurLine(const float* in, float* out, int n, int stride, int radius)
{
    // Sliding window over one row or column; outside the buffer counts as 0.
    const float inv = 1.0f / (2 * radius + 1);
    float acc = 0;
    for (int i = -radius; i <= radius; i++) if (i >= 0 && i < n) acc += in[i * stride];
    for (int i = 0; i < n; i++) {
        out[i * stride] = acc * inv;
        int add = i + radius + 1, sub = i - radius;
        if (add < n) acc += in[add * stride];
        if (sub >= 0) acc -= in[sub * stride];
    }
}

void GaussianBlur(std::vector<float>& buf, int w, int h, double sigma)
{
    if (sigma < 0.3) return;
    // Three box passes approximate a gaussian (standard construction).
    double ideal = std::sqrt(12 * sigma * sigma / 3 + 1);
    int wl = int(std::floor(ideal));
    if (wl % 2 == 0) wl--;
    int wu = wl + 2;
    double mIdeal = (12 * sigma * sigma - 3.0 * wl * wl - 12.0 * wl - 9) / (-4.0 * wl - 4);
    int m = int(std::round(mIdeal));
    std::vector<float> tmp(buf.size());
    for (int pass = 0; pass < 3; pass++) {
        int size = pass < m ? wl : wu;
        int r = std::max(0, (size - 1) / 2);
        for (int y = 0; y < h; y++) BoxBlurLine(&buf[size_t(y) * w], &tmp[size_t(y) * w], w, 1, r);
        for (int x = 0; x < w; x++) BoxBlurLine(&tmp[x], &buf[x], h, w, r);
    }
}

/// The shadow of a w x h rounded rectangle, padded on every side; premultiplied BGRA, black.
std::vector<uint8_t> MakeShadow(int w, int h, double radius, double sigma, int pad, int* outW, int* outH)
{
    int W = w + 2 * pad, H = h + 2 * pad;
    std::vector<float> cov(size_t(W) * H, 0.0f);
    double hw = w / 2.0, hh = h / 2.0, cx = pad + hw, cy = pad + hh;
    double r = std::min(radius, std::min(hw, hh));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double qx = std::fabs(x + 0.5 - cx) - (hw - r), qy = std::fabs(y + 0.5 - cy) - (hh - r);
            double ox = std::max(qx, 0.0), oy = std::max(qy, 0.0);
            double sdf = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0) - r;
            cov[size_t(y) * W + x] = float(std::clamp(0.5 - sdf, 0.0, 1.0));
        }
    GaussianBlur(cov, W, H, sigma);
    std::vector<uint8_t> out(size_t(W) * H * 4);
    for (size_t i = 0; i < cov.size(); i++) {
        out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = 0;
        out[i * 4 + 3] = uint8_t(std::clamp(cov[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    *outW = W; *outH = H;
    return out;
}

}  // namespace

// ---------------------------------------------------------------- surface

Surface::Surface() {}
Surface::~Surface() { Release(); }

void Surface::Release()
{
    ClearCaches();
    layer_.Reset();
    rt_.Reset();
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    if (mem_) { DeleteDC(mem_); mem_ = nullptr; }
    bits_ = nullptr;
    w_ = h_ = 0;
}

void Surface::ClearCaches()
{
    brushes_.clear();
    bitmaps_.clear();
    shadows_.clear();
}

void Surface::Resize(int w, int h)
{
    w = std::max(1, w); h = std::max(1, h);
    if (w == w_ && h == h_ && dib_) return;
    Release();
    w_ = w; h_ = h;
    mem_ = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dib_ = CreateDIBSection(mem_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (!dib_) { Release(); return; }
    SelectObject(mem_, dib_);

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
    if (FAILED(D2D()->CreateDCRenderTarget(&props, rt_.Put()))) Release();
}

ID2D1DCRenderTarget* Surface::Begin()
{
    if (!ready()) return nullptr;
    RECT rc{ 0, 0, w_, h_ };
    rt_->BindDC(mem_, &rc);
    rt_->BeginDraw();
    drawing_ = true;
    float s = float(Scale());
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(s, s));
    rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->Clear(D2D1::ColorF(0, 0.0f));
    return rt_.Get();
}

bool Surface::End()
{
    if (!drawing_) return false;
    drawing_ = false;
    HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        int w = w_, h = h_;
        Release();
        Resize(w, h);
        return false;
    }
    return SUCCEEDED(hr);
}

void Surface::Present(HWND hwnd, int x, int y, BYTE alpha)
{
    if (!mem_) return;
    POINT pos{ x, y }, src{ 0, 0 };
    SIZE sz{ w_, h_ };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(hwnd, screen, &pos, &sz, mem_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

ID2D1SolidColorBrush* Surface::Solid(uint32_t argb)
{
    auto it = brushes_.find(argb);
    if (it != brushes_.end()) return it->second.Get();
    Com<ID2D1SolidColorBrush> b;
    if (FAILED(rt_->CreateSolidColorBrush(Argb(argb), b.Put()))) return nullptr;
    if (brushes_.size() > 256) brushes_.clear();
    auto* raw = b.Get();
    brushes_[argb] = b;
    return raw;
}

ID2D1Bitmap* Surface::BitmapFor(const Pixels& px)
{
    auto it = bitmaps_.find(px.uid);
    if (it != bitmaps_.end()) return it->second.Get();
    Com<ID2D1Bitmap> bmp;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(rt_->CreateBitmap(D2D1::SizeU(UINT(px.w), UINT(px.h)), px.bgra.data(), UINT(px.stride()), &bp, bmp.Put()))) return nullptr;
    if (bitmaps_.size() > 40) bitmaps_.clear();   // pictures that left the line are not kept
    auto* raw = bmp.Get();
    bitmaps_[px.uid] = bmp;
    return raw;
}

ID2D1Layer* Surface::Layer()
{
    if (!layer_) rt_->CreateLayer(nullptr, layer_.Put());
    return layer_.Get();
}

void Surface::DrawMips(const Mips& m, const D2D1_RECT_F& dest, float opacity)
{
    const Pixels* px = m.Pick((dest.right - dest.left) * Scale());
    if (!px) return;
    if (auto* bmp = BitmapFor(*px)) rt_->DrawBitmap(bmp, dest, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void Surface::FillMips(const Mips& m, const D2D1_RECT_F& dest, float radius, float opacity, bool cover)
{
    const Pixels* px = m.Pick((dest.right - dest.left) * Scale());
    if (!px) return;
    ID2D1Bitmap* bmp = BitmapFor(*px);
    if (!bmp) return;
    Com<ID2D1BitmapBrush> brush;
    D2D1_BITMAP_BRUSH_PROPERTIES bbp = D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    D2D1_BRUSH_PROPERTIES bp = D2D1::BrushProperties(opacity);
    if (FAILED(rt_->CreateBitmapBrush(bmp, &bbp, &bp, brush.Put()))) return;
    float sx = (dest.right - dest.left) / px->w, sy = (dest.bottom - dest.top) / px->h;
    float ox = dest.left, oy = dest.top;
    if (cover) {
        float k = std::max(sx, sy);   // the larger factor fills the shape; the rest is cropped, centred
        ox = dest.left + ((dest.right - dest.left) - px->w * k) / 2;
        oy = dest.top + ((dest.bottom - dest.top) - px->h * k) / 2;
        sx = sy = k;
    }
    brush->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(ox, oy));
    rt_->FillRoundedRectangle(D2D1::RoundedRect(dest, radius, radius), brush.Get());
}

void Surface::DrawShadow(const D2D1_RECT_F& rect, float radius, float blurRadius, float dx, float dy, float alpha)
{
    if (alpha <= 0.001f) return;
    const double s = Scale();
    double sigma = blurRadius * 0.45 * s;
    int pad = int(std::ceil(3 * sigma)) + 1;
    int wpx = std::max(1, int(std::lround((rect.right - rect.left) * s)));
    int hpx = std::max(1, int(std::lround((rect.bottom - rect.top) * s)));
    double rpx = radius * s;

    auto bitmapFor = [&](int w, int h, uint64_t key) -> ID2D1Bitmap* {
        auto it = shadows_.find(key);
        if (it != shadows_.end()) return it->second.Get();
        int W, H;
        auto px = MakeShadow(w, h, rpx, sigma, pad, &W, &H);
        Com<ID2D1Bitmap> bmp;
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(rt_->CreateBitmap(D2D1::SizeU(UINT(W), UINT(H)), px.data(), UINT(W * 4), &bp, bmp.Put()))) return nullptr;
        if (shadows_.size() > 48) shadows_.clear();
        auto* raw = bmp.Get();
        shadows_[key] = bmp;
        return raw;
    };
    auto keyOf = [&](int w, int h) {
        return (uint64_t(w) << 40) ^ (uint64_t(h) << 20) ^ uint64_t(std::lround(sigma * 10)) ^ (uint64_t(std::lround(rpx)) << 52);
    };

    const float inv = float(1.0 / s);
    float left = rect.left + dx - pad * inv, top = rect.top + dy - pad * inv;

    int corner = pad + int(std::ceil(rpx)) + 6;
    bool sliced = (wpx > 2 * corner + 8 && hpx > 2 * corner + 8) && (long(wpx) * hpx > 40000);
    if (!sliced) {
        ID2D1Bitmap* bmp = bitmapFor(wpx, hpx, keyOf(wpx, hpx));
        if (!bmp) return;
        D2D1_SIZE_F sz = bmp->GetSize();
        rt_->DrawBitmap(bmp, D2D1::RectF(left, top, left + sz.width * inv, top + sz.height * inv), alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }

    // Large shadow: a small template cut into nine pieces; the corners stay, the edges and middle stretch.
    int tpx = 2 * corner + 16;
    ID2D1Bitmap* bmp = bitmapFor(tpx, tpx, keyOf(tpx, tpx));
    if (!bmp) return;
    float Wt = float(tpx + 2 * pad);                 // template bitmap size
    float c = float(corner + pad);                   // corner piece, including the padding
    float totalW = float(wpx + 2 * pad), totalH = float(hpx + 2 * pad);
    float xs[4] = { 0, c, totalW - c, totalW };
    float ys[4] = { 0, c, totalH - c, totalH };
    float sxs[4] = { 0, c, Wt - c, Wt };
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            D2D1_RECT_F src = D2D1::RectF(sxs[i], sxs[j], sxs[i + 1], sxs[j + 1]);
            D2D1_RECT_F dst = D2D1::RectF(left + xs[i] * inv, top + ys[j] * inv, left + xs[i + 1] * inv, top + ys[j + 1] * inv);
            rt_->DrawBitmap(bmp, dst, alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
        }
}

// ---------------------------------------------------------------- text

IDWriteTextFormat* TextFormat(float size, DWRITE_FONT_WEIGHT weight)
{
    static std::unordered_map<uint64_t, Com<IDWriteTextFormat>> cache;
    uint64_t key = (uint64_t(std::lround(size * 100)) << 16) | uint64_t(weight);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.Get();
    Com<IDWriteTextFormat> f;
    if (FAILED(DWrite()->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", f.Put())))
        return nullptr;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    auto* raw = f.Get();
    cache[key] = f;
    return raw;
}

void Surface::DrawText(const wchar_t* text, float size, DWRITE_FONT_WEIGHT weight, const D2D1_RECT_F& rect, uint32_t argb)
{
    IDWriteTextFormat* fmt = TextFormat(size, weight);
    if (!fmt) return;
    rt_->DrawText(text, UINT32(wcslen(text)), fmt, rect, Solid(argb), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

float Surface::MeasureText(const wchar_t* text, float size, DWRITE_FONT_WEIGHT weight)
{
    IDWriteTextFormat* fmt = TextFormat(size, weight);
    if (!fmt) return 0;
    Com<IDWriteTextLayout> layout;
    if (FAILED(DWrite()->CreateTextLayout(text, UINT32(wcslen(text)), fmt, 10000, 1000, layout.Put()))) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

}  // namespace am
