#pragma once

#include <wx/bmpbndl.h>
#include <wx/colour.h>

namespace ui {

// The button glyphs used by the File Explorer chrome.
//
// Each icon is embedded as SVG source and rasterised on demand, rather than
// pulled from wxART_* stock art or shipped as a PNG. Stock art resolves to
// whatever the desktop theme happens to provide — different drawings on GTK,
// MSW and macOS, and missing entirely under a minimal icon theme — so the
// toolbar would look like a different product per platform. A fixed-size PNG
// solves that but blurs on the HiDPI displays this window is routinely dragged
// between. Path data carried in the binary is the only form that is both
// identical everywhere and sharp at every scale.
enum class Icon {
    Back,
    Forward,
    Up,
    Refresh,
    NewFolder,
    CopyRight,
    CopyLeft,
    SplitPanes,
    Cancel,
    CancelAll,
    ClearFinished,
};

// Toolbar glyph edge length in device-independent pixels. wxBitmapBundle
// renders whatever multiple of this the display actually asks for, so this is
// a logical size, not a raster size.
inline constexpr int kToolbarIconDip = 16;

// Renders `icon` as a bundle drawn in `stroke`. The glyphs are stroke-only by
// construction, which is what lets a single colour re-theme the whole set:
// callers pass the same colour they use for button labels, and the icon can
// never fall out of contrast with the surface it sits on.
wxBitmapBundle IconBundle(Icon icon, const wxColour& stroke,
                          int sizeDip = kToolbarIconDip);

} // namespace ui
