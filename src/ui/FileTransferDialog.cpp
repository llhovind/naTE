#include "ui/FileTransferDialog.h"

#include <wx/app.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>

namespace ui {

FileTransferDialog::FileTransferDialog(wxWindow* parent,
                                       term::session::SessionId sessionId,
                                       term::session::SessionManager& sm,
                                       const std::string& remoteDescription)
    : wxDialog(parent, wxID_ANY,
               remoteDescription.empty()
                   ? wxString("Transfer Files")
                   : wxString::Format("Transfer Files to %s",
                                      wxString::FromUTF8(remoteDescription)),
               wxDefaultPosition, wxSize(520, 380),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , sessionId_(sessionId)
    , sm_(sm)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    // --- File list ---
    auto* filesLabel = new wxStaticText(this, wxID_ANY, "Local Files:");
    outer->Add(filesLabel, 0, wxLEFT | wxTOP, 10);

    auto* listRow = new wxBoxSizer(wxHORIZONTAL);
    fileList_ = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                              0, nullptr, wxLB_EXTENDED);
    listRow->Add(fileList_, 1, wxEXPAND | wxRIGHT, 6);

    auto* btnCol = new wxBoxSizer(wxVERTICAL);
    auto* addBtn = new wxButton(this, wxID_ANY, "Add Files...");
    removeBtn_   = new wxButton(this, wxID_ANY, "Remove");
    removeBtn_->Disable();
    btnCol->Add(addBtn,    0, wxBOTTOM, 4);
    btnCol->Add(removeBtn_, 0);
    listRow->Add(btnCol, 0, wxALIGN_TOP);

    outer->Add(listRow, 1, wxEXPAND | wxALL, 10);

    // --- Destination ---
    auto* destLabel = new wxStaticText(this, wxID_ANY, "Remote Destination Folder:");
    outer->Add(destLabel, 0, wxLEFT | wxBOTTOM, 10);
    destCtrl_ = new wxTextCtrl(this, wxID_ANY);
    destCtrl_->SetHint("Leave blank to use remote current directory");
    outer->Add(destCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    // --- Status ---
    statusLabel_ = new wxStaticText(this, wxID_ANY, "Ready");
    outer->Add(statusLabel_, 0, wxLEFT | wxBOTTOM, 10);

    // --- Buttons ---
    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    btnRow->AddStretchSpacer();
    transferBtn_ = new wxButton(this, wxID_ANY, "Transfer");
    transferBtn_->Disable();
    auto* closeBtn = new wxButton(this, wxID_CANCEL, "Close");
    btnRow->Add(transferBtn_, 0, wxRIGHT, 6);
    btnRow->Add(closeBtn,     0);
    outer->Add(btnRow, 0, wxALL, 10);

    SetSizerAndFit(outer);
    SetMinSize(wxSize(400, 300));

    addBtn->Bind(wxEVT_BUTTON, &FileTransferDialog::OnAddFiles, this);
    removeBtn_->Bind(wxEVT_BUTTON, &FileTransferDialog::OnRemove, this);
    transferBtn_->Bind(wxEVT_BUTTON, &FileTransferDialog::OnTransfer, this);
    fileList_->Bind(wxEVT_LISTBOX, &FileTransferDialog::OnListSelectionChanged, this);
}

void FileTransferDialog::OnAddFiles(wxCommandEvent&)
{
    wxFileDialog dlg(this, "Select Files to Transfer",
                     wxEmptyString, wxEmptyString,
                     "All files (*)|*",
                     wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;

    wxArrayString paths;
    dlg.GetPaths(paths);
    for (const auto& p : paths) {
        if (fileList_->FindString(p) == wxNOT_FOUND)
            fileList_->Append(p);
    }
    transferBtn_->Enable(!fileList_->IsEmpty());
}

void FileTransferDialog::OnRemove(wxCommandEvent&)
{
    wxArrayInt selected;
    fileList_->GetSelections(selected);
    // Remove in reverse order to preserve indices.
    for (int i = static_cast<int>(selected.size()) - 1; i >= 0; --i)
        fileList_->Delete(static_cast<unsigned int>(selected[i]));
    removeBtn_->Disable();
    transferBtn_->Enable(!fileList_->IsEmpty());
}

void FileTransferDialog::OnListSelectionChanged(wxCommandEvent&)
{
    wxArrayInt sel;
    fileList_->GetSelections(sel);
    removeBtn_->Enable(!sel.IsEmpty());
}

void FileTransferDialog::OnTransfer(wxCommandEvent&)
{
    transferBtn_->Disable();
    transferIndex_ = 0;
    transferTotal_ = static_cast<int>(fileList_->GetCount());
    TransferNext();
}

void FileTransferDialog::TransferNext()
{
    if (transferIndex_ >= transferTotal_) {
        statusLabel_->SetLabel("Transfer complete.");
        transferBtn_->Enable(!fileList_->IsEmpty());
        return;
    }

    const int idx = transferIndex_;
    statusLabel_->SetLabel(
        wxString::Format("Transferring %s... (%d / %d)",
                         fileList_->GetString(static_cast<unsigned int>(idx)),
                         idx + 1, transferTotal_));

    const std::string localPath =
        fileList_->GetString(static_cast<unsigned int>(idx)).ToStdString();
    wxString dest = destCtrl_->GetValue().Trim();
    if (dest.IsEmpty()) dest = ".";
    const std::string remoteDir = dest.ToStdString();

    sm_.TransferFile(sessionId_, localPath, remoteDir,
        [this, idx](bool success, std::string error) {
            // Callback arrives on the transport worker thread; dispatch to UI thread.
            wxTheApp->CallAfter([this, success, idx, err = std::move(error)]() mutable {
                if (!success) {
                    statusLabel_->SetLabel("Transfer failed.");
                    transferBtn_->Enable();
                    wxMessageBox(
                        wxString::Format("Failed to transfer '%s':\n\n%s",
                                         fileList_->GetString(
                                             static_cast<unsigned int>(idx)),
                                         wxString::FromUTF8(err)),
                        "Transfer Failed", wxOK | wxICON_ERROR, this);
                    return;
                }
                ++transferIndex_;
                TransferNext();
            });
        });
}

} // namespace ui
