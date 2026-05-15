#include "ui/TerminalTile.h"
#include "ui/TerminalPanel.h"
#include "ui/TabStrip.h"
#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/utils.h>
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

    // Query callbacks — TabStrip reads these on every paint; no data is duplicated.
    tabStrip_->SetTabCountCallback([this]() {
        return static_cast<int>(tabs_.size());
    });
    tabStrip_->SetLabelQueryCallback([this](int i) -> wxString {
        return (i >= 0 && i < (int)tabs_.size()) ? tabs_[i].label : wxString{};
    });
    tabStrip_->SetActiveTabCallback([this]() {
        return activeTabIdx_;
    });
    tabStrip_->SetBgColourCallback([this]() {
        return titleBar_->GetBackgroundColour();
    });

    // Wire TabStrip notification callbacks back into the tile.
    tabStrip_->SetTabSelectedCallback([this](int idx) {
        if (idx < 0 || idx >= (int)tabs_.size()) return;
        ActivateTab(idx);
        EmitTileAction(TileAction::ActivateSession, tabs_[idx].sessionId);
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
        EmitTileAction(TileAction::ActivateSession, GetActiveSessionId());
    });

    tabStrip_->SetHeaderDragStartCallback([this](wxPoint screenPt) {
        if (dragStartCb_) dragStartCb_(this, screenPt);
    });

    // TabStrip reads broadcast state from TabEntry on every paint — no parallel array.
    tabStrip_->SetBroadcastQueryCallback([this](int i) {
        return i >= 0 && i < (int)tabs_.size() && tabs_[i].inBroadcast;
    });

    tabStrip_->SetHeaderCtrlClickCallback([this]() {
        EmitTerminalAction(TerminalAction::ToggleBroadcast);
    });

    tabStrip_->SetHeaderRightClickCallback([this](int tabIdx) {
        const bool onTab = tabIdx >= 0 && tabIdx < (int)tabs_.size();
        if (onTab) {
            // Tab context menu — scoped to the right-clicked tab's session.
            const auto sid = tabs_[tabIdx].sessionId;
            wxMenu menu;
            auto* broadcastItem = menu.Append(wxID_ANY, tabs_[tabIdx].inBroadcast
                                              ? "Remove from Broadcast" : "Add to Broadcast");
            menu.Bind(wxEVT_MENU, [this, sid](wxCommandEvent&) {
                TerminalActionEvent evt(TerminalAction::ToggleBroadcast, sid);
                ProcessWindowEvent(evt);
            }, broadcastItem->GetId());
            menu.AppendSeparator();
            auto* newTileItem = menu.Append(wxID_ANY, "Move to New Tile");
            menu.Bind(wxEVT_MENU, [this, sid](wxCommandEvent&) {
                EmitTileAction(TileAction::MoveToNewTile, sid);
            }, newTileItem->GetId());
            auto* newWindowItem = menu.Append(wxID_ANY, "Move to New Window");
            menu.Bind(wxEVT_MENU, [this, sid](wxCommandEvent&) {
                EmitTileAction(TileAction::MoveToNewWindow, sid);
            }, newWindowItem->GetId());
            PopupMenu(&menu);
        } else {
            // Tile context menu — background area, no specific tab.
            OnShowTileMenu();
        }
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
    panel->SetFocusCallback([this, id]() {
        EmitTileAction(TileAction::ActivateSession, id);
    });
    tabs_.push_back({ id, panel, label });
    tabStrip_->Refresh();
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
    tabStrip_->Refresh();

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
            tabStrip_->Refresh();
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

int TerminalTile::GetDropIndexAt(wxPoint screenPt) const
{
    return tabStrip_->DropIndexAt(tabStrip_->ScreenToClient(screenPt).x);
}

void TerminalTile::MoveTabToIndex(term::session::SessionId id, int insertBefore)
{
    const auto it = std::find_if(tabs_.begin(), tabs_.end(),
        [id](const TabEntry& e) { return e.sessionId == id; });
    if (it == tabs_.end()) return;

    const int fromIdx = static_cast<int>(std::distance(tabs_.begin(), it));
    const int n       = static_cast<int>(tabs_.size());
    const int toIdx   = std::clamp(insertBefore, 0, n);

    if (toIdx == fromIdx || toIdx == fromIdx + 1) return;

    if (toIdx < fromIdx) {
        std::rotate(tabs_.begin() + toIdx,
                    tabs_.begin() + fromIdx,
                    tabs_.begin() + fromIdx + 1);
    } else {
        std::rotate(tabs_.begin() + fromIdx,
                    tabs_.begin() + fromIdx + 1,
                    tabs_.begin() + toIdx);
    }

    if (activeTabIdx_ == fromIdx) {
        activeTabIdx_ = (toIdx < fromIdx) ? toIdx : toIdx - 1;
    } else if (toIdx < fromIdx) {
        if (activeTabIdx_ >= toIdx && activeTabIdx_ < fromIdx) ++activeTabIdx_;
    } else {
        if (activeTabIdx_ > fromIdx && activeTabIdx_ < toIdx) --activeTabIdx_;
    }

    tabStrip_->Refresh();
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
    UpdateTitleBarColor();  // calls titleBar_->Refresh(), which repaints TabStrip as child

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
        EmitTerminalAction(TerminalAction::ToggleBroadcast);
        return;
    }
    if (auto* p = GetActivePanel()) p->SetFocus();
    EmitTileAction(TileAction::ActivateSession, GetActiveSessionId());
    dragAnchor_ = evt.GetEventObject()
        ? static_cast<wxWindow*>(evt.GetEventObject())->ClientToScreen(evt.GetPosition())
        : evt.GetPosition();
    dragPending_ = true;
    wxSetCursor(wxCursor(wxCURSOR_HAND));
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
    if (std::abs(dx) > ui::kDragThreshold || std::abs(dy) > ui::kDragThreshold) {
        dragPending_ = false;
        if (dragStartCb_) dragStartCb_(this, dragAnchor_);
    }
    evt.Skip();
}

void TerminalTile::OnTitleUp(wxMouseEvent& evt)
{
    if (dragPending_)
        wxSetCursor(wxNullCursor);
    dragPending_ = false;
    evt.Skip();
}

void TerminalTile::OnShowTileMenu()
{
    wxMenu menu;
    auto* broadcastItem = menu.Append(wxID_ANY, inBroadcast_ ? "Remove from Broadcast"
                                                              : "Add to Broadcast");
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        EmitTerminalAction(TerminalAction::ToggleBroadcast);
    }, broadcastItem->GetId());
    menu.AppendSeparator();
    auto* newWindowItem = menu.Append(wxID_ANY, "Move to New Window");
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        EmitTileAction(TileAction::MoveAllToNewWindow, 0);
    }, newWindowItem->GetId());
    PopupMenu(&menu);
}

void TerminalTile::OnTitleRightClick(wxMouseEvent&)
{
    OnShowTileMenu();
}

bool TerminalTile::DropSession(std::span<const term::session::SessionId> ids,
                               ui::DragIntent intent,
                               wxPoint screenPt)
{
    for (auto id : ids) {
        for (int i = 0; i < GetTabCount(); ++i) {
            if (GetSessionIdByTabIndex(i) == id) {
                if (intent != ui::DragIntent::Tabs) return false;
                MoveTabToIndex(id, GetDropIndexAt(screenPt));
                return true;
            }
        }
    }
    if (!dropSessionCb_) return false;
    TerminalTile* dst = (intent == ui::DragIntent::Tile) ? nullptr : this;
    return dropSessionCb_(ids, dst);
}
