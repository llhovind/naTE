#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>

namespace term::transport {

// Which of libssh2's operation state machines a task's current step drives.
//
// libssh2 keeps that state on the SFTP *session*, not per call: one
// `stat_state`, `stat_packet` and `stat_request_id` for the whole connection,
// and likewise for every other operation. A second stat issued while the first
// is awaiting its reply finds the state already "sent", never builds a packet
// of its own, and waits on the first request's id — so it is handed the first
// stat's answer and reports it as its own. Two listings, two stats or two
// transfers overlapping trade results, silently and plausibly: a link to a file
// comes back wearing the attributes of the link to a directory beside it.
//
// Slots make that impossible. Different slots stay free to interleave, which is
// safe because libssh2 demultiplexes replies by request id and queues the ones
// it is not waiting for — so a listing can still overtake a transfer mid-flight,
// which is the whole reason the queue is round-robin rather than serial.
enum class SftpSlot : size_t {
    Init,     // libssh2_sftp_init
    Open,     // open, opendir
    ReadDir,  // readdir
    Read,     // file reads
    Write,    // file writes
    Stat,     // stat, lstat, setstat
    FStat,    // fstat on an open handle
    Unlink,
    Rename,
    MkDir,
    RmDir,
    Symlink,  // symlink, readlink, realpath — one state machine for all three
    // Closing a handle. Unlike the slots above, libssh2 keeps close state on
    // the handle rather than the session, so two closes could safely overlap.
    // They share a slot anyway because a waiting task has to name one, and a
    // close is a single round-trip — serialising them costs nothing worth the
    // extra slot. What it must never be is folded into the slot the task was
    // already using: a download's close is not a read, and parking it on
    // Slot::Read would block every other transfer behind a finished one.
    Close,
    Count,
};

// One queued operation: how to advance it, and which slot the step it is about
// to run needs. The slot is a query rather than a value because a task moves
// between them as it progresses — a download opens, fstats, then reads.
struct SftpTask {
    // Advances the operation by one non-blocking step. Returns true when there
    // is more to do, which also means "a reply may be outstanding on my slot".
    std::function<bool()>     step;
    std::function<SftpSlot()> slot;
    // The slot this task holds while it waits. Set when a step reports more
    // work, released when it next runs; until then no other task may take it.
    std::optional<SftpSlot>   held;
};

// The service's work queue, and the one place that knows an SFTP session can
// only carry one operation per slot at a time.
//
// Separated from the service that fills it because this is a scheduling policy,
// not an I/O concern: it has no libssh2 in it, and the invariant it enforces is
// the kind that fails by quietly returning the wrong answer rather than by
// crashing, so it has to be testable on its own.
//
// Thread-safe. Producers push from any thread; exactly one consumer thread
// calls RunNext. Steps run *outside* the lock — they invoke user callbacks that
// may queue further work, and holding the lock across that would deadlock.
class SftpTaskQueue {
public:
    void Push(SftpTask task)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        queue_.push_back(std::move(task));
    }

    // Advances the first task whose slot is available. Returns false when there
    // is nothing to run — either the queue is empty, or everything in it is
    // waiting on a slot another task is holding.
    bool RunNext()
    {
        SftpTask task;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            const auto it = std::find_if(queue_.begin(), queue_.end(),
                                         [this](const SftpTask& t) { return Runnable(t); });
            if (it == queue_.end()) return false;
            task = std::move(*it);
            queue_.erase(it);

            // Released before the step runs and re-taken against whatever slot
            // the task moves to: an operation that completed this step must not
            // leave its slot pinned by the task that has finished with it.
            if (task.held) Set(*task.held, false);
        }

        const bool again = task.step();

        std::lock_guard<std::mutex> lk(mutex_);
        if (!again) return true;   // retired; its slot stays free

        task.held = task.slot();
        Set(*task.held, true);
        // Back of the queue, not the front: a long transfer must yield to a
        // directory listing enqueued behind it rather than starving it. The
        // slot it holds is what makes that yielding safe.
        queue_.push_back(std::move(task));
        return true;
    }

    // Empties the queue and frees every slot, handing the tasks back so the
    // caller can retire each one. Retiring is the caller's business: only it
    // knows what a task owes its callback.
    std::deque<SftpTask> Drain()
    {
        std::deque<SftpTask> taken;
        std::lock_guard<std::mutex> lk(mutex_);
        taken.swap(queue_);
        held_.fill(false);   // nothing is left to finish what it started
        return taken;
    }

    bool Empty() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return queue_.empty();
    }

private:
    // A task holding a slot is always runnable: it is the only one that can
    // finish the operation occupying that slot, so skipping it would leave the
    // slot held forever.
    bool Runnable(const SftpTask& task) const
    {
        if (task.held) return true;
        return !held_[static_cast<size_t>(task.slot())];
    }

    void Set(SftpSlot slot, bool value)
    {
        held_[static_cast<size_t>(slot)] = value;
    }

    mutable std::mutex   mutex_;
    std::deque<SftpTask> queue_;
    std::array<bool, static_cast<size_t>(SftpSlot::Count)> held_{};
};

} // namespace term::transport
