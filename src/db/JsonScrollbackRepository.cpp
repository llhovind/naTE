#include "db/JsonScrollbackRepository.h"
#include "db/DocumentSerialisation.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace term::db {

JsonScrollbackRepository::JsonScrollbackRepository(std::string dir)
    : m_dir(std::move(dir))
{}

namespace {

std::string SegPath(const std::string& dir, const std::string& uuid, int idx)
{
    return dir + "/" + uuid + "-" + std::to_string(idx) + ".ndjson";
}

std::string CompactPath(const std::string& dir, const std::string& uuid)
{
    return dir + "/" + uuid + ".ndjson";
}

// Read all DocLines from a plain NDJSON file, appending into out.
void ReadNdjson(const std::string& path, std::vector<DocLine>& out)
{
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        try {
            out.push_back(DeserialiseDocLine(json::parse(line)));
        } catch (const json::exception& e) {
            std::cerr << "[Scrollback] Skipping malformed line in " << path
                      << ": " << e.what() << '\n';
        }
    }
}

// Extract UUID from a scrollback filename.
// Strips "-0.ndjson", "-1.ndjson", or ".ndjson" suffix.
// Returns empty string if the filename doesn't match any pattern.
std::string ExtractUuid(std::string_view filename)
{
    if (filename.size() > 9 && filename.substr(filename.size() - 9) == "-0.ndjson")
        return std::string(filename.substr(0, filename.size() - 9));
    if (filename.size() > 9 && filename.substr(filename.size() - 9) == "-1.ndjson")
        return std::string(filename.substr(0, filename.size() - 9));
    if (filename.size() > 7 && filename.substr(filename.size() - 7) == ".ndjson")
        return std::string(filename.substr(0, filename.size() - 7));
    return {};
}

} // namespace

ScrollbackSnapshot JsonScrollbackRepository::Load(const std::string& uuid,
                                                             size_t maxLines) const
{
    ScrollbackSnapshot snap;

    const std::string seg0 = SegPath(m_dir, uuid, 0);
    const std::string seg1 = SegPath(m_dir, uuid, 1);
    const bool hasSeg0 = fs::exists(seg0);
    const bool hasSeg1 = fs::exists(seg1);

    std::vector<DocLine> all;
    if (hasSeg0 || hasSeg1) {
        // Crash-recovery / live-session path: segments are more recent than compact.
        ReadNdjson(seg0, all);
        ReadNdjson(seg1, all);
    } else {
        // Clean-exit path: read compact file.
        ReadNdjson(CompactPath(m_dir, uuid), all);
    }

    // Take only the last maxLines entries.
    if (maxLines > 0 && all.size() > maxLines)
        all.erase(all.begin(), all.end() - static_cast<ptrdiff_t>(maxLines));

    snap.lines = std::move(all);
    return snap;
}

bool JsonScrollbackRepository::Exists(const std::string& uuid) const
{
    return fs::exists(SegPath(m_dir, uuid, 0))
        || fs::exists(SegPath(m_dir, uuid, 1))
        || fs::exists(CompactPath(m_dir, uuid));
}

void JsonScrollbackRepository::Delete(const std::string& uuid)
{
    std::error_code ec;
    fs::remove(SegPath(m_dir, uuid, 0), ec);
    fs::remove(SegPath(m_dir, uuid, 1), ec);
    fs::remove(CompactPath(m_dir, uuid), ec);
}

std::vector<std::string> JsonScrollbackRepository::ListAll() const
{
    std::set<std::string> uuids;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(m_dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string uuid = ExtractUuid(entry.path().filename().string());
        if (!uuid.empty())
            uuids.insert(uuid);
    }
    return {uuids.begin(), uuids.end()};
}

} // namespace term::db
