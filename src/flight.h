// The capture lifting off the screen and flying up to the line, and a discarded
// card falling off it. Both are drawn in a window of their own, over everything.
#pragma once
#include "common.h"
#include "gfx.h"

namespace am {

/// `fromPx` / `toPx` are in screen pixels: the captured area, and the card's frame on the line.
void FlightFly(Mips image, RectD fromPx, RectD toPx, double tilt, std::function<void()> arrived);
void FlightFall(Mips image, RectD cardPx, double tilt);

bool FlightsActive();
/// Advances and draws every flight; returns whether any is still running.
bool FlightsFrame(double dt);

}  // namespace am
