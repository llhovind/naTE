#include "ui/PortForwardPanel.h"
#include "ui/AddPortForwardDialog.h"
#include "ui/ColorUtils.h"

#include <wx/button.h>
#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

// ---------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------

static constexpr int kRowPadH = 6;   // horizontal inner padding for each row
static constexpr int kRowPadV = 2;   // vertical inner padding
static constexpr int kColGap  = 8;   // gap between row columns

// ---------------------------------------------------------------------------

PortForwardPanel::PortForwardPanel(wxWindow* parent, const AppConfig& cfg)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , labelFg_(toWx(UiColors{}.controlActive))
{
    SetBackgroundColour(toWx(UiColors{}.searchBarBg));
    SetForegroundColour(labelFg_);
    SetMinSize(wxSize(360, -1));

    // Header: title label + stretch spacer + [+ Add] button.
    auto* headerSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* titleLbl = new wxStaticText(this, wxID_ANY, "Port Forwards");
    titleLbl->SetForegroundColour(labelFg_);
    headerSizer->AddSpacer(kRowPadH);
    headerSizer->Add(titleLbl, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, kRowPadV + 2);
    headerSizer->AddStretchSpacer(1);
    auto* addBtn = new wxButton(this, wxID_ANY, "+ Add",
                                wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxBU_EXACTFIT);
    addBtn->SetForegroundColour(labelFg_);
    headerSizer->Add(addBtn, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM | wxRIGHT,
                     kRowPadV);
    headerSizer->AddSpacer(kRowPadH);

    addBtn->Bind(wxEVT_BUTTON, &PortForwardPanel::OnAddClicked, this);

    // Scrolled area for forward rows.
    scroll_ = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                   wxVSCROLL | wxBORDER_NONE);
    scroll_->SetScrollRate(0, 10);
    scroll_->SetBackgroundColour(GetBackgroundColour());

    rowSizer_ = new wxBoxSizer(wxVERTICAL);
    scroll_->SetSizer(rowSizer_);

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(headerSizer, 0, wxEXPAND);
    outer->Add(scroll_,     1, wxEXPAND);
    SetSizer(outer);

    // Initially no rows, so collapse the scroll area.
    scroll_->SetMinSize(wxSize(-1, 0));
    Fit();
    Hide();
}

// ---------------------------------------------------------------------------

void PortForwardPanel::SetCallbacks(
    std::function<term::transport::PortForwardId(term::transport::PortForwardDesc)> onAdd,
    std::function<void(term::transport::PortForwardId)>                             onRemove,
    std::function<void(term::transport::PortForwardId)>                             onSaveToProfile)
{
    onAdd_           = std::move(onAdd);
    onRemove_        = std::move(onRemove);
    onSaveToProfile_ = std::move(onSaveToProfile);
}

// ---------------------------------------------------------------------------

void PortForwardPanel::UpdateStatus(
    std::vector<term::transport::PortForwardStatus> status,
    std::vector<term::transport::PortForwardDesc>   descs)
{
    status_ = std::move(status);
    descs_  = std::move(descs);

    RebuildRows();

    // Fire any pending result callbacks that now have a definitive state.
    for (const auto& s : status_) {
        auto it = pendingResultCbs_.find(s.id);
        if (it == pendingResultCbs_.end()) continue;

        const bool settled = s.active || !s.error.empty();
        if (!settled) continue;

        auto cb = std::move(it->second);
        pendingResultCbs_.erase(it);
        cb(s.error.empty(), s.error);
    }
}

// ---------------------------------------------------------------------------

