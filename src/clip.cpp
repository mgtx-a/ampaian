#include "clip.h"

#include "gfx.h"

#include <thread>

namespace am {

namespace {

const UINT CF_DIB_ = 8, CF_DIBV5_ = 17;

UINT PngFormat() { static UINT f = RegisterClipboardFormatW(L"PNG"); return f; }
UINT HtmlFormat() { static UINT f = RegisterClipboardFormatW(L"HTML Format"); return f; }
UINT RtfFormat() { static UINT f = RegisterClipboardFormatW(L"Rich Text Format"); return f; }

HGLOBAL AllocCopy(const void* data, size_t size)
{
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) return nullptr;
    void* p = GlobalLock(mem);
    memcpy(p, data, size);
    GlobalUnlock(mem);
    return mem;
}

/// Decodes the picture a band at a time straight into memory that Windows will
/// own, so a 4K screenshot costs one 33 MB buffer and not several.
HGLOBAL BuildDib(const wstring& path)
{
    Com<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, stream.Put()))) return nullptr;
    Com<IWICBitmapDecoder> dec;
    if (FAILED(Wic()->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.Put()))) return nullptr;
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, frame.Put()))) return nullptr;
    Com<IWICFormatConverter> conv;
    if (FAILED(Wic()->CreateFormatConverter(conv.Put()))) return nullptr;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return nullptr;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h) return nullptr;
    UINT stride = w * 4;

    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, 40 + size_t(stride) * h);
    if (!mem) return nullptr;
    auto* p = static_cast<uint8_t*>(GlobalLock(mem));
    BITMAPINFOHEADER bih{};
    bih.biSize = 40; bih.biWidth = LONG(w); bih.biHeight = LONG(h);   // positive: bottom-up
    bih.biPlanes = 1; bih.biBitCount = 32; bih.biCompression = BI_RGB; bih.biSizeImage = stride * h;
    memcpy(p, &bih, 40);

    const UINT band = 64;
    std::vector<uint8_t> rows(size_t(stride) * band);
    for (UINT y = 0; y < h; y += band) {
        UINT n = std::min(band, h - y);
        WICRect r{ 0, INT(y), INT(w), INT(n) };
        if (FAILED(conv->CopyPixels(&r, stride, stride * n, rows.data()))) { GlobalUnlock(mem); GlobalFree(mem); return nullptr; }
        for (UINT i = 0; i < n; i++)
            memcpy(p + 40 + size_t(h - 1 - (y + i)) * stride, rows.data() + size_t(i) * stride, stride);
    }
    GlobalUnlock(mem);
    return mem;
}

/// PNG bytes for the clipboard: the file itself when it is a PNG, otherwise encoded.
bool PngBytes(const wstring& path, std::vector<uint8_t>& out)
{
    if (Extension(path) == L".png") return ReadAllBytes(path, out);
    Com<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, stream.Put()))) return false;
    Com<IWICBitmapDecoder> dec;
    if (FAILED(Wic()->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.Put()))) return false;
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, frame.Put()))) return false;

    Com<IStream> mem;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, mem.Put()))) return false;
    Com<IWICBitmapEncoder> enc;
    if (FAILED(Wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.Put()))) return false;
    if (FAILED(enc->Initialize(mem.Get(), WICBitmapEncoderNoCache))) return false;
    Com<IWICBitmapFrameEncode> fe;
    Com<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(fe.Put(), props.Put()))) return false;
    fe->Initialize(props.Get());
    UINT w, h;
    frame->GetSize(&w, &h);
    fe->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    fe->SetPixelFormat(&fmt);
    if (FAILED(fe->WriteSource(frame.Get(), nullptr))) return false;
    fe->Commit();
    enc->Commit();
    STATSTG st{};
    mem->Stat(&st, STATFLAG_NONAME);
    out.resize(size_t(st.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    mem->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG got = 0;
    return SUCCEEDED(mem->Read(out.data(), ULONG(out.size()), &got)) && got == out.size();
}

HGLOBAL BuildDrop(const wstring& path)
{
    size_t bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!mem) return nullptr;
    auto* df = static_cast<DROPFILES*>(GlobalLock(mem));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy(reinterpret_cast<uint8_t*>(df) + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
    GlobalUnlock(mem);
    return mem;
}

bool OpenClipboardRetry(int attempts)
{
    for (int i = 0; i < attempts; i++) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(25);
    }
    return false;
}

}  // namespace

