#pragma once
#include <string>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>

namespace ui {

// Reads text from the system clipboard (or the X11 primary selection when
// primary is true) and returns it as UTF-8. wxDF_UNICODETEXT is preferred
// over wxDF_TEXT per the wx clipboard contract; wxTextDataObject handles
// the extraction for both. Returns an empty string when no text is available.
inline std::string ReadClipboardText(bool primary = false)
{
#ifdef __WXGTK__
    if (primary) wxTheClipboard->UsePrimarySelection(true);
#endif
    wxString text;
    if (wxTheClipboard->Open()) {
        if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT) ||
            wxTheClipboard->IsSupported(wxDF_TEXT)) {
            wxTextDataObject data;
            wxTheClipboard->GetData(data);
            text = data.GetText();
        }
        wxTheClipboard->Close();
    }
#ifdef __WXGTK__
    if (primary) wxTheClipboard->UsePrimarySelection(false);
#endif
    return std::string(text.ToUTF8());
}

} // namespace ui
