#include "ui/TerminalPanel.h"
#include "ui/SearchBar.h"
#include "ui/wxKeyAdapter.h"
#include <wx/clipbrd.h>
#include <wx/utils.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/graphics.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <algorithm>

// Base wxID for context menu action items.
static constexpr int kActionMenuBaseId   = wxID_HIGHEST + 100;
static constexpr int kUrlMenuOpenBrowser = wxID_HIGHEST + 200;
static constexpr int kUrlMenuCopyLink    = wxID_HIGHEST + 201;

static int QueryScrollbarThickness()
{
    const int t = wxSystemSettings::GetMetric(wxSYS_VSCROLL_X);
    return (t > 0) ? t : 16;
}

static constexpr int kResizeDebounceMs = 80;

static wxFont BuildFont(const AppConfig& cfg, bool bold, bool italic)
{
    const wxFontWeight weight = bold   ? wxFONTWEIGHT_BOLD   : wxFONTWEIGHT_NORMAL;
    const wxFontStyle  fstyle = italic ? wxFONTSTYLE_ITALIC   : wxFONTSTYLE_NORMAL;
    if (!cfg.fontFamily.empty()) {
        wxFont f(cfg.fontSize, wxFONTFAMILY_TELETYPE, fstyle, weight,
                 false, wxString::FromUTF8(cfg.fontFamily));
        if (f.IsOk()) return f;
    }
    return wxFont(cfg.fontSize, wxFONTFAMILY_TELETYPE, fstyle, weight);
}

TerminalPanel::TerminalPanel(wxWindow* parent, const AppConfig& cfg,
                             unsigned short cols, unsigned short rows)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS),
      m_cfg(cfg),
      m_font(BuildFont(cfg, false, false)),
      m_boldFont(BuildFont(cfg, true, false)),
      m_italicFont(BuildFont(cfg, false, true)),
      m_boldItalicFont(BuildFont(cfg, true, true)),
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
    SetMinClientSize({cols  * m_charSize.x + m_sbThick + 2 * m_cfg.padding,
                      rows  * m_charSize.y + m_sbThick + 2 * m_cfg.padding});

    Bind(wxEVT_PAINT,      &TerminalPanel::OnPaint,          this);
    Bind(wxEVT_SIZE,       &TerminalPanel::OnSize,           this);
    Bind(wxEVT_TIMER,      &TerminalPanel::OnResizeTimer,    this, resizeTimer_.GetId());
    Bind(wxEVT_TIMER,      &TerminalPanel::OnSelScrollTimer, this, m_selScrollTimer_.GetId());
    Bind(wxEVT_MOUSEWHEEL, &TerminalPanel::OnMouseWheel,     this);
    Bind(wxEVT_LEFT_DOWN,  &TerminalPanel::OnLeftDown,       this);
    Bind(wxEVT_LEFT_UP,    &TerminalPanel::OnLeftUp,         this);
    Bind(wxEVT_MOTION,       &TerminalPanel::OnMouseMove,    this);
    Bind(wxEVT_LEAVE_WINDOW, &TerminalPanel::OnLeaveWindow,  this);
    Bind(wxEVT_RIGHT_DOWN,   &TerminalPanel::OnRightDown,    this);
    Bind(wxEVT_KEY_DOWN,   &TerminalPanel::OnKeyDown,        this);
    Bind(wxEVT_CHAR,       &TerminalPanel::OnChar,           this);
    Bind(wxEVT_SET_FOCUS,  &TerminalPanel::OnFocus,          this);
    Bind(wxEVT_KILL_FOCUS, &TerminalPanel::OnKillFocus,      this);
    m_flashTimer_.SetOwner(this);
    Bind(wxEVT_TIMER,      &TerminalPanel::OnFlashTimer,     this, m_flashTimer_.GetId());
}

