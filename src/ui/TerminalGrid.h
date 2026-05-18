#pragma once
#include <vector>
#include <wx/scrolwin.h>

class TerminalTile;

enum class GridDirection { RowFirst, ColumnFirst };

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
    void SetDirection(GridDirection dir);

    // Returns the column count for N tiles using the balanced-grid strategy
    // (cols = ceil(sqrt(N))). Single source of truth for layout and sizing.
    static int ComputeGridColumns(int n);

    // Returns the pixel content size needed to display all current tiles at
    // their min size under the active layout strategy. Used by UIManager to
    // resize the frame after tiles are added.
    wxSize ComputeIdealGridSize() const;

    // Returns tiles in insertion order (left-to-right, top-to-bottom under the
    // current layout). Used by App::SaveRestoreSnapshot to enumerate the layout.
    const std::vector<TerminalTile*>& GetTiles() const { return tiles_; }

    static constexpr int kGap = 2;  // px between tiles

private:
    void RelayoutTiles();
    void OnSize(wxSizeEvent& evt);

    GridDirection            direction_ = GridDirection::RowFirst;
    std::vector<TerminalTile*> tiles_;   // insertion order; wx-parent-owned
};
