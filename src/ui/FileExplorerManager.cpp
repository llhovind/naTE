#include "ui/FileExplorerManager.h"

#include <wx/msgdlg.h>
#include <wx/window.h>

namespace ui {

FileExplorerManager::FileExplorerManager(
    term::session::SessionManager& sm,
    const AppConfig& cfg,
    std::function<void(term::session::SessionId, std::string)> onOpenInEditor,
    std::function<void(int, int, int)> onGeometryChanged)
    : sm_(sm)
    , cfg_(cfg)
    , onOpenInEditor_(std::move(onOpenInEditor))
    , onGeometryChanged_(std::move(onGeometryChanged))
{}

FileExplorerManager::~FileExplorerManager()
{
    // Clear every callback back into this object before it can fire: the
    // frames are owned by wx and may outlive this manager during application
    // teardown, and either callback would then touch a destroyed manager.
    for (auto& [id, frame] : frames_) {
        if (!frame) continue;
        frame->SetOnClosed(nullptr);
        frame->SetOnGeometryChanged(nullptr);
    }
}

void FileExplorerManager::OpenForSession(wxWindow* parent,
                                         term::session::SessionId id,
                                         FileExplorerMode mode)
{
    if (!id) return;

    if (const auto it = frames_.find(id); it != frames_.end() && it->second) {
        it->second->SetMode(mode);
        it->second->Raise();
        it->second->SetFocus();
        return;
    }

    if (!sm_.SupportsFileTransfer(id)) {
        wxMessageBox("This session has no remote filesystem.\n\n"
                     "The file explorer is available for SSH sessions.",
                     "File Explorer", wxOK | wxICON_INFORMATION, parent);
        return;
    }

    auto* frame = new FileExplorerFrame(
        parent, id, sm_, cfg_, sm_.GetRemoteDescription(id),
        [this, id](std::string remotePath) {
            if (onOpenInEditor_) onOpenInEditor_(id, std::move(remotePath));
        });

    frame->SetOnClosed([this, id] { frames_.erase(id); });

    // Mode before the geometry callback: the PersistGeometry inside ApplyMode
    // then no-ops, so merely opening a window never rewrites config.ini.
    frame->SetMode(mode);
    frame->SetOnGeometryChanged(
        [this](int width, int height, int sash) {
            // Mirror it locally too, so a window opened later this session
            // starts the same shape without waiting for a config reload.
            cfg_.fileExplorerWidth  = width;
            cfg_.fileExplorerHeight = height;
            cfg_.fileExplorerSash   = sash;
            if (onGeometryChanged_) onGeometryChanged_(width, height, sash);
        });
    frames_[id] = frame;
    frame->Show();
}

void FileExplorerManager::OnSessionDestroyed(term::session::SessionId id)
{
    // Broadcast rather than look up: a window opened for one session may have
    // either of its panes pointed at another, so every frame has to decide for
    // itself whether this one mattered to it.
    for (auto& [openedFor, frame] : frames_)
        if (frame) frame->OnSessionEnded(id);
}

void FileExplorerManager::UpdateConfig(const AppConfig& cfg)
{
    cfg_ = cfg;
    for (auto& [id, frame] : frames_)
        if (frame) frame->ApplyConfig(cfg_);
}

} // namespace ui
