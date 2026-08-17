#include "ui/AppIconBundle.h"

// The generated header holds the PNG bytes in arrays with internal linkage, so
// it is included in this one translation unit only — every other includer would
// carry its own copy of them into the binary.
#include "ui/resources/AppIcon.h"

#include <wx/bitmap.h>
#include <wx/icon.h>
#include <wx/image.h>
#include <wx/mstream.h>

namespace ui {

const wxIconBundle& AppIconBundle()
{
    // Built on first use rather than at static-init time: decoding a PNG needs
    // the wxImage handlers, which are not registered until the app starts.
    static const wxIconBundle bundle = [] {
        wxIconBundle icons;
        for (const auto& entry : kAppIcons) {
            wxMemoryInputStream stream(entry.data, entry.len);
            wxImage img(stream, wxBITMAP_TYPE_PNG);
            // A size that failed to decode is skipped rather than fatal: the
            // desktop picks from whatever sizes the bundle does offer, and a
            // window with a smaller icon beats a window that would not open.
            if (!img.IsOk()) continue;
            wxIcon icon;
            icon.CopyFromBitmap(wxBitmap(img));
            icons.AddIcon(icon);
        }
        return icons;
    }();
    return bundle;
}

} // namespace ui
