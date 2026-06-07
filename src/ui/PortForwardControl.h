#pragma once
#include "ui/TileIndicatorControl.h"
#include "transport/PortForward.h"
#include <vector>

// Tile header indicator for SSH port forwarding. Hidden for non-SSH sessions.
// Glyph: subdued cloud dome (1px pen) over two bold bidirectional arrows (2px pen).
// Arrows are the dominant visual element; cloud reads as "tunnel through network".
class PortForwardControl : public TileIndicatorControl
{
public:
    explicit PortForwardControl(wxWindow* parent);
    void SetStatus(const std::vector<term::transport::PortForwardStatus>& status);

private:
    void OnPaint(wxPaintEvent&);
    bool hasActive_ = false;
};
