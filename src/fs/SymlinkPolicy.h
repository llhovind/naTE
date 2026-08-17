#pragma once
#include <string>

namespace term::fs {

// What a copy does when it meets a symbolic link.
//
// There is no defensible default policy here, which is why this is a setting
// and not a rule. Copying a release tree where "latest -> v1.2.3" should keep
// the link; copying a config directory onto a host that lacks the structure the
// links point into should resolve them. Both are ordinary, and which one is
// wanted is knowledge the administrator has and this code does not.
//
// A preserved link can land dangling on the far side. That is a consequence of
// what was asked for, not a failure to be prevented.
enum class SymlinkPolicy {
    // Reproduce the link, pointing at the same target text. The default: it is
    // the common case, and it is the only mode that copies a tree back as the
    // shape it was.
    Preserve,
    // Leave links out of the copy entirely. They still appear in the queue as
    // skipped, so a copy never quietly omits something.
    Skip,
    // Copy what the link points at, in its place. Reserved: the enum names it
    // so adding it later is not a breaking change, but nothing implements it
    // yet and no UI offers it. Following links inside a recursive walk needs
    // visited-directory tracking first — a link to "/" or to an ancestor is a
    // cycle, and the tree walk's depth cap is a backstop, not a defence.
    Follow,
};

// Spelling used in config.ini. Unknown or not-yet-implemented values fall back
// to the default rather than failing to load a config over one bad line.
inline const char* ToString(SymlinkPolicy policy)
{
    switch (policy) {
        case SymlinkPolicy::Skip:     return "skip";
        case SymlinkPolicy::Follow:   return "follow";
        case SymlinkPolicy::Preserve: break;
    }
    return "preserve";
}

inline SymlinkPolicy SymlinkPolicyFromString(const std::string& text)
{
    if (text == "skip") return SymlinkPolicy::Skip;
    // "follow" is deliberately not accepted: the enum reserves it, but nothing
    // implements it, and silently treating it as another mode would be worse
    // than ignoring a value no released build could have written.
    return SymlinkPolicy::Preserve;
}

} // namespace term::fs
