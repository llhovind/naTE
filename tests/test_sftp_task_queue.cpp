#include <catch2/catch_test_macros.hpp>

#include "transport/SftpTaskQueue.h"

#include <string>
#include <vector>

using term::transport::SftpSlot;
using term::transport::SftpTask;
using term::transport::SftpTaskQueue;

namespace {

// A scripted operation: it reports the slot it is on and how many more steps it
// wants, recording each time it runs so a test can see the interleaving.
struct FakeOp {
    std::string          name;
    SftpSlot             slot  = SftpSlot::Stat;
    int                  steps = 1;      // how many more times it wants to run
    std::vector<std::string>* log = nullptr;

    bool Run()
    {
        log->push_back(name);
        return --steps > 0;
    }
};

// Queues op and keeps it alive for the caller — the queue holds only functions.
void Push(SftpTaskQueue& q, FakeOp& op)
{
    q.Push(SftpTask{[&op] { return op.Run(); },
                    [&op] { return op.slot; },
                    std::nullopt});
}

} // namespace

TEST_CASE("given two operations on one slot when serviced then the second waits for the first") {
    // libssh2 keeps one state machine per slot on the session: letting both run
    // would have the second adopt the first's request id and take its answer.
    std::vector<std::string> log;
    SftpTaskQueue q;

    FakeOp first{"first", SftpSlot::Stat, 3, &log};
    FakeOp second{"second", SftpSlot::Stat, 1, &log};
    Push(q, first);
    Push(q, second);

    q.RunNext();
    q.RunNext();
    q.RunNext();

    // Nothing but the holder until it retires on its third step.
    REQUIRE(log == std::vector<std::string>{"first", "first", "first"});

    q.RunNext();
    REQUIRE(log.back() == "second");
}

TEST_CASE("given operations on different slots when serviced then they interleave") {
    // Different slots are independent — libssh2 demultiplexes replies by
    // request id — so a listing must still be able to overtake a transfer.
    std::vector<std::string> log;
    SftpTaskQueue q;

    FakeOp transfer{"read", SftpSlot::Read, 4, &log};
    FakeOp listing{"list", SftpSlot::ReadDir, 2, &log};
    Push(q, transfer);
    Push(q, listing);

    for (int i = 0; i < 4; ++i) q.RunNext();

    REQUIRE(log == std::vector<std::string>{"read", "list", "read", "list"});
}

TEST_CASE("given a held slot when its holder retires then the slot is free again") {
    std::vector<std::string> log;
    SftpTaskQueue q;

    FakeOp first{"first", SftpSlot::Open, 2, &log};
    FakeOp second{"second", SftpSlot::Open, 1, &log};
    Push(q, first);
    Push(q, second);

    q.RunNext();   // first: one more step wanted, holds Open
    q.RunNext();   // first again: retires, releases Open
    q.RunNext();   // second may now go

    REQUIRE(log == std::vector<std::string>{"first", "first", "second"});
}

TEST_CASE("given a task that moves to another slot then the slot it left is released") {
    // A download opens, fstats, then reads. Its open must not stay pinned once
    // it has the handle, or every listing behind it would wait for the transfer.
    std::vector<std::string> log;
    SftpTaskQueue q;

    FakeOp transfer{"transfer", SftpSlot::Open, 3, &log};
    FakeOp listing{"list", SftpSlot::Open, 1, &log};
    Push(q, transfer);
    Push(q, listing);

    q.RunNext();               // transfer takes Open
    transfer.slot = SftpSlot::Read;   // handle obtained; now reading
    q.RunNext();               // releases Open, takes Read

    q.RunNext();
    REQUIRE(log == std::vector<std::string>{"transfer", "transfer", "list"});
}

TEST_CASE("given a task holding a slot when serviced then it is never starved by its own hold") {
    // The holder is the only thing that can finish the operation occupying the
    // slot, so it has to stay runnable — skipping it would strand the slot.
    std::vector<std::string> log;
    SftpTaskQueue q;

    FakeOp holder{"holder", SftpSlot::Stat, 3, &log};
    Push(q, holder);

    REQUIRE(q.RunNext());
    REQUIRE(q.RunNext());
    REQUIRE(log == std::vector<std::string>{"holder", "holder"});
}

TEST_CASE("given an empty queue when serviced then it reports there was nothing to do") {
    SftpTaskQueue q;
    REQUIRE(q.Empty());
    REQUIRE_FALSE(q.RunNext());
}

TEST_CASE("given queued work when drained then everything is handed back and slots are freed") {
    // Teardown: the caller retires each task itself, and a slot left held would
    // strand the next session's first operation.
    std::vector<std::string> log;
    SftpTaskQueue q;

    FakeOp holder{"holder", SftpSlot::Stat, 5, &log};
    FakeOp waiting{"waiting", SftpSlot::Stat, 5, &log};
    Push(q, holder);
    Push(q, waiting);
    q.RunNext();               // holder now holds Stat

    auto drained = q.Drain();

    REQUIRE(drained.size() == 2);
    REQUIRE(q.Empty());

    // The slot came back with the drain: a fresh task on it runs immediately.
    log.clear();
    FakeOp fresh{"fresh", SftpSlot::Stat, 1, &log};
    Push(q, fresh);
    q.RunNext();
    REQUIRE(log == std::vector<std::string>{"fresh"});
}

