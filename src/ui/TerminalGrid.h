#pragma once
#include "config/Config.h"
#include <limits>
#include <vector>
#include <wx/scrolwin.h>

class TerminalTile;

class TerminalGrid : public wxScrolledWindow {
public:
    explicit TerminalGrid(wxWindow* parent);

    void AddTile(TerminalTile* tile);
    void RemoveTile(TerminalTile* tile);

    // Reorder: move src to just before or after ref, based on screenPt vs. ref's midpoint.
    void MoveTileNear(TerminalTile* src, TerminalTile* ref, wxPoint screenPt);

    // Marks one tile as focused (blue title bar); clears all others.
    void SetActiveTile(TerminalTile* tile);

    // Changes the fill axis and reflows immediately.
    void      SetDirection(TileLayout dir);
    TileLayout GetDirection() const { return direction_; }

    // Returns the column count for N tiles using the balanced-grid strategy
    // (cols = ceil(sqrt(N))). Used by RowFirst layout and sizing.
    static int ComputeGridColumns(int n);

    // Returns the pixel content size needed to display all current tiles at
    // their min size under the active layout strategy. Used by UIManager to
    // resize the frame after tiles are added.
    //
    // For ColumnFirst, maxHeight constrains the column height so the simulation
    // matches what RelayoutTiles() will actually produce on screen.  Pass the
    // display work-area client height; defaults to unconstrained.
    wxSize ComputeIdealGridSize(
        int maxHeight = std::numeric_limits<int>::max()) const;

    // Returns tiles in insertion order (left-to-right, top-to-bottom under the
    // current layout). Used by App::SaveRestoreState to enumerate the layout.
    const std::vector<TerminalTile*>& GetTiles() const { return tiles_; }

    static constexpr int kGap = 2;  // px between tiles

private:
    void RelayoutTiles();
    void OnSize(wxSizeEvent& evt);

    TileLayout               direction_ = TileLayout::RowFirst;
    std::vector<TerminalTile*> tiles_;   // insertion order; wx-parent-owned
};
