// Drawing: Direct2D on the CPU (so no GPU driver is loaded and memory stays small),
// presented through layered windows with per-pixel alpha.
#pragma once
#include "common.h"

namespace am {

/// 0xAARRGGBB, straight (not premultiplied) alpha.
inline D2D1_COLOR_F Argb(uint32_t c)
{
    return D2D1::ColorF(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, ((c >> 24) & 255) / 255.0f);
}
inline uint32_t MakeArgb(int a, int r, int g, int b) { return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b); }

ID2D1Factory* D2D();
IDWriteFactory* DWrite();
IWICImagingFactory* Wic();
bool GfxInit();     // COM, factories

// ---------------------------------------------------------------- pictures

/// Premultiplied BGRA pixels.
struct Pixels {
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;
    uint64_t uid = 0;       // identifies the picture in per-surface bitmap caches
    Pixels();
    int stride() const { return w * 4; }
};
using PixelsPtr = std::shared_ptr<Pixels>;

/// A picture and smaller copies of it (halved each time), so that drawing it
/// small samples a copy at most twice as big: sharp and free of aliasing.
struct Mips {
    std::vector<PixelsPtr> levels;
    int width() const { return levels.empty() ? 0 : levels[0]->w; }
    int height() const { return levels.empty() ? 0 : levels[0]->h; }
    /// The smallest level that is still at least `pixelsWide` wide.
    const Pixels* Pick(double pixelsWide) const;
    bool empty() const { return levels.empty(); }
};

/// Decodes `path` through WIC. The longest side is scaled to at most maxPixels.
/// Also reports the original size. Returns empty Mips on failure.
Mips LoadMips(const wstring& path, int maxPixels, int* origW = nullptr, int* origH = nullptr);
bool ImageSize(const wstring& path, int* w, int* h);
/// Small BGR copy used to tell whether two captures are the same picture.
bool LoadSignature(const wstring& path, std::vector<uint8_t>& out, int* origW, int* origH);

// ---------------------------------------------------------------- canvas

/// A Direct2D render target over a window-sized pixel buffer.
class Surface {
public:
    Surface();
    ~Surface();
    /// Pixel size. Recreates the buffer only when it changes.
    void Resize(int w, int h);
    void Release();                  // give the buffer back (hidden windows hold nothing)
    int width() const { return w_; }
    int height() const { return h_; }
    bool ready() const { return rt_ && bits_; }

    /// Starts a frame: transparent, drawing in DIPs (scaled by the system DPI).
    ID2D1DCRenderTarget* Begin();
    bool End();
    /// Shows the frame in a layered window at (x, y) in screen pixels.
    void Present(HWND hwnd, int x, int y, BYTE alpha = 255);

    // Caches, valid for this surface.
    ID2D1SolidColorBrush* Solid(uint32_t argb);
    ID2D1Bitmap* BitmapFor(const Pixels& px);
    ID2D1Layer* Layer();
    void ClearCaches();

    /// A soft shadow of a rounded rectangle (DIPs): the blurred shape, offset, tinted black.
    void DrawShadow(const D2D1_RECT_F& rect, float radius, float blurRadius, float dx, float dy, float alpha);

    /// Draws a picture (premultiplied) into dest (DIPs), choosing the right mip level.
    void DrawMips(const Mips& m, const D2D1_RECT_F& dest, float opacity = 1.0f);
    /// The same, clipped to a rounded rectangle.
    /// With cover=true the picture fills the whole rectangle, cropped to its shape.
    void FillMips(const Mips& m, const D2D1_RECT_F& dest, float radius, float opacity = 1.0f, bool cover = false);
    /// Text, left/top aligned inside rect.
    void DrawText(const wchar_t* text, float size, DWRITE_FONT_WEIGHT weight, const D2D1_RECT_F& rect, uint32_t argb);
    float MeasureText(const wchar_t* text, float size, DWRITE_FONT_WEIGHT weight);

private:
    int w_ = 0, h_ = 0;
    HDC mem_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* bits_ = nullptr;
    Com<ID2D1DCRenderTarget> rt_;
    std::unordered_map<uint32_t, Com<ID2D1SolidColorBrush>> brushes_;
    std::unordered_map<uint64_t, Com<ID2D1Bitmap>> bitmaps_;
    std::unordered_map<uint64_t, Com<ID2D1Bitmap>> shadows_;
    Com<ID2D1Layer> layer_;
    bool drawing_ = false;
};

IDWriteTextFormat* TextFormat(float size, DWRITE_FONT_WEIGHT weight);

inline D2D1_RECT_F RectF(double x, double y, double w, double h) { return D2D1::RectF(float(x), float(y), float(x + w), float(y + h)); }

}  // namespace am
