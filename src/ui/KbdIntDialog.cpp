#include "ui/KbdIntDialog.h"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>

namespace ui {

KbdIntDialog::KbdIntDialog(wxWindow*                                parent,
                            const term::transport::KbdIntChallenge& challenge)
    : wxDialog(parent, wxID_ANY,
               challenge.name.empty()
                   ? "SSH Authentication"
                   : wxString::FromUTF8(challenge.name),
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    if (!challenge.instruction.empty()) {
        auto* lbl = new wxStaticText(this, wxID_ANY,
                                     wxString::FromUTF8(challenge.instruction));
        lbl->Wrap(400);
        outer->Add(lbl, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    }

    auto* grid = new wxFlexGridSizer(static_cast<int>(challenge.prompts.size()), 2, 6, 8);
    grid->AddGrowableCol(1, 1);

    for (const auto& prompt : challenge.prompts) {
        grid->Add(new wxStaticText(this, wxID_ANY,
                                   wxString::FromUTF8(prompt.text)),
                  0, wxALIGN_CENTER_VERTICAL);
        const long style = prompt.echo ? 0 : wxTE_PASSWORD;
        auto* ctrl = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                    wxDefaultPosition, wxSize(260, -1), style);
        grid->Add(ctrl, 1, wxEXPAND);
        inputs_.push_back(ctrl);
    }

    outer->Add(grid, 0, wxEXPAND | wxALL, 12);

    auto* btns = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    outer->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    SetSizerAndFit(outer);
    SetMinSize(GetSize());

    if (!inputs_.empty())
        inputs_.front()->SetFocus();
}

std::vector<std::string> KbdIntDialog::GetResponses() const
{
    std::vector<std::string> out;
    out.reserve(inputs_.size());
    for (auto* ctrl : inputs_)
        out.push_back(ctrl->GetValue().ToStdString());
    return out;
}

} // namespace ui
