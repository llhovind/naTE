#include "ui/RemoteEditManager.h"
#include "ui/EditorLauncher.h"
#include "fs/EditTempPath.h"
#include <filesystem>
#include <wx/app.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <system_error>
#include <vector>

namespace ui {

namespace {

// Creates a private directory for one working copy and returns the path the
// file will take inside it.
//
// mkdtemp rather than a name this process invents: the uniqueness has to hold
// against the other naTE windows and the other naTE processes editing the same
// remote file, and only the filesystem can arbitrate that. It also creates the
// directory 0700, which is the right default for a local copy of a file whose
// remote permissions we deliberately do not replicate.
//
// Returns an empty path on failure, with the reason in err.
std::string CreateWorkingCopyDir(const term::fs::EditTempPath& layout,
                                 std::string& err)
{
    std::error_code ec;
    const auto parent = std::filesystem::path(layout.dirTemplate).parent_path();
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        err = "Failed to create temp directory: " + ec.message();
        return {};
    }

    // mkdtemp rewrites the template in place, so it needs a mutable buffer.
    std::vector<char> tmpl(layout.dirTemplate.begin(), layout.dirTemplate.end());
    tmpl.push_back('\0');
    if (!::mkdtemp(tmpl.data())) {
        err = "Failed to create temp directory: " +
              std::error_code(errno, std::generic_category()).message();
        return {};
    }

    return std::string(tmpl.data()) + "/" + layout.fileName;
}

} // namespace

RemoteEditManager::RemoteEditManager(term::session::SessionManager& sm)
    : sm_(sm)
{}

RemoteEditManager::~RemoteEditManager()
{
    // Retire before joining, so an in-flight continuation cannot land on a
    // half-torn-down manager. The guard would do this on destruction anyway,
    // but that is after this body has run.
    guard_.Retire();
    for (auto& s : sessions_)
        s->Stop();
    sessions_.clear();
}

void RemoteEditManager::OpenRemoteFile(term::session::SessionId  id,
                                        const std::string&        remotePath,
                                        const std::string&        editorCommand,
                                        std::function<void(bool, std::string)> onReady)
{
    const std::string hostname = sm_.GetRemoteDescription(id);
    const auto layout = term::fs::MakeEditTempPath(hostname, remotePath);
    if (layout.fileName.empty()) {
        if (onReady) onReady(false, "Not a file: " + remotePath);
        return;
    }

    // Every open gets its own working copy, even of a file already being
    // edited. Re-using one would mean serving whatever was downloaded the first
    // time — by now possibly nothing like what is on the remote — and the fresh
    // download that avoids that would truncate a file the first editor is still
    // holding, which the watch on it would upload straight back.
    std::string err;
    const std::string localPath = CreateWorkingCopyDir(layout, err);
    if (localPath.empty()) {
        if (onReady) onReady(false, err);
        return;
    }

    auto ctx = guard_.For(this);
    sm_.DownloadFile(id, remotePath, localPath,
        [ctx, id, remotePath, localPath, editorCommand, onReady = std::move(onReady)]
        (term::transport::FsError err) mutable {
            ctx.Post([id, remotePath, localPath, editorCommand,
                      err = std::move(err), onReady = std::move(onReady)]
                     (RemoteEditManager& mgr) mutable {
                if (err.Failed()) {
                    // No session will be built to own this working copy, so
                    // nothing else would ever come back for the directory that
                    // was created to hold it.
                    term::fs::RemoveWorkingCopy(localPath);
                    if (onReady) onReady(false, err.message);
                    return;
                }

                auto session = std::make_unique<RemoteEditSession>(
                    id, remotePath, localPath, mgr.sm_);
                // Safe to capture the manager: a session's callbacks are
                // retired by Stop(), which runs before it leaves sessions_ and
                // before the manager itself is taken apart.
                session->SetOnSaved(
                    [&mgr](term::session::SessionId sid, const std::string& path) {
                        if (mgr.onFileSaved_) mgr.onFileSaved_(sid, path);
                    });
                session->SetOnSaveFailed(
                    [&mgr](const SaveFailure& failure) {
                        if (mgr.onFileSaveFailed_) mgr.onFileSaveFailed_(failure);
                    });
                session->Start();
                mgr.sessions_.push_back(std::move(session));

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
