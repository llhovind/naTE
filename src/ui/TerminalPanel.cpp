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

TerminalPanel::TerminalPanel(wxWindow* parent, const AppConfig& cfg)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS),
      m_cfg(cfg),
      m_font(cfg.fontSize, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL),
      m_sbThick(QueryScrollbarThickness())
{
    wxMemoryDC dc;
    dc.SetFont(m_font);
    m_charSize = dc.GetTextExtent("M");

    m_vScroll = new wxScrollBar(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxSB_VERTICAL);
    m_hScroll = new wxScrollBar(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxSB_HORIZONTAL);

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

    Bind(wxEVT_PAINT,      &TerminalPanel::OnPaint,      this);
    Bind(wxEVT_SIZE,       &TerminalPanel::OnSize,       this);
    Bind(wxEVT_MOUSEWHEEL, &TerminalPanel::OnMouseWheel, this);
}

void TerminalPanel::SetDocLayout(::DocLayout* docLayout)
{
    docLayout_ = docLayout;
    if (docLayout_) {
        const wxSize v = ViewportChars();
        docLayout_->SetViewportSize(v.x, v.y);
        docLayout_->ScrollToEnd();
    }
    UpdateScrollbars();
    Refresh();
}

wxSize TerminalPanel::ViewportChars() const
{
    const wxSize sz = GetClientSize();
    return {std::max(1, (sz.x - m_sbThick) / m_charSize.x),
            std::max(1, (sz.y - m_sbThick) / m_charSize.y)};
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
    const wxSize view   = ViewportChars();
    const int    vTop   = docLayout_ ? docLayout_->GetTopRow() : 0;
    const int    vTotal = docLayout_
        ? std::max(docLayout_->GetLineCount(), view.y)
        : view.y;
    m_vScroll->SetScrollbar(vTop, view.y, vTotal, view.y);
    m_hScroll->SetScrollbar(0, view.x, view.x, view.x); // H stub
}

void TerminalPanel::OnDocumentUpdate()
{
    // DocLayout has already adjusted topRow_ internally (autoScroll_ policy).
    UpdateScrollbars();
    Refresh();
}

void TerminalPanel::OnSize(wxSizeEvent& e)
{
    if (docLayout_) {
        const wxSize v = ViewportChars();
        docLayout_->SetViewportSize(v.x, v.y);
    }
    LayoutScrollbars();
    e.Skip();
}

void TerminalPanel::OnScroll(wxScrollEvent& e)
{
    if (docLayout_)
        docLayout_->SetTopRow(m_vScroll->GetThumbPosition());
    UpdateScrollbars();
    Refresh();
    e.Skip();
}

void TerminalPanel::OnMouseWheel(wxMouseEvent& e)
{
    if (!docLayout_) return;
    const int delta = (e.GetWheelRotation() > 0) ? -3 : 3;
    docLayout_->SetTopRow(docLayout_->GetTopRow() + delta);
    UpdateScrollbars();
    Refresh();
}

void TerminalPanel::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(m_cfg.bgColour));
    dc.Clear();
    dc.SetFont(m_font);
    dc.SetBackgroundMode(wxSOLID);

    if (!docLayout_)
        return;

    const wxSize view    = ViewportChars();
    const int    cw      = m_charSize.x;
    const int    ch      = m_charSize.y;
    const int    topRow  = docLayout_->GetTopRow();

    for (int r = 0; r < view.y; ++r) {
        const int layoutRow = topRow + r;
        if (layoutRow >= docLayout_->GetLineCount())
            break;

        const DocLayoutLine line     = docLayout_->GetLine(layoutRow);
        const DocLine&      dline    = line.line;
        const size_t        docStart = line.startCol;
        const size_t        docEnd   = std::min(docStart + (size_t)line.cols,
                                                dline.text.size());

        size_t          runIndex = 0;
        const StyleRun* run      = nullptr;

        while (runIndex < dline.styles.size()) {
            const auto& sr = dline.styles[runIndex];
            if (sr.start + sr.length > docStart) { run = &sr; break; }
            ++runIndex;
        }

        for (size_t docCol = docStart; docCol < docEnd; ++docCol) {
            while (run && docCol >= run->start + run->length) {
                ++runIndex;
                run = (runIndex < dline.styles.size())
                    ? &dline.styles[runIndex] : nullptr;
            }

            const Style& style = run ? run->style : Style{};

            // Map ANSI colour codes (30–37 fg, 40–47 bg) to RGB.
            // Out-of-range codes mean "no colour set" — fall back to the
            // panel's configured default colours rather than wxWHITE/wxBLACK.
            auto ansiToColour = [this](int code, bool isFg) -> wxColour {
                static const wxColour palette[8] = {
                    wxColour(0,0,0),       // 0 black
                    wxColour(170,0,0),     // 1 red
                    wxColour(0,170,0),     // 2 green
                    wxColour(170,170,0),   // 3 yellow
                    wxColour(0,0,170),     // 4 blue
                    wxColour(170,0,170),   // 5 magenta
                    wxColour(0,170,170),   // 6 cyan
                    wxColour(170,170,170), // 7 white
                };
                const int idx = isFg ? (code - 30) : (code - 40);
                if (idx < 0 || idx > 7)
                    return isFg ? m_cfg.textColour : m_cfg.bgColour;
                return palette[idx];
            };

            const wxColour fg = ansiToColour(style.fg, true);
            const wxColour bg = ansiToColour(style.bg, false);

            const int x = (int)(docCol - docStart) * cw;
            const int y = r * ch;

            dc.SetTextForeground(fg);
            dc.SetTextBackground(bg);

            // DrawText with wxSOLID background paints the cell bg automatically.
            wxString glyph(static_cast<wchar_t>(dline.text[docCol]));
            dc.DrawText(glyph, x, y);
        }
    }

    // --- Cursor ---
    const CursorPos cursorPos      = docLayout_->GetCursorPos();
    const int       cursorScreenRow = (int)cursorPos.line - topRow;
    if (cursorScreenRow >= 0 && cursorScreenRow < view.y) {
        const int cx = (int)cursorPos.col * cw;
        const int cy = cursorScreenRow * ch;

        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(*wxBLUE));
        dc.DrawRectangle(cx, cy, cw, ch);

        // Redraw the character under the cursor with inverted colours.
        if (cursorPos.line < (size_t)docLayout_->GetLineCount()) {
            const DocLayoutLine cursorLine  = docLayout_->GetLine((int)cursorPos.line);
            const DocLine&      cursorDLine = cursorLine.line;
            const size_t        docCol      = cursorLine.startCol + cursorPos.col;
            if (docCol < cursorDLine.text.size()) {
                dc.SetTextForeground(*wxBLACK);
                dc.SetTextBackground(*wxWHITE);
                dc.SetBackgroundMode(wxSOLID);
                wxString glyph(static_cast<wchar_t>(cursorDLine.text[docCol]));
                dc.DrawText(glyph, cx, cy);
            }
        }
    }
}
