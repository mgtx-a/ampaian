// The geometry of the line, in DIPs: the same constants as the macOS and C# versions.
#pragma once
#include "common.h"

namespace am::L {

constexpr double PanelHeight = 210;
constexpr double RopeTop = 10;
constexpr double Spacing = 174;
constexpr double CardWidth = 150;
constexpr double PinAbove = 9.5;
constexpr double CardOffsetBelowTop = 26 - 12;   // clip (26 tall) over the card by 12
constexpr double Radius = 16;                    // glass frame
constexpr double Inset = 4;                      // frame around the photo

/// The rope hangs as a parabola from edge to edge of the screen.
inline double Sag(double width) { return std::min(30.0, width * 0.018); }

inline double RopeY(double x, double width)
{
    if (width <= 0) return RopeTop;
    double f = x / width;
    return RopeTop + 4 * Sag(width) * f * (1 - f);
}

inline double X(int index, int count, double width)
{
    double total = std::max(count - 1, 0) * Spacing;
    return width / 2 - total / 2 + index * Spacing;
}

struct Size { double w, h; };

/// The photo fits inside the card area keeping its proportions.
inline Size PhotoSize(int tw, int th)
{
    double maxW = CardWidth - 14, maxH = 104;
    if (tw <= 0 || th <= 0) return { maxW, maxH };
    double s = std::min(maxW / tw, maxH / th);
    return { tw * s, th * s };
}

inline Size CardSize(int tw, int th)
{
    Size p = PhotoSize(tw, th);
    return { p.w + Inset * 2, p.h + Inset * 2 };
}

}  // namespace am::L