void TerminalPanel::ApplyConfig(const AppConfig& cfg)
{
    const bool fontChanged    = cfg.fontFamily != m_cfg.fontFamily
                             || cfg.fontSize   != m_cfg.fontSize;
    const bool paddingChanged = cfg.padding    != m_cfg.padding;

    m_cfg = cfg;

    if (fontChanged) {
        m_font           = BuildFont(m_cfg, false, false);
        m_boldFont       = BuildFont(m_cfg, true,  false);
        m_italicFont     = BuildFont(m_cfg, false, true);
        m_boldItalicFont = BuildFont(m_cfg, true,  true);
        wxMemoryDC dc;
        dc.SetFont(m_font);
        m_charSize = dc.GetTextExtent("M");
    }

    SetBackgroundColour(wxColour(m_cfg.bgColour.r, m_cfg.bgColour.g, m_cfg.bgColour.b));

    if (fontChanged || paddingChanged) {
        SetMinClientSize({0, 0});
        Layout();
        // The EVT_SIZE → OnSize → debounced resizeCb_ path propagates the new
        // cols×rows to the session without TerminalPanel needing to own that logic.
    }

    Refresh();
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
    return {std::max(1, (sz.x - m_sbThick - 2 * m_cfg.padding) / m_charSize.x),
            std::max(1, (sz.y - m_sbThick - 2 * m_cfg.padding) / m_charSize.y)};
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
    const bool hEnabled = docLayout_ && !docLayout_->GetWrapMode();
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

    if (!docLayout_) { Refresh(); return; }

    const DocLayout::DirtySnapshot dirty = docLayout_->TakeAndClearDirty();
    if (dirty.all || dirty.rows.empty()) {
        Refresh();
        return;
    }

    // Invalidate only the rows that changed, letting OnPaint skip the rest via
    // GetUpdateClientRect() clipping.  Batching all rects before Update() lets
    // GTK coalesce them into a single gdk_window_process_updates() flush.
    const int ch = m_charSize.y;
    const int w  = GetClientSize().x;
    for (int r = 0; r < (int)dirty.rows.size(); ++r) {
        if (dirty.rows[r])
            RefreshRect(wxRect(0, r * ch + m_cfg.padding, w, ch), false);
    }
    Update();
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

wxSize TerminalPanel::ComputeRequiredPanelSize(unsigned short cols, unsigned short rows) const
{
    return { static_cast<int>(cols) * m_charSize.x + m_sbThick + 2 * m_cfg.padding,
             static_cast<int>(rows) * m_charSize.y + m_sbThick + 2 * m_cfg.padding };
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
    if (isHorizontal && !docLayout_->GetWrapMode()) {
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
    m_hasFocus_ = true;
    Refresh();
    if (focusCb_) focusCb_();
    e.Skip();
}

void TerminalPanel::OnKillFocus(wxFocusEvent& e)
{
    m_hasFocus_ = false;
    Refresh();
    e.Skip();
}

void TerminalPanel::Flash()
{
    m_flashing_ = true;
    Refresh();
    m_flashTimer_.StartOnce(150);
}

void TerminalPanel::OnFlashTimer(wxTimerEvent&)
{
    m_flashing_ = false;
    Refresh();
}

void TerminalPanel::SetBroadcastCursorState(bool modeActive, bool inGroup)
{
    if (m_broadcastModeActive_ == modeActive && m_inBroadcast_ == inGroup) return;
    m_broadcastModeActive_ = modeActive;
    m_inBroadcast_         = inGroup;
    Refresh();
}

// ---------------------------------------------------------------------------
// Coordinate helpers
// ---------------------------------------------------------------------------

std::pair<int, int> TerminalPanel::PixelToViewportChar(wxPoint px) const
{
    const wxSize view = ViewportChars();
    const int col = std::clamp((px.x - m_cfg.padding) / m_charSize.x, 0, view.x - 1);
    const int row = std::clamp((px.y - m_cfg.padding) / m_charSize.y, 0, view.y - 1);
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
    // Explicitly activate the session — GTK's can-focus is not set on wxPanel,
    // so wxEVT_SET_FOCUS may never fire from SetFocus() on this platform.
    if (focusCb_) focusCb_();

    // Ctrl+Click on a hovered URL → open in browser; skip selection start.
    if (e.ControlDown() && m_urlHovered_ && !m_hoveredUrl_.empty()) {
        std::string utf8;
        utf8.reserve(m_hoveredUrl_.size());
        for (char32_t cp : m_hoveredUrl_)
            utf8 += static_cast<char>(cp & 0x7F);
        wxLaunchDefaultBrowser(wxString::FromUTF8(utf8));
        return;
    }

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
    if (m_selecting_) {
        m_lastMousePos_ = e.GetPosition();
        ExtendSelectionTo(e.GetPosition());
        Refresh();
        return;
    }

    if (docLayout_) {
        auto [row, col]  = PixelToViewportChar(e.GetPosition());
        const auto pos   = docLayout_->HitTest(row, col);
        const auto span  = docLayout_->FindUrlAt(pos);

        if (span) {
            const DocLayout::DocPosition urlStart{pos.docLine,
                                                  static_cast<int>(span->col)};
            if (!m_urlHovered_ || span->url != m_hoveredUrl_) {
                m_hoveredUrl_  = span->url;
                m_urlHovered_  = true;
                docLayout_->SetHoveredUrl(urlStart, static_cast<int>(span->len));
                wxSetCursor(wxCursor(wxCURSOR_HAND));
            }
        } else if (m_urlHovered_) {
            m_urlHovered_ = false;
            m_hoveredUrl_.clear();
            docLayout_->SetHoveredUrl(std::nullopt);
            wxSetCursor(wxNullCursor);
        }
    }

    e.Skip();
}

void TerminalPanel::OnLeaveWindow(wxMouseEvent& e)
{
    if (m_urlHovered_) {
        m_urlHovered_ = false;
        m_hoveredUrl_.clear();
        if (docLayout_) docLayout_->SetHoveredUrl(std::nullopt);
        wxSetCursor(wxNullCursor);
    }
    e.Skip();
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
    if (!docLayout_) { e.Skip(); return; }

    // Fresh hit-test at the right-click position — independent of hover state.
    auto [row, col]     = PixelToViewportChar(e.GetPosition());
    const auto clickPos = docLayout_->HitTest(row, col);
    const auto urlSpan  = docLayout_->FindUrlAt(clickPos);
    const bool hasUrl   = urlSpan.has_value();
    const bool hasSel   = docLayout_->HasSelection() && actionRegistry_;

    if (!hasUrl && !hasSel) { e.Skip(); return; }

    // Convert URL to UTF-8 once (RFC 3986 URLs are ASCII).
    std::string urlUtf8;
    if (hasUrl) {
        urlUtf8.reserve(urlSpan->url.size());
        for (char32_t cp : urlSpan->url)
            urlUtf8 += static_cast<char>(cp & 0x7F);
    }

    wxMenu menu;

    if (hasUrl) {
        menu.Append(kUrlMenuOpenBrowser, "Open in Browser");
        menu.Append(kUrlMenuCopyLink,    "Copy Link Address");
    }
    if (hasUrl && hasSel)
        menu.AppendSeparator();
    if (hasSel)
        actionRegistry_->PopulateMenu(menu, kActionMenuBaseId);

    if (hasUrl) {
        menu.Bind(wxEVT_MENU, [urlUtf8](wxCommandEvent&) {
            wxLaunchDefaultBrowser(wxString::FromUTF8(urlUtf8));
        }, kUrlMenuOpenBrowser);

        menu.Bind(wxEVT_MENU, [urlUtf8](wxCommandEvent&) {
            if (wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(urlUtf8)));
                wxTheClipboard->Close();
            }
        }, kUrlMenuCopyLink);
    }

    if (hasSel) {
        const std::u32string selected = docLayout_->GetSelectedText();
        menu.Bind(wxEVT_MENU, [this, selected](wxCommandEvent& ev) {
            if (actionRegistry_)
                actionRegistry_->Execute(ev.GetId() - kActionMenuBaseId, selected);
        });
    }

    PopupMenu(&menu);
}

