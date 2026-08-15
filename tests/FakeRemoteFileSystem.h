#pragma once
#include "fs/TransferQueue.h"
#include "transport/IRemoteFileSystem.h"

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace testing {

// Runs posted work only when told to, so a test can place itself precisely
// between an operation completing and the queue reacting to it.
//
// Posting is thread-safe, because the dispatcher it stands in for is: a real
// adapter may complete on a worker thread, and the local one does. Running is
// not — draining happens on the test's own thread, which is the point.
struct ManualExecutor {
    mutable std::mutex                mutex;
    std::deque<std::function<void()>> queue;

    term::fs::Dispatcher AsDispatcher()
    {
        return [this](std::function<void()> fn) {
            std::lock_guard<std::mutex> lk(mutex);
            queue.push_back(std::move(fn));
        };
    }

    // Drains the queue, including work posted while draining. The iteration
    // cap turns a continuation cycle into a test failure rather than a hang.
    size_t RunAll(size_t maxIterations = 10000)
    {
        size_t ran = 0;
        for (; ran < maxIterations; ++ran) {
            std::function<void()> fn;
            {
                std::lock_guard<std::mutex> lk(mutex);
                if (queue.empty()) break;
                fn = std::move(queue.front());
                queue.pop_front();
            }
            // Run unlocked: the work may post more, from this thread or another.
            fn();
        }
        return ran;
    }

    bool Empty() const
    {
        std::lock_guard<std::mutex> lk(mutex);
        return queue.empty();
    }
};

// An in-memory IRemoteFileSystem.
//
// Metadata operations complete synchronously: TransferQueue already routes
// every callback through the dispatcher, so ManualExecutor supplies all the
// asynchrony a test needs. Transfers are the exception — they stay pending so
// a test can drive progress, failure and cancellation deliberately.
class FakeRemoteFileSystem : public term::transport::IRemoteFileSystem {
public:
    using FileInfo       = term::transport::FileInfo;
    using FsError        = term::transport::FsError;
    using FsErrorCode    = term::transport::FsErrorCode;
    using TransferHandle = term::transport::TransferHandle;

    // --- Seeded state --------------------------------------------------------
    // Paths that exist, for Stat. Directory listings, for List.
    std::map<std::string, FileInfo>              existing;
    std::map<std::string, std::vector<FileInfo>> listings;
    // Directories whose listing should fail, and with what.
    std::map<std::string, FsError>               listErrors;
    // Paths whose stat should fail with something other than "not there" — a
    // parent the user cannot traverse, a session that just died.
    std::map<std::string, FsError>               statErrors;

    // --- Recorded calls ------------------------------------------------------
    std::vector<std::string> mkdirCalls;
    std::vector<std::string> removeCalls;
    std::vector<std::string> renameCalls;
    std::vector<std::string> statCalls;
    std::vector<std::string> listCalls;
    std::vector<TransferHandle> cancelCalls;

    struct ChmodCall {
        std::string path;
        uint32_t    mode = 0;
    };
    std::vector<ChmodCall> chmodCalls;

    // Recorded as (target, linkPath) — the order the port declares.
    struct SymlinkCall {
        std::string target;
        std::string linkPath;
    };
    std::vector<SymlinkCall> symlinkCalls;
    // Stands in for a server whose SFTP implementation has no SSH_FXP_SYMLINK.
    bool symlinkUnsupported = false;

    // --- Volume space --------------------------------------------------------
    // Space by path, so a test can give one host two volumes and check that the
    // figure follows the path rather than the filesystem. A path with no entry
    // gets defaultSpace, which is roomy enough that tests uninterested in space
    // never have to seed any.
    std::map<std::string, term::transport::FsSpaceInfo> spaceByPath;
    term::transport::FsSpaceInfo defaultSpace{1ULL << 40, 1ULL << 40, false};
    // Stands in for a server without statvfs@openssh.com.
    bool spaceUnsupported = false;
    // Paths that report as missing rather than answering, so a test can model a
    // destination directory a walk has not created yet.
    std::set<std::string>    spaceMissing;
    std::vector<std::string> spaceCalls;

    struct PendingTransfer {
        TransferHandle handle = 0;
        bool           isUpload = false;
        std::string    source;
        std::string    dest;
        // What the caller asked the destination to be created with, so a test
        // can assert that a mode survived the trip rather than inspecting a
        // real file.
        std::optional<uint32_t> sourceMode;
        term::transport::ProgressCallback onProgress;
        term::transport::DoneCallback     onDone;
        bool           finished = false;
    };
    std::vector<PendingTransfer> transfers;

    // --- Test controls -------------------------------------------------------
    // When set, Download and Upload finish before they return, as an adapter
    // over a local disk does. The port permits this, so anything driving the
    // port has to stay correct when a completion arrives during the call that
    // started it.
    bool completeTransfersInline = false;

    PendingTransfer* Active()
    {
        for (auto& t : transfers)
            if (!t.finished) return &t;
        return nullptr;
    }

    size_t ActiveCount() const
    {
        size_t n = 0;
        for (const auto& t : transfers) if (!t.finished) ++n;
        return n;
    }

    void CompleteActive(FsError err = {})
    {
        PendingTransfer* t = Active();
        if (!t) return;
        t->finished = true;
        auto done = t->onDone;
        if (done) done(std::move(err));
    }

    void ProgressActive(uint64_t transferred, uint64_t total)
    {
        PendingTransfer* t = Active();
        if (!t || !t->onProgress) return;
        t->onProgress(transferred, total);
    }

    // Convenience for seeding: registers a file and, when parent is given,
    // adds it to that directory's listing.
    void AddFile(const std::string& path, const std::string& name,
                 uint64_t size, const std::string& parent = {},
                 uint32_t mode = 0100644)
    {
        FileInfo info;
        info.name  = name;
        info.size  = size;
        info.mode  = mode;
        existing[path] = info;
        if (!parent.empty()) listings[parent].push_back(info);
    }

    void AddDirectory(const std::string& path, const std::string& name,
                      const std::string& parent = {})
    {
        FileInfo info;
        info.name  = name;
        info.mode  = 0040755;
        info.isDir = true;
        existing[path] = info;
        listings[path];                    // exists, possibly empty
        if (!parent.empty()) listings[parent].push_back(info);
    }

    void AddSymlink(const std::string& path, const std::string& name,
                    const std::string& parent)
    {
        FileInfo info;
        info.name      = name;
        info.mode      = 0120777;
        info.isSymlink = true;
        existing[path] = info;
        listings[parent].push_back(info);
    }

    // --- IRemoteFileSystem ---------------------------------------------------
    // --- Deferred listings ---------------------------------------------------
    // Holds List callbacks open so a test can see how many are in flight at
    // once. libssh2 keeps readdir state per session, so two overlapping
    // listings trade answers — the only way to prove that cannot happen is to
    // stop answering and count.
    bool deferListings = false;

    struct PendingList {
        std::string                  path;
        term::transport::ListCallback onDone;
    };
    std::vector<PendingList> pendingLists;

    // Answers the oldest outstanding listing. Returns false when there is none.
    bool CompleteOldestListing()
    {
        if (pendingLists.empty()) return false;
        PendingList taken = std::move(pendingLists.front());
        pendingLists.erase(pendingLists.begin());

        if (const auto it = listErrors.find(taken.path); it != listErrors.end()) {
            taken.onDone({}, it->second);
            return true;
        }
        if (const auto it = listings.find(taken.path); it != listings.end()) {
            taken.onDone(it->second, FsError::Success());
            return true;
        }
        taken.onDone({}, FsError::Make(FsErrorCode::NoSuchFile,
                                       "no such directory: " + taken.path));
        return true;
    }

    void List(const std::string& path, term::transport::ListCallback onDone) override
    {
        listCalls.push_back(path);
        if (deferListings) {
            pendingLists.push_back({path, std::move(onDone)});
            return;
        }
        if (const auto it = listErrors.find(path); it != listErrors.end()) {
            onDone({}, it->second);
            return;
        }
        if (const auto it = listings.find(path); it != listings.end()) {
            onDone(it->second, FsError::Success());
            return;
        }
        onDone({}, FsError::Make(FsErrorCode::NoSuchFile, "no such directory: " + path));
    }

    void RealPath(const std::string& path, term::transport::PathCallback onDone) override
    {
        onDone(path, FsError::Success());
    }

    void Stat(const std::string& path, term::transport::StatCallback onDone) override
    {
        statCalls.push_back(path);
        if (const auto it = statErrors.find(path); it != statErrors.end()) {
            onDone({}, it->second);
            return;
        }
        if (const auto it = existing.find(path); it != existing.end()) {
            onDone(it->second, FsError::Success());
            return;
        }
        onDone({}, FsError::Make(FsErrorCode::NoSuchFile, "no such file: " + path));
    }

    void ReadLink(const std::string& path, term::transport::PathCallback onDone) override
    {
        onDone(path + "-target", FsError::Success());
    }

    void QuerySpace(const std::string& path,
                    term::transport::SpaceCallback onDone) override
    {
        spaceCalls.push_back(path);
        if (spaceUnsupported) {
            onDone({}, FsError::Make(FsErrorCode::Unsupported,
                                     "This server cannot report free space"));
            return;
        }
        if (spaceMissing.count(path)) {
            onDone({}, FsError::Make(FsErrorCode::NoSuchFile,
                                     "no such directory: " + path));
            return;
        }
        if (const auto it = spaceByPath.find(path); it != spaceByPath.end()) {
            onDone(it->second, FsError::Success());
            return;
        }
        onDone(defaultSpace, FsError::Success());
    }

    void MakeDirectory(const std::string& path, uint32_t,
                       term::transport::DoneCallback onDone) override
    {
        mkdirCalls.push_back(path);
        AddDirectory(path, path);
        onDone(FsError::Success());
    }

    void Remove(const std::string& path, bool,
                term::transport::DoneCallback onDone) override
    {
        removeCalls.push_back(path);
        existing.erase(path);
        onDone(FsError::Success());
    }

    void Rename(const std::string& from, const std::string& to,
                term::transport::DoneCallback onDone) override
    {
        renameCalls.push_back(from + " -> " + to);
        onDone(FsError::Success());
    }

    void CreateSymlink(const std::string& target, const std::string& linkPath,
                       term::transport::DoneCallback onDone) override
    {
        if (symlinkUnsupported) {
            onDone(FsError::Make(FsErrorCode::Unsupported,
                                 "This server cannot create links"));
            return;
        }
        symlinkCalls.push_back({target, linkPath});
        FileInfo info;
        info.name      = linkPath;
        info.isSymlink = true;
        existing[linkPath] = info;
        onDone(FsError::Success());
    }

    void SetPermissions(const std::string& path, uint32_t mode,
                        term::transport::DoneCallback onDone) override
    {
        chmodCalls.push_back({path, mode});
        onDone(FsError::Success());
    }

    TransferHandle Download(const std::string& remotePath,
                            const std::string& localPath,
                            std::optional<uint32_t> sourceMode,
                            term::transport::ProgressCallback onProgress,
                            term::transport::DoneCallback onDone) override
    {
        const TransferHandle h = nextHandle_++;
        transfers.push_back({h, false, remotePath, localPath, sourceMode,
                             std::move(onProgress), std::move(onDone), false});
        if (completeTransfersInline) CompleteActive();
        return h;
    }

    TransferHandle Upload(const std::string& localPath,
                          const std::string& remotePath,
                          std::optional<uint32_t> sourceMode,
                          term::transport::ProgressCallback onProgress,
                          term::transport::DoneCallback onDone) override
    {
        const TransferHandle h = nextHandle_++;
        transfers.push_back({h, true, localPath, remotePath, sourceMode,
                             std::move(onProgress), std::move(onDone), false});
        if (completeTransfersInline) CompleteActive();
        return h;
    }

    void Cancel(TransferHandle handle) override
    {
        cancelCalls.push_back(handle);
        for (auto& t : transfers) {
            if (t.handle != handle || t.finished) continue;
            t.finished = true;
            auto done = t.onDone;
            if (done)
                done(FsError::Make(FsErrorCode::Cancelled, "Transfer cancelled"));
        }
    }

private:
    TransferHandle nextHandle_ = 1;
};

} // namespace testing
