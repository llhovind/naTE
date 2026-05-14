#include "ui/TerminalTile.h"
#include "ui/TerminalPanel.h"
#include "ui/TabStrip.h"
#include <wx/menu.h>
#include <wx/sizer.h>
#include <algorithm>
#include <cmath>

wxDEFINE_EVENT(EVT_TERMINAL_ACTION, TerminalActionEvent);
wxDEFINE_EVENT(EVT_TILE_ACTION,     TileActionEvent);

TerminalTile::TerminalTile(wxWindow* parent, const AppConfig& /*cfg*/)
    : wxPanel(parent, wxID_ANY)
{
    // Title bar — plain panel so we can set background colour independently.
    titleBar_ = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                            wxSize(-1, kTitleBarHeight));
    titleBar_->SetBackgroundColour(colInactive_);

    // TabStrip occupies the left portion of the title bar.
    tabStrip_ = new TabStrip(titleBar_);
    tabStrip_->SetBgColour(colInactive_);

    // Wire TabStrip callbacks back into the tile.
    tabStrip_->SetTabSelectedCallback([this](int idx) {
        if (idx < 0 || idx >= (int)tabs_.size()) return;
        ActivateTab(idx);
        if (activateCb_) activateCb_(tabs_[idx].sessionId);
    });

    tabStrip_->SetTabCloseCallback([this](int idx) {
        if (idx < 0 || idx >= (int)tabs_.size()) return;
        TerminalActionEvent evt(TerminalAction::CloseSession, tabs_[idx].sessionId);
        ProcessWindowEvent(evt);
    });

    tabStrip_->SetTabDragStartCallback([this](int idx, wxPoint screenPt) {
        if (idx < 0 || idx >= (int)tabs_.size()) return;
        if (tabDragStartCb_) tabDragStartCb_(tabs_[idx].sessionId, screenPt);
    });

    tabStrip_->SetNewTabCallback([this]() {
        EmitTileAction(TileAction::NewTabHere, GetActiveSessionId());
    });

    // Blank header area (right of "+" button) — activate tile and arm whole-tile drag.
    tabStrip_->SetHeaderActivateCallback([this]() {
        if (auto* p = GetActivePanel()) p->SetFocus();
        if (activateCb_) activateCb_(GetActiveSessionId());
    });

    tabStrip_->SetHeaderDragStartCallback([this](wxPoint screenPt) {
        if (dragStartCb_) dragStartCb_(this, screenPt);
    });

    // TabStrip reads broadcast state from TabEntry on every paint — no parallel array.
    tabStrip_->SetBroadcastQueryCallback([this](int i) {
        return i >= 0 && i < (int)tabs_.size() && tabs_[i].inBroadcast;
    });

    tabStrip_->SetHeaderCtrlClickCallback([this]() {
        if (broadcastToggleCb_) broadcastToggleCb_(GetActiveSessionId());
    });

    tabStrip_->SetHeaderRightClickCallback([this]() {
        if (!broadcastToggleCb_) return;
        const bool activeInBroadcast = activeTabIdx_ >= 0
                                    && activeTabIdx_ < (int)tabs_.size()
                                    && tabs_[activeTabIdx_].inBroadcast;
        wxMenu menu;
        const wxString label = activeInBroadcast ? "Remove from Broadcast"
                                                 : "Add to Broadcast";
        menu.Append(wxID_ANY, label);
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            if (broadcastToggleCb_) broadcastToggleCb_(GetActiveSessionId());
        });
        PopupMenu(&menu);
    });

    wxBitmap wrapBmp(wxT("./src/ui/icons/text-wrap.png"), wxBITMAP_TYPE_PNG);
    wxASSERT_MSG(wrapBmp.IsOk(), "Failed to load text-wrap.png");
    wrapBtn_ = new wxBitmapButton(titleBar_, wxID_ANY, wrapBmp,
                                  wxDefaultPosition, wxSize(16, 16), wxBORDER_NONE);
    wrapBtn_->SetBackgroundColour(colInactive_);
    wrapBtn_->Bind(wxEVT_BUTTON, &TerminalTile::OnWrapClick, this);

    auto* hSizer = new wxBoxSizer(wxHORIZONTAL);
    hSizer->Add(tabStrip_, 1, wxEXPAND);
    hSizer->Add(wrapBtn_,  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    titleBar_->SetSizer(hSizer);

    // Content area — the active TerminalPanel fills this completely.
    contentArea_ = new wxPanel(this, wxID_ANY);
    contentArea_->SetBackgroundColour(*wxBLACK);

    // Route title bar events for tile drag (moves the active session to another window).
    titleBar_->Bind(wxEVT_LEFT_DOWN, &TerminalTile::OnTitleDown,       this);
    titleBar_->Bind(wxEVT_MOTION,    &TerminalTile::OnTitleMotion,      this);
    titleBar_->Bind(wxEVT_LEFT_UP,   &TerminalTile::OnTitleUp,          this);
    titleBar_->Bind(wxEVT_RIGHT_DOWN, &TerminalTile::OnTitleRightClick, this);

    Bind(wxEVT_SIZE, &TerminalTile::OnSize, this);
}

