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

    // Marks one tile as focused (blue title bar); clears all others.
    void SetActiveTile(TerminalTile* tile);

    // Changes the fill axis and reflows immediately.
    void SetDirection(GridDirection dir);

    static constexpr int kGap = 8;  // px between tiles

private:
    void RelayoutTiles();
    void OnSize(wxSizeEvent& evt);

    GridDirection            direction_ = GridDirection::RowFirst;
    std::vector<TerminalTile*> tiles_;   // insertion order; wx-parent-owned
};
