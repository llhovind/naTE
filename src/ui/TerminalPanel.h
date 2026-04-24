#pragma once
#include <functional>
#include <wx/panel.h>
#include <wx/scrolbar.h>
#include <wx/timer.h>
#include "ui/DocLayout.h"
#include "config/Config.h"

class TerminalPanel : public wxPanel
{
public:
    using ScrollCallback = std::function<void(int topRow)>;
    using ResizeCallback = std::function<void(unsigned short cols, unsigned short rows)>;
    using FocusCallback  = std::function<void()>;

    TerminalPanel(wxWindow* parent, const AppConfig& cfg);

    void SetDocLayout(::DocLayout* docLayout);

    // UIManager wires these at construction; if unset the panel drives DocLayout directly.
    void SetScrollCallback(ScrollCallback cb) { scrollCb_ = std::move(cb); }
    void SetResizeCallback(ResizeCallback cb) { resizeCb_ = std::move(cb); }
    void SetFocusCallback(FocusCallback  cb) { focusCb_  = std::move(cb); }

    // Called by the document-refresh chain after DocLayout has updated topRow_.
    void OnDocumentUpdate();

    // Scroll the viewport to make the cursor visible and re-enable follow-tail.
    // Called on keyboard input so that typing while scrolled up snaps back.
    void EnsureCursorVisible();

private:
    ::DocLayout* docLayout_ = nullptr;

    void OnPaint(wxPaintEvent&);
    void OnSize(wxSizeEvent&);
    void OnResizeTimer(wxTimerEvent&);
    void OnScroll(wxScrollEvent&);
    void OnMouseWheel(wxMouseEvent&);
    void OnFocus(wxFocusEvent&);

    void LayoutScrollbars();
    void UpdateScrollbars();
    wxSize ViewportChars() const;

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
};
