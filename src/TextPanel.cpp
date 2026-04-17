#include "TextPanel.h"
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>

TextPanel::TextPanel(wxWindow* parent, const AppConfig& cfg)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                       wxHSCROLL | wxVSCROLL)
    , m_cfg(cfg)
    , m_font(cfg.fontSize, wxFONTFAMILY_TELETYPE,
             wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL)
{
    wxMemoryDC dc;
    dc.SetFont(m_font);
    m_charSize = dc.GetTextExtent("M");

    SetScrollRate(m_charSize.x, m_charSize.y);
    SetVirtualSize(cfg.columns * m_charSize.x, cfg.rows * m_charSize.y);
    SetMinClientSize({cfg.columns * m_charSize.x, cfg.rows * m_charSize.y});
    SetBackgroundColour(cfg.bgColour);
    SetForegroundColour(cfg.textColour);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    AlwaysShowScrollbars(true, true);

    Bind(wxEVT_PAINT, &TextPanel::OnPaint, this);
}

void TextPanel::OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    PrepareDC(dc);
    dc.SetBackground(wxBrush(m_cfg.bgColour));
    dc.Clear();
    dc.SetFont(m_font);
    dc.SetTextForeground(m_cfg.textColour);
}
