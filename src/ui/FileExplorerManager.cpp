#include "ui/FileExplorerManager.h"

#include <wx/msgdlg.h>
#include <wx/window.h>

namespace ui {

FileExplorerManager::FileExplorerManager(
    term::session::SessionManager& sm,
    const AppConfig& cfg,
    std::function<void(term::session::SessionId, std::string)> onOpenInEditor)
    : sm_(sm)
    , cfg_(cfg)
    , onOpenInEditor_(std::move(onOpenInEditor))
{}

FileExplorerManager::~FileExplorerManager()
{
    // Clear each frame's closed-callback before it can fire: the frames are
    // owned by wx and may outlive this object during application teardown, and
    // a callback into a destroyed manager would be a use-after-free.
    for (auto& [id, frame] : frames_)
        if (frame) frame->SetOnClosed(nullptr);
}

void FileExplorerManager::OpenForSession(wxWindow* parent,
                                         term::session::SessionId id)
{
    if (!id) return;

    if (const auto it = frames_.find(id); it != frames_.end() && it->second) {
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
    frames_[id] = frame;
    frame->Show();
}

void FileExplorerManager::OnSessionDestroyed(term::session::SessionId id)
{
    const auto it = frames_.find(id);
    if (it == frames_.end() || !it->second) return;
    it->second->OnSessionEnded();
}

void FileExplorerManager::UpdateConfig(const AppConfig& cfg)
{
    cfg_ = cfg;
    for (auto& [id, frame] : frames_)
        if (frame) frame->ApplyConfig(cfg_);
}

} // namespace ui
