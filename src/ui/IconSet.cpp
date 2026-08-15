#include "ui/IconSet.h"

#include <wx/features.h>

#include <string>

#ifndef wxHAS_SVG
#error "IconSet needs wxBitmapBundle::FromSVG; this wxWidgets port lacks raw bitmap access."
#endif

namespace ui {

namespace {

// Geometry of the glyphs, on the 16x16 grid declared by the viewBox below.
//
// The coordinates are the drawing — the same way an SGR code in parser/ is the
// escape sequence — so they are not constants waiting to be named. Every glyph
// is stroke-only and carries no colour of its own; the stroke is applied once
// at the root element in Compose(), which is what makes the whole set
// re-themeable from a single wxColour.
const char* PathsFor(Icon icon)
{
    switch (icon) {
        case Icon::Back:
            return R"(<path d="M10 3.5 L5.5 8 L10 12.5"/>)";

        case Icon::Forward:
            return R"(<path d="M6 3.5 L10.5 8 L6 12.5"/>)";

        case Icon::Up:
            return R"(<path d="M8 13.2 V3.4"/>)"
                   R"(<path d="M3.6 7.8 L8 3.4 L12.4 7.8"/>)";

        // Reload: a near-complete circle broken at the upper right, closed by
        // an arrowhead that reads as direction of travel even at 16px.
        case Icon::Refresh:
            return R"(<path d="M12.6 6.2 A5 5 0 1 0 12.9 9.6"/>)"
                   R"(<path d="M12.9 2.6 L12.9 6.4 L9.2 6.4"/>)";

        // Plus centred in the folder body rather than tucked into a corner:
        // at toolbar size a corner badge merges with the folder outline.
        case Icon::NewFolder:
            return R"(<path d="M1.75 12.5 V3.5 H6 L7.6 5.5 H14.25 V12.5 Z"/>)"
                   R"(<path d="M8 7.6 V11.4 M6.1 9.5 H9.9"/>)";

        case Icon::CopyRight:
            return R"(<path d="M2.5 8 H12.2"/>)"
                   R"(<path d="M8.4 4.2 L12.2 8 L8.4 11.8"/>)";

        case Icon::CopyLeft:
            return R"(<path d="M13.5 8 H3.8"/>)"
                   R"(<path d="M7.6 4.2 L3.8 8 L7.6 11.8"/>)";

        // Two panes side by side — the shape the window takes in Transfer mode.
        case Icon::SplitPanes:
            return R"(<path d="M1.75 3.25 H6.75 V12.75 H1.75 Z"/>)"
                   R"(<path d="M9.25 3.25 H14.25 V12.75 H9.25 Z"/>)";

        case Icon::Cancel:
            return R"(<path d="M4.2 4.2 L11.8 11.8 M11.8 4.2 L4.2 11.8"/>)";

        // The same cross, ringed: "every one of them", visibly the broader
        // sibling of Cancel rather than an unrelated glyph.
        case Icon::CancelAll:
            return R"(<circle cx="8" cy="8" r="6"/>)"
                   R"(<path d="M5.6 5.6 L10.4 10.4 M10.4 5.6 L5.6 10.4"/>)";

        // The two transport-control glyphs everyone already knows. Stroked, not
        // filled, like the rest of the set: Compose sets fill="none" once for
        // the whole sheet and supplies only a stroke colour, so a filled shape
        // here would have to name a colour this function cannot see — and an
        // unknown colour rasterises to black, which is an invisible icon on a
        // dark theme rather than a visible failure.
        case Icon::Pause:
            return R"(<path d="M6 3.75 V12.25 M10 3.75 V12.25"/>)";

        case Icon::Resume:
            return R"(<path d="M5.5 3.75 L12 8 L5.5 12.25 Z"/>)";

        case Icon::ClearFinished:
            return R"(<path d="M2.75 4.5 H13.25"/>)"
                   R"(<path d="M6.25 4.5 V2.75 H9.75 V4.5"/>)"
                   R"(<path d="M4 4.5 L4.8 13.25 H11.2 L12 4.5"/>)"
                   R"(<path d="M6.6 7 V11 M9.4 7 V11"/>)";
    }
    return "";
}

std::string Compose(Icon icon, const wxColour& stroke)
{
    // wxC2S_HTML_SYNTAX yields "#rrggbb", the one colour form every SVG parser
    // agrees on. Named colours and currentColor are not portable here: the
    // rasteriser behind FromSVG resolves an unknown colour to black, which on
    // a dark theme is an invisible icon rather than a visible failure.
    const std::string colour(stroke.GetAsString(wxC2S_HTML_SYNTAX).ToUTF8());

    return R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" )"
           R"(viewBox="0 0 16 16" fill="none" stroke=")" + colour +
           R"(" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">)" +
           PathsFor(icon) + "</svg>";
}

} // namespace

wxBitmapBundle IconBundle(Icon icon, const wxColour& stroke, int sizeDip)
{
    const std::string svg = Compose(icon, stroke);
    return wxBitmapBundle::FromSVG(reinterpret_cast<const wxByte*>(svg.data()),
                                   svg.size(), wxSize(sizeDip, sizeDip));
}

} // namespace ui
