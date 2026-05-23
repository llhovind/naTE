#pragma once
#include <cstddef>

enum class DocChangeType {
    InsertLine,
    DeleteLine,
    UpdateLine,   // text or styles changed
    CursorMove,
    TitleChanged,
    CanvasReset,  // virtualDocStartLine_ advanced; lineIndex = new origin
};

class IDocumentListener {
public:
    virtual ~IDocumentListener() = default;
    virtual void OnDocumentChanged(DocChangeType type, size_t lineIndex) = 0;
};
