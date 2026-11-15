// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"

// A small question window in the Skyline look (Mica, Settings colours), used
// instead of MessageBox. It doesn't block: the answer comes back through onClose.
namespace Dialog {

struct Spec {
    std::wstring caption;      // title bar, e.g. "Skyline – update"
    std::wstring heading;      // big first line
    std::wstring body;         // wraps; may be empty
    std::wstring note;         // smaller, dimmer text under the body; may be empty
    std::wstring primary;      // accent button, e.g. "Update now"; empty = only the other button
    std::wstring secondary;    // e.g. "Later" or "Close"
    float width = 460;         // DIPs
    HWND owner = nullptr;      // centred on and kept above this window; none = topmost on the main display
};

// onClose(true) = the primary button was chosen; false = the other button,
// Esc or the close box. Showing a new dialog closes an open one (as false).
void Show(const Spec& spec, std::function<void(bool)> onClose);

} // namespace Dialog
