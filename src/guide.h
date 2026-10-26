// The first-run tour: a small card that walks you through the app, moving on
// by itself as you do each thing. Also available from the tray menu.
#pragma once
#include "common.h"

namespace am {

struct TourHooks {
    std::function<int()> liveCount;     // photos on the line
    std::function<bool()> revealed;     // the line is down
    std::function<bool()> copied;       // a photo was just copied
    std::function<bool()> markupOpen;   // the editor is open
};

void StartTour(TourHooks hooks);
bool TourActive();

}  // namespace am
