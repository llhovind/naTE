#include "ui/TerminalPanel.h"
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/settings.h>
#include <algorithm>

static int QueryScrollbarThickness() {
    const int t = wxSystemSettings::GetMetric(wxSYS_VSCROLL_X);
    return (t > 0) ? t : 16;
}

TerminalPanel::TerminalPanel(wxWindow* parent, const AppConfig& cfg)
    : wxPanel(parent, wxID_ANY)
    , m_cfg(cfg)
    , m_font(cfg.fontSize, wxFONTFAMILY_TELETYPE,
             wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL)
    , m_origin(0, 0)
    , m_sbThick(QueryScrollbarThickness())
    , m_buffer(cfg.columns, cfg.rows, cfg.textColour, cfg.bgColour)
{
    wxMemoryDC dc;
    dc.SetFont(m_font);
    m_charSize = dc.GetTextExtent("M");

    m_vScroll = new wxScrollBar(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxSB_VERTICAL);
    m_hScroll = new wxScrollBar(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxSB_HORIZONTAL);

    // Bind every scroll action so arrows, page clicks, and dragging all update the origin.
    auto bindScrollEvents = [this](wxScrollBar* sb) {
        sb->Bind(wxEVT_SCROLL_TOP,        &TerminalPanel::OnScroll, this);
        sb->Bind(wxEVT_SCROLL_BOTTOM,     &TerminalPanel::OnScroll, this);
        sb->Bind(wxEVT_SCROLL_LINEUP,     &TerminalPanel::OnScroll, this);
        sb->Bind(wxEVT_SCROLL_LINEDOWN,   &TerminalPanel::OnScroll, this);
        sb->Bind(wxEVT_SCROLL_PAGEUP,     &TerminalPanel::OnScroll, this);
        sb->Bind(wxEVT_SCROLL_PAGEDOWN,   &TerminalPanel::OnScroll, this);
        sb->Bind(wxEVT_SCROLL_THUMBTRACK, &TerminalPanel::OnScroll, this);
        sb->Bind(wxEVT_SCROLL_CHANGED,    &TerminalPanel::OnScroll, this);
    };
    bindScrollEvents(m_hScroll);
    bindScrollEvents(m_vScroll);

    SetBackgroundColour(cfg.bgColour);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinClientSize({cfg.columns * m_charSize.x + m_sbThick,
                      cfg.rows    * m_charSize.y + m_sbThick});

    Bind(wxEVT_PAINT, &TerminalPanel::OnPaint, this);
    Bind(wxEVT_SIZE,  &TerminalPanel::OnSize,  this);
}

wxSize TerminalPanel::ViewportChars() const {
    const wxSize sz = GetClientSize();
    return {(sz.x - m_sbThick) / m_charSize.x,
            (sz.y - m_sbThick) / m_charSize.y};
}

void TerminalPanel::LayoutScrollbars() {
    const wxSize sz = GetClientSize();
    const int vx = sz.x - m_sbThick;
    const int vy = sz.y - m_sbThick;
    m_vScroll->SetSize(vx, 0,  m_sbThick, vy);
    m_hScroll->SetSize(0,  vy, vx,        m_sbThick);
    UpdateScrollbars();
}

void TerminalPanel::UpdateScrollbars() {
    const wxSize view = ViewportChars();
    const int hRange = std::max(m_buffer.cols(), view.x);
    const int vRange = std::max(m_buffer.rows(), view.y);
    m_hScroll->SetScrollbar(m_origin.x, view.x, hRange, view.x);
    m_vScroll->SetScrollbar(m_origin.y, view.y, vRange, view.y);
}

void TerminalPanel::OnSize(wxSizeEvent& e) {
    LayoutScrollbars();
    e.Skip();
}

void TerminalPanel::OnScroll(wxScrollEvent& e) {
    m_origin.x = m_hScroll->GetThumbPosition();
    m_origin.y = m_vScroll->GetThumbPosition();
    Refresh();
    e.Skip();
}

void TerminalPanel::OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(m_cfg.bgColour));
    dc.Clear();
    dc.SetFont(m_font);
    dc.SetBackgroundMode(wxSOLID);

    const wxSize view = ViewportChars();
    for (int r = 0; r < view.y; ++r) {
        const int bufRow = m_origin.y + r;
        if (bufRow >= m_buffer.rows()) break;
        for (int c = 0; c < view.x; ++c) {
            const int bufCol = m_origin.x + c;
            if (bufCol >= m_buffer.cols()) break;
            const Cell& cell = m_buffer.at(bufRow, bufCol);
            dc.SetTextForeground(cell.fg);
            dc.SetTextBackground(cell.bg);
            dc.DrawText(wxString(cell.ch), c * m_charSize.x, r * m_charSize.y);
        }
    }
}
