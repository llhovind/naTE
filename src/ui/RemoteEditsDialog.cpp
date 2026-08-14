#include "ui/RemoteEditsDialog.h"

#include <wx/button.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace ui {

RemoteEditsDialog::RemoteEditsDialog(wxWindow* parent, RemoteEditManager& mgr)
    : wxDialog(parent, wxID_ANY, "Remote Edits",
               wxDefaultPosition, wxSize(520, 300))
    , mgr_(mgr)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    list_ = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                          0, nullptr, wxLB_SINGLE);
    sizer->Add(list_, 1, wxEXPAND | wxALL, 8);

    hint_ = new wxStaticText(this, wxID_ANY, "");
    sizer->Add(hint_, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    finishBtn_     = new wxButton(this, wxID_ANY,     "Finish Editing");
    auto* closeBtn = new wxButton(this, wxID_CANCEL,  "Close");
    finishBtn_->Enable(false);
    btnSizer->AddStretchSpacer();
    btnSizer->Add(finishBtn_, 0, wxRIGHT, 4);
    btnSizer->Add(closeBtn,   0);
    sizer->Add(btnSizer, 0, wxEXPAND | wxALL, 8);

    SetSizer(sizer);

    list_->Bind(wxEVT_LISTBOX,     &RemoteEditsDialog::OnSelectionChanged, this);
    finishBtn_->Bind(wxEVT_BUTTON, &RemoteEditsDialog::OnFinish,           this);

    Repopulate();
}

void RemoteEditsDialog::Repopulate()
{
    edits_ = mgr_.ListActiveEdits();

    list_->Clear();
    for (const auto& e : edits_) {
        list_->Append(wxString::FromUTF8(
            e.host.empty() ? e.remotePath : e.remotePath + "   (" + e.host + ")"));
    }

    finishBtn_->Enable(false);
    hint_->SetLabel(edits_.empty()
        ? "No files are being edited."
        : "Finishing an edit deletes its local working copy.");
}

void RemoteEditsDialog::OnSelectionChanged(wxCommandEvent&)
{
    finishBtn_->Enable(list_->GetSelection() != wxNOT_FOUND);
}

void RemoteEditsDialog::OnFinish(wxCommandEvent&)
{
    const int sel = list_->GetSelection();
    if (sel == wxNOT_FOUND || static_cast<size_t>(sel) >= edits_.size())
        return;

    const ActiveEdit edit = edits_[static_cast<size_t>(sel)];

    // Named in full, because the working copy is the only place unsaved changes
    // exist: an editor still holding the file has nowhere to write it back to
    // once this is gone.
    const std::string text =
        "Stop editing this file?\n\n"
        + edit.remotePath + "\n\n"
        "naTE will stop watching for saves and delete the local working copy:\n"
        + edit.localPath + "\n\n"
        "Close the file in your editor first if you have unsaved changes.";

    if (wxMessageBox(wxString::FromUTF8(text), "Finish Editing",
                     wxYES_NO | wxICON_QUESTION, this) != wxYES)
        return;

    mgr_.StopSession(edit.localPath);
    Repopulate();
}

} // namespace ui
