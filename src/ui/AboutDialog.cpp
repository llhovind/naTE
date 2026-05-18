#include "ui/AboutDialog.h"
#include "version.h"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>

AboutDialog::AboutDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "About naTE",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    outer->Add(new wxStaticText(this, wxID_ANY, "naTE"),
               0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxLEFT | wxRIGHT, 16);
    outer->Add(new wxStaticText(this, wxID_ANY, APP_VERSION_FULL),
               0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxLEFT | wxRIGHT, 8);

    outer->Add(CreateStdDialogButtonSizer(wxOK), 0,
               wxEXPAND | wxALL, 12);

    SetSizerAndFit(outer);
}
