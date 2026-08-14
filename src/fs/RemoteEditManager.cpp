#include "fs/RemoteEditManager.h"
#include "fs/EditWorkspace.h"
#include <filesystem>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <optional>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace term::fs {

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
std::string CreateWorkingCopyDir(const WorkingCopyPath& layout,
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

RemoteEditManager::RemoteEditManager(Dispatcher     dispatch,
                                     LaunchEditorFn launchEditor)
    : dispatch_(std::move(dispatch))
    , launchEditor_(std::move(launchEditor))
    , guard_(dispatch_)
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

void RemoteEditManager::OpenRemoteFile(EditEndpoint       endpoint,
                                       const std::string& remotePath,
                                       const std::string& editorCommand,
                                       std::function<void(bool, std::string)> onReady)
{
    // Checked once, here, so no session can exist without somewhere to save to
    // and no upload has to re-ask.
    if (!endpoint.Valid()) {
        if (onReady) onReady(false, "Session has no remote filesystem");
        return;
    }

    // Stamped with this process so a later run can tell our working copies from
    // a live sibling instance's when it reclaims what a crash left behind.
    const auto layout =
        MakeWorkingCopyPath(endpoint.label, remotePath, ::getpid());
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

    // No mode: this fetches a working copy for an editor, not a faithful copy
    // of the file. The caller has not stat'd the remote and inventing a mode
    // from nothing would be worse than the adapter's default.
    auto ctx = guard_.For(this);
    endpoint.fs->Download(remotePath, localPath, std::nullopt, nullptr,
        [ctx, endpoint, remotePath, localPath, editorCommand,
         onReady = std::move(onReady)]
        (transport::FsError err) mutable {
            ctx.Post([endpoint = std::move(endpoint), remotePath, localPath,
                      editorCommand, err = std::move(err),
                      onReady = std::move(onReady)]
                     (RemoteEditManager& mgr) mutable {
                if (err.Failed()) {
                    // No session will be built to own this working copy, so
                    // nothing else would ever come back for the directory that
                    // was created to hold it.
                    RemoveWorkingCopy(localPath);
                    if (onReady) onReady(false, err.message);
                    return;
                }

                auto session = std::make_unique<RemoteEditSession>(
                    std::move(endpoint), remotePath, localPath, mgr.dispatch_);
                // Safe to capture the manager: a session's callbacks are
                // retired by Stop(), which runs before it leaves sessions_ and
                // before the manager itself is taken apart.
                session->SetOnSaved(
                    [&mgr](transport::IRemoteFileSystem* fs,
                           const std::string& path) {
                        if (mgr.onFileSaved_) mgr.onFileSaved_(fs, path);
                    });
                session->SetOnSaveFailed(
                    [&mgr](const SaveFailure& failure) {
                        if (mgr.onFileSaveFailed_) mgr.onFileSaveFailed_(failure);
                    });
                session->Start();
                mgr.sessions_.push_back(std::move(session));

                if (mgr.launchEditor_) mgr.launchEditor_(editorCommand, localPath);

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

std::vector<ActiveEdit> RemoteEditManager::ListActiveEdits() const
{
    std::vector<ActiveEdit> edits;
    edits.reserve(sessions_.size());
    for (const auto& s : sessions_)
        edits.push_back({s->GetRemotePath(), s->GetLocalPath(), s->GetHost()});
    return edits;
}

void RemoteEditManager::StopEditsForFilesystem(
    const transport::IRemoteFileSystem* fs)
{
    if (!fs) return;

    sessions_.erase(
        std::remove_if(sessions_.begin(), sessions_.end(),
            [fs](const std::unique_ptr<RemoteEditSession>& s) {
                if (s->GetFileSystem() != fs) return false;
                s->Stop();
                return true;
            }),
        sessions_.end());
}

} // namespace term::fs