void TerminalPanel::OnSelScrollTimer(wxTimerEvent&)
{
    if (!m_selecting_ || !docLayout_) {
        m_selScrollTimer_.Stop();
        return;
    }

    const wxSize clientSz = GetClientSize();
    const int    top      = m_cfg.padding;
    const int    bottom   = clientSz.y - m_sbThick - m_cfg.padding;
    const int    left     = m_cfg.padding;
    const int    right    = clientSz.x - m_sbThick - m_cfg.padding;

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

    if (!docLayout_->GetWrapMode()) {
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
        // Clear keyboard-driven selection on bare navigation keys.
        if (docLayout_ && !e.ShiftDown()) {
            const int k = e.GetKeyCode();
            if (k == WXK_LEFT || k == WXK_RIGHT || k == WXK_UP || k == WXK_DOWN ||
                k == WXK_HOME || k == WXK_END) {
                docLayout_->ClearSelection();
                Refresh();
            }
        }
        // Special keys and ctrl-combos route directly; printable chars go to OnChar.
        const int k = e.GetKeyCode();
        const bool isSpecial =
            k == WXK_RETURN   || k == WXK_BACK     || k == WXK_TAB      || k == WXK_ESCAPE  ||
            k == WXK_UP       || k == WXK_DOWN      || k == WXK_LEFT     || k == WXK_RIGHT   ||
            k == WXK_HOME     || k == WXK_END       || k == WXK_PAGEUP   || k == WXK_PAGEDOWN||
            k == WXK_INSERT   || k == WXK_DELETE    ||
            (k >= WXK_F1 && k <= WXK_F12);
        if ((isSpecial || e.ControlDown()) && keyCb_) {
            keyCb_(term::ui::wx::ConvertKey(e));
            return;
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

void TerminalPanel::OnChar(wxKeyEvent& e)
{
    const wxChar uc = e.GetUnicodeKey();
    if (uc == WXK_NONE || e.ControlDown() || uc < 32) {
        e.Skip();
        return;
    }
    // Let Alt+letter propagate to the frame so menu mnemonics fire.
    // Terminal Alt sequences (readline Alt+b/f etc.) remain accessible via ESC+letter.
    if (e.AltDown() && !e.ControlDown()) {
        e.Skip();
        return;
    }
    if (keyCb_) {
        term::input::KeyEvent evt;
        evt.key   = term::input::Key::Character;
        evt.ctrl  = false;
        evt.alt   = e.AltDown();
        evt.shift = e.ShiftDown();
        evt.code  = static_cast<uint32_t>(uc);
        const wxString s(uc);
        evt.text  = s.ToStdString(wxConvUTF8);
        keyCb_(evt);
    }
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

    // Resolve the foreground or background colour from a Style, handling
    // 24-bit true colour, 256-colour palette, and terminal defaults.
    // Indices 0–15: standard ANSI palette. 16–231: 6×6×6 cube. 232–255: greyscale.
    auto resolveColour = [this](const Style& s, bool isFg) -> wxColour {
        const auto& rgb_opt = isFg ? s.fgRgb : s.bgRgb;
        if (rgb_opt) return wxColour(rgb_opt->r, rgb_opt->g, rgb_opt->b);
        const int index = isFg ? s.fg : s.bg;
        if (index < 0) {
            const Rgb& c = isFg ? m_cfg.textColour : m_cfg.bgColour;
            return wxColour(c.r, c.g, c.b);
        }
        if (index < 16) {
            const Rgb& c = m_cfg.ansiColors[index];
            return wxColour(c.r, c.g, c.b);
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

    // Clip row iteration to the damaged region so that RefreshRect() calls
    // from OnDocumentUpdate only repaint the rows that actually changed.
    const wxRect clip     = GetUpdateClientRect();
    const int firstRow    = std::max(0, (clip.y - m_cfg.padding) / ch);
    const int lastRow     = std::min(view.y, (clip.GetBottom() - m_cfg.padding) / ch + 1);

    // GetRenderedLine(r) is viewport-relative: 0 = topmost visible line.
    // It returns an empty RenderedLine (no text, no cursor) once past the
    // end of the document, so we stop early when the document is short.
    for (int r = firstRow; r < lastRow; ++r) {
        const RenderedLine row = docLayout_->GetRenderedLine(r);
        const int rowY = r * ch + m_cfg.padding;

        // Batch consecutive characters that share the same style into a single
        // DrawText call.  This reduces draw calls from O(cols) to O(style-runs),
        // typically 1 per row for plain text and 5–10 for colorised prompts.
        int runStart = 0;
        const int textLen = static_cast<int>(row.text.size());

        auto flushRun = [&](int start, int end) {
            if (start >= end) return;
            const Style& s = row.attrs[start];

            const wxFont& font = (s.bold && s.italic) ? m_boldItalicFont
                               : s.bold               ? m_boldFont
                               : s.italic             ? m_italicFont
                               :                        m_font;
            dc.SetFont(font);

            // Reverse video swaps fg and bg roles.
            wxColour fg = resolveColour(s, !s.reverse);
            wxColour bg = resolveColour(s, s.reverse);

            // Dim blends fg toward bg at 50%.
            if (s.dim) {
                fg = wxColour(
                    static_cast<unsigned char>((fg.Red()   + bg.Red())   / 2),
                    static_cast<unsigned char>((fg.Green() + bg.Green()) / 2),
                    static_cast<unsigned char>((fg.Blue()  + bg.Blue())  / 2)
                );
            }

            dc.SetTextForeground(fg);
            dc.SetTextBackground(bg);

            std::wstring runStr;
            runStr.reserve(static_cast<size_t>(end - start));
            for (int c = start; c < end; ++c)
                runStr += static_cast<wchar_t>(row.text[c]);

            const int startX = start * cw + m_cfg.padding;
            dc.DrawText(wxString(runStr), startX, rowY);

            if (s.underline) {
                dc.SetPen(wxPen(fg, 1));
                dc.DrawLine(startX, rowY + ch - 1, startX + (end - start) * cw, rowY + ch - 1);
            }
        };

        for (int col = 1; col < textLen; ++col) {
            if (row.attrs[col] != row.attrs[runStart]) {
                flushRun(runStart, col);
                runStart = col;
            }
        }
        flushRun(runStart, textLen);

        if (row.hasCursor) {
            const int cx = row.cursorCol * cw + m_cfg.padding;
            const int cy = rowY;
            const wxColour cursorWx(m_cfg.cursorColour.r,
                                    m_cfg.cursorColour.g,
                                    m_cfg.cursorColour.b);
            if (m_inBroadcast_ || (m_hasFocus_ && !m_broadcastModeActive_)) {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(cursorWx));
                dc.DrawRectangle(cx, cy, cw, ch);
                if (row.cursorCol < textLen) {
                    dc.SetTextForeground(wxColour(m_cfg.bgColour.r,   m_cfg.bgColour.g,   m_cfg.bgColour.b));
                    dc.SetTextBackground(wxColour(m_cfg.textColour.r, m_cfg.textColour.g, m_cfg.textColour.b));
                    dc.DrawText(wxString(static_cast<wchar_t>(row.text[row.cursorCol])), cx, cy);
                }
            } else {
                dc.SetPen(wxPen(cursorWx, 1));
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawRectangle(cx, cy, cw, ch);
            }
        }
    }

    if (m_flashing_) {
        if (auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc))) {
            gc->SetBrush(gc->CreateBrush(wxBrush(wxColour(255, 255, 255, 120))));
            gc->SetPen(wxNullPen);
            const wxSize sz = GetClientSize();
            gc->DrawRectangle(0, 0, sz.x, sz.y);
        }
    }
}
