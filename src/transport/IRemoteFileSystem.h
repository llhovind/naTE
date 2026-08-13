#pragma once
#include <cstdint>
#include <functional>
#include <optional>
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
    // A name the caller supplied could not be used — empty, containing a
    // separator, or one of the directory entries every directory already has.
    // Rejected before anything reaches the server, so it is the one code no
    // adapter ever produces.
    InvalidName,
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

// Selects the bits a file creation may carry: the nine permission bits plus
// setuid/setgid/sticky. Adapters apply it to any mode they are handed, so a
// caller passing a whole st_mode cannot smuggle file-type bits into an open().
inline constexpr uint32_t kModeBitsMask = 07777;

// What a file an adapter creates gets when the caller does not know the
// source's mode. rw-r--r-- is what a shell redirect produces under the
// conventional 022 umask, so such a file looks like any other the user made.
inline constexpr uint32_t kDefaultFileMode = 0644;

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
// Threading contract, identical for every method. It is deliberately the
// weakest of what the adapters guarantee, because a caller has to be correct
// against any of them:
//   - A callback may fire on ANY thread, including the caller's own before the
//     call has returned. An adapter reaching a remote host defers to a worker
//     thread; one reading a local disk answers inline. Callers must therefore
//     marshal — UI code via wxTheApp->CallAfter() — and must never assume a
//     callback is deferred. In particular, do not rely on a method returning
//     before its callback runs: assign a returned TransferHandle before it can
//     be observed, or the completion path may see a stale one.
//   - A call may block for as long as the underlying medium takes. Adapters
//     that reach the network do not block; the local-disk adapter does, and on
//     an unresponsive mount that stall lands on the calling thread.
//   - Every callback is invoked exactly once, including when the session dies
//     mid-operation (FsErrorCode::NotConnected) or the caller cancels
//     (FsErrorCode::Cancelled). Callers can rely on being told.
//
// Adapters are free to be stronger — SftpService is fully non-blocking and
// always defers — but no caller may depend on it.
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

    // Creates a symbolic link at linkPath pointing at target.
    //
    // The arguments are in that order for a reason worth stating: SFTP's
    // SSH_FXP_SYMLINK is a known interoperability wart. The draft specifies
    // (linkpath, target); OpenSSH implemented it reversed and documents the
    // deviation, and libssh2 matches OpenSSH. So this works against OpenSSH —
    // which is very nearly everything — and may produce a reversed link on a
    // server that follows the draft strictly.
    //
    // target is stored verbatim and is NOT resolved or validated. A link whose
    // target does not exist on this filesystem is a legal thing to create, and
    // whether that is wanted is the caller's business.
    //
    // Servers that do not implement it report FsErrorCode::Unsupported.
    virtual void CreateSymlink(const std::string& target,
                               const std::string& linkPath,
                               DoneCallback onDone) = 0;

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
    //
    // sourceMode carries the source file's permission bits so a copy can
    // reproduce them rather than landing under a default that has nothing to do
    // with the original — an uploaded script that arrives without its execute
    // bit is the case that matters. It applies only where the destination is
    // *created*: an existing file keeps the permissions it already has and the
    // destination's umask still narrows what is asked for, which is exactly
    // what cp(1) does. Pass nullopt when the mode is genuinely unknown; zero is
    // a real mode and is honoured as one.
    //
    // onProgress may be empty. Returns a handle usable with Cancel().
    virtual TransferHandle Download(const std::string& remotePath,
                                    const std::string& localPath,
                                    std::optional<uint32_t> sourceMode,
                                    ProgressCallback onProgress,
                                    DoneCallback onDone) = 0;

    // Copies localPath to the exact remote path remotePath, truncating it.
    // sourceMode is as for Download.
    virtual TransferHandle Upload(const std::string& localPath,
                                  const std::string& remotePath,
                                  std::optional<uint32_t> sourceMode,
                                  ProgressCallback onProgress,
                                  DoneCallback onDone) = 0;

    // True only for the adapter that *is* the machine naTE runs on.
    //
    // Download and Upload are defined relative to the local disk — they are
    // the primitives SFTP offers and cannot be expressed symmetrically. A
    // caller moving bytes between two arbitrary endpoints therefore has to
    // know which side, if either, is that disk: it decides whether one call
    // suffices or the transfer must be staged through local storage.
    virtual bool IsLocalDisk() const noexcept { return false; }

    // Requests cancellation. Safe to call from any thread, with an unknown or
    // already-finished handle, or more than once. The transfer's DoneCallback
    // still fires — with FsErrorCode::Cancelled if the request landed before
    // it completed. A partially written destination file is NOT removed;
    // the caller decides whether a partial result is worth keeping.
    virtual void Cancel(TransferHandle handle) = 0;
};

} // namespace term::transport