void PortForwardPanel::RebuildRows()
{
    // Destroy all existing row widgets.
    rowSizer_->Clear(true);

    using namespace term::transport;

    for (const auto& desc : descs_) {
        // Find the corresponding status entry (may be absent for profile-only descs).
        const PortForwardStatus* st = nullptr;
        for (const auto& s : status_)
            if (s.id == desc.id) { st = &s; break; }

        const bool hasError = st && !st->error.empty();

        auto* rowPanel = new wxPanel(scroll_, wxID_ANY, wxDefaultPosition,
                                     wxDefaultSize, wxBORDER_NONE);
        rowPanel->SetBackgroundColour(GetBackgroundColour());

        auto* rowSizer = new wxBoxSizer(wxHORIZONTAL);
        rowSizer->AddSpacer(kRowPadH);

        // Direction arrow
        const wxString arrow = (desc.direction == PortForwardDirection::Local)
                               ? wxString::FromUTF8("\xe2\x86\x92")   // →
                               : wxString::FromUTF8("\xe2\x86\x90");  // ←
        auto* dirLbl = new wxStaticText(rowPanel, wxID_ANY, arrow,
                                        wxDefaultPosition, wxSize(14, -1));
        dirLbl->SetForegroundColour(labelFg_);
        rowSizer->Add(dirLbl, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, kRowPadV);
        rowSizer->AddSpacer(kColGap);

        // Ports and host description
        wxString portDesc;
        if (desc.direction == PortForwardDirection::Local) {
            portDesc = wxString::Format("%d ", desc.localPort)
                     + wxString::FromUTF8("\xe2\x86\x92 ")
                     + wxString::FromUTF8(desc.remoteHost)
                     + wxString::Format(":%d", desc.remotePort);
        } else {
            portDesc = wxString::Format("%d ", desc.remotePort)
                     + wxString::FromUTF8("\xe2\x86\x90 ")
                     + wxString::FromUTF8(desc.remoteHost)
                     + wxString::Format(":%d", desc.localPort);
        }
        auto* portLbl = new wxStaticText(rowPanel, wxID_ANY, portDesc);
        portLbl->SetForegroundColour(labelFg_);
        rowSizer->Add(portLbl, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, kRowPadV);
        rowSizer->AddSpacer(kColGap);

        // Label or error — expands to fill available space.
        wxString infoText;
        if (hasError)
            infoText = wxString::FromUTF8(st->error);
        else if (!desc.label.empty())
            infoText = wxString::FromUTF8(desc.label);

        auto* infoLbl = new wxStaticText(rowPanel, wxID_ANY, infoText,
                                          wxDefaultPosition, wxDefaultSize,
                                          wxST_ELLIPSIZE_END);
        infoLbl->SetForegroundColour(hasError ? wxColour(220, 60, 60) : labelFg_);

        rowSizer->Add(infoLbl, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, kRowPadV);
        rowSizer->AddSpacer(kColGap);

        // Connection count badge (show when active and > 0).
        if (st && st->active && st->connections > 0) {
            auto* cntLbl = new wxStaticText(rowPanel, wxID_ANY,
                                             wxString::Format("(%zu)", st->connections));
            wxColour dimmed = labelFg_;
            dimmed.Set(dimmed.Red(), dimmed.Green(), dimmed.Blue(), 150);
            cntLbl->SetForegroundColour(dimmed);
            rowSizer->Add(cntLbl, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, kRowPadV);
            rowSizer->AddSpacer(kColGap);
        }

        // Remove button.
        const term::transport::PortForwardId fwdId = desc.id;
        auto* removeBtn = new wxButton(rowPanel, wxID_ANY, wxString::FromUTF8("\xe2\x9c\x95"),
                                        wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxBU_EXACTFIT);
        removeBtn->SetForegroundColour(labelFg_);
        removeBtn->SetToolTip("Remove forward");
        removeBtn->Bind(wxEVT_BUTTON, [this, fwdId](wxCommandEvent&) {
            if (onRemove_) onRemove_(fwdId);
        });
        rowSizer->Add(removeBtn, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, kRowPadV);
        rowSizer->AddSpacer(kRowPadH);

        rowPanel->SetSizer(rowSizer);

        // Right-click context menu for "Save to Profile".
        rowPanel->Bind(wxEVT_RIGHT_DOWN, [this, fwdId](wxMouseEvent& e) {
            wxMenu menu;
            auto* saveItem = menu.Append(wxID_ANY, "Save to Profile");
            menu.Bind(wxEVT_MENU, [this, fwdId](wxCommandEvent&) {
                if (onSaveToProfile_) onSaveToProfile_(fwdId);
            }, saveItem->GetId());
            PopupMenu(&menu, e.GetPosition());
        });

        rowSizer_->Add(rowPanel, 0, wxEXPAND);
    }

    // Resize the scrolled area to show all rows (clamped at kMaxScrollHeight).
    scroll_->FitInside();
    const int rowsH = rowSizer_->GetMinSize().GetHeight();
    scroll_->SetMinSize(wxSize(-1, std::min(rowsH, kMaxScrollHeight)));

    if (GetSizer()) GetSizer()->Layout();
    Fit();

    // If visible, re-run the parent tile's OnSize() so it repositions us at
    // the correct right-aligned overlay rect after a row-count change.
    if (IsShown())
        if (auto* p = GetParent()) p->SendSizeEvent();
}

// ---------------------------------------------------------------------------

void PortForwardPanel::OnAddClicked(wxCommandEvent&)
{
    if (!onAdd_) return;

    // Live verify-then-close flow.
    // onSubmit_ receives (desc, resultCb) from the dialog. We call onAdd_(desc) which
    // assigns an ID synchronously, then stash resultCb keyed by that ID so
    // UpdateStatus() can deliver the transport result while ShowModal() spins its loop.
    auto* dlg = new ui::AddPortForwardDialog(
        GetParent(),
        [this](term::transport::PortForwardDesc desc, ResultCb resultCb) {
            const term::transport::PortForwardId id = onAdd_(std::move(desc));
            if (id == 0) {
                resultCb(false, "Failed to submit forward request.");
                return;
            }
            pendingResultCbs_[id] = std::move(resultCb);
        });

    dlg->ShowModal();
    dlg->Destroy();
}

// ---------------------------------------------------------------------------

void PortForwardPanel::ApplyConfig(const AppConfig& cfg)
{
    SetBackgroundColour(toWx(cfg.uiColors.searchBarBg));
    SetForegroundColour(toWx(cfg.uiColors.controlActive));
    labelFg_ = toWx(cfg.uiColors.controlActive);
    if (scroll_) scroll_->SetBackgroundColour(GetBackgroundColour());
    RebuildRows();
    Refresh();
}
