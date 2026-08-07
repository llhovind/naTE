#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace term::transport {

// ---------------------------------------------------------------------------
// Typed errors
// ---------------------------------------------------------------------------

// Failure categories a caller can branch on without parsing message text.
// Anything the adapter cannot classify becomes Protocol with the underlying
// message preserved — an unrecognised failure must never be silently mapped
// onto a specific code a caller might act on.
enum class FsErrorCode {
    None,
    NotConnected,      // session is gone or shutting down
    Unsupported,       // remote has no working SFTP subsystem
    NoSuchFile,
    PermissionDenied,
    NotADirectory,
    AlreadyExists,
    DirectoryNotEmpty,
    LocalIoError,      // failure on the client side, not the server
    Cancelled,
    Protocol,
};

struct FsError {
    FsErrorCode code = FsErrorCode::None;
    std::string message;

    bool Ok()     const noexcept { return code == FsErrorCode::None; }
    bool Failed() const noexcept { return code != FsErrorCode::None; }

    static FsError Success() { return {}; }
    static FsError Make(FsErrorCode c, std::string msg)
    {
        return FsError{c, std::move(msg)};
    }
};

// ---------------------------------------------------------------------------
// File metadata
// ---------------------------------------------------------------------------

// One directory entry or stat result. Carries raw values only: formatting a
// mode for display or a size for humans is the caller's business, so there is
// no derived state here to fall out of sync.
struct FileInfo {
    // Leaf name as the server sent it. These are bytes, not necessarily valid
    // UTF-8 — anything building a path must use this verbatim, and anything
    // displaying it must decode defensively.
    std::string name;

    uint64_t size  = 0;
    uint32_t mode  = 0;   // POSIX mode bits; see fs/FileMode.h
    uint32_t uid   = 0;
    uint32_t gid   = 0;
    int64_t  mtime = 0;   // Unix seconds; 0 when the server omitted it

    // Owner and group *names*, parsed from the server's `ls -l` style long
    // entry. SFTP attributes carry numeric ids only, so these are empty when
    // the server does not supply a long entry.
    std::string owner;
    std::string group;

    bool isDir     = false;
    bool isSymlink = false;

    // Target of a symlink, populated only by ReadLink. Directory listings do
    // not resolve links — that would cost a round trip per entry.
    std::string linkTarget;
};

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------

// Identifies an in-flight transfer for cancellation. Zero is never a valid
// handle, so a default-constructed one is safely inert.
using TransferHandle = uint64_t;
inline constexpr TransferHandle kInvalidTransferHandle = 0;

// Reports bytes moved so far. totalBytes is 0 when the size is not yet known
// (before the remote stat completes) — callers must treat it as indeterminate
// rather than dividing by it.
using ProgressCallback = std::function<void(uint64_t transferredBytes,
                                            uint64_t totalBytes)>;

using DoneCallback = std::function<void(FsError)>;
using ListCallback = std::function<void(std::vector<FileInfo>, FsError)>;
using PathCallback = std::function<void(std::string, FsError)>;
using StatCallback = std::function<void(FileInfo, FsError)>;

// ---------------------------------------------------------------------------
// The port
// ---------------------------------------------------------------------------

// A remote filesystem, decoupled from how it is reached. SFTP over an existing
// SSH session is the only adapter today; a local-filesystem adapter and a test
// fake satisfy the same contract.
//
// Threading contract, identical for every method:
//   - Calls are non-blocking. They enqueue work and return immediately, and
//     are safe to make from the UI thread.
//   - Callbacks fire on the adapter's worker thread, NOT the caller's. UI code
//     must marshal via wxTheApp->CallAfter(). This is deliberate: the port has
//     no way to know what event loop, if any, the caller lives on.
//   - Every callback is invoked exactly once, including when the session dies
//     mid-operation (FsErrorCode::NotConnected) or the caller cancels
//     (FsErrorCode::Cancelled). Callers can rely on being told.
//
// Lifetime: an instance is owned by its transport and dies with it. Callers
// must not retain the pointer across a session teardown.
class IRemoteFileSystem {
public:
    virtual ~IRemoteFileSystem() = default;

    // Lists path's contents. "." and ".." are omitted. A directory that fails
    // partway yields the entries read so far alongside the error, so a caller
    // can show a partial result rather than discarding useful work.
    virtual void List(const std::string& path, ListCallback onDone) = 0;

    // Server-side canonicalisation: resolves ".", "..", "~" and symlinks.
    virtual void RealPath(const std::string& path, PathCallback onDone) = 0;

    // Metadata for a single path, following symlinks. The result's `name` is
    // left empty — the caller supplied the path and already knows the leaf.
    virtual void Stat(const std::string& path, StatCallback onDone) = 0;

    // Reads a symlink's target without following it. The result is returned
    // verbatim and may be relative to the link's own directory.
    virtual void ReadLink(const std::string& path, PathCallback onDone) = 0;

    // Creates a single directory. Parents must already exist; failing when
    // they do not is the server's POSIX behaviour and is reported as-is.
    virtual void MakeDirectory(const std::string& path, uint32_t mode,
                               DoneCallback onDone) = 0;

    // Removes one file or one empty directory. isDir selects rmdir over
    // unlink; it is the caller's job to know which, because guessing here
    // would mean an extra stat on every delete. Recursive removal is composed
    // from this by a higher layer, never hidden inside it.
    virtual void Remove(const std::string& path, bool isDir,
                        DoneCallback onDone) = 0;

    virtual void Rename(const std::string& from, const std::string& to,
                        DoneCallback onDone) = 0;

    // Applies POSIX permission bits. Ownership is intentionally absent: SFTP
    // can carry uid/gid, but without a name service on the client any UI for
    // it would be numeric guesswork.
    virtual void SetPermissions(const std::string& path, uint32_t mode,
                                DoneCallback onDone) = 0;

    // Copies remotePath to the exact local path localPath, truncating it.
    // onProgress may be empty. Returns a handle usable with Cancel().
    virtual TransferHandle Download(const std::string& remotePath,
                                    const std::string& localPath,
                                    ProgressCallback onProgress,
                                    DoneCallback onDone) = 0;

    // Copies localPath to the exact remote path remotePath, truncating it.
    virtual TransferHandle Upload(const std::string& localPath,
                                  const std::string& remotePath,
                                  ProgressCallback onProgress,
                                  DoneCallback onDone) = 0;

    // Requests cancellation. Safe to call from any thread, with an unknown or
    // already-finished handle, or more than once. The transfer's DoneCallback
    // still fires — with FsErrorCode::Cancelled if the request landed before
    // it completed. A partially written destination file is NOT removed;
    // the caller decides whether a partial result is worth keeping.
    virtual void Cancel(TransferHandle handle) = 0;
};

} // namespace term::transport
