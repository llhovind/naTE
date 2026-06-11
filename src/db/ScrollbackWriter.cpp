#include "db/ScrollbackWriter.h"
#include "db/DocumentSerialisation.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace term::db {

namespace {

std::string SegPath(const std::string& dir, const std::string& uuid, int idx)
{
    return dir + "/" + uuid + "-" + std::to_string(idx) + ".ndjson";
}

std::string CompactPath(const std::string& dir, const std::string& uuid)
{
    return dir + "/" + uuid + ".ndjson";
}

} // namespace

ScrollbackWriter::ScrollbackWriter(const Document& doc, std::string dir, std::string uuid,
                                   size_t scrollbackSaveLines, bool saveStyles)
    : doc_(doc)
    , dir_(std::move(dir))
    , uuid_(std::move(uuid))
    , scrollbackSaveLines_(scrollbackSaveLines)
    , saveStyles_(saveStyles)
{}

ScrollbackWriter::~ScrollbackWriter() = default;

void ScrollbackWriter::Start(const ScrollbackSnapshot* initial)
{
    std::lock_guard<std::mutex> lk(writerMutex_);

    std::error_code ec;
    fs::create_directories(dir_, ec);

    current_.close();
    currentSegmentIdx_     = 0;
    linesInCurrentSegment_ = 0;

    // Fix 1: pre-populate segment 0 with the restored snapshot so segments are
    // the single source of truth for crash recovery after a restore.
    // Only create the file when there is actual data to write (lazy otherwise —
    // an empty segment file would shadow the compact file in Load()).
    if (initial && !initial->lines.empty()) {
        current_.open(SegPath(dir_, uuid_, 0), std::ios::out | std::ios::trunc);
        for (const auto& line : initial->lines)
            WriteLine(line);
        current_.flush();
    }
}

void ScrollbackWriter::OnDocumentChanged(DocChangeType type, size_t lineIndex)
{
    if (type != DocChangeType::InsertLine) return;
    if (lineIndex == 0) return;

    // Line lineIndex was just appended; line lineIndex-1 is now stable.
    std::lock_guard<std::mutex> lk(writerMutex_);
    std::shared_lock<std::shared_mutex> rlk(doc_.GetLinesMutex());
    const auto& lines = doc_.GetLines();
    const size_t stableIdx = lineIndex - 1;
    if (stableIdx < lines.size())
        WriteLine(lines[stableIdx]);
}

void ScrollbackWriter::WriteLine(const DocLine& line)
{
    // Lazy open: create the segment file only when there is actual data to write.
    if (!current_.is_open())
        current_.open(SegPath(dir_, uuid_, currentSegmentIdx_),
                      std::ios::out | std::ios::trunc);
    if (!current_.is_open()) return;
    current_ << SerialiseDocLine(line, saveStyles_).dump() << "\n";
    ++linesInCurrentSegment_;
    if (linesInCurrentSegment_ >= scrollbackSaveLines_)
        Rotate();
}

void ScrollbackWriter::Rotate()
{
    // Fix 2: close current segment, open next with trunc.
    // The closed file becomes the "older"; trunc on the next index supersedes
    // whatever was there (the previous older). No rename needed.
    // Rotate is only called from WriteLine so we know a write is coming — open eagerly.
    current_.close();
    currentSegmentIdx_     = 1 - currentSegmentIdx_;
    linesInCurrentSegment_ = 0;
    current_.open(SegPath(dir_, uuid_, currentSegmentIdx_),
                  std::ios::out | std::ios::trunc);
}

void ScrollbackWriter::Compact()
{
    std::lock_guard<std::mutex> lk(writerMutex_);

    current_.close();

    const std::string seg0    = SegPath(dir_, uuid_, 0);
    const std::string seg1    = SegPath(dir_, uuid_, 1);
    const std::string compact = CompactPath(dir_, uuid_);
    const std::string tmp     = compact + ".tmp";

    {
        std::ofstream out(tmp, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[ScrollbackWriter] Cannot write compact file: " << tmp << '\n';
            current_.open(SegPath(dir_, uuid_, currentSegmentIdx_),
                          std::ios::out | std::ios::app);
            return;
        }
        for (const auto& segPath : {seg0, seg1}) {
            std::ifstream in(segPath);
            if (!in.is_open()) continue;
            out << in.rdbuf();
        }
    }

    std::error_code ec;
    fs::rename(tmp, compact, ec);
    if (ec)
        std::cerr << "[ScrollbackWriter] Compact rename failed: " << ec.message() << '\n';

    fs::remove(seg0, ec);
    fs::remove(seg1, ec);

    currentSegmentIdx_     = 0;
    linesInCurrentSegment_ = 0;
    // Don't open a fresh segment eagerly — an empty segment would shadow the
    // compact file in Load(). Open lazily in WriteLine() when new data arrives.
}

void ScrollbackWriter::Truncate()
{
    std::lock_guard<std::mutex> lk(writerMutex_);

    current_.close();

    // Fix 3: delete all segment files regardless of how many are present.
    std::error_code ec;
    fs::remove(SegPath(dir_, uuid_, 0), ec);
    fs::remove(SegPath(dir_, uuid_, 1), ec);
    fs::remove(CompactPath(dir_, uuid_), ec);

    currentSegmentIdx_     = 0;
    linesInCurrentSegment_ = 0;
    // Don't reopen — lazy open in WriteLine() when new data arrives.
}

} // namespace term::db