void WriteClipboardAsync(const wstring& path, std::function<void(bool)> done)
{
    std::thread([path, done = std::move(done)] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HGLOBAL dib = BuildDib(path);
        HGLOBAL drop = BuildDrop(path);
        std::vector<uint8_t> png;
        HGLOBAL pngMem = PngBytes(path, png) ? AllocCopy(png.data(), png.size()) : nullptr;
        png = {};

        bool ok = false;
        // Other apps (remote desktop tools, clipboard managers) sometimes hold the clipboard
        // open for a moment, hence the patience.
        if (OpenClipboardRetry(40)) {
            EmptyClipboard();
            if (drop && SetClipboardData(CF_HDROP, drop)) drop = nullptr;
            if (pngMem && SetClipboardData(PngFormat(), pngMem)) pngMem = nullptr;
            if (dib && SetClipboardData(CF_DIB_, dib)) dib = nullptr;
            CloseClipboard();
            ok = true;
        }
        if (dib) GlobalFree(dib);
        if (drop) GlobalFree(drop);
        if (pngMem) GlobalFree(pngMem);
        CoUninitialize();
        PostToUi([done, ok] { done(ok); });
    }).detach();
}

ClipImage InspectClipboard()
{
    if (!OpenClipboardRetry(6)) return ClipImage::Busy;
    ClipImage r = ClipImage::None;
    bool image = IsClipboardFormatAvailable(CF_DIB_) || IsClipboardFormatAvailable(CF_DIBV5_) || IsClipboardFormatAvailable(PngFormat());
    if (image) {
        bool other = IsClipboardFormatAvailable(CF_HDROP) || IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT) ||
                     IsClipboardFormatAvailable(HtmlFormat()) || IsClipboardFormatAvailable(RtfFormat());
        r = other ? ClipImage::NotAScreenshot : ClipImage::Bare;
    }
    CloseClipboard();
    return r;
}

bool SaveClipboardImage(const wstring& pngPath)
{
    if (!OpenClipboardRetry(6)) return false;
    bool ok = false;
    HANDLE h = GetClipboardData(PngFormat());
    if (h) {
        if (void* p = GlobalLock(h)) {
            ok = WriteAllBytes(pngPath, p, GlobalSize(h));
            GlobalUnlock(h);
        }
    } else if (HANDLE dibH = GetClipboardData(CF_DIB_)) {
        // A bare DIB: wrap it in a BMP header and let WIC turn it into a PNG.
        if (void* p = GlobalLock(dibH)) {
            size_t size = GlobalSize(dibH);
            auto* bih = static_cast<BITMAPINFOHEADER*>(p);
            size_t colors = bih->biClrUsed ? bih->biClrUsed : (bih->biBitCount <= 8 ? (size_t(1) << bih->biBitCount) : 0);
            size_t masks = (bih->biCompression == BI_BITFIELDS && bih->biSize == 40) ? 12 : 0;
            BITMAPFILEHEADER bfh{};
            bfh.bfType = 0x4D42;
            bfh.bfSize = DWORD(sizeof bfh + size);
            bfh.bfOffBits = DWORD(sizeof bfh + bih->biSize + masks + colors * 4);
            std::vector<uint8_t> bmp(sizeof bfh + size);
            memcpy(bmp.data(), &bfh, sizeof bfh);
            memcpy(bmp.data() + sizeof bfh, p, size);
            GlobalUnlock(dibH);

            Com<IStream> in;
            in = Com<IStream>(SHCreateMemStream(bmp.data(), UINT(bmp.size())));
            Com<IWICBitmapDecoder> dec;
            Com<IWICBitmapFrameDecode> frame;
            if (in && SUCCEEDED(Wic()->CreateDecoderFromStream(in.Get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.Put())) &&
                SUCCEEDED(dec->GetFrame(0, frame.Put()))) {
                Com<IStream> out;
                Com<IWICBitmapEncoder> enc;
                Com<IWICBitmapFrameEncode> fe;
                Com<IPropertyBag2> props;
                if (SUCCEEDED(SHCreateStreamOnFileEx(pngPath.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, out.Put())) &&
                    SUCCEEDED(Wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.Put())) &&
                    SUCCEEDED(enc->Initialize(out.Get(), WICBitmapEncoderNoCache)) &&
                    SUCCEEDED(enc->CreateNewFrame(fe.Put(), props.Put())) && SUCCEEDED(fe->Initialize(props.Get()))) {
                    UINT w, h;
                    frame->GetSize(&w, &h);
                    fe->SetSize(w, h);
                    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
                    fe->SetPixelFormat(&fmt);
                    Com<IWICFormatConverter> conv;
                    Wic()->CreateFormatConverter(conv.Put());
                    conv->Initialize(frame.Get(), fmt, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
                    ok = SUCCEEDED(fe->WriteSource(conv.Get(), nullptr)) && SUCCEEDED(fe->Commit()) && SUCCEEDED(enc->Commit());
                }
            }
        }
    }
    CloseClipboard();
    return ok;
}

}  // namespace am
