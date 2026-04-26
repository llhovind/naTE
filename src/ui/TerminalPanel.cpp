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

static constexpr int kResizeDebounceMs = 80;

TerminalPanel::TerminalPanel(wxWindow* parent, const AppConfig& cfg)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS),
      m_cfg(cfg),
      m_font(cfg.fontSize, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL),
      m_sbThick(QueryScrollbarThickness()),
      resizeTimer_(this)
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

    SetBackgroundColour(wxColour(cfg.bgColour.r, cfg.bgColour.g, cfg.bgColour.b));
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinClientSize({cfg.columns * m_charSize.x + m_sbThick,
                      cfg.rows    * m_charSize.y + m_sbThick});

    Bind(wxEVT_PAINT,      &TerminalPanel::OnPaint,       this);
    Bind(wxEVT_SIZE,       &TerminalPanel::OnSize,        this);
    Bind(wxEVT_TIMER,      &TerminalPanel::OnResizeTimer, this);
    Bind(wxEVT_MOUSEWHEEL, &TerminalPanel::OnMouseWheel,  this);
    Bind(wxEVT_SET_FOCUS,  &TerminalPanel::OnFocus,       this);
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

void TerminalPanel::EnsureCursorVisible()
{
    if (!docLayout_) return;
    docLayout_->EnsureCursorVisible();
    UpdateScrollbars();
    Refresh();
}

void TerminalPanel::OnSize(wxSizeEvent& e)
{
    if (docLayout_) {
        const wxSize v = ViewportChars();
        docLayout_->SetViewportSize(v.x, v.y);
        if (resizeCb_) {
            pendingResize_ = v;
            resizeTimer_.StartOnce(kResizeDebounceMs);
        }
    }
    LayoutScrollbars();
    e.Skip();
}

void TerminalPanel::OnResizeTimer(wxTimerEvent&)
{
    if (resizeCb_ && pendingResize_.x > 0 && pendingResize_.y > 0)
        resizeCb_(static_cast<unsigned short>(pendingResize_.x),
                  static_cast<unsigned short>(pendingResize_.y));
}

void TerminalPanel::OnScroll(wxScrollEvent& e)
{
    if (docLayout_) {
        const int topRow = m_vScroll->GetThumbPosition();
        if (scrollCb_)
            scrollCb_(topRow);
        else
            docLayout_->SetTopRow(topRow);
    }
    UpdateScrollbars();
    Refresh();
    e.Skip();
}

void TerminalPanel::OnMouseWheel(wxMouseEvent& e)
{
    if (!docLayout_) return;
    const int delta  = (e.GetWheelRotation() > 0) ? -3 : 3;
    const int topRow = docLayout_->GetTopRow() + delta;
    if (scrollCb_)
        scrollCb_(topRow);
    else
        docLayout_->SetTopRow(topRow);
    UpdateScrollbars();
    Refresh();
}

void TerminalPanel::OnFocus(wxFocusEvent& e)
{
    if (focusCb_) focusCb_();
    e.Skip();
}

void TerminalPanel::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(wxColour(m_cfg.bgColour.r, m_cfg.bgColour.g, m_cfg.bgColour.b)));
    dc.Clear();
    dc.SetFont(m_font);
    dc.SetBackgroundMode(wxSOLID);

    if (!docLayout_)
        return;

    const wxSize view    = ViewportChars();
    const int    cw      = m_charSize.x;
    const int    ch      = m_charSize.y;
    const int    topRow  = docLayout_->GetTopRow();

    // Resolve a palette index (0–255, or -1 for terminal default) to RGB.
    // Indices 0–15: standard 16-colour ANSI palette.
    // Indices 16–231: 6×6×6 colour cube (xterm-256 formula).
    // Indices 232–255: 24-step greyscale ramp.
    auto resolveColour = [this](int index, bool isFg) -> wxColour {
        static const wxColour kAnsi16[16] = {
            {  0,   0,   0},  //  0 black
            {170,   0,   0},  //  1 red
            {  0, 170,   0},  //  2 green
            {170, 170,   0},  //  3 yellow
            {  0,   0, 170},  //  4 blue
            {170,   0, 170},  //  5 magenta
            {  0, 170, 170},  //  6 cyan
            {170, 170, 170},  //  7 white
            { 85,  85,  85},  //  8 bright black
            {255,  85,  85},  //  9 bright red
            { 85, 255,  85},  // 10 bright green
            {255, 255,  85},  // 11 bright yellow
            { 85,  85, 255},  // 12 bright blue
            {255,  85, 255},  // 13 bright magenta
            { 85, 255, 255},  // 14 bright cyan
            {255, 255, 255},  // 15 bright white
        };
        if (index < 0) {
            const Rgb& c = isFg ? m_cfg.textColour : m_cfg.bgColour;
            return wxColour(c.r, c.g, c.b);
        }
        if (index < 16) {
            return kAnsi16[index];
        }
        if (index < 232) {
            const int n = index - 16;
            const int b = n % 6;
            const int g = (n / 6) % 6;
            const int r = n / 36;
            auto level = [](int v) -> unsigned char {
                return v == 0 ? 0 : static_cast<unsigned char>(55 + v * 40);
            };
            return wxColour(level(r), level(g), level(b));
        }
        // Greyscale ramp: 232 → 8, 255 → 238 (step 10)
        const auto v = static_cast<unsigned char>(8 + (index - 232) * 10);
        return wxColour(v, v, v);
    };

    for (int r = 0; r < view.y; ++r) {
        const int layoutRow = topRow + r;
        if (layoutRow >= docLayout_->GetLineCount())
            break;

        const RenderedLine row = docLayout_->GetRenderedLine(layoutRow);

        for (int col = 0; col < (int)row.text.size(); ++col) {
            dc.SetTextForeground(resolveColour(row.attrs[col].fg, true));
            dc.SetTextBackground(resolveColour(row.attrs[col].bg, false));
            dc.DrawText(wxString(static_cast<wchar_t>(row.text[col])), col * cw, r * ch);
        }

        if (row.hasCursor) {
            const int cx = row.cursorCol * cw;
            const int cy = r * ch;
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(*wxBLUE));
            dc.DrawRectangle(cx, cy, cw, ch);
            if (row.cursorCol < (int)row.text.size()) {
                dc.SetTextForeground(wxColour(m_cfg.bgColour.r,   m_cfg.bgColour.g,   m_cfg.bgColour.b));
                dc.SetTextBackground(wxColour(m_cfg.textColour.r, m_cfg.textColour.g, m_cfg.textColour.b));
                dc.DrawText(wxString(static_cast<wchar_t>(row.text[row.cursorCol])), cx, cy);
            }
        }
    }
}
