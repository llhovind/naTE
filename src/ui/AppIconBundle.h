#pragma once

#include <wx/iconbndl.h>

namespace ui {

// The application icon at every size that ships with naTE.
//
// Any top-level window the application opens should wear it: the desktop draws
// it in the taskbar, the alt-tab switcher and the window's own frame, and it is
// what identifies a window as naTE's before a single character of the title is
// read. A frame that does not set it gets the desktop's generic placeholder.
//
// The bundle is built once and shared: decoding five PNGs per window would be
// waste, and the icons never change at runtime.
const wxIconBundle& AppIconBundle();

} // namespace ui
