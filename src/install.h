// Keeping Ampaian on this PC: the downloaded exe offers, once, to copy itself to the
// per-user apps folder with a Start menu shortcut and a start at sign-in. No admin needed.
#pragma once
#include "common.h"

namespace am {

wstring InstallDir();                 // %LOCALAPPDATA%\Programs\Ampaian
bool RunningInstalled();              // this exe is the installed copy
bool AutoStartOn();
void SetAutoStart(bool on);           // the HKCU Run value, pointing at this exe
wstring OfferInstall();               // the installed exe to start instead of this one, or empty
wstring Install();                    // from the tray menu: asks first; the installed exe, or empty
bool Uninstall();                     // asks first; true when this copy should quit

}  // namespace am