// ---------------------------------------------------------------------------
// Tab management
// ---------------------------------------------------------------------------

int TerminalTile::AddTab(term::session::SessionId id, TerminalPanel* panel,
                         const wxString& label)
{
    panel->Hide();  // hidden until ActivateTab selects it
    tabStrip_->AddTab(label);
    tabs_.push_back({ id, panel, label });
    const int newIdx = static_cast<int>(tabs_.size()) - 1;

    if ((int)tabs_.size() == 1) {
        // First tab: set tile min-size from the panel's requirements.
        const wxSize panelMin = panel->GetMinClientSize();
        SetMinSize({ panelMin.x, panelMin.y + kTitleBarHeight });
        ActivateTab(0);
    }
    return newIdx;
}

bool TerminalTile::RemoveTab(term::session::SessionId id)
{
    const auto it = std::find_if(tabs_.begin(), tabs_.end(),
        [id](const TabEntry& e) { return e.sessionId == id; });
    if (it == tabs_.end()) return tabs_.empty();

    const int removedIdx = static_cast<int>(std::distance(tabs_.begin(), it));

    // Destroy the panel (wx-child-owned; Destroy() is safe on UI thread).
    if (it->panel) it->panel->Destroy();
    tabs_.erase(it);
    tabStrip_->RemoveTab(removedIdx);

    if (tabs_.empty()) {
        activeTabIdx_ = -1;
        return true;
    }

    // Re-activate an adjacent tab.
    if (activeTabIdx_ == removedIdx) {
        activeTabIdx_ = -1;  // reset so ActivateTab doesn't skip
        ActivateTab(std::min(removedIdx, (int)tabs_.size() - 1));
    } else if (activeTabIdx_ > removedIdx) {
        --activeTabIdx_;
    }
    return false;
}

void TerminalTile::ActivateTabById(term::session::SessionId id)
{
    for (int i = 0; i < (int)tabs_.size(); ++i) {
        if (tabs_[i].sessionId == id) {
            ActivateTab(i);
            return;
        }
    }
}

void TerminalTile::SetTabLabel(term::session::SessionId id, const wxString& label)
{
    for (int i = 0; i < (int)tabs_.size(); ++i) {
        if (tabs_[i].sessionId == id) {
            tabs_[i].label = label;
            tabStrip_->SetTabLabel(i, label);
            return;
        }
    }
}

term::session::SessionId TerminalTile::GetSessionIdByTabIndex(int index) const
{
    if (index >= 0 && index < (int)tabs_.size())
        return tabs_[index].sessionId;
    return 0;
}

term::session::SessionId TerminalTile::GetActiveSessionId() const
{
    if (activeTabIdx_ >= 0 && activeTabIdx_ < (int)tabs_.size())
        return tabs_[activeTabIdx_].sessionId;
    return 0;
}

