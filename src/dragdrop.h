// Dragging a photo out of the line as a real file.
//
// Each destination means one thing: an app gets a copy and the photo stays on the
// line; a folder or the Desktop keeps the file and the photo leaves the line; the
// Recycle Bin discards it; nowhere that accepts it, and the photo flies back.
#pragma once
#include "common.h"
#include "gfx.h"

namespace am {

enum class DragEffect { None, Copy, Move };

struct DragStart {
    wstring path;
    Mips thumb;
    double photoW = 0, photoH = 0;    // DIPs, as the card shows the photo
    POINT photoCenterPx{};            // where the photo sits on screen when the drag starts
};

/// Runs the drag (blocks until it ends). If the drop is refused, the ghost flies
/// back and `finished` is called when it lands; otherwise right away.
void RunFileDrag(const DragStart& start, std::function<void(DragEffect)> finished);

/// Advances the ghost's fly-back; returns whether it is still running.
bool DragGhostsFrame(double dt);
bool DragGhostsActive();

}  // namespace am
