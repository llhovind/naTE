#include "ui/TerminalPanel.h"
#include "ui/SearchBar.h"
#include <wx/clipbrd.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <algorithm>

// Base wxID for context menu action items.
static constexpr int kActionMenuBaseId = wxID_HIGHEST + 100;

static int QueryScrollbarThickness()
{
    const int t = wxSystemSettings::GetMetric(wxSYS_VSCROLL_X);
    return (t > 0) ? t : 16;
}

static constexpr int kResizeDebounceMs = 80;
static constexpr int kInnerPad         = 4;   // px inset between content and panel/scrollbar edges

TerminalPanel::TerminalPanel(wxWindow* parent, const AppConfig& cfg, unsigned short cols)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS),
      m_cfg(cfg),
      m_font(cfg.fontSize, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL),
      m_sbThick(QueryScrollbarThickness()),
      resizeTimer_(this),
      m_selScrollTimer_(this)
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
    SetMinClientSize({cols       * m_charSize.x + m_sbThick + 2 * kInnerPad,
                      cfg.rows   * m_charSize.y + m_sbThick + 2 * kInnerPad});

    Bind(wxEVT_PAINT,      &TerminalPanel::OnPaint,          this);
    Bind(wxEVT_SIZE,       &TerminalPanel::OnSize,           this);
    Bind(wxEVT_TIMER,      &TerminalPanel::OnResizeTimer,    this, resizeTimer_.GetId());
    Bind(wxEVT_TIMER,      &TerminalPanel::OnSelScrollTimer, this, m_selScrollTimer_.GetId());
    Bind(wxEVT_MOUSEWHEEL, &TerminalPanel::OnMouseWheel,     this);
    Bind(wxEVT_LEFT_DOWN,  &TerminalPanel::OnLeftDown,       this);
    Bind(wxEVT_LEFT_UP,    &TerminalPanel::OnLeftUp,         this);
    Bind(wxEVT_MOTION,     &TerminalPanel::OnMouseMove,      this);
    Bind(wxEVT_RIGHT_DOWN, &TerminalPanel::OnRightDown,      this);
    Bind(wxEVT_KEY_DOWN,   &TerminalPanel::OnKeyDown,        this);
    Bind(wxEVT_SET_FOCUS,  &TerminalPanel::OnFocus,          this);
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
    return {std::max(1, (sz.x - m_sbThick - 2 * kInnerPad) / m_charSize.x),
            std::max(1, (sz.y - m_sbThick - 2 * kInnerPad) / m_charSize.y)};
}

void TerminalPanel::SetSearchBar(SearchBar* bar)
{
    searchBar_ = bar;
    if (searchBar_) {
        searchBar_->Hide();
        searchBar_->SetCloseCallback([this]() { ShowSearchBar(false); });
    }
}

void TerminalPanel::ShowSearchBar(bool show)
{
    if (!searchBar_) return;
    if (show) {
        if (searchBarHeight_ > 0) {
            searchBar_->FocusInput();
            return;
        }
        const wxSize best = searchBar_->GetBestSize();
        searchBarHeight_  = best.y;
        searchBar_->SetSize(GetClientSize().x - best.x, 0, best.x, best.y);
        searchBar_->Raise();
        searchBar_->Show();
        searchBar_->FocusInput();
    } else {
        if (searchBarHeight_ == 0) return;
        searchBarHeight_ = 0;
        searchBar_->Hide();
        searchBar_->Reset();
        EnsureCursorVisible();
        SetFocus();
    }
    Refresh();
}

bool TerminalPanel::HasSearchBarFocus() const
{
    if (!searchBar_ || searchBarHeight_ == 0) return false;
    const wxWindow* focused = wxWindow::FindFocus();
    if (!focused) return false;
    const wxWindow* w = focused;
    while (w) {
        if (w == searchBar_) return true;
        w = w->GetParent();
    }
    return false;
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
    const int    vTop   = docLayout_ ? docLayout_->GetTopVisualRow()         : 0;
    const int    vTotal = docLayout_
        ? std::max(docLayout_->GetTotalVisualLineCount(), view.y)
        : view.y;
    m_vScroll->SetScrollbar(vTop, view.y, vTotal, view.y);
    const bool hEnabled = docLayout_ && !docLayout_->GetWordWrap();
    const int  hLeft    = hEnabled ? docLayout_->GetLeftCol()                               : 0;
    const int  hTotal   = hEnabled ? std::max(docLayout_->GetMaxVisibleWidth(), view.x) : view.x;
    m_hScroll->SetScrollbar(hLeft, view.x, hTotal, view.x);
    m_hScroll->Enable(hEnabled);
}

void TerminalPanel::OnDocumentUpdate()
{
    // DocLayout has already adjusted topRow_ internally (autoScroll_ policy).
    UpdateScrollbars();
    // GTK defers widget redraws to idle processing, which only runs when a
    // native input event is in flight. On the data path (CallAfter from a PTY
    // read) there is no input event, so Update() forces gdk_window_process_updates()
    // synchronously rather than waiting for the next keystroke or mouse move.
    m_vScroll->Update();
    m_hScroll->Update();
    Refresh();
}

