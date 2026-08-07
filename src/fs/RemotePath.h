#pragma once
#include <string>

// Pure POSIX path arithmetic for remote filesystems.
//
// Deliberately not std::filesystem::path: that follows the *host* platform's
// rules, and a remote path is always POSIX regardless of what naTE runs on.
// These functions are lexical only — they never touch a filesystem, which is
// what makes them exhaustively testable and safe to call from any thread.
namespace term::fs::path {

bool IsAbsolute(const std::string& path) noexcept;

// Appends leaf to dir, inserting a separator only when needed. An absolute
// leaf replaces dir entirely, matching the POSIX resolution rule.
std::string Join(const std::string& dir, const std::string& leaf);

// The final component. Trailing slashes are ignored ("/etc/nginx/" -> "nginx").
// Returns "/" for the root and "" for the empty path.
std::string Leaf(const std::string& path);

// The containing directory. Returns "/" for the root, "." for a bare relative
// leaf, and otherwise the path with its final component removed.
std::string Parent(const std::string& path);

// Lexically resolves "." and ".." and collapses duplicate separators.
// Leading ".." components survive on relative paths (there is no base to
// resolve them against); on absolute paths they are absorbed at the root, as
// the kernel does. Never returns "" — a fully cancelled relative path is ".".
//
// Purely textual: with no knowledge of which components are symlinks this can
// differ from the server's own resolution, so it is a display and
// navigation aid, not a substitute for the remote's realpath.
std::string Normalise(const std::string& path);

} // namespace term::fs::path
