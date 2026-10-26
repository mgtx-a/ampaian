// The quick markup editor.
#pragma once
#include "common.h"

namespace am {

/// Opens the editor on a picture. `saved` is called (on the UI thread) once the
/// file has been written over.
void OpenMarkup(const wstring& path, std::function<void(const wstring&)> saved);
bool MarkupIsOpen();

}  // namespace am