TerminalPanel* TerminalTile::GetActivePanel() const
{
    if (activeTabIdx_ >= 0 && activeTabIdx_ < (int)tabs_.size())
        return tabs_[activeTabIdx_].panel;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void TerminalTile::ActivateTab(int index)
{
    if (index < 0 || index >= (int)tabs_.size()) return;
    if (activeTabIdx_ == index) return;

    if (activeTabIdx_ >= 0 && activeTabIdx_ < (int)tabs_.size())
        tabs_[activeTabIdx_].panel->Hide();

    activeTabIdx_ = index;

    auto* panel = tabs_[index].panel;
    // Size the panel to fill the content area before showing to prevent flicker.
    const wxSize contentSz = contentArea_->GetClientSize();
    panel->SetSize(0, 0, contentSz.x, contentSz.y);
    panel->Show();

    // Sync header colour with the newly active tab's broadcast state.
    inBroadcast_ = tabs_[index].inBroadcast;
    UpdateTitleBarColor();

    tabStrip_->SetActiveTab(index);
    contentArea_->Refresh();
}

void TerminalTile::OnSize(wxSizeEvent& evt)
{
    const wxSize sz = GetClientSize();
    titleBar_->SetSize(0, 0, sz.x, kTitleBarHeight);
    contentArea_->SetSize(0, kTitleBarHeight, sz.x, sz.y - kTitleBarHeight);

    // Keep the active panel sized to fill the content area.
    if (activeTabIdx_ >= 0 && activeTabIdx_ < (int)tabs_.size()) {
        if (auto* p = tabs_[activeTabIdx_].panel)
            p->SetSize(0, 0, sz.x, sz.y - kTitleBarHeight);
    }
    evt.Skip();
}

void TerminalTile::UpdateTitleBarColor()
{
    const wxColour& c = inBroadcast_ ? colBroadcast_
                        : isFocused_ ? colActive_
                                     : colInactive_;
    titleBar_->SetBackgroundColour(c);
    titleBar_->Refresh();
    if (wrapBtn_) {
        wrapBtn_->SetBackgroundColour(c);
        wrapBtn_->Refresh();
    }
    if (tabStrip_) {
        tabStrip_->SetBgColour(c);
    }
}

void TerminalTile::SetFocused(bool focused)
{
    isFocused_ = focused;
    UpdateTitleBarColor();
}

void TerminalTile::SetBroadcastActive(bool active)
{
    inBroadcast_ = active;
    UpdateTitleBarColor();
}

void TerminalTile::SetTabBroadcast(term::session::SessionId id, bool inBroadcast)
{
    for (auto& tab : tabs_) {
        if (tab.sessionId == id) {
            tab.inBroadcast = inBroadcast;
            tabStrip_->Refresh();  // TabStrip queries tabs_ via BroadcastQueryCallback on next paint
            return;
        }
    }
}

void TerminalTile::SetWrapMode(bool /*wrap*/)
{
    // Visual toggle state for the wrap button can be extended here when a
    // toggled icon variant is available.
}

void TerminalTile::EmitTerminalAction(TerminalAction action)
{
    TerminalActionEvent evt(action, GetActiveSessionId());
    ProcessWindowEvent(evt);
}

void TerminalTile::EmitTileAction(TileAction action, term::session::SessionId id)
{
    TileActionEvent evt(action, id, this);
    ProcessWindowEvent(evt);
}

void TerminalTile::OnWrapClick(wxCommandEvent&)
{
    EmitTerminalAction(TerminalAction::ToggleWrap);
}

void TerminalTile::OnTitleDown(wxMouseEvent& evt)
{
    if (evt.ControlDown()) {
        if (broadcastToggleCb_) broadcastToggleCb_(GetActiveSessionId());
        return;
    }
    if (auto* p = GetActivePanel()) p->SetFocus();
    if (activateCb_) activateCb_(GetActiveSessionId());
    dragAnchor_ = evt.GetEventObject()
        ? static_cast<wxWindow*>(evt.GetEventObject())->ClientToScreen(evt.GetPosition())
        : evt.GetPosition();
    dragPending_ = true;
    evt.Skip();
}

void TerminalTile::OnTitleMotion(wxMouseEvent& evt)
{
    if (!dragPending_ || !evt.LeftIsDown()) {
        dragPending_ = false;
        evt.Skip();
        return;
    }
    const wxPoint cur = static_cast<wxWindow*>(evt.GetEventObject())
                            ->ClientToScreen(evt.GetPosition());
    const int dx = cur.x - dragAnchor_.x;
    const int dy = cur.y - dragAnchor_.y;
    if (std::abs(dx) > kDragThreshold || std::abs(dy) > kDragThreshold) {
        dragPending_ = false;
        if (dragStartCb_) dragStartCb_(this, dragAnchor_);
    }
    evt.Skip();
}

void TerminalTile::OnTitleUp(wxMouseEvent& evt)
{
    dragPending_ = false;
    evt.Skip();
}

void TerminalTile::OnTitleRightClick(wxMouseEvent&)
{
    if (!broadcastToggleCb_) return;
    wxMenu menu;
    const wxString label = inBroadcast_ ? "Remove from Broadcast"
                                        : "Add to Broadcast";
    menu.Append(wxID_ANY, label);
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (broadcastToggleCb_) broadcastToggleCb_(GetActiveSessionId());
    });
    PopupMenu(&menu);
}
