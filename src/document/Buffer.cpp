#include "document/Buffer.h"

Buffer::Buffer(int cols, int rows, wxColour defaultFg, wxColour defaultBg)
    : m_cols(cols)
    , m_rows(rows)
    , m_cells(cols * rows, Cell{L' ', defaultFg, defaultBg})
{}
