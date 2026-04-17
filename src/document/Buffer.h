#pragma once
#include <wx/colour.h>
#include <vector>

struct Cell {
    wchar_t  ch = L' ';
    wxColour fg;
    wxColour bg;
};

class Buffer {
public:
    Buffer(int cols, int rows, wxColour defaultFg, wxColour defaultBg);

    Cell&       at(int row, int col)       { return m_cells[row * m_cols + col]; }
    const Cell& at(int row, int col) const { return m_cells[row * m_cols + col]; }

    int cols() const { return m_cols; }
    int rows() const { return m_rows; }

private:
    int               m_cols;
    int               m_rows;
    std::vector<Cell> m_cells;
};
