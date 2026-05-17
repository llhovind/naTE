#pragma once
#include <wx/dialog.h>
#include <wx/spinctrl.h>

class GeometryDialog : public wxDialog
{
public:
    GeometryDialog(wxWindow*      parent,
                   unsigned short currentCols,
                   unsigned short currentRows);

    unsigned short GetCols() const;
    unsigned short GetRows() const;

private:
    wxSpinCtrl* m_cols;
    wxSpinCtrl* m_rows;
};
