// The clipboard: putting a photo on it, and picking up a bare screenshot from it.
#pragma once
#include "common.h"

namespace am {

/// Puts the photo on the clipboard as a file, a PNG and a bitmap, from a
/// background thread. `done` runs on the UI thread.
void WriteClipboardAsync(const wstring& path, std::function<void(bool ok)> done);

enum class ClipImage { None, Bare, NotAScreenshot, Busy };
/// Whether the clipboard holds just a picture: Win+Shift+S and PrtSc leave exactly that.
/// Anything that also carries text, HTML or files is an ordinary copy from an app.
ClipImage InspectClipboard();

/// Saves the clipboard's picture as a PNG file. False while the clipboard is busy.
bool SaveClipboardImage(const wstring& pngPath);

}  // namespace am
