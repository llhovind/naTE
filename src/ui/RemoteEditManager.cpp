#include "ui/RemoteEditManager.h"
#include <filesystem>
#include <wx/app.h>
#include <wx/utils.h>
#include <algorithm>

namespace ui {

RemoteEditManager::RemoteEditManager(term::session::SessionManager& sm)
    : sm_(sm)
{}

RemoteEditManager::~RemoteEditManager()
{
    // Stop all sessions before destruction to join their watch threads.
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

    sm_.SftpDownloadFile(id, remotePath, localPath,
        [this, id, remotePath, localPath, editorCommand, onReady = std::move(onReady)]
        (bool ok, std::string err) mutable {
            wxTheApp->CallAfter(
                [this, id, remotePath, localPath, editorCommand,
                 ok, err = std::move(err), onReady = std::move(onReady)]() mutable {
                    if (!ok) {
                        if (onReady) onReady(false, err);
                        return;
                    }

                    auto session = std::make_unique<RemoteEditSession>(
                        id, remotePath, localPath, sm_);
                    session->Start();
                    sessions_.push_back(std::move(session));

                    // Resolve command: use editorCommand if non-empty, else $EDITOR.
                    std::string cmd = editorCommand;
                    if (cmd.empty()) {
                        const char* envEditor = std::getenv("EDITOR");
                        cmd = envEditor ? envEditor : "xterm -e vi";
                    }

                    wxExecute(wxString::FromUTF8(cmd + " " + localPath), wxEXEC_ASYNC);

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