void TerminalPanel::SyncScrollbars()
{
    UpdateScrollbars();
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
    if (searchBar_ && searchBarHeight_ > 0) {
        const wxSize best = searchBar_->GetBestSize();
        searchBar_->SetSize(GetClientSize().x - best.x, 0, best.x, best.y);
    }

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
        if (!m_selecting_)
            docLayout_->ClearSelection();
        if (e.GetEventObject() == m_hScroll) {
            docLayout_->SetLeftCol(m_hScroll->GetThumbPosition());
        } else {
            const int topRow = m_vScroll->GetThumbPosition();
            if (scrollCb_)
                scrollCb_(topRow);
            else
                docLayout_->SetTopVisualRow(topRow);
        }
    }
    UpdateScrollbars();
    Refresh();
    e.Skip();
}

void TerminalPanel::OnMouseWheel(wxMouseEvent& e)
{
    if (!docLayout_) return;
    if (!m_selecting_)
        docLayout_->ClearSelection();
    const int delta = (e.GetWheelRotation() > 0) ? -3 : 3;
    const bool isHorizontal = (e.GetWheelAxis() == wxMOUSE_WHEEL_HORIZONTAL)
                              || e.ShiftDown();
    if (isHorizontal && !docLayout_->GetWordWrap()) {
        docLayout_->SetLeftCol(docLayout_->GetLeftCol() + delta);
    } else {
        const int topRow = docLayout_->GetTopVisualRow() + delta;
        if (scrollCb_)
            scrollCb_(topRow);
        else
            docLayout_->SetTopVisualRow(topRow);
    }
    UpdateScrollbars();
    Refresh();
}

void TerminalPanel::OnFocus(wxFocusEvent& e)
{
    if (focusCb_) focusCb_();
    e.Skip();
}

// ---------------------------------------------------------------------------
// Coordinate helpers
// ---------------------------------------------------------------------------

std::pair<int, int> TerminalPanel::PixelToViewportChar(wxPoint px) const
{
    const wxSize view = ViewportChars();
    const int col = std::clamp((px.x - kInnerPad) / m_charSize.x, 0, view.x - 1);
    const int row = std::clamp((px.y - kInnerPad) / m_charSize.y, 0, view.y - 1);
    return {row, col};
}

void TerminalPanel::ExtendSelectionTo(wxPoint px)
{
    if (!docLayout_) return;
    auto [row, col]    = PixelToViewportChar(px);
    const auto docPos  = docLayout_->HitTest(row, col);
    auto sel           = docLayout_->GetSelection();
    sel.extent         = docPos;
    sel.active         = true;
    docLayout_->SetSelection(sel);
}

// ---------------------------------------------------------------------------
// Mouse selection handlers
// ---------------------------------------------------------------------------

void TerminalPanel::OnLeftDown(wxMouseEvent& e)
{
    if (!docLayout_) { e.Skip(); return; }
    SetFocus();

    auto [row, col]            = PixelToViewportChar(e.GetPosition());
    const auto anchor          = docLayout_->HitTest(row, col);
    const DocLayout::TextSelection sel{anchor, anchor, true};
    docLayout_->SetSelection(sel);

    m_selecting_    = true;
    m_lastMousePos_ = e.GetPosition();
    CaptureMouse();
    m_selScrollTimer_.Start(50);
    Refresh();
}

void TerminalPanel::OnMouseMove(wxMouseEvent& e)
{
    if (!m_selecting_) return;
    m_lastMousePos_ = e.GetPosition();
    ExtendSelectionTo(e.GetPosition());
    Refresh();
}

void TerminalPanel::OnLeftUp(wxMouseEvent& e)
{
    if (!m_selecting_) { e.Skip(); return; }
    m_selecting_ = false;
    m_selScrollTimer_.Stop();
    if (HasCapture()) ReleaseMouse();
    ExtendSelectionTo(e.GetPosition());
    Refresh();
}

void TerminalPanel::OnRightDown(wxMouseEvent& e)
{
    if (!docLayout_ || !actionRegistry_ || !docLayout_->HasSelection()) {
        e.Skip();
        return;
    }

    const std::u32string selected = docLayout_->GetSelectedText();
    wxMenu menu;
    actionRegistry_->PopulateMenu(menu, kActionMenuBaseId);

    // Dispatch menu events inline so the registry reference is captured safely.
    menu.Bind(wxEVT_MENU, [this, selected](wxCommandEvent& ev) {
        if (actionRegistry_)
            actionRegistry_->Execute(ev.GetId() - kActionMenuBaseId, selected);
    });

    PopupMenu(&menu);
}

