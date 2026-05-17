#include "ui/GeometryDialog.h"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>

GeometryDialog::GeometryDialog(wxWindow*      parent,
                               unsigned short currentCols,
                               unsigned short currentRows)
    : wxDialog(parent, wxID_ANY, "Set Geometry",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(new wxStaticText(this, wxID_ANY, "Cols:"),
             0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_cols = new wxSpinCtrl(this, wxID_ANY, "",
                            wxDefaultPosition, wxDefaultSize,
                            wxSP_ARROW_KEYS, 10, 999,
                            static_cast<int>(currentCols));
    row->Add(m_cols, 0, wxRIGHT, 16);

    row->Add(new wxStaticText(this, wxID_ANY, "Rows:"),
             0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_rows = new wxSpinCtrl(this, wxID_ANY, "",
                            wxDefaultPosition, wxDefaultSize,
                            wxSP_ARROW_KEYS, 1, 200,
                            static_cast<int>(currentRows));
    row->Add(m_rows, 0);

    outer->Add(row, 0, wxALL, 12);
    outer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0,
               wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    SetSizerAndFit(outer);
    m_cols->SetFocus();
}

unsigned short GeometryDialog::GetCols() const
{
    return static_cast<unsigned short>(m_cols->GetValue());
}

unsigned short GeometryDialog::GetRows() const
{
    return static_cast<unsigned short>(m_rows->GetValue());
}
