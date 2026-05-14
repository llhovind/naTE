#pragma once

#include <functional>
#include <vector>
#include <wx/window.h>
#include <wx/string.h>

// Custom-drawn tab strip embedded in the TerminalTile title bar.
// Renders N session tabs left-justified and a "+" new-tab button immediately
// after the last tab.  The remaining space to the right is "blank header"
// which fires HeaderActivate / HeaderDragStart callbacks so TerminalTile can
// handle tile-level activation and drag from that region.
class TabStrip : public wxWindow {
public:
    explicit TabStrip(wxWindow* parent);

    // -------------------------------------------------------------------------
    // Tab data management — called by TerminalTile
    // -------------------------------------------------------------------------
    void AddTab(const wxString& label);
    void RemoveTab(int index);
    void SetTabLabel(int index, const wxString& label);
    void SetActiveTab(int index);

    int  GetActiveTab()  const { return activeIdx_; }
    int  GetTabCount()   const { return static_cast<int>(labels_.size()); }

    // The background colour must track the tile's title bar colour.
    void SetBgColour(const wxColour& c);

    // -------------------------------------------------------------------------
    // Tab-level callbacks wired by TerminalTile
    // -------------------------------------------------------------------------
    using TabSelectedCallback    = std::function<void(int index)>;
    using TabCloseCallback       = std::function<void(int index)>;
    using TabDragStartCallback   = std::function<void(int index, wxPoint screenPt)>;
    using NewTabCallback         = std::function<void()>;

    void SetTabSelectedCallback (TabSelectedCallback  cb) { selectedCb_  = std::move(cb); }
    void SetTabCloseCallback    (TabCloseCallback     cb) { closeCb_     = std::move(cb); }
    void SetTabDragStartCallback(TabDragStartCallback cb) { tabDragCb_   = std::move(cb); }
    void SetNewTabCallback      (NewTabCallback       cb) { newTabCb_    = std::move(cb); }

    // -------------------------------------------------------------------------
    // Header-level callbacks — fired for clicks/drags anywhere in the strip
    // so TerminalTile can handle tile activation, whole-tile drag, broadcast
    // toggle, and context menu without needing forwarded mouse events.
    // -------------------------------------------------------------------------
    using HeaderActivateCallback   = std::function<void()>;
    using HeaderDragStartCallback  = std::function<void(wxPoint screenPt)>;
    using HeaderCtrlClickCallback  = std::function<void()>;          // Ctrl+left anywhere
    using HeaderRightClickCallback = std::function<void()>;          // right-click anywhere

    // Query callback: TabStrip calls this during OnPaint to ask whether a given
    // tab index is in broadcast.  Ownership of broadcast state stays in TerminalTile.
    using BroadcastQueryCallback   = std::function<bool(int tabIndex)>;

    void SetHeaderActivateCallback  (HeaderActivateCallback   cb) { headerActivateCb_   = std::move(cb); }
    void SetHeaderDragStartCallback (HeaderDragStartCallback  cb) { headerDragCb_        = std::move(cb); }
    void SetHeaderCtrlClickCallback (HeaderCtrlClickCallback  cb) { headerCtrlClickCb_  = std::move(cb); }
    void SetHeaderRightClickCallback(HeaderRightClickCallback cb) { headerRightClickCb_ = std::move(cb); }
    void SetBroadcastQueryCallback  (BroadcastQueryCallback   cb) { broadcastQueryCb_   = std::move(cb); }

private:
    // Per-tab geometry computed from current client size.
    struct TabGeom {
        int tabW  = 0;   // individual tab width (uniform)
        int plusX = 0;   // x origin of "+" button
        int plusW = 0;   // width of "+" button area (always kPlusW)
    };
    TabGeom ComputeGeom() const;

    // Returns tab index under x-coordinate, or -1 for non-tab area.
    // Sets closeHit=true if the click lands on the close widget of that tab.
    // Does NOT consider the "+" button — caller checks that zone separately.
    int HitTest(int x, bool& closeHit) const;

    void OnPaint      (wxPaintEvent&);
    void OnLeftDown   (wxMouseEvent&);
    void OnRightDown  (wxMouseEvent&);
    void OnMiddleDown (wxMouseEvent&);
    void OnMotion     (wxMouseEvent&);
    void OnLeftUp     (wxMouseEvent&);
    void OnMouseLeave (wxMouseEvent&);

    std::vector<wxString> labels_;
    int                   activeIdx_ = -1;
    wxColour              bgColour_     { 131, 136, 141 };
    wxColour              colBroadcast_ { 255, 140,   0 };

    // Tab drag state (dragging a specific tab)
    int     dragTabIdx_      = -1;
    wxPoint dragAnchor_      { -1, -1 };
    bool    dragPending_     = false;

    // Header drag state (dragging from blank area → tile move)
    bool    headerDragPending_ = false;

    static constexpr int kDragThreshold = 5;
    static constexpr int kCloseW  = 18;   // close "×" width inside each tab
    static constexpr int kPlusW   = 24;   // "+" button width
    static constexpr int kMinTabW = 50;
    static constexpr int kMaxTabW = 200;

    TabSelectedCallback     selectedCb_;
    TabCloseCallback        closeCb_;
    TabDragStartCallback    tabDragCb_;
    NewTabCallback          newTabCb_;
    HeaderActivateCallback  headerActivateCb_;
    HeaderDragStartCallback headerDragCb_;
    HeaderCtrlClickCallback  headerCtrlClickCb_;
    HeaderRightClickCallback headerRightClickCb_;
    BroadcastQueryCallback   broadcastQueryCb_;
};
