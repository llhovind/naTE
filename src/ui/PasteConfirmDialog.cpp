#include "ui/PasteConfirmDialog.h"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>

namespace ui {

PasteConfirmDialog::PasteConfirmDialog(wxWindow* parent, const std::string& utf8Text)
    : wxDialog(parent, wxID_ANY, "Multi-line Paste",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* lbl = new wxStaticText(this, wxID_ANY,
        "The text to paste contains multiple lines. Without bracketed-paste "
        "support the shell may execute each line as a command.");
    lbl->Wrap(460);
    outer->Add(lbl, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

    auto* preview = new wxTextCtrl(this, wxID_ANY,
        wxString::FromUTF8(utf8Text),
        wxDefaultPosition, wxSize(480, 180),
        wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP | wxHSCROLL);
    outer->Add(preview, 1, wxEXPAND | wxALL, 12);

    auto* btns      = new wxStdDialogButtonSizer();
    auto* pasteBtn  = new wxButton(this, wxID_OK,     "Paste");
    auto* cancelBtn = new wxButton(this, wxID_CANCEL, "Cancel");
    btns->AddButton(pasteBtn);
    btns->AddButton(cancelBtn);
    btns->Realize();
    cancelBtn->SetDefault();
    cancelBtn->SetFocus();

    outer->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    SetSizerAndFit(outer);
    SetMinSize(wxSize(360, 240));
}

} // namespace ui
