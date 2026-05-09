#pragma once
#include <functional>
#include <wx/panel.h>
#include <wx/scrolbar.h>
#include <wx/timer.h>
#include "ui/DocLayout.h"
#include "ui/SelectionActionRegistry.h"
#include "config/Config.h"

class SearchBar;

class TerminalPanel : public wxPanel
{
public:
    using ScrollCallback = std::function<void(int topRow)>;
    using ResizeCallback = std::function<void(unsigned short cols, unsigned short rows)>;
    using FocusCallback  = std::function<void()>;

    TerminalPanel(wxWindow* parent, const AppConfig& cfg, unsigned short cols);

    void SetDocLayout(::DocLayout* docLayout);

    // UIManager wires these at construction; if unset the panel drives DocLayout directly.
    void SetScrollCallback(ScrollCallback cb) { scrollCb_ = std::move(cb); }
    void SetResizeCallback(ResizeCallback cb) { resizeCb_ = std::move(cb); }
    void SetFocusCallback(FocusCallback  cb) { focusCb_  = std::move(cb); }

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
    void OnRightDown(wxMouseEvent&);
    void OnSelScrollTimer(wxTimerEvent&);
    void OnKeyDown(wxKeyEvent&);
    void OnFocus(wxFocusEvent&);

    void LayoutScrollbars();
    void UpdateScrollbars();
    wxSize ViewportChars() const;

    // Convert a pixel position to a clamped (viewportRow, viewportCol) pair.
    std::pair<int, int> PixelToViewportChar(wxPoint px) const;

    // Extend the active selection to the given pixel position.
    void ExtendSelectionTo(wxPoint px);

    AppConfig    m_cfg;
    wxFont       m_font;
    wxSize       m_charSize;
    wxScrollBar* m_hScroll;
    wxScrollBar* m_vScroll;
    int          m_sbThick;

    ScrollCallback scrollCb_;
    ResizeCallback resizeCb_;
    FocusCallback  focusCb_;

    wxTimer resizeTimer_;
    wxSize  pendingResize_{0, 0};

    SearchBar* searchBar_       = nullptr;  // wx-parent-owned; non-owning here
    int        searchBarHeight_ = 0;

    // Selection state
    bool    m_selecting_    = false;
    wxTimer m_selScrollTimer_;
    wxPoint m_lastMousePos_{-1, -1};

    // Keyboard selection cursor (independent of the PTY document cursor)
    DocLayout::DocPosition m_kbCursor_{0, 0};

    SelectionActionRegistry* actionRegistry_ = nullptr;  // non-owning
};
