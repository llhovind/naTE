#pragma once

#include "document/Document.h"
#include "document/IDocumentListener.h"
#include <fstream>
#include <mutex>
#include <string>

namespace term::db {

// Streams terminal lines to NDJSON segment files as they are produced.
//
// Lifecycle:
//   1. Construct with (doc, dir, uuid, scrollbackSaveLines, saveStyles).
//      doc must outlive the writer (SessionRecord owns both).
//   2. Call Start() — optionally pass a restored snapshot to pre-populate segment 0
//      so segments are the single source of truth after a restore (Fix 1).
//   3. Register as a document listener; OnDocumentChanged fires on the parser thread.
//   4. On clean exit or explicit workspace save, call Compact() to merge segments
//      into {uuid}.ndjson and remove the segment files.
//   5. On FullReset(clearContent=true), call Truncate() to wipe all segments.
//
// Thread safety: writerMutex_ serialises parser-thread OnDocumentChanged calls
// and UI-thread Truncate() calls (Fix 4).
class ScrollbackWriter : public IDocumentListener {
public:
    ScrollbackWriter(const Document& doc, std::string dir, std::string uuid,
                     size_t scrollbackSaveLines, bool saveStyles);
    ~ScrollbackWriter() override;

    // Pre-populates segment 0 from a restored snapshot, then opens the writer
    // ready for new content. Pass nullptr for a fresh (non-restored) session.
    void Start(const ScrollbackSnapshot* initial = nullptr);

    // IDocumentListener — called on the parser thread.
    // Responds to InsertLine(N) by writing the now-stable line N-1 to disk.
    // Responds to CanvasReset by calling Truncate().
    void OnDocumentChanged(DocChangeType type, size_t lineIndex) override;

    // Merges segments → {uuid}.ndjson (plain NDJSON); deletes segment files.
    // Opens a fresh segment 0 so the writer remains usable afterwards.
    void Compact();

    // Deletes all segment files and the compact file; opens a fresh segment 0.
    // Called when the user resets the terminal with content cleared.
    void Truncate();

private:
    // Fix 2: toggle between segment 0 and 1. Close current, open next with trunc.
    // The closed segment becomes the "older"; the new segment is the new "current".
    void Rotate();

    // Write one line to the current segment. Caller holds writerMutex_.
    void WriteLine(const DocLine& line);

    const Document& doc_;
    std::string     dir_;
    std::string     uuid_;
    size_t          scrollbackSaveLines_;
    bool            saveStyles_;
    std::ofstream   current_;
    size_t          linesInCurrentSegment_ = 0;
    int             currentSegmentIdx_     = 0;  // 0 or 1
    std::mutex      writerMutex_;               // Fix 4
};

} // namespace term::db
