#include <catch2/catch_test_macros.hpp>

#include "fs/DirModel.h"

using namespace term::fs;
using term::transport::FileInfo;
using term::transport::FsError;
using term::transport::FsErrorCode;

namespace {

FileInfo file(std::string name, uint64_t size = 0, int64_t mtime = 0,
              std::string owner = "root")
{
    FileInfo f;
    f.name  = std::move(name);
    f.size  = size;
    f.mtime = mtime;
    f.owner = std::move(owner);
    f.mode  = 0100644;
    return f;
}

FileInfo dir(std::string name, int64_t mtime = 0)
{
    FileInfo d;
    d.name  = std::move(name);
    d.mtime = mtime;
    d.mode  = 0040755;
    d.isDir = true;
    return d;
}

FileInfo link(std::string name, uint64_t size = 0)
{
    FileInfo l;
    l.name      = std::move(name);
    l.size      = size;
    l.mode      = 0120777;
    l.isSymlink = true;
    return l;
}

// Collects the visible names in order, which is what every ordering assertion
// below is really about.
std::vector<std::string> names(const DirModel& m)
{
    std::vector<std::string> out;
    for (size_t i = 0; i < m.VisibleCount(); ++i) out.push_back(m.At(i).name);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Glob matching
// ---------------------------------------------------------------------------

TEST_CASE("given a glob when matched then wildcards span any run of characters") {
    REQUIRE(GlobMatch("*.conf", "nginx.conf"));
    REQUIRE(GlobMatch("*.conf", ".conf"));
    REQUIRE_FALSE(GlobMatch("*.conf", "nginx.confx"));
    REQUIRE(GlobMatch("nginx.*", "nginx.conf"));
    REQUIRE(GlobMatch("*", "anything"));
    REQUIRE(GlobMatch("*", ""));
    REQUIRE(GlobMatch("a*b*c", "axxbyyc"));
    REQUIRE_FALSE(GlobMatch("a*b*c", "axxbyy"));
}

TEST_CASE("given a question mark glob when matched then exactly one character is consumed") {
    REQUIRE(GlobMatch("log.?", "log.1"));
    REQUIRE_FALSE(GlobMatch("log.?", "log.10"));
    REQUIRE_FALSE(GlobMatch("log.?", "log."));
}

TEST_CASE("given a glob differing only in case when matched then it still matches") {
    REQUIRE(GlobMatch("*.CONF", "nginx.conf"));
    REQUIRE(GlobMatch("README*", "readme.md"));
}

TEST_CASE("given a repeated wildcard pattern when matched then it terminates") {
    // Backtracking is iterative precisely so a pattern like this cannot blow
    // the stack on an attacker-supplied filename.
    REQUIRE_FALSE(GlobMatch("*a*a*a*a*a*a*b",
                            std::string(200, 'a')));
}

TEST_CASE("given a pattern when tested for metacharacters then only star and question count") {
    REQUIRE(IsGlobPattern("*.conf"));
    REQUIRE(IsGlobPattern("log.?"));
    REQUIRE_FALSE(IsGlobPattern("nginx"));
    REQUIRE_FALSE(IsGlobPattern(""));
}

// ---------------------------------------------------------------------------
// Hidden files
// ---------------------------------------------------------------------------

TEST_CASE("given dotfiles when show hidden is off then they are excluded") {
    DirModel m;
    m.SetEntries({file("visible.txt"), file(".hidden"), dir(".git")});

    REQUIRE(m.VisibleCount() == 1);
    REQUIRE(m.TotalCount() == 3);
    REQUIRE(names(m) == std::vector<std::string>{"visible.txt"});
}

TEST_CASE("given dotfiles when show hidden is enabled then they appear") {
    DirModel m;
    m.SetEntries({file("visible.txt"), file(".hidden"), dir(".git")});
    m.SetShowHidden(true);

    REQUIRE(m.VisibleCount() == 3);
    REQUIRE(names(m) == std::vector<std::string>{".git", ".hidden", "visible.txt"});
}

// ---------------------------------------------------------------------------
// Ordering
// ---------------------------------------------------------------------------

TEST_CASE("given a mixed listing when sorted by name then directories lead") {
    DirModel m;
    m.SetEntries({file("alpha.txt"), dir("zulu"), file("beta.txt"), dir("apex")});

    REQUIRE(names(m) ==
            std::vector<std::string>{"apex", "zulu", "alpha.txt", "beta.txt"});
}

TEST_CASE("given a descending sort when applied then directories still lead") {
    // Reversing the sort should reorder the files, not bury the way back up
    // the tree beneath them.
    DirModel m;
    m.SetEntries({file("alpha.txt"), dir("zulu"), file("beta.txt"), dir("apex")});
    m.SetSort(SortKey::Name, SortOrder::Descending);

    REQUIRE(names(m) ==
            std::vector<std::string>{"zulu", "apex", "beta.txt", "alpha.txt"});
}

TEST_CASE("given directories first disabled when sorted then type is ignored") {
    DirModel m;
    m.SetEntries({file("alpha.txt"), dir("zulu"), file("beta.txt"), dir("apex")});
    m.SetDirectoriesFirst(false);

    REQUIRE(names(m) ==
            std::vector<std::string>{"alpha.txt", "apex", "beta.txt", "zulu"});
}

TEST_CASE("given names differing in case when sorted then comparison folds case") {
    DirModel m;
    m.SetEntries({file("Zebra"), file("apple"), file("Banana")});

    REQUIRE(names(m) == std::vector<std::string>{"apple", "Banana", "Zebra"});
}

TEST_CASE("given a size sort when sizes tie then name breaks the tie") {
    DirModel m;
    m.SetEntries({file("c.txt", 100), file("a.txt", 100), file("b.txt", 5)});
    m.SetSort(SortKey::Size, SortOrder::Ascending);

    REQUIRE(names(m) == std::vector<std::string>{"b.txt", "a.txt", "c.txt"});
}

TEST_CASE("given a modified sort when applied then oldest comes first ascending") {
    DirModel m;
    m.SetEntries({file("new.txt", 0, 300), file("old.txt", 0, 100),
                  file("mid.txt", 0, 200)});
    m.SetSort(SortKey::Modified, SortOrder::Ascending);

    REQUIRE(names(m) == std::vector<std::string>{"old.txt", "mid.txt", "new.txt"});
}

TEST_CASE("given an owner sort when owners tie then name breaks the tie") {
    DirModel m;
    m.SetEntries({file("b.txt", 0, 0, "root"), file("a.txt", 0, 0, "root"),
                  file("c.txt", 0, 0, "daemon")});
    m.SetSort(SortKey::Owner, SortOrder::Ascending);

    REQUIRE(names(m) == std::vector<std::string>{"c.txt", "a.txt", "b.txt"});
}

// ---------------------------------------------------------------------------
// Filtering
// ---------------------------------------------------------------------------

TEST_CASE("given a glob filter when applied then only matching files remain") {
    DirModel m;
    m.SetEntries({file("nginx.conf"), file("mime.types"), file("other.conf")});
    m.SetNameFilter("*.conf");

    REQUIRE(names(m) == std::vector<std::string>{"nginx.conf", "other.conf"});
}

TEST_CASE("given a plain text filter when applied then it matches as a substring") {
    DirModel m;
    m.SetEntries({file("access.log"), file("error.log"), file("nginx.conf")});
    m.SetNameFilter("LOG");

    REQUIRE(names(m) == std::vector<std::string>{"access.log", "error.log"});
}

TEST_CASE("given a name filter when applied then directories remain visible") {
    // A filter that hid directories would strand the user in a directory they
    // could no longer navigate out of.
    DirModel m;
    m.SetEntries({dir("conf.d"), file("nginx.conf"), file("mime.types")});
    m.SetNameFilter("*.conf");

    REQUIRE(names(m) == std::vector<std::string>{"conf.d", "nginx.conf"});
}

TEST_CASE("given a filter when cleared then all entries return") {
    DirModel m;
    m.SetEntries({file("a.conf"), file("b.txt")});
    m.SetNameFilter("*.conf");
    REQUIRE(m.VisibleCount() == 1);

    m.SetNameFilter("");
    REQUIRE(m.VisibleCount() == 2);
}

// ---------------------------------------------------------------------------
// Counts
// ---------------------------------------------------------------------------

TEST_CASE("given a filtered listing when counted then totals cover only visible rows") {
    DirModel m;
    m.SetEntries({dir("conf.d"), file("a.conf", 100), file("b.txt", 50),
                  file("c.conf", 25)});
    m.SetNameFilter("*.conf");

    REQUIRE(m.VisibleDirectoryCount() == 1);
    REQUIRE(m.VisibleFileCount() == 2);
    REQUIRE(m.VisibleByteTotal() == 125);
    REQUIRE(m.TotalCount() == 4);
}

// ---------------------------------------------------------------------------
// Partial failure
// ---------------------------------------------------------------------------

TEST_CASE("given a listing that failed with no entries then it is an error but not partial") {
    DirModel m;
    m.SetEntries({}, FsError::Make(FsErrorCode::PermissionDenied, "denied"));

    REQUIRE(m.HasError());
    REQUIRE_FALSE(m.IsPartial());
    REQUIRE(m.Error().code == FsErrorCode::PermissionDenied);
}

TEST_CASE("given a listing that failed after reading rows then it is partial and keeps them") {
    DirModel m;
    m.SetEntries({file("a.txt"), file("b.txt")},
                 FsError::Make(FsErrorCode::Protocol, "read error"));

    REQUIRE(m.HasError());
    REQUIRE(m.IsPartial());
    REQUIRE(m.VisibleCount() == 2);
}

TEST_CASE("given a model with an error when new entries arrive then the error is replaced") {
    DirModel m;
    m.SetEntries({}, FsError::Make(FsErrorCode::PermissionDenied, "denied"));
    m.SetEntries({file("a.txt")});

    REQUIRE_FALSE(m.HasError());
    REQUIRE(m.VisibleCount() == 1);
}

TEST_CASE("given a populated model when cleared then it is empty and error free") {
    DirModel m;
    m.SetEntries({file("a.txt")}, FsError::Make(FsErrorCode::Protocol, "x"));
    m.Clear();

    REQUIRE(m.VisibleCount() == 0);
    REQUIRE(m.TotalCount() == 0);
    REQUIRE_FALSE(m.HasError());
}

// ---------------------------------------------------------------------------
// Row lookup
// ---------------------------------------------------------------------------

TEST_CASE("given a re-sorted listing when a name is looked up then its new row is returned") {
    // The view uses this to keep the cursor on the same file across a refresh
    // that reorders the rows.
    DirModel m;
    m.SetEntries({file("a.txt", 10), file("b.txt", 30), file("c.txt", 20)});
    REQUIRE(m.IndexOfName("c.txt") == 2);

    m.SetSort(SortKey::Size, SortOrder::Descending);
    REQUIRE(m.IndexOfName("c.txt") == 1);
}

TEST_CASE("given a name hidden by the filter when looked up then the miss is reported") {
    DirModel m;
    m.SetEntries({file("a.conf"), file("b.txt")});
    m.SetNameFilter("*.conf");

    REQUIRE(m.IndexOfName("b.txt") == m.VisibleCount());
}

// ---------------------------------------------------------------------------
// Symbolic links
// ---------------------------------------------------------------------------

TEST_CASE("given a listing with links when nothing has been resolved then they are unresolved") {
    // A listing describes links, never their targets — so the model must not
    // pretend to know, and must be able to say which ones it needs answers for.
    DirModel m;
    m.SetEntries({dir("etc"), link("latest"), file("a.txt")});

    REQUIRE(m.LinkTargetAt(m.IndexOfName("latest")) == LinkTarget::Unresolved);
    REQUIRE(m.UnresolvedLinkNames() == std::vector<std::string>{"latest"});
}

TEST_CASE("given a link to a directory when resolved then it is ordered with the directories") {
    DirModel m;
    m.SetEntries({file("a.txt"), link("zlink"), dir("bin")});

    // Before the answer it can only be shown as what the listing said it was.
    REQUIRE(names(m) == std::vector<std::string>{"bin", "a.txt", "zlink"});

    m.ApplyLinkTargets({{"zlink", LinkTarget::Directory}});

    REQUIRE(names(m) == std::vector<std::string>{"bin", "zlink", "a.txt"});
    REQUIRE(m.IsDirectoryLike(m.IndexOfName("zlink")));
}

TEST_CASE("given resolved links when counted then a link to a directory counts as one") {
    // The status line describes the same two groups the rows are ordered into,
    // or it contradicts what the user is looking at.
    DirModel m;
    m.SetEntries({dir("bin"), link("zlink"), link("broken"), file("a.txt", 100)});
    m.ApplyLinkTargets({{"zlink", LinkTarget::Directory}, {"broken", LinkTarget::Broken}});

    REQUIRE(m.VisibleDirectoryCount() == 2);
    REQUIRE(m.VisibleFileCount() == 2);
}

TEST_CASE("given a link to a file or nowhere when resolved then it stays with the files") {
    DirModel m;
    m.SetEntries({dir("bin"), link("conf"), link("dangling")});
    m.ApplyLinkTargets({{"conf", LinkTarget::File}, {"dangling", LinkTarget::Broken}});

    REQUIRE(names(m) == std::vector<std::string>{"bin", "conf", "dangling"});
    REQUIRE_FALSE(m.IsDirectoryLike(m.IndexOfName("conf")));
    REQUIRE_FALSE(m.IsDirectoryLike(m.IndexOfName("dangling")));
    REQUIRE(m.LinkTargetAt(m.IndexOfName("dangling")) == LinkTarget::Broken);
}

TEST_CASE("given a descending sort when links are resolved then directories still lead") {
    DirModel m;
    m.SetEntries({file("a.txt"), link("zlink"), dir("bin")});
    m.ApplyLinkTargets({{"zlink", LinkTarget::Directory}});
    m.SetSort(SortKey::Name, SortOrder::Descending);

    REQUIRE(names(m) == std::vector<std::string>{"zlink", "bin", "a.txt"});
}

TEST_CASE("given a name filter when a link leads to a directory then it is exempt like one") {
    // A filter narrows what you are looking at; it must not close the ways out
    // of the directory, whichever kind of doorway they are.
    DirModel m;
    m.SetEntries({file("nginx.conf"), file("a.txt"), link("sites")});
    m.SetNameFilter("*.conf");
    REQUIRE(names(m) == std::vector<std::string>{"nginx.conf"});

    m.ApplyLinkTargets({{"sites", LinkTarget::Directory}});
    REQUIRE(names(m) == std::vector<std::string>{"sites", "nginx.conf"});
}

TEST_CASE("given resolutions for names that have gone when applied then they are ignored") {
    // Answers can outlive the listing that asked for them.
    DirModel m;
    m.SetEntries({dir("bin"), link("here")});
    m.ApplyLinkTargets({{"gone", LinkTarget::Directory}, {"here", LinkTarget::File}});

    REQUIRE(m.VisibleCount() == 2);
    REQUIRE(m.LinkTargetAt(m.IndexOfName("here")) == LinkTarget::File);
}

TEST_CASE("given resolved links when a new listing arrives then every link is unresolved again") {
    // What a link points at is exactly the thing that may have changed, so a
    // previous listing's answers cannot be carried over.
    DirModel m;
    m.SetEntries({link("latest")});
    m.ApplyLinkTargets({{"latest", LinkTarget::Directory}});
    REQUIRE(m.IsDirectoryLike(0));

    m.SetEntries({link("latest")});
    REQUIRE_FALSE(m.IsDirectoryLike(0));
    REQUIRE(m.UnresolvedLinkNames() == std::vector<std::string>{"latest"});
}

TEST_CASE("given a hidden link when unresolved names are collected then it is still included") {
    // Toggling hidden files must not cost a second round of lookups, so the
    // filter has no say in what gets resolved.
    DirModel m;
    m.SetEntries({link(".cache"), file("a.txt")});
    REQUIRE(m.VisibleCount() == 1);

    REQUIRE(m.UnresolvedLinkNames() == std::vector<std::string>{".cache"});
}

TEST_CASE("given an unanswerable link when it stays unresolved then it is not asked about twice") {
    // A lookup that failed for a reason other than absence leaves the link
    // unresolved — and a refresh is the only thing that should retry it.
    DirModel m;
    m.SetEntries({link("guarded")});
    m.ApplyLinkTargets({{"guarded", LinkTarget::Unresolved}});

    REQUIRE(m.UnresolvedLinkNames() == std::vector<std::string>{"guarded"});
    REQUIRE_FALSE(m.IsDirectoryLike(0));
}
