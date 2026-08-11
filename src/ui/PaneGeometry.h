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

// The controls row beneath the panes, measured for one job: putting the seam
// between the two copy buttons directly under the sash. Each button then sits
// over the pane it copies *out of*, and its arrow points across the sash at the
// pane it copies into — so the row reads as the movement it performs, and it
// keeps reading that way after the user drags the sash off centre.
//
// Widths are the buttons' own; the row does not stretch them.
struct ControlsRowMetrics {
    int contentLeft    = 0;  // x the first item in the row starts at
    int contentRight   = 0;  // x the row must not spill past
    int leading        = 0;  // everything before the gap (mode button + its margin)
    int sourceButton   = 0;  // the button left of the seam
    int destButton     = 0;  // the button right of the seam
    int betweenButtons = 0;  // margin separating the two
    int minGap         = 0;  // closest the pair may come to the leading items
};

// Width of the spacer between the leading items and the copy pair that lands
// the seam on `sashCentre`. Both bounds are honoured over the alignment: a
// window narrow enough — or labels long enough — that the pair cannot reach
// the sash gets the nearest legal position rather than a button hidden under
// its neighbour or off the right edge.
inline int LeadingGapForSashAlignedPair(int sashCentre, ControlsRowMetrics m)
{
    // The seam is the centre of the margin between the buttons, not the edge of
    // either one: an odd margin would otherwise bias the pair one pixel left.
    const int wanted = sashCentre - m.contentLeft - m.leading - m.sourceButton
                     - m.betweenButtons / 2;

    const int widest = m.contentRight - m.contentLeft - m.leading - m.sourceButton
                     - m.betweenButtons - m.destButton;

    const int upper = std::max(0, widest);
    return std::clamp(wanted, std::min(m.minGap, upper), upper);
}

} // namespace ui
