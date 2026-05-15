#pragma once

#include <functional>
#include <wx/window.h>
#include <wx/string.h>
#include <wx/colour.h>

// Custom-drawn tab strip embedded in the TerminalTile title bar.
// Renders N session tabs left-justified and a "+" new-tab button immediately
// after the last tab.  The remaining space to the right is "blank header"
// which fires HeaderActivate / HeaderDragStart callbacks so TerminalTile can
// handle tile-level activation and drag from that region.
//
// TabStrip owns no tab data.  All state is queried on-demand via callbacks
// wired by TerminalTile at construction time.
class TabStrip : public wxWindow {
public:
    explicit TabStrip(wxWindow* parent);

    // -------------------------------------------------------------------------
    // Query callbacks — wired by TerminalTile; called during paint and hit-test.
    // TabStrip owns no copies of this data.
    // -------------------------------------------------------------------------
    using TabCountCallback   = std::function<int()>;
    using LabelQueryCallback = std::function<wxString(int index)>;
    using ActiveTabCallback  = std::function<int()>;
    using BgColourCallback   = std::function<wxColour()>;

    void SetTabCountCallback  (TabCountCallback   cb) { tabCountCb_  = std::move(cb); }
    void SetLabelQueryCallback(LabelQueryCallback cb) { labelCb_     = std::move(cb); }
    void SetActiveTabCallback (ActiveTabCallback  cb) { activeTabCb_ = std::move(cb); }
    void SetBgColourCallback  (BgColourCallback   cb) { bgColourCb_  = std::move(cb); }

    // Query callback: TabStrip calls this during OnPaint to ask whether a given
    // tab index is in broadcast.  Ownership of broadcast state stays in TerminalTile.
    using BroadcastQueryCallback = std::function<bool(int tabIndex)>;
    void SetBroadcastQueryCallback(BroadcastQueryCallback cb) { broadcastQueryCb_ = std::move(cb); }

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
    // tabIdx >= 0: right-clicked a specific tab; -1: right-clicked background.
    using HeaderRightClickCallback = std::function<void(int tabIdx)>;

    void SetHeaderActivateCallback  (HeaderActivateCallback   cb) { headerActivateCb_   = std::move(cb); }
    void SetHeaderDragStartCallback (HeaderDragStartCallback  cb) { headerDragCb_        = std::move(cb); }
    void SetHeaderCtrlClickCallback (HeaderCtrlClickCallback  cb) { headerCtrlClickCb_  = std::move(cb); }
    void SetHeaderRightClickCallback(HeaderRightClickCallback cb) { headerRightClickCb_ = std::move(cb); }

    // Returns the insert-before index (0..n) for a drop at x-coordinate.
    // Left half of tab i → insert before i; right half → insert after (i+1).
    // Returns 0 when there are no tabs.
    int DropIndexAt(int x) const;

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

    // Tab drag state (dragging a specific tab)
    int     dragTabIdx_      = -1;
    wxPoint dragAnchor_      { -1, -1 };
    bool    dragPending_     = false;

    // Header drag state (dragging from blank area → tile move)
    bool    headerDragPending_ = false;

    // Hover tracking for tooltip on truncated labels
    int     hoverTabIdx_     = -1;

    static constexpr int kCloseW  = 18;   // close "×" width inside each tab
    static constexpr int kPlusW   = 24;   // "+" button width
    static constexpr int kMinTabW = 50;
    static constexpr int kMaxTabW = 200;

    // Query callbacks
    TabCountCallback       tabCountCb_;
    LabelQueryCallback     labelCb_;
    ActiveTabCallback      activeTabCb_;
    BgColourCallback       bgColourCb_;
    BroadcastQueryCallback broadcastQueryCb_;

    // Notification callbacks
    TabSelectedCallback     selectedCb_;
    TabCloseCallback        closeCb_;
    TabDragStartCallback    tabDragCb_;
    NewTabCallback          newTabCb_;
    HeaderActivateCallback  headerActivateCb_;
    HeaderDragStartCallback headerDragCb_;
    HeaderCtrlClickCallback  headerCtrlClickCb_;
    HeaderRightClickCallback headerRightClickCb_;
};
