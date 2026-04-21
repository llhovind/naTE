#pragma once
#include <wx/panel.h>
#include <wx/scrolbar.h>
#include "ui/Layout.h"
#include "config/Config.h"

class TerminalPanel : public wxPanel
{
public:
    TerminalPanel(wxWindow* parent, const AppConfig& cfg);

    void SetLayout(::Layout* layout);

private:
    ::Layout* layout_ = nullptr;

    void OnPaint(wxPaintEvent&);
    void OnSize(wxSizeEvent&);
    void OnScroll(wxScrollEvent&);

    void LayoutScrollbars();
    void UpdateScrollbars();
    wxSize ViewportChars() const;

    AppConfig   m_cfg;
    wxFont      m_font;
    wxSize      m_charSize;
    wxScrollBar* m_hScroll;
    wxScrollBar* m_vScroll;
    wxPoint     m_origin;
    int         m_sbThick;
};
