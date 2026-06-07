#pragma once
#include "transport/PortForward.h"
#include "ui/AddPortForwardDialog.h"
#include "config/Config.h"
#include <functional>
#include <map>
#include <vector>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>

// Inline panel that appears below the TerminalTile title bar when the
// port-forward indicator icon is clicked.  Lists active/failed forwards in
// row form and provides an [+ Add] button that opens AddPortForwardDialog.
//
// Owned by TerminalTile.  Toggled with Show()/Hide() + parent Layout().
// UIManager wires callbacks once via SetCallbacks() immediately after
// construction.
class PortForwardPanel : public wxPanel
{
public:
    using ResultCb = ui::AddPortForwardDialog::ResultCb;

    PortForwardPanel(wxWindow* parent, const AppConfig& cfg);

    void SetCallbacks(
        std::function<term::transport::PortForwardId(term::transport::PortForwardDesc)> onAdd,
        std::function<void(term::transport::PortForwardId)>                             onRemove,
        std::function<void(term::transport::PortForwardId)>                             onSaveToProfile);

    // Delivers new status + desc vectors from the session (called on UI thread).
    // Fires any pending result callbacks whose forward now has a definitive state.
    void UpdateStatus(std::vector<term::transport::PortForwardStatus> status,
                      std::vector<term::transport::PortForwardDesc>   descs);

    void ApplyConfig(const AppConfig& cfg);

private:
    void OnAddClicked(wxCommandEvent&);
    void RebuildRows();

    // Max height for the scrolled row area (px). Panel stops growing past this.
    static constexpr int kMaxScrollHeight = 120;

    wxScrolledWindow*   scroll_   = nullptr;
    wxBoxSizer*         rowSizer_ = nullptr;
    wxColour            labelFg_;   // foreground for all row labels; set from uiColors.controlActive

    std::vector<term::transport::PortForwardStatus> status_;
    std::vector<term::transport::PortForwardDesc>   descs_;

    // Maps forward ID → pending result callback from an in-flight AddPortForwardDialog.
    std::map<term::transport::PortForwardId, ResultCb> pendingResultCbs_;

    std::function<term::transport::PortForwardId(term::transport::PortForwardDesc)> onAdd_;
    std::function<void(term::transport::PortForwardId)>                             onRemove_;
    std::function<void(term::transport::PortForwardId)>                             onSaveToProfile_;
};
