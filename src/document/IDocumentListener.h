#pragma once
#include <cstddef>

enum class DocChangeType {
    InsertLine,
    DeleteLine,
    UpdateLine,  // text or styles changed
    CursorMove
};

class IDocumentListener {
public:
    virtual ~IDocumentListener() = default;
    virtual void OnDocumentChanged(DocChangeType type, size_t lineIndex) = 0;
};
