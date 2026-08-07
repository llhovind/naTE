#include <catch2/catch_test_macros.hpp>

#include "fs/FileMode.h"
#include "fs/RemotePath.h"

using namespace term::fs;

// ---------------------------------------------------------------------------
// RemotePath
// ---------------------------------------------------------------------------

TEST_CASE("given a directory and a leaf when joined then exactly one separator appears") {
    SECTION("directory without trailing slash") {
        REQUIRE(path::Join("/etc/nginx", "nginx.conf") == "/etc/nginx/nginx.conf");
    }
    SECTION("directory with trailing slash") {
        REQUIRE(path::Join("/etc/nginx/", "nginx.conf") == "/etc/nginx/nginx.conf");
    }
    SECTION("root directory") {
        REQUIRE(path::Join("/", "etc") == "/etc");
    }
    SECTION("relative directory") {
        REQUIRE(path::Join(".", "notes.txt") == "./notes.txt");
    }
}

TEST_CASE("given an absolute leaf when joined then it replaces the directory") {
    REQUIRE(path::Join("/home/user", "/etc/passwd") == "/etc/passwd");
}

TEST_CASE("given an empty operand when joined then the other side is returned unchanged") {
    REQUIRE(path::Join("", "file") == "file");
    REQUIRE(path::Join("/var/log", "") == "/var/log");
}

TEST_CASE("given a path when taking the leaf then trailing slashes are ignored") {
    REQUIRE(path::Leaf("/etc/nginx/nginx.conf") == "nginx.conf");
    REQUIRE(path::Leaf("/etc/nginx/")           == "nginx");
    REQUIRE(path::Leaf("/etc/nginx///")         == "nginx");
    REQUIRE(path::Leaf("notes.txt")             == "notes.txt");
    REQUIRE(path::Leaf("/")                     == "/");
    REQUIRE(path::Leaf("")                      == "");
}

TEST_CASE("given a path when taking the parent then one component is removed") {
    REQUIRE(path::Parent("/etc/nginx/nginx.conf") == "/etc/nginx");
    REQUIRE(path::Parent("/etc/nginx")            == "/etc");
    REQUIRE(path::Parent("/etc")                  == "/");
    REQUIRE(path::Parent("/etc/nginx/")           == "/etc");
    REQUIRE(path::Parent("notes.txt")             == ".");
}

TEST_CASE("given the root when taking the parent then it stays at the root") {
    REQUIRE(path::Parent("/") == "/");
}

TEST_CASE("given dot segments when normalised then they are resolved lexically") {
    REQUIRE(path::Normalise("/etc/./nginx")            == "/etc/nginx");
    REQUIRE(path::Normalise("/etc/nginx/..")           == "/etc");
    REQUIRE(path::Normalise("/etc/nginx/../apache2")   == "/etc/apache2");
    REQUIRE(path::Normalise("/etc//nginx///conf.d")    == "/etc/nginx/conf.d");
    REQUIRE(path::Normalise("/etc/nginx/../..")        == "/");
}

TEST_CASE("given more parent segments than components when normalised then an absolute path stops at the root") {
    // The kernel treats the root as its own parent; walking off the top must
    // not produce a path that escapes it.
    REQUIRE(path::Normalise("/../../etc") == "/etc");
    REQUIRE(path::Normalise("/..")        == "/");
}

TEST_CASE("given a relative path when normalised then leading parent segments survive") {
    // There is no base to resolve them against, so dropping them would change
    // which directory the path refers to.
    REQUIRE(path::Normalise("../sibling")    == "../sibling");
    REQUIRE(path::Normalise("./../sibling")  == "../sibling");
    REQUIRE(path::Normalise("a/../../b")     == "../b");
}

TEST_CASE("given a fully cancelled relative path when normalised then the result is the current directory") {
    REQUIRE(path::Normalise("a/..")   == ".");
    REQUIRE(path::Normalise("./")     == ".");
}

TEST_CASE("given a current directory when moving up then the parent of the login directory is reached") {
    // The browser navigates up by joining ".." and normalising; from the
    // server-relative ".", that must yield "..", not a no-op.
    REQUIRE(path::Normalise(path::Join(".", "..")) == "..");
}

// ---------------------------------------------------------------------------
// FileMode
// ---------------------------------------------------------------------------

TEST_CASE("given a mode when formatted then it reads as ls -l would print it") {
    REQUIRE(FormatPermissions(0040755) == "drwxr-xr-x");
    REQUIRE(FormatPermissions(0100644) == "-rw-r--r--");
    REQUIRE(FormatPermissions(0120777) == "lrwxrwxrwx");
    REQUIRE(FormatPermissions(0100000) == "----------");
}

TEST_CASE("given setuid setgid or sticky bits when formatted then they overload the execute column") {
    REQUIRE(FormatPermissions(0104755) == "-rwsr-xr-x");  // setuid, owner executable
    REQUIRE(FormatPermissions(0104644) == "-rwSr--r--");  // setuid, owner not executable
    REQUIRE(FormatPermissions(0102755) == "-rwxr-sr-x");  // setgid, group executable
    REQUIRE(FormatPermissions(0041777) == "drwxrwxrwt");  // sticky, as on /tmp
}

TEST_CASE("given a mode when its file type is queried then the type bits decide") {
    REQUIRE(IsDirectory(0040755));
    REQUIRE_FALSE(IsDirectory(0100644));
    REQUIRE(IsSymlink(0120777));
    REQUIRE_FALSE(IsSymlink(0040755));
    REQUIRE(IsRegularFile(0100644));
    REQUIRE_FALSE(IsRegularFile(0040755));
}

TEST_CASE("given a mode when formatted as octal then only the permission bits appear") {
    REQUIRE(FormatOctal(0100644) == "644");
    REQUIRE(FormatOctal(0040755) == "755");
    REQUIRE(FormatOctal(0100000) == "000");
}

TEST_CASE("given an octal string when parsed then the value round-trips") {
    uint32_t mode = 0;
    REQUIRE(ParseOctal("644", mode));
    REQUIRE(mode == 0644u);
    REQUIRE(ParseOctal("4755", mode));
    REQUIRE(mode == 04755u);
    REQUIRE(ParseOctal("7", mode));
    REQUIRE(mode == 07u);
}

TEST_CASE("given a malformed octal string when parsed then it is rejected") {
    uint32_t mode = 0xFFFFFFFF;
    REQUIRE_FALSE(ParseOctal("", mode));
    REQUIRE_FALSE(ParseOctal("64444", mode));   // too long
    REQUIRE_FALSE(ParseOctal("648", mode));     // 8 is not an octal digit
    REQUIRE_FALSE(ParseOctal("6a4", mode));
    REQUIRE(mode == 0xFFFFFFFFu);               // untouched on failure
}

TEST_CASE("given an octal string encoding file type bits when parsed then they are masked away") {
    // A chmod dialog must never be able to smuggle a file-type change through
    // the permission field.
    uint32_t mode = 0;
    REQUIRE(ParseOctal("7777", mode));
    REQUIRE(mode == 07777u);
    REQUIRE((mode & kFileTypeMask) == 0u);
}
