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
