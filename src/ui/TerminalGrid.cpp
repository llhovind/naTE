#include "ui/TerminalGrid.h"
#include "ui/TerminalTile.h"
#include <algorithm>
#include <wx/sizer.h>

TerminalGrid::TerminalGrid(wxWindow* parent)
    : wxScrolledWindow(parent, wxID_ANY)
{
    SetScrollRate(8, 8);
    SetBackgroundColour(wxColour(40, 40, 40));
    Bind(wxEVT_SIZE, &TerminalGrid::OnSize, this);
}

void TerminalGrid::AddTile(TerminalTile* tile)
{
    tiles_.push_back(tile);
    RelayoutTiles();
}

void TerminalGrid::RemoveTile(TerminalTile* tile)
{
    tiles_.erase(std::remove(tiles_.begin(), tiles_.end(), tile), tiles_.end());
    RelayoutTiles();
}

void TerminalGrid::SetActiveTile(TerminalTile* active)
{
    for (TerminalTile* t : tiles_)
        t->SetFocused(t == active);
}

void TerminalGrid::SetDirection(GridDirection dir)
{
    direction_ = dir;
    RelayoutTiles();
}

void TerminalGrid::OnSize(wxSizeEvent& evt)
{
    RelayoutTiles();
    evt.Skip();
}

void TerminalGrid::RelayoutTiles()
{
    if (tiles_.empty()) {
        SetVirtualSize(0, 0);
        return;
    }

    const wxSize client = GetClientSize();

    if (direction_ == GridDirection::RowFirst) {
        int x = kGap;
        int y = kGap;
        int maxRowH = 0;

        for (TerminalTile* tile : tiles_) {
            const wxSize sz = tile->GetMinSize();

            // Wrap to next row when the tile won't fit — but never on the
            // first tile in a row (x == kGap) to avoid infinite empty rows.
            if (x > kGap && x + sz.x + kGap > client.x) {
                x = kGap;
                y += maxRowH + kGap;
                maxRowH = 0;
            }

            tile->SetSize(sz);
            tile->SetPosition(wxPoint(x, y));

            x += sz.x + kGap;
            maxRowH = std::max(maxRowH, sz.y);
        }

        SetVirtualSize(client.x, y + maxRowH + kGap);

    } else {  // ColumnFirst
        int x = kGap;
        int y = kGap;
        int maxColW = 0;

        for (TerminalTile* tile : tiles_) {
            const wxSize sz = tile->GetMinSize();

            // Wrap to next column when the tile won't fit vertically.
            if (y > kGap && y + sz.y + kGap > client.y) {
                y = kGap;
                x += maxColW + kGap;
                maxColW = 0;
            }

            tile->SetSize(sz);
            tile->SetPosition(wxPoint(x, y));

            y += sz.y + kGap;
            maxColW = std::max(maxColW, sz.x);
        }

        SetVirtualSize(x + maxColW + kGap, client.y);
    }
}
