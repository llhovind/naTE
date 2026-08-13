#pragma once
#include "fs/TransferQueue.h"
#include "transport/IRemoteFileSystem.h"

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace testing {

// Runs posted work only when told to, so a test can place itself precisely
// between an operation completing and the queue reacting to it.
struct ManualExecutor {
    std::deque<std::function<void()>> queue;

    term::fs::Dispatcher AsDispatcher()
    {
        return [this](std::function<void()> fn) { queue.push_back(std::move(fn)); };
    }

    // Drains the queue, including work posted while draining. The iteration
    // cap turns a continuation cycle into a test failure rather than a hang.
    size_t RunAll(size_t maxIterations = 10000)
    {
        size_t ran = 0;
        while (!queue.empty() && ran < maxIterations) {
            auto fn = std::move(queue.front());
            queue.pop_front();
            fn();
            ++ran;
        }
        return ran;
    }

    bool Empty() const { return queue.empty(); }
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

    struct PendingTransfer {
        TransferHandle handle = 0;
        bool           isUpload = false;
        std::string    source;
        std::string    dest;
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
                 uint64_t size, const std::string& parent = {})
    {
        FileInfo info;
        info.name  = name;
        info.size  = size;
        info.mode  = 0100644;
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
    void List(const std::string& path, term::transport::ListCallback onDone) override
    {
        listCalls.push_back(path);
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
                            term::transport::ProgressCallback onProgress,
                            term::transport::DoneCallback onDone) override
    {
        const TransferHandle h = nextHandle_++;
        transfers.push_back({h, false, remotePath, localPath,
                             std::move(onProgress), std::move(onDone), false});
        if (completeTransfersInline) CompleteActive();
        return h;
    }

    TransferHandle Upload(const std::string& localPath,
                          const std::string& remotePath,
                          term::transport::ProgressCallback onProgress,
                          term::transport::DoneCallback onDone) override
    {
        const TransferHandle h = nextHandle_++;
        transfers.push_back({h, true, localPath, remotePath,
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
