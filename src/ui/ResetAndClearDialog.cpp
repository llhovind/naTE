#include "ui/ResetAndClearDialog.h"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace ui {

ResetAndClearDialog::ResetAndClearDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "Reset and Clear")
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* lbl = new wxStaticText(this, wxID_ANY,
        "Save scrollback before clearing?");
    outer->Add(lbl, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    auto* btns      = new wxStdDialogButtonSizer();
    auto* saveBtn   = new wxButton(this, wxID_YES,    "Save and Clear");
    auto* clearBtn  = new wxButton(this, wxID_NO,     "Clear");
    auto* cancelBtn = new wxButton(this, wxID_CANCEL, "Cancel");
    btns->AddButton(saveBtn);
    btns->AddButton(clearBtn);
    btns->AddButton(cancelBtn);
    btns->Realize();
    cancelBtn->SetDefault();
    cancelBtn->SetFocus();

    // wxDialog only auto-closes for wxID_OK / wxID_CANCEL; wire Yes/No
    // explicitly so each button ends the modal loop with its own id.
    saveBtn->Bind(wxEVT_BUTTON,  [this](wxCommandEvent&) { EndModal(wxID_YES); });
    clearBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_NO);  });

    outer->Add(btns, 0, wxEXPAND | wxALL, 12);

    SetSizerAndFit(outer);
}

} // namespace ui
