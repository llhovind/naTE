#pragma once

#include <functional>
#include <vector>
#include <wx/window.h>
#include <wx/string.h>

// Custom-drawn tab strip embedded in the TerminalTile title bar.
// Renders N session tabs left-justified and a "+" new-tab button at the right
// edge of its allocated space.  Fills the full height of its parent (the title
// bar) so no separate height constant is needed.
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
    // Callbacks wired by TerminalTile
    // -------------------------------------------------------------------------
    using TabSelectedCallback    = std::function<void(int index)>;
    using TabCloseCallback       = std::function<void(int index)>;
    using TabDragStartCallback   = std::function<void(int index, wxPoint screenPt)>;
    using NewTabCallback         = std::function<void()>;

    void SetTabSelectedCallback (TabSelectedCallback  cb) { selectedCb_  = std::move(cb); }
    void SetTabCloseCallback    (TabCloseCallback     cb) { closeCb_     = std::move(cb); }
    void SetTabDragStartCallback(TabDragStartCallback cb) { dragStartCb_ = std::move(cb); }
    void SetNewTabCallback      (NewTabCallback       cb) { newTabCb_    = std::move(cb); }

private:
    // Per-tab geometry computed from current client size.
    struct TabGeom {
        int tabW  = 0;   // individual tab width (uniform)
        int plusX = 0;   // x origin of "+" button
        int plusW = 0;   // width of "+" button area
    };
    TabGeom ComputeGeom() const;

    // Returns tab index under x-coordinate, or -1 if in "+" zone or blank.
    // Sets closeHit=true if the click lands on the close widget of that tab.
    int HitTest(int x, bool& closeHit) const;

    void OnPaint      (wxPaintEvent&);
    void OnLeftDown   (wxMouseEvent&);
    void OnMiddleDown (wxMouseEvent&);
    void OnMotion     (wxMouseEvent&);
    void OnLeftUp     (wxMouseEvent&);
    void OnMouseLeave (wxMouseEvent&);

    std::vector<wxString> labels_;
    int                   activeIdx_ = -1;
    wxColour              bgColour_  { 131, 136, 141 };  // matches colInactive_

    // Drag state
    int     dragTabIdx_  = -1;
    wxPoint dragAnchor_  { -1, -1 };
    bool    dragPending_ = false;
    static constexpr int kDragThreshold = 5;

    // Close button width inside each tab (pixels)
    static constexpr int kCloseW  = 18;
    // New-tab "+" button width
    static constexpr int kPlusW   = 24;
    // Minimum per-tab width before truncation
    static constexpr int kMinTabW = 50;
    // Maximum per-tab width
    static constexpr int kMaxTabW = 200;

    TabSelectedCallback    selectedCb_;
    TabCloseCallback       closeCb_;
    TabDragStartCallback   dragStartCb_;
    NewTabCallback         newTabCb_;
};
