#include "ui/RemoteEditManager.h"
#include "ui/EditorLauncher.h"
#include <filesystem>
#include <wx/app.h>
#include <algorithm>

namespace ui {

RemoteEditManager::RemoteEditManager(term::session::SessionManager& sm)
    : sm_(sm)
    , alive_(std::make_shared<std::atomic<bool>>(true))
{}

RemoteEditManager::~RemoteEditManager()
{
    // Invalidate before joining so any in-flight CallAfter callbacks no-op.
    alive_->store(false, std::memory_order_release);
    for (auto& s : sessions_)
        s->Stop();
    sessions_.clear();
}

void RemoteEditManager::OpenRemoteFile(term::session::SessionId  id,
                                        const std::string&        remotePath,
                                        const std::string&        editorCommand,
                                        std::function<void(bool, std::string)> onReady)
{
    const std::string hostname  = sm_.GetRemoteDescription(id);
    const std::string localPath = RemoteEditSession::MakeTempPath(hostname, remotePath);

    std::error_code ec;
    const auto parentDir = std::filesystem::path(localPath).parent_path();
    std::filesystem::create_directories(parentDir, ec);
    if (ec) {
        if (onReady) onReady(false, "Failed to create temp directory: " + ec.message());
        return;
    }

    std::weak_ptr<std::atomic<bool>> weakAlive = alive_;
    sm_.DownloadFile(id, remotePath, localPath,
        [this, weakAlive, id, remotePath, localPath, editorCommand, onReady = std::move(onReady)]
        (term::transport::FsError err) mutable {
            wxTheApp->CallAfter(
                [this, weakAlive, id, remotePath, localPath, editorCommand,
                 err = std::move(err), onReady = std::move(onReady)]() mutable {
                    auto alive = weakAlive.lock();
                    if (!alive || !alive->load(std::memory_order_acquire))
                        return;

                    if (err.Failed()) {
                        if (onReady) onReady(false, err.message);
                        return;
                    }

                    auto session = std::make_unique<RemoteEditSession>(
                        id, remotePath, localPath, sm_);
                    session->Start();
                    sessions_.push_back(std::move(session));

                    LaunchEditor(editorCommand, localPath);

                    if (onReady) onReady(true, "");
                });
        });
}

void RemoteEditManager::StopSession(const std::string& localPath)
{
    auto it = std::find_if(sessions_.begin(), sessions_.end(),
        [&localPath](const std::unique_ptr<RemoteEditSession>& s) {
            return s->GetLocalPath() == localPath;
        });

    if (it == sessions_.end())
        return;

    (*it)->Stop();
    sessions_.erase(it);
}

void RemoteEditManager::OnSessionDestroyed(term::session::SessionId id)
{
    const bool any = std::any_of(sessions_.begin(), sessions_.end(),
        [id](const std::unique_ptr<RemoteEditSession>& s) {
            return s->GetSessionId() == id;
        });

    if (!any) return;

    sessions_.erase(
        std::remove_if(sessions_.begin(), sessions_.end(),
            [id](const std::unique_ptr<RemoteEditSession>& s) {
                if (s->GetSessionId() != id) return false;
                s->Stop();
                return true;
            }),
        sessions_.end());

}

} // namespace ui
