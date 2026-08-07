#include "ui/TerminalTile.h"
#include "ui/ColorUtils.h"
#include "ui/TerminalPanel.h"
#include "ui/TabStrip.h"
#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/utils.h>
#include <algorithm>
#include <cmath>

wxDEFINE_EVENT(EVT_TERMINAL_ACTION, TerminalActionEvent);
wxDEFINE_EVENT(EVT_TILE_ACTION,     TileActionEvent);

TerminalTile::TerminalTile(wxWindow* parent, const AppConfig& cfg)
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

    tabStrip_->SetUnreadQueryCallback([this](int i) {
        return i >= 0 && i < (int)tabs_.size() && tabs_[i].hasUnreadOutput;
    });

    tabStrip_->SetStatusQueryCallback([this](int i) {
        using S = term::session::SessionStatus;
        if (i < 0 || i >= (int)tabs_.size()) return S::Connected;
        return statusProvider_ ? statusProvider_(tabs_[i].sessionId) : S::Connected;
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
            menu.AppendSeparator();
            auto* resetItem = menu.Append(wxID_ANY, "Reset Terminal");
            menu.Bind(wxEVT_MENU, [this, sid](wxCommandEvent&) {
                TerminalActionEvent evt(TerminalAction::ResetTerminal, sid);
                ProcessWindowEvent(evt);
            }, resetItem->GetId());
            auto* resetClearItem = menu.Append(wxID_ANY, "Reset and Clear...");
            menu.Bind(wxEVT_MENU, [this, sid](wxCommandEvent&) {
                TerminalActionEvent evt(TerminalAction::ResetAndClear, sid);
                ProcessWindowEvent(evt);
            }, resetClearItem->GetId());
            menu.AppendSeparator();
            auto* saveItem = menu.Append(wxID_ANY, "Save Session to File...");
            menu.Bind(wxEVT_MENU, [this, sid](wxCommandEvent&) {
                TerminalActionEvent evt(TerminalAction::SaveToFile, sid);
                ProcessWindowEvent(evt);
            }, saveItem->GetId());
            menu.AppendSeparator();
            const bool sshTab = tabs_[tabIdx].supportsFileTransfer;
            auto* xferItem = menu.Append(wxID_ANY, "Transfer Files...");
            xferItem->Enable(fileTransferAvailableCb_ && fileTransferAvailableCb_());
            menu.Bind(wxEVT_MENU, [this, sid](wxCommandEvent&) {
                TerminalActionEvent evt(TerminalAction::TransferFiles, sid);
                ProcessWindowEvent(evt);
            }, xferItem->GetId());
            auto* editRemoteItem = menu.Append(wxID_ANY, "Edit Remote File...");
            editRemoteItem->Enable(sshTab);
            menu.Bind(wxEVT_MENU, [this, sid](wxCommandEvent&) {
                TerminalActionEvent evt(TerminalAction::EditRemoteFile, sid);
                ProcessWindowEvent(evt);
            }, editRemoteItem->GetId());
            PopupMenu(&menu);
        } else {
            // Tile context menu — background area, no specific tab.
            OnShowTileMenu();
        }
    });

    wrapCtrl_ = new WrapControl(titleBar_);
    wrapCtrl_->SetClickCallback([this] { EmitTerminalAction(TerminalAction::ToggleWrap); });

    altScrCtrl_ = new AltScrControl(titleBar_);
    altScrCtrl_->SetClickCallback([this] { EmitTerminalAction(TerminalAction::ToggleAltScr); });

    x11Ctrl_ = new X11Control(titleBar_);
    // X11Control is a status indicator only — no click action.

    portFwdCtrl_ = new PortForwardControl(titleBar_);
    portFwdCtrl_->SetClickCallback([this] {
        if (!portFwdPanel_) return;
        const bool show = !portFwdPanel_->IsShown();
        portFwdPanel_->Show(show);
        if (show) {
            const wxSize sz   = GetClientSize();
            const wxSize best = portFwdPanel_->GetBestSize();
            portFwdPanel_->SetSize(sz.x - best.x, kTitleBarHeight, best.x, best.y);
            portFwdPanel_->Raise();
        }
    });

    auto* hSizer = new wxBoxSizer(wxHORIZONTAL);
    hSizer->Add(tabStrip_,    1, wxEXPAND);
    hSizer->Add(portFwdCtrl_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    hSizer->Add(x11Ctrl_,     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    hSizer->Add(altScrCtrl_,  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    hSizer->Add(wrapCtrl_,    0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    titleBar_->SetSizer(hSizer);

    // Content area — the active TerminalPanel fills this completely.
    contentArea_ = new wxPanel(this, wxID_ANY);
    // TerminalPanel will set its own bg; this is just the gap between panels.
    contentArea_->SetBackgroundColour(toWx(cfg.uiColors.frameBackground));

    // Created after contentArea_ so it is higher in Z-order and can overlay it.
    portFwdPanel_ = new PortForwardPanel(this, cfg);

    // Route title bar events for tile drag (moves the active session to another window).
    titleBar_->Bind(wxEVT_LEFT_DOWN, &TerminalTile::OnTitleDown,       this);
    titleBar_->Bind(wxEVT_MOTION,    &TerminalTile::OnTitleMotion,      this);
    titleBar_->Bind(wxEVT_LEFT_UP,   &TerminalTile::OnTitleUp,          this);
    titleBar_->Bind(wxEVT_RIGHT_DOWN, &TerminalTile::OnTitleRightClick, this);

    Bind(wxEVT_SIZE, &TerminalTile::OnSize, this);

    // Apply initial theme colors (uses UiColors defaults if cfg has no palette).
    ApplyConfig(cfg);
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

std::vector<term::session::SessionId> TerminalTile::GetTabOrder() const
{
    std::vector<term::session::SessionId> ids;
    ids.reserve(tabs_.size());
    for (const auto& t : tabs_)
        ids.push_back(t.sessionId);
    return ids;
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

    if (activeTabIdx_ >= 0 && activeTabIdx_ < (int)tabs_.size()) {
        tabs_[activeTabIdx_].panel->SetBroadcastCursorState(broadcastModeActive_, false);
        tabs_[activeTabIdx_].panel->Hide();
    }

    activeTabIdx_ = index;
    tabs_[index].hasUnreadOutput = false;

    auto* panel = tabs_[index].panel;
    // Size the panel to fill the content area before showing to prevent flicker.
    const wxSize contentSz = contentArea_->GetClientSize();
    panel->SetSize(0, 0, contentSz.x, contentSz.y);
    panel->Show();
    panel->SetBroadcastCursorState(broadcastModeActive_, tabs_[index].inBroadcast);

    // Sync header colour with the newly active tab's broadcast state.
    inBroadcast_ = tabs_[index].inBroadcast;
    UpdateTitleBarColor();  // calls titleBar_->Refresh(), which repaints TabStrip as child

    contentArea_->Refresh();
}

void TerminalTile::OnSize(wxSizeEvent& evt)
{
    const wxSize sz = GetClientSize();
    titleBar_->SetSize(0, 0, sz.x, kTitleBarHeight);

    const int contentH = std::max(sz.y - kTitleBarHeight, 0);
    contentArea_->SetSize(0, kTitleBarHeight, sz.x, contentH);

    // Port forward panel overlays the top-right of the content area (like SearchBar).
    if (portFwdPanel_ && portFwdPanel_->IsShown()) {
        const wxSize best = portFwdPanel_->GetBestSize();
        portFwdPanel_->SetSize(sz.x - best.x, kTitleBarHeight, best.x, best.y);
        portFwdPanel_->Raise();
    }

    // Keep the active panel sized to fill the content area.
    if (activeTabIdx_ >= 0 && activeTabIdx_ < (int)tabs_.size()) {
        if (auto* p = tabs_[activeTabIdx_].panel)
            p->SetSize(0, 0, sz.x, contentH);
    }
    evt.Skip();
}

void TerminalTile::ApplyConfig(const AppConfig& cfg)
{
    const auto& u = cfg.uiColors;
    colActive_    = toWx(u.tileActive);
    colInactive_  = toWx(u.tileInactive);
    colBroadcast_ = toWx(u.tileBroadcast);

    if (tabStrip_) tabStrip_->SetUiColors(u);

    glyphBright_ = toWx(u.controlActive);
    UpdateTitleBarColor();
}

void TerminalTile::UpdateTitleBarColor()
{
    // isFocused_ only produces the blue "active" tint when broadcast mode is
    // off — otherwise the focused tile has no more input claim than any other
    // non-broadcasting tile and should appear inactive.
    const wxColour& c = inBroadcast_                           ? colBroadcast_
                        : (isFocused_ && !broadcastModeActive_) ? colActive_
                                                               : colInactive_;
    titleBar_->SetBackgroundColour(c);

    // Derive glyph colors from the current tile background so they always
    // contrast regardless of focus state or theme.  "off" glyphs are blended
    // 45 % toward the background — visually dimmed but never invisible.
    const wxColour glyphOn  = glyphBright_;
    const wxColour glyphOff = blendWx(glyphBright_, c, 0.45);
    if (wrapCtrl_)    wrapCtrl_->SetGlyphColours(glyphOn, glyphOff);
    if (altScrCtrl_)  altScrCtrl_->SetGlyphColours(glyphOn, glyphOff);
    if (x11Ctrl_)     x11Ctrl_->SetGlyphColours(glyphOn, glyphOff);
    if (portFwdCtrl_) portFwdCtrl_->SetGlyphColours(glyphOn, glyphOff);

    titleBar_->Refresh();
}

void TerminalTile::SetFocused(bool focused)
{
    isFocused_ = focused;
    UpdateTitleBarColor();
}

void TerminalTile::SetBroadcastState(bool modeActive, bool tileHasBroadcast)
{
    broadcastModeActive_ = modeActive;
    inBroadcast_         = tileHasBroadcast;
    const bool activeInGroup = activeTabIdx_ >= 0 && activeTabIdx_ < (int)tabs_.size()
                               && tabs_[activeTabIdx_].inBroadcast;
    if (auto* p = GetActivePanel()) p->SetBroadcastCursorState(modeActive, activeInGroup);
    UpdateTitleBarColor();
}

void TerminalTile::SetTabBroadcast(term::session::SessionId id, bool inBroadcast)
{
    for (int i = 0; i < (int)tabs_.size(); ++i) {
        if (tabs_[i].sessionId == id) {
            tabs_[i].inBroadcast = inBroadcast;
            if (i == activeTabIdx_)
                if (auto* p = GetActivePanel()) p->SetBroadcastCursorState(broadcastModeActive_, inBroadcast);
            tabStrip_->Refresh();  // TabStrip queries tabs_ via BroadcastQueryCallback on next paint
            return;
        }
    }
}

void TerminalTile::SetTabSupportsFileTransfer(term::session::SessionId id, bool supports)
{
    for (auto& tab : tabs_)
        if (tab.sessionId == id) { tab.supportsFileTransfer = supports; return; }
}

void TerminalTile::SetTabUnread(term::session::SessionId id, bool hasUnread)
{
    for (int i = 0; i < (int)tabs_.size(); ++i) {
        if (tabs_[i].sessionId == id) {
            if (tabs_[i].hasUnreadOutput == hasUnread) return;
            tabs_[i].hasUnreadOutput = hasUnread;
            tabStrip_->Refresh();
            return;
        }
    }
}

void TerminalTile::SetStatusProvider(
    std::function<term::session::SessionStatus(term::session::SessionId)> provider)
{
    statusProvider_ = std::move(provider);
}

void TerminalTile::SetWrapMode(bool wrap)
{
    if (wrapCtrl_) wrapCtrl_->SetWrapActive(wrap);
}

void TerminalTile::SetAltScrActive(bool active)
{
    if (altScrCtrl_) altScrCtrl_->SetAltScrActive(active);
}

void TerminalTile::SetX11Active(bool active)
{
    if (x11Ctrl_) x11Ctrl_->SetX11Active(active);
}

void TerminalTile::ShowX11Control(bool visible, bool active)
{
    if (!x11Ctrl_) return;
    x11Ctrl_->SetX11Active(visible && active);
    if (x11Ctrl_->IsShown() == visible) return;
    x11Ctrl_->Show(visible);
    titleBar_->Layout();
}

void TerminalTile::ShowPortForwardControl(bool visible)
{
    if (!portFwdCtrl_) return;
    if (portFwdCtrl_->IsShown() == visible) return;
    portFwdCtrl_->Show(visible);
    if (!visible && portFwdPanel_ && portFwdPanel_->IsShown()) {
        portFwdPanel_->Hide();
        SendSizeEvent();
    }
    titleBar_->Layout();
}

void TerminalTile::SetPortForwardCallbacks(
    std::function<term::transport::PortForwardId(term::transport::PortForwardDesc)> onAdd,
    std::function<void(term::transport::PortForwardId)> onRemove,
    std::function<void(term::transport::PortForwardId)> onSaveToProfile)
{
    if (portFwdPanel_)
        portFwdPanel_->SetCallbacks(std::move(onAdd), std::move(onRemove),
                                    std::move(onSaveToProfile));
}

void TerminalTile::SetPortForwardStatus(
    std::vector<term::transport::PortForwardStatus> status,
    std::vector<term::transport::PortForwardDesc>   descs)
{
    if (portFwdCtrl_) portFwdCtrl_->SetStatus(status);
    if (portFwdPanel_) portFwdPanel_->UpdateStatus(std::move(status), std::move(descs));
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
    // Read the capability off the active tab at menu-build time rather than
    // caching a copy, so the item can never be stale.
    const term::session::SessionId activeId = GetActiveSessionId();
    const auto activeTab = std::find_if(
        tabs_.begin(), tabs_.end(),
        [activeId](const TabEntry& e) { return e.sessionId == activeId; });

    auto* explorerItem = menu.Append(wxID_ANY, "File Explorer...");
    explorerItem->Enable(activeTab != tabs_.end() && activeTab->supportsFileTransfer);
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        EmitTileAction(TileAction::OpenFileExplorer, GetActiveSessionId());
    }, explorerItem->GetId());

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
