#pragma once
#include "session/EnvVar.h"
#include <string>
#include <vector>

namespace term::transport {

// Parse a KEY=value env file. '#' comment lines, blank lines, and malformed
// lines (no '=', empty key) are silently skipped. The value is everything
// after the first '=', so values containing '=' are preserved verbatim.
// Returns an empty vector if the file cannot be opened — never throws.
std::vector<term::session::EnvVar> ParseEnvFile(const std::string& path);

// Replace a leading '~' in path with homeDir. Returns path unchanged if it
// does not start with '~'. homeDir is provided by the caller so this function
// remains pure and testable.
std::string ExpandTilde(const std::string& path, const std::string& homeDir);

// Snapshot the current process environment (environ) into a vector of
// "KEY=VALUE" strings. This is the only function that reads global state;
// call it before forking so the pure BuildEnvBlock can work on a snapshot.
std::vector<std::string> CaptureParentEnv();

struct EnvBlock {
    std::vector<std::string> storage;  // owns all "KEY=VALUE" string data
    std::vector<char*>       ptrs;     // non-owning pointers into storage; last element is nullptr
};

// Build a merged environment suitable for execve()/execvpe().
//
// Merge order — later layers win on key conflict:
//   1. parentEnv   — inherited parent environment
//   2. appDefaults — app-wide defaults from AppConfig
//   3. envFileVars — vars loaded from a .env file (already parsed)
//   4. profileVars — per-profile explicit KEY=VALUE pairs (always win)
//
// The returned EnvBlock owns all string storage; ptrs remain valid until the
// EnvBlock is destroyed.
EnvBlock BuildEnvBlock(
    const std::vector<std::string>&           parentEnv,
    const std::vector<term::session::EnvVar>& appDefaults,
    const std::vector<term::session::EnvVar>& envFileVars,
    const std::vector<term::session::EnvVar>& profileVars);

// Resolve the effective working directory.
// Returns profileDir (tilde-expanded) if non-empty; otherwise appDefault
// (tilde-expanded). Returns empty string if both are empty — caller must
// not chdir() in that case.
std::string ResolveWorkingDir(const std::string& profileDir,
                               const std::string& appDefault,
                               const std::string& homeDir);

// Build the login-shell argv[0] from a shell binary path.
// "/bin/bash" → "-bash",  "/usr/bin/zsh" → "-zsh".
std::string MakeLoginShellArg0(const std::string& shellPath);

// Wrap s in single-quotes, escaping embedded single-quotes as '\''.
// Produces a POSIX shell token that is safe regardless of spaces or
// special characters.
std::string ShellQuote(const std::string& s);

} // namespace term::transport