void TerminalPanel::OnSelScrollTimer(wxTimerEvent&)
{
    if (!m_selecting_ || !docLayout_) {
        m_selScrollTimer_.Stop();
        return;
    }

    const wxSize clientSz = GetClientSize();
    const int    top      = kInnerPad;
    const int    bottom   = clientSz.y - m_sbThick - kInnerPad;
    const int    left     = kInnerPad;
    const int    right    = clientSz.x - m_sbThick - kInnerPad;

    bool scrolled = false;

    if (m_lastMousePos_.y < top) {
        const int topRow = docLayout_->GetTopVisualRow() - 1;
        if (scrollCb_) scrollCb_(topRow); else docLayout_->SetTopVisualRow(topRow);
        scrolled = true;
    } else if (m_lastMousePos_.y > bottom) {
        const int topRow = docLayout_->GetTopVisualRow() + 1;
        if (scrollCb_) scrollCb_(topRow); else docLayout_->SetTopVisualRow(topRow);
        scrolled = true;
    }

    if (!docLayout_->GetWordWrap()) {
        if (m_lastMousePos_.x < left) {
            docLayout_->SetLeftCol(docLayout_->GetLeftCol() - 1);
            scrolled = true;
        } else if (m_lastMousePos_.x > right) {
            docLayout_->SetLeftCol(docLayout_->GetLeftCol() + 1);
            scrolled = true;
        }
    }

    if (scrolled) {
        ExtendSelectionTo(m_lastMousePos_);
        UpdateScrollbars();
        Refresh();
    }
}

// ---------------------------------------------------------------------------
// Keyboard selection (Shift+Arrow/Home/End extends selection without PTY send)
// ---------------------------------------------------------------------------

void TerminalPanel::OnKeyDown(wxKeyEvent& e)
{
    if (!docLayout_ || !e.ShiftDown()) {
        // Non-Shift key: clear any keyboard-driven selection and let the event
        // propagate to the parent (PTY input path).
        if (docLayout_ && !e.ShiftDown()) {
            const int k = e.GetKeyCode();
            if (k == WXK_LEFT || k == WXK_RIGHT || k == WXK_UP || k == WXK_DOWN ||
                k == WXK_HOME || k == WXK_END) {
                docLayout_->ClearSelection();
                Refresh();
            }
        }
        e.Skip();
        return;
    }

    const int k = e.GetKeyCode();
    if (k != WXK_LEFT && k != WXK_RIGHT && k != WXK_UP && k != WXK_DOWN &&
        k != WXK_HOME && k != WXK_END) {
        e.Skip();
        return;
    }

    // Shift + navigation key: extend or begin keyboard selection.
    auto sel = docLayout_->GetSelection();
    if (!sel.active) {
        // Anchor at the current document cursor.
        const CursorPos cur = docLayout_->GetCursorDocPos();
        m_kbCursor_ = {(int)cur.line, (int)cur.col};
        sel.anchor  = m_kbCursor_;
        sel.extent  = m_kbCursor_;
        sel.active  = true;
    }

    const int lineCount = docLayout_->GetLineCount();

    DocLayout::DocPosition& ext = sel.extent;

    switch (k) {
    case WXK_LEFT:
        if (ext.docCol > 0) { --ext.docCol; }
        else if (ext.docLine > 0) { --ext.docLine; ext.docCol = INT_MAX; }
        break;
    case WXK_RIGHT:
        ++ext.docCol;
        break;
    case WXK_UP:
        if (ext.docLine > 0) --ext.docLine;
        break;
    case WXK_DOWN:
        if (ext.docLine < lineCount - 1) ++ext.docLine;
        break;
    case WXK_HOME:
        ext.docCol = 0;
        break;
    case WXK_END:
        ext.docCol = INT_MAX;
        break;
    }

    m_kbCursor_ = ext;
    docLayout_->SetSelection(sel);
    Refresh();
    // Do NOT call e.Skip() — swallow the event so it doesn't reach the PTY.
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

    const wxSize view = ViewportChars();
    const int    cw   = m_charSize.x;
    const int    ch   = m_charSize.y;

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

    // GetRenderedLine(r) is viewport-relative: 0 = topmost visible line.
    // It returns an empty RenderedLine (no text, no cursor) once past the
    // end of the document, so we stop early when the document is short.
    for (int r = 0; r < view.y; ++r) {
        const RenderedLine row = docLayout_->GetRenderedLine(r);

        for (int col = 0; col < (int)row.text.size(); ++col) {
            dc.SetTextForeground(resolveColour(row.attrs[col].fg, true));
            dc.SetTextBackground(resolveColour(row.attrs[col].bg, false));
            dc.DrawText(wxString(static_cast<wchar_t>(row.text[col])), col * cw + kInnerPad, r * ch + kInnerPad);
        }

        if (row.hasCursor) {
            const int cx = row.cursorCol * cw + kInnerPad;
            const int cy = r * ch + kInnerPad;
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
