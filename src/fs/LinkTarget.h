#pragma once
#include <string>
#include <vector>

namespace term::fs {

// What a symbolic link turned out to point at.
//
// A directory listing describes links, never their targets: readdir and lstat
// report the link itself, and following every entry would cost a round trip per
// row. So every link starts Unresolved and is only ever refined by a stat that
// someone decided was worth paying for — which is why Unresolved is a first
// class value here rather than an absence. A caller that has not paid must be
// able to say "I do not know yet" instead of guessing "file".
enum class LinkTarget {
    Unresolved,  // not looked up, or the lookup could not answer
    Directory,
    File,        // anything that is not a directory: regular, socket, device
    Broken,      // the target does not exist — a dangling link
};

// One answer, keyed by the leaf name it belongs to.
//
// Name rather than index: a listing can be re-sorted or re-filtered while the
// lookups are in flight, and an index captured beforehand would then name a
// different row. Within one directory the name is the identity.
struct LinkResolution {
    std::string name;
    LinkTarget  target = LinkTarget::Unresolved;
};

} // namespace term::fs
