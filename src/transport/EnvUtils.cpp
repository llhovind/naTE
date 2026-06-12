#include "transport/EnvUtils.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>

extern char** environ;

namespace term::transport {

std::vector<term::session::EnvVar> ParseEnvFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
        return {};

    std::vector<term::session::EnvVar> result;
    std::string line;
    while (std::getline(file, line)) {
        // Strip carriage returns (Windows-style line endings)
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        // Skip blank lines and comment lines
        if (line.empty() || line.front() == '#')
            continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        if (key.empty())
            continue;

        result.push_back({std::move(key), line.substr(eq + 1)});
    }
    return result;
}

std::string ExpandTilde(const std::string& path, const std::string& homeDir)
{
    if (path.empty() || path.front() != '~')
        return path;

    // "~" alone or "~/..."
    if (path.size() == 1)
        return homeDir;

    if (path[1] == '/')
        return homeDir + path.substr(1);

    // "~something" — not a home-dir expansion, return unchanged
    return path;
}

std::vector<std::string> CaptureParentEnv()
{
    std::vector<std::string> result;
    for (char** e = environ; *e != nullptr; ++e)
        result.emplace_back(*e);
    return result;
}

EnvBlock BuildEnvBlock(
    const std::vector<std::string>&           parentEnv,
    const std::vector<term::session::EnvVar>& appDefaults,
    const std::vector<term::session::EnvVar>& envFileVars,
    const std::vector<term::session::EnvVar>& profileVars)
{
    // Build an ordered map preserving insertion order of keys as we encounter
    // them from lowest to highest priority, overwriting values as needed.
    // Use a vector for insertion-order preservation + map for O(1) lookup.
    std::vector<std::string>                  keys;
    std::unordered_map<std::string, std::string> values;

    auto upsert = [&](const std::string& key, const std::string& value) {
        if (values.find(key) == values.end())
            keys.push_back(key);
        values[key] = value;
    };

    // Layer 1 — parent environment
    for (const auto& entry : parentEnv) {
        const auto eq = entry.find('=');
        if (eq == std::string::npos) continue;
        upsert(entry.substr(0, eq), entry.substr(eq + 1));
    }

    // Layer 2 — app-wide defaults
    for (const auto& ev : appDefaults)
        upsert(ev.key, ev.value);

    // Layer 3 — env file vars
    for (const auto& ev : envFileVars)
        upsert(ev.key, ev.value);

    // Layer 4 — profile vars (highest priority)
    for (const auto& ev : profileVars)
        upsert(ev.key, ev.value);

    EnvBlock block;
    block.storage.reserve(keys.size());
    block.ptrs.reserve(keys.size() + 1);

    for (const auto& key : keys)
        block.storage.push_back(key + '=' + values.at(key));

    for (auto& s : block.storage)
        block.ptrs.push_back(s.data());

    block.ptrs.push_back(nullptr);
    return block;
}

std::string ResolveWorkingDir(const std::string& profileDir,
                               const std::string& appDefault,
                               const std::string& homeDir)
{
    if (!profileDir.empty())
        return ExpandTilde(profileDir, homeDir);
    if (!appDefault.empty())
        return ExpandTilde(appDefault, homeDir);
    return {};
}

std::string MakeLoginShellArg0(const std::string& shellPath)
{
    const auto pos = shellPath.rfind('/');
    const std::string name = (pos == std::string::npos)
        ? shellPath
        : shellPath.substr(pos + 1);
    return '-' + name;
}

std::string ShellQuote(const std::string& s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += "'";
    return out;
}

} // namespace term::transport
