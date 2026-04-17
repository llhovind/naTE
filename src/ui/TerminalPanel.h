#pragma once
#include <wx/panel.h>
#include <wx/scrolbar.h>
#include "document/Buffer.h"
#include "config/Config.h"

class TerminalPanel : public wxPanel {
public:
    TerminalPanel(wxWindow* parent, const AppConfig& cfg);

    Buffer&       buffer()       { return m_buffer; }
    const Buffer& buffer() const { return m_buffer; }

private:
    void OnPaint(wxPaintEvent&);
    void OnSize(wxSizeEvent&);
    void OnScroll(wxScrollEvent&);

    void   LayoutScrollbars();
    void   UpdateScrollbars();
    wxSize ViewportChars() const;

    AppConfig    m_cfg;
    wxFont       m_font;
    wxSize       m_charSize;
    wxScrollBar* m_hScroll;
    wxScrollBar* m_vScroll;
    wxPoint      m_origin;   // scroll position in character units
    int          m_sbThick;  // scrollbar thickness in pixels
    Buffer       m_buffer;
};
