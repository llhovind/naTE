#pragma once
#include <functional>
#include <string>
#include <wx/panel.h>
#include <wx/scrolbar.h>
#include <wx/timer.h>
#include "ui/DocLayout.h"
#include "ui/SelectionActionRegistry.h"
#include "config/Config.h"
#include "input/KeyEvent.hpp"

class SearchBar;

class TerminalPanel : public wxWindow
{
public:
    using ScrollCallback = std::function<void(int topRow)>;
    using ResizeCallback = std::function<void(unsigned short cols, unsigned short rows)>;
    using FocusCallback  = std::function<void()>;
    using KeyCallback    = std::function<void(const term::input::KeyEvent&)>;

    TerminalPanel(wxWindow* parent, const AppConfig& cfg, unsigned short cols, unsigned short rows);

    void SetDocLayout(::DocLayout* docLayout);

    // UIManager wires these at construction; if unset the panel drives DocLayout directly.
    void SetScrollCallback(ScrollCallback cb) { scrollCb_ = std::move(cb); }
    void SetResizeCallback(ResizeCallback cb) { resizeCb_ = std::move(cb); }
    void SetFocusCallback(FocusCallback  cb) { focusCb_  = std::move(cb); }
    void SetKeyCallback(KeyCallback      cb) { keyCb_    = std::move(cb); }

    // Called by TerminalTile whenever broadcast state changes.
    // modeActive — broadcast mode is globally on.
    // inGroup    — this panel's session is a member of the broadcast group.
    // Cursor is solid only when the panel will actually receive input:
    //   inGroup, OR (focused AND broadcast mode is off).
    void SetBroadcastCursorState(bool modeActive, bool inGroup);

    // Called by the document-refresh chain after DocLayout has updated topRow_.
    void OnDocumentUpdate();

    // Sync scrollbar thumb positions after an externally-driven viewport change
    // (e.g. search navigation) that doesn't go through the normal document-update path.
    void SyncScrollbars();

    // Scroll the viewport to make the cursor visible and re-enable follow-tail.
    // Called on keyboard input so that typing while scrolled up snaps back.
    void EnsureCursorVisible();

    // Search bar — created by UIManager; panel handles layout and focus routing.
    void SetSearchBar(SearchBar* bar);
    void ShowSearchBar(bool show);
    bool HasSearchBarFocus() const;

    // Selection actions — non-owning; UIManager owns the registry.
    void SetActionRegistry(SelectionActionRegistry* reg) { actionRegistry_ = reg; }

    // Apply updated config to this panel in place (font, colors, padding).
    // Triggers relayout and repaint; font/padding changes fire the existing
    // resize callback so the PTY adapts cols×rows automatically.
    void ApplyConfig(const AppConfig& cfg);

    // Trigger a short visual bell flash. Safe to call from the UI thread only.
    void Flash();

    // Pixel size this panel needs to display exactly cols×rows characters.
    // Use this to size the containing tile before calling ResizeFrameToFitTiles().
    wxSize ComputeRequiredPanelSize(unsigned short cols, unsigned short rows) const;

private:
    ::DocLayout* docLayout_ = nullptr;

    void OnPaint(wxPaintEvent&);
    void OnSize(wxSizeEvent&);
    void OnResizeTimer(wxTimerEvent&);
    void OnScroll(wxScrollEvent&);
    void OnMouseWheel(wxMouseEvent&);
    void OnLeftDown(wxMouseEvent&);
    void OnLeftUp(wxMouseEvent&);
    void OnMouseMove(wxMouseEvent&);
    void OnLeaveWindow(wxMouseEvent&);
    void OnRightDown(wxMouseEvent&);
    void OnSelScrollTimer(wxTimerEvent&);
    void OnKeyDown(wxKeyEvent&);
    void OnChar(wxKeyEvent&);
    void OnFocus(wxFocusEvent&);
    void OnKillFocus(wxFocusEvent&);
    void OnFlashTimer(wxTimerEvent&);

    void LayoutScrollbars();
    void UpdateScrollbars();
    wxSize ViewportChars() const;

    // Convert a pixel position to a clamped (viewportRow, viewportCol) pair.
    std::pair<int, int> PixelToViewportChar(wxPoint px) const;

    // Extend the active selection to the given pixel position.
    void ExtendSelectionTo(wxPoint px);

    AppConfig    m_cfg;
    wxFont       m_font;
    wxFont       m_boldFont;
    wxFont       m_italicFont;
    wxFont       m_boldItalicFont;
    wxSize       m_charSize;
    wxScrollBar* m_hScroll;
    wxScrollBar* m_vScroll;
    int          m_sbThick;

    ScrollCallback scrollCb_;
    ResizeCallback resizeCb_;
    FocusCallback  focusCb_;
    KeyCallback    keyCb_;

    wxTimer resizeTimer_;
    wxSize  pendingResize_{0, 0};

    SearchBar* searchBar_       = nullptr;  // wx-parent-owned; non-owning here
    int        searchBarHeight_ = 0;

    // Selection state
    bool    m_selecting_    = false;
    wxTimer m_selScrollTimer_;
    wxPoint m_lastMousePos_{-1, -1};

    bool m_hasFocus_            = false;
    bool m_inBroadcast_         = false;
    bool m_broadcastModeActive_ = false;
    bool m_flashing_            = false;
    wxTimer m_flashTimer_;

    // URL hover state
    std::u32string m_hoveredUrl_;
    bool           m_urlHovered_ = false;

    // Keyboard selection cursor (independent of the PTY document cursor)
    DocLayout::DocPosition m_kbCursor_{0, 0};

    SelectionActionRegistry* actionRegistry_ = nullptr;  // non-owning
};
