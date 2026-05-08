#include "ui/SelectionActions.h"
#include "ui/StringUtils.h"
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/filedlg.h>
#include <wx/utils.h>
#include <fstream>

// ---------------------------------------------------------------------------
// CopyAction
// ---------------------------------------------------------------------------

void CopyAction::Execute(const std::u32string& text)
{
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(ToWxString(text)));
        wxTheClipboard->Close();
    }
}

// ---------------------------------------------------------------------------
// SaveToFileAction
// ---------------------------------------------------------------------------

void SaveToFileAction::Execute(const std::u32string& text)
{
    wxFileDialog dlg(nullptr, "Save selected text", "", "",
                     "Text files (*.txt)|*.txt|All files (*)|*",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK)
        return;

    const wxString path = dlg.GetPath();
    std::ofstream out(path.ToStdString(), std::ios::binary);
    if (!out) return;

    // Write UTF-8
    const wxString wx = ToWxString(text);
    const auto utf8 = wx.ToUTF8();
    out.write(utf8.data(), utf8.length());
}

// ---------------------------------------------------------------------------
// WebSearchAction
// ---------------------------------------------------------------------------

// Percent-encode a UTF-8 byte string for use in a URL query parameter.
static std::string PercentEncode(const std::string& utf8)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

void WebSearchAction::Execute(const std::u32string& text)
{
    const wxString wx = ToWxString(text);
    const auto utf8 = wx.ToUTF8();
    const std::string encoded = PercentEncode(std::string(utf8.data(), utf8.length()));
    const std::string url = "https://www.google.com/search?q=" + encoded;
    wxLaunchDefaultBrowser(url);
}
