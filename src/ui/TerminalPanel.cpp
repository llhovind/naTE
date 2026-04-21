#include "ui/TerminalPanel.h"
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/settings.h>
#include <algorithm>

static int QueryScrollbarThickness()
{
    const int t = wxSystemSettings::GetMetric(wxSYS_VSCROLL_X);
    return (t > 0) ? t : 16;
}

TerminalPanel::TerminalPanel(wxWindow *parent, const AppConfig &cfg)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS),
      m_cfg(cfg), m_font(cfg.fontSize, wxFONTFAMILY_TELETYPE,
                                                    wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL),
      m_origin(0, 0), m_sbThick(QueryScrollbarThickness())
{
    wxMemoryDC dc;
    dc.SetFont(m_font);
    m_charSize = dc.GetTextExtent("M");

    m_vScroll = new wxScrollBar(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxSB_VERTICAL);
    m_hScroll = new wxScrollBar(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxSB_HORIZONTAL);

    // Bind every scroll action so arrows, page clicks, and dragging all update the origin.
    auto bindScrollEvents = [this](wxScrollBar *sb)
    {
        sb->Bind(wxEVT_SCROLL_TOP, &TerminalPanel::OnScroll, this);
        sb->Bind(wxEVT_SCROLL_BOTTOM, &TerminalPanel::OnScroll, this);
        sb->Bind(wxEVT_SCROLL_LINEUP, &TerminalPanel::OnScroll, this);
        sb->Bind(wxEVT_SCROLL_LINEDOWN, &TerminalPanel::OnScroll, this);
        sb->Bind(wxEVT_SCROLL_PAGEUP, &TerminalPanel::OnScroll, this);
        sb->Bind(wxEVT_SCROLL_PAGEDOWN, &TerminalPanel::OnScroll, this);
        sb->Bind(wxEVT_SCROLL_THUMBTRACK, &TerminalPanel::OnScroll, this);
        sb->Bind(wxEVT_SCROLL_CHANGED, &TerminalPanel::OnScroll, this);
    };
    bindScrollEvents(m_hScroll);
    bindScrollEvents(m_vScroll);

    SetBackgroundColour(cfg.bgColour);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinClientSize({cfg.columns * m_charSize.x + m_sbThick,
                      cfg.rows * m_charSize.y + m_sbThick});

    Bind(wxEVT_PAINT, &TerminalPanel::OnPaint, this);
    Bind(wxEVT_SIZE, &TerminalPanel::OnSize, this);
}

wxSize TerminalPanel::ViewportChars() const
{
    const wxSize sz = GetClientSize();
    return {(sz.x - m_sbThick) / m_charSize.x,
            (sz.y - m_sbThick) / m_charSize.y};
}

void TerminalPanel::LayoutScrollbars()
{
    const wxSize sz = GetClientSize();
    const int vx = sz.x - m_sbThick;
    const int vy = sz.y - m_sbThick;
    m_vScroll->SetSize(vx, 0, m_sbThick, vy);
    m_hScroll->SetSize(0, vy, vx, m_sbThick);
    UpdateScrollbars();
}

void TerminalPanel::UpdateScrollbars()
{
    const wxSize view = ViewportChars();
    // const int hRange = std::max(m_buffer.cols(), view.x);
    // const int vRange = std::max(m_buffer.rows(), view.y);
    // m_hScroll->SetScrollbar(m_origin.x, view.x, hRange, view.x);
    // m_vScroll->SetScrollbar(m_origin.y, view.y, vRange, view.y);
}

void TerminalPanel::OnSize(wxSizeEvent &e)
{
    LayoutScrollbars();
    e.Skip();
}

void TerminalPanel::OnScroll(wxScrollEvent &e)
{
    m_origin.x = m_hScroll->GetThumbPosition();
    m_origin.y = m_vScroll->GetThumbPosition();
    Refresh();
    e.Skip();
}

void TerminalPanel::SetDocument(const Document* document)
{
    doc = document;

    if (doc)
    {
        layout.emplace(*doc, 80);
    }

    Refresh();
}

void TerminalPanel::RebuildLayout()
{
    if (!layout)
        return;

    int cols = GetClientSize().GetWidth() / charWidth;
    if (cols <= 0)
        cols = 1;

    layout->Rebuild(cols);

    // auto-scroll to bottom
    int rows = GetClientSize().GetHeight() / charHeight;
    scrollOffset = std::max(0, layout->GetLineCount() - rows);
}

void TerminalPanel::OnPaint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(m_cfg.bgColour));
    dc.Clear();
    dc.SetFont(m_font);
    dc.SetBackgroundMode(wxSOLID);

    int rows = GetClientSize().GetHeight() / charHeight;

    if (!layout)
        return;

    for (int r = 0; r < rows; ++r)
    {
        int layoutRow = scrollOffset + r;
        if (layoutRow >= layout->GetLineCount())
            break;

        auto line = layout->GetLine(layoutRow);

        // render using same logic as before, but drawing
        size_t docStart = line.startCol;
        const DocLine &dline = line.line;
        size_t docEnd = std::min(docStart + line.cols, dline.text.size());

        size_t runIndex = 0;
        const StyleRun *run = nullptr;

        while (runIndex < dline.styles.size())
        {
            const auto &r = dline.styles[runIndex];
            if (r.start + r.length > docStart)
            {
                run = &r;
                break;
            }
            runIndex++;
        }

        for (size_t docCol = docStart; docCol < docEnd; ++docCol)
        {
            while (run && docCol >= run->start + run->length)
            {
                runIndex++;
                run = (runIndex < dline.styles.size())
                          ? &dline.styles[runIndex]
                          : nullptr;
            }

            char ch = (char)dline.text[docCol];

            wxColour fg = *wxWHITE;
            wxColour bg = *wxBLACK;

            if (run)
            {
                fg = wxColour(run->style.fg, run->style.fg, run->style.fg);
            }

            int x = (docCol - docStart) * charWidth;
            int y = r * charHeight;

            dc.SetTextForeground(fg);
            dc.DrawText(wxString(ch), x, y);
        }
    }
}