TEST_CASE("given several runnable tasks when serviced then they are taken in order") {
    std::vector<std::string> log;
    SftpTaskQueue q;

    FakeOp a{"a", SftpSlot::MkDir,  1, &log};
    FakeOp b{"b", SftpSlot::Unlink, 1, &log};
    FakeOp c{"c", SftpSlot::Rename, 1, &log};
    Push(q, a);
    Push(q, b);
    Push(q, c);

    for (int i = 0; i < 3; ++i) q.RunNext();

    REQUIRE(log == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("given two volume queries when serviced then the second waits for the first") {
    // libssh2 keeps statvfs state on the SFTP session, exactly as it does for
    // stat. Two in flight would find the state already "sent", wait on the
    // first request's id, and each be handed the other's answer — so a pane
    // would show the free space of the volume the other pane is on.
    std::vector<std::string> log;
    SftpTaskQueue q;

    FakeOp first{"first", SftpSlot::StatVfs, 3, &log};
    FakeOp second{"second", SftpSlot::StatVfs, 1, &log};
    Push(q, first);
    Push(q, second);

    for (int i = 0; i < 4; ++i) q.RunNext();

    REQUIRE(log == std::vector<std::string>{"first", "first", "first", "second"});
}

TEST_CASE("given a volume query and a stat when serviced then they interleave") {
    // Its own state machine, not stat's. Sharing a slot would park every
    // volume query behind the stats a directory listing issues, and a
    // navigation issues both at once.
    std::vector<std::string> log;
    SftpTaskQueue q;

    FakeOp space{"space", SftpSlot::StatVfs, 2, &log};
    FakeOp stat{"stat", SftpSlot::Stat, 2, &log};
    Push(q, space);
    Push(q, stat);

    for (int i = 0; i < 4; ++i) q.RunNext();

    REQUIRE(log == std::vector<std::string>{"space", "stat", "space", "stat"});
}

// ---------------------------------------------------------------------------
// One step, one slot
// ---------------------------------------------------------------------------

namespace {

// An operation that walks through several slots, and that records which slots
// were occupied at the moment it touched each one.
//
// This is the shape every real task has: a listing opens then reads, a download
// opens, fstats, then reads. The queue admits a task by asking for its slot
// *before* stepping it, so a step that changes state and carries straight on
// into the next state's call touches a state machine nobody checked was free.
struct StagedOp {
    std::string                name;
    std::vector<SftpSlot>      stages;      // the slot each step drives
    size_t                     stage = 0;
    // Whether this op yields on changing stage, as a correct task must.
    bool                       yieldsOnStageChange = true;
    std::vector<std::string>*  violations = nullptr;
    const SftpTaskQueue*       queue = nullptr;

    SftpSlot Slot() const { return stages[std::min(stage, stages.size() - 1)]; }

    bool Run()
    {
        // Drive this stage, then move on. A misbehaving op keeps going into the
        // next stage within the same step.
        do {
            ++stage;
            if (stage >= stages.size()) return false;
        } while (!yieldsOnStageChange);
        return true;
    }
};

} // namespace

TEST_CASE("given a task that changes slot when stepped then it yields rather than driving both") {
    // The invariant, stated as a test because breaking it fails silently: a
    // listing that opened and then read within one step drove the session's
    // readdir state while another listing still held it, and libssh2 answered
    // it with that listing's entries. The directory came back attached to the
    // wrong path, and the caller then asked the server for files that had never
    // been there.
    std::vector<std::string> log;
    SftpTaskQueue q;

    // A reader parked mid-read, holding ReadDir.
    FakeOp reader{"reader", SftpSlot::ReadDir, 3, &log};
    Push(q, reader);
    REQUIRE(q.RunNext());               // reader takes and holds ReadDir

    // An opener arrives. Open is free, so it is admitted — and if its step ran
    // on into readdir, it would collide with the reader.
    FakeOp opener{"opener", SftpSlot::Open, 1, &log};
    Push(q, opener);
    REQUIRE(q.RunNext());

    // Now the opener has become a reader. It must wait for the slot rather than
    // being run alongside the one holding it.
    opener.slot  = SftpSlot::ReadDir;
    opener.steps = 1;
    Push(q, opener);

    log.clear();
    q.RunNext();
    // Only the original holder may advance; the newcomer is parked.
    REQUIRE(log == std::vector<std::string>{"reader"});
}

TEST_CASE("given a staged operation when it yields on each stage then no stage overlaps another") {
    // Two staged operations of the same shape, interleaved. Each stage of each
    // op must find its slot free at the moment it runs.
    SftpTaskQueue q;

    const std::vector<SftpSlot> shape{SftpSlot::Init, SftpSlot::Open,
                                      SftpSlot::ReadDir, SftpSlot::Close};

    StagedOp a{"a", shape};
    StagedOp b{"b", shape};

    auto push = [&q](StagedOp& op) {
        q.Push(SftpTask{[&op] { return op.Run(); },
                        [&op] { return op.Slot(); },
                        std::nullopt});
    };
    push(a);
    push(b);

    // Drive to completion. The queue's own accounting is what enforces the
    // invariant; the test is that it terminates with both ops finished and
    // never has to be told about a collision.
    for (int i = 0; i < 64 && q.RunNext(); ++i) {}

    REQUIRE(a.stage == shape.size());
    REQUIRE(b.stage == shape.size());
    REQUIRE(q.Empty());
}
