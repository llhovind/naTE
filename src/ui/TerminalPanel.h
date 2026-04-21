#pragma once
#include <wx/panel.h>
#include <wx/scrolbar.h>
#include <optional>
#include "document/Document.h"
#include "ui/Layout.h"
#include "config/Config.h"

class TerminalPanel : public wxPanel
{
public:
    TerminalPanel(wxWindow *parent, const AppConfig &cfg);

    void SetDocument(const Document *document);
    // Buffer&       buffer()       { return m_buffer; }
    // const Buffer& buffer() const { return m_buffer; }

private:
    const Document *doc = nullptr;
    std::optional<::Layout> layout;

    int scrollOffset = 0;

    int charWidth = 10;
    int charHeight = 18;

    void RebuildLayout();

    void OnPaint(wxPaintEvent &);
    void OnSize(wxSizeEvent &);
    void OnScroll(wxScrollEvent &);

    void LayoutScrollbars();
    void UpdateScrollbars();
    wxSize ViewportChars() const;

    AppConfig m_cfg;
    wxFont m_font;
    wxSize m_charSize;
    wxScrollBar *m_hScroll;
    wxScrollBar *m_vScroll;
    wxPoint m_origin; // scroll position in character units
    int m_sbThick;    // scrollbar thickness in pixels
    // Buffer       m_buffer;
};
