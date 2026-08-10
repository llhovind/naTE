#pragma once

#include <algorithm>

namespace ui {

// Pure sizing policy for a window measured in whole panes: the file explorer
// is one pane wide while exploring and two equal panes wide while transferring,
// so switching mode is arithmetic on the pane already on screen rather than a
// remembered size that can go stale. Kept free of wx so it can be unit-tested
// headlessly; the frame supplies the measurements.
struct PaneMetrics {
    int chrome  = 0;  // window border plus the sizer margins either side
    int sash    = 0;  // divider drawn between two panes
    int minPane = 0;  // narrowest a single pane may become
};

// Width the frame needs to show `panes` panes of `paneWidth` each. A pane
// narrower than the minimum is treated as the minimum: a window cannot be
// doubled from a width it was never allowed to have.
inline int FrameWidthForPanes(int panes, int paneWidth, PaneMetrics m)
{
    if (panes <= 0) return m.chrome;
    const int pane = std::max(paneWidth, m.minPane);
    return m.chrome + panes * pane + (panes - 1) * m.sash;
}

// Narrowest the frame may become while still showing `panes` usable panes.
// `frameMinimum` is the window's own floor, which wins when it is the larger:
// one pane at its minimum is still too cramped to be worth showing.
inline int MinFrameWidthForPanes(int panes, PaneMetrics m, int frameMinimum)
{
    return std::max(frameMinimum, FrameWidthForPanes(panes, m.minPane, m));
}

} // namespace ui
