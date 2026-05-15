#include "ui/TerminalGrid.h"
#include "ui/TerminalTile.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <wx/sizer.h>

TerminalGrid::TerminalGrid(wxWindow* parent)
    : wxScrolledWindow(parent, wxID_ANY)
{
    SetScrollRate(8, 8);
    SetBackgroundColour(wxColour(145, 145, 156));
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

void TerminalGrid::MoveTileNear(TerminalTile* src, TerminalTile* ref, wxPoint screenPt)
{
    auto itSrc = std::find(tiles_.begin(), tiles_.end(), src);
    auto itRef = std::find(tiles_.begin(), tiles_.end(), ref);
    if (itSrc == tiles_.end() || itRef == tiles_.end()) return;

    const wxRect r = ref->GetScreenRect();
    const bool insertBefore = (direction_ == GridDirection::RowFirst)
        ? screenPt.x < r.GetLeft() + r.GetWidth()  / 2
        : screenPt.y < r.GetTop()  + r.GetHeight() / 2;

    tiles_.erase(itSrc);
    itRef = std::find(tiles_.begin(), tiles_.end(), ref);  // re-find after erase
    tiles_.insert(insertBefore ? itRef : std::next(itRef), src);
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

// Partition n items (insertion order) into k contiguous groups minimising the
// maximum group-sum. weights[i] is the "width" of item i (tile_width + kGap).
// Returns group sizes in order. Classic linear-partition DP: O(n²k).
static std::vector<int> LinearPartition(const std::vector<int>& weights, int k)
{
    const int n = static_cast<int>(weights.size());
    k = std::min(k, n);

    std::vector<int> prefix(n + 1, 0);
    for (int i = 0; i < n; i++)
        prefix[i + 1] = prefix[i] + weights[i];

    const int INF = std::numeric_limits<int>::max() / 2;
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(k + 1, INF));
    std::vector<std::vector<int>> from(n + 1, std::vector<int>(k + 1, 0));

    for (int i = 1; i <= n; i++)
        dp[i][1] = prefix[i];

    for (int j = 2; j <= k; j++) {
        for (int i = j; i <= n; i++) {
            for (int m = j - 1; m < i; m++) {
                if (dp[m][j - 1] == INF) continue;
                const int cost = std::max(dp[m][j - 1], prefix[i] - prefix[m]);
                if (cost < dp[i][j]) {
                    dp[i][j] = cost;
                    from[i][j] = m;
                }
            }
        }
    }

    std::vector<int> sizes;
    int i = n, j = k;
    while (j > 1) {
        const int m = from[i][j];
        sizes.push_back(i - m);
        i = m;
        --j;
    }
    sizes.push_back(i);
    std::reverse(sizes.begin(), sizes.end());
    return sizes;
}

int TerminalGrid::ComputeGridColumns(int n)
{
    if (n <= 0) return 1;
    return static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
}

wxSize TerminalGrid::ComputeIdealGridSize() const
{
    if (tiles_.empty()) return wxSize(0, 0);

    const int n    = static_cast<int>(tiles_.size());
    const int cols = ComputeGridColumns(n);
    const int rows = (n + cols - 1) / cols;

    // Each tile contributes (width + kGap) to its row's total; the leading
    // kGap (left margin) is added once per row in the pixel pass below.
    std::vector<int> weights(n);
    for (int i = 0; i < n; i++)
        weights[i] = tiles_[i]->GetMinSize().x + kGap;

    // Optimal contiguous partition: minimises the maximum row width.
    const std::vector<int> rowSizes = LinearPartition(weights, rows);

    int totalW  = 0;
    int totalH  = kGap;  // top gap
    int tileIdx = 0;

    for (const int rowSize : rowSizes) {
        int rowW = kGap;  // left gap
        int rowH = 0;
        for (int j = 0; j < rowSize; j++) {
            const wxSize sz = tiles_[tileIdx++]->GetMinSize();
            rowW += sz.x + kGap;
            rowH  = std::max(rowH, sz.y);
        }
        totalW  = std::max(totalW, rowW);
        totalH += rowH + kGap;
    }

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
        int x        = kGap;
        int y        = kGap;
        int maxRowH  = 0;
        int contentW = 0;

        for (TerminalTile* tile : tiles_) {
            const wxSize sz = tile->GetMinSize();

            // Wrap when the next tile won't fit — but never on the first tile
            // in a row (x == kGap) so a tile wider than the frame still lands.
            if (x > kGap && x + sz.x + kGap > client.x) {
                x       = kGap;
                y      += maxRowH + kGap;
                maxRowH = 0;
            }

            tile->SetSize(sz);
            tile->SetPosition(wxPoint(x, y));

            x += sz.x + kGap;
            contentW = std::max(contentW, x);
            maxRowH  = std::max(maxRowH, sz.y);
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
