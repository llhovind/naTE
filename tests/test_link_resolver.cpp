#include <catch2/catch_test_macros.hpp>

#include "FakeRemoteFileSystem.h"
#include "fs/LinkResolver.h"

#include <algorithm>
#include <optional>

using namespace term::fs;
using term::transport::FsError;
using term::transport::FsErrorCode;
using testing::FakeRemoteFileSystem;
using testing::ManualExecutor;

namespace {

// Answers for a whole batch, indexed by name so an assertion does not depend on
// the order the lookups happened to finish in.
struct Answers {
    std::optional<std::vector<LinkResolution>> batch;

    LinkTarget Of(const std::string& name) const
    {
        if (!batch) return LinkTarget::Unresolved;
        for (const LinkResolution& r : *batch)
            if (r.name == name) return r.target;
        return LinkTarget::Unresolved;
    }

    size_t Count() const { return batch ? batch->size() : 0; }
};

// A directory holding one link to a directory, one to a file and one to
// nothing at all.
void SeedLinks(FakeRemoteFileSystem& fs)
{
    fs.AddDirectory("/srv", "srv");
    fs.AddDirectory("/srv/releases", "releases", "/srv");

    fs.AddSymlink("/srv/current", "current", "/srv");
    fs.existing["/srv/current"].isDir = true;      // stat follows the link

    fs.AddSymlink("/srv/config", "config", "/srv");
    fs.existing["/srv/config"].isDir = false;

    // Present in the listing, absent from stat: a dangling link.
    fs.AddSymlink("/srv/dangling", "dangling", "/srv");
    fs.existing.erase("/srv/dangling");
}

std::vector<std::string> ManyNames(size_t count)
{
    std::vector<std::string> names;
    for (size_t i = 0; i < count; ++i) names.push_back("link" + std::to_string(i));
    return names;
}

} // namespace

TEST_CASE("given links of each kind when resolved then each is reported for what it leads to") {
    FakeRemoteFileSystem fs;
    SeedLinks(fs);
    ManualExecutor exec;
    LinkResolver r(fs, exec.AsDispatcher());

    Answers answers;
    r.Resolve("/srv", {"current", "config", "dangling"},
              [&](std::vector<LinkResolution> batch) { answers.batch = std::move(batch); });
    exec.RunAll();

    REQUIRE(answers.Count() == 3);
    REQUIRE(answers.Of("current")  == LinkTarget::Directory);
    REQUIRE(answers.Of("config")   == LinkTarget::File);
    REQUIRE(answers.Of("dangling") == LinkTarget::Broken);
}

TEST_CASE("given a lookup that fails for a reason other than absence then the link stays unresolved") {
    // Only "there is nothing there" makes a link broken. A parent the user
    // cannot traverse says nothing about the link, and painting it as dead
    // would be a guess shown to the user as a fact.
    FakeRemoteFileSystem fs;
    SeedLinks(fs);
    fs.statErrors["/srv/current"] =
        FsError::Make(FsErrorCode::PermissionDenied, "denied");

    ManualExecutor exec;
    LinkResolver r(fs, exec.AsDispatcher());

    Answers answers;
    r.Resolve("/srv", {"current"},
              [&](std::vector<LinkResolution> batch) { answers.batch = std::move(batch); });
    exec.RunAll();

    REQUIRE(answers.Count() == 1);
    REQUIRE(answers.Of("current") == LinkTarget::Unresolved);
}

TEST_CASE("given more links than the concurrency limit when resolving then only that many are in flight") {
    // The lookups share the connection with the next directory listing, so the
    // number outstanding is what keeps navigation responsive.
    FakeRemoteFileSystem fs;
    fs.AddDirectory("/many", "many");
    ManualExecutor exec;
    LinkResolver r(fs, exec.AsDispatcher());

    const size_t total = kMaxConcurrentLinkLookups * 3;
    r.Resolve("/many", ManyNames(total), [](std::vector<LinkResolution>) {});

    REQUIRE(fs.statCalls.size() == kMaxConcurrentLinkLookups);
}

TEST_CASE("given a long batch when it is driven to completion then every name is answered") {
    FakeRemoteFileSystem fs;
    fs.AddDirectory("/many", "many");
    ManualExecutor exec;
    LinkResolver r(fs, exec.AsDispatcher());

    const size_t total = kMaxConcurrentLinkLookups * 3 + 1;
    Answers answers;
    r.Resolve("/many", ManyNames(total),
              [&](std::vector<LinkResolution> batch) { answers.batch = std::move(batch); });
    exec.RunAll();

    REQUIRE(fs.statCalls.size() == total);
    REQUIRE(answers.Count() == total);
}

TEST_CASE("given a batch in flight when a new one starts then only the newer one is delivered") {
    // The abandoned answers describe a listing that is no longer on screen.
    FakeRemoteFileSystem fs;
    SeedLinks(fs);
    ManualExecutor exec;
    LinkResolver r(fs, exec.AsDispatcher());

    bool firstDelivered = false;
    Answers second;
    r.Resolve("/srv", {"current"},
              [&](std::vector<LinkResolution>) { firstDelivered = true; });
    r.Resolve("/srv", {"config"},
              [&](std::vector<LinkResolution> batch) { second.batch = std::move(batch); });
    exec.RunAll();

    REQUIRE_FALSE(firstDelivered);
    REQUIRE(second.Count() == 1);
    REQUIRE(second.Of("config") == LinkTarget::File);
}

TEST_CASE("given a cancelled batch when work is drained then nothing further is issued") {
    FakeRemoteFileSystem fs;
    fs.AddDirectory("/many", "many");
    ManualExecutor exec;
    LinkResolver r(fs, exec.AsDispatcher());

    bool delivered = false;
    r.Resolve("/many", ManyNames(kMaxConcurrentLinkLookups * 4),
              [&](std::vector<LinkResolution>) { delivered = true; });
    const size_t issuedBeforeCancel = fs.statCalls.size();

    r.Cancel();
    exec.RunAll();

    REQUIRE(fs.statCalls.size() == issuedBeforeCancel);
    REQUIRE_FALSE(delivered);
}

TEST_CASE("given nothing to resolve when asked then the batch is delivered empty") {
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    LinkResolver r(fs, exec.AsDispatcher());

    Answers answers;
    r.Resolve("/srv", {},
              [&](std::vector<LinkResolution> batch) { answers.batch = std::move(batch); });

    REQUIRE(answers.batch.has_value());
    REQUIRE(answers.Count() == 0);
    REQUIRE(fs.statCalls.empty());
}

TEST_CASE("given a resolver destroyed with work in flight when it completes then nothing is touched") {
    // The transport still holds the callbacks; the guard is what makes that
    // safe, and this is the test that would crash if it were removed.
    FakeRemoteFileSystem fs;
    SeedLinks(fs);
    ManualExecutor exec;

    bool delivered = false;
    {
        LinkResolver r(fs, exec.AsDispatcher());
        r.Resolve("/srv", {"current", "config"},
                  [&](std::vector<LinkResolution>) { delivered = true; });
    }
    exec.RunAll();

    REQUIRE_FALSE(delivered);
}
