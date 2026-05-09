#include "ui/TerminalGrid.h"
#include "ui/TerminalTile.h"
#include <algorithm>
#include <cmath>
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

int TerminalGrid::ComputeGridColumns(int n)
{
    if (n <= 0) return 1;
    return static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
}

wxSize TerminalGrid::ComputeIdealGridSize() const
{
    if (tiles_.empty()) return wxSize(0, 0);

    // Mirrors the RowFirst layout pass so the result stays in sync when the
    // wrap condition changes (e.g. a future width-aware bin-pack PR).
    const int n    = static_cast<int>(tiles_.size());
    const int cols = ComputeGridColumns(n);

    int totalW = 0;
    int totalH = kGap;  // top gap
    int col    = 0;
    int rowW   = kGap;  // left gap of current row
    int rowH   = 0;

    for (TerminalTile* t : tiles_) {
        const wxSize sz = t->GetMinSize();

        if (col > 0 && col >= cols) {
            totalW  = std::max(totalW, rowW);
            totalH += rowH + kGap;
            rowW    = kGap;
            rowH    = 0;
            col     = 0;
        }

        rowW += sz.x + kGap;
        rowH  = std::max(rowH, sz.y);
        col++;
    }
    totalW  = std::max(totalW, rowW);
    totalH += rowH + kGap;  // bottom gap

    return wxSize(totalW, totalH);
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
        const int n    = static_cast<int>(tiles_.size());
        const int cols = ComputeGridColumns(n);

        int x        = kGap;
        int y        = kGap;
        int maxRowH  = 0;
        int col      = 0;
        int contentW = 0;

        for (TerminalTile* tile : tiles_) {
            const wxSize sz = tile->GetMinSize();

            if (col > 0 && col >= cols) {
                x       = kGap;
                y      += maxRowH + kGap;
                maxRowH = 0;
                col     = 0;
            }

            tile->SetSize(sz);
            tile->SetPosition(wxPoint(x, y));

            x += sz.x + kGap;
            contentW = std::max(contentW, x);
            maxRowH  = std::max(maxRowH, sz.y);
            col++;
        }

        SetVirtualSize(std::max(client.x, contentW), y + maxRowH + kGap);

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
