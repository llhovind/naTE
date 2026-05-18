#include "ui/FileTransferDialog.h"
#include "ui/RemoteFileBrowserDialog.h"

#include <wx/app.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>

namespace ui {

FileTransferDialog::FileTransferDialog(wxWindow* parent,
                                       term::session::SessionId sessionId,
                                       term::session::SessionManager& sm,
                                       const std::string& remoteDescription,
                                       TransferDirection direction)
    : wxDialog(parent, wxID_ANY,
               [&]() -> wxString {
                   const wxString remote = wxString::FromUTF8(remoteDescription);
                   if (direction == TransferDirection::Send)
                       return remoteDescription.empty()
                              ? wxString("Transfer Files")
                              : wxString::Format("Transfer Files to %s", remote);
                   return remoteDescription.empty()
                          ? wxString("Receive Files")
                          : wxString::Format("Receive Files from %s", remote);
               }(),
               wxDefaultPosition, wxSize(520, 380),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , sessionId_(sessionId)
    , sm_(sm)
    , direction_(direction)
{
    const bool isSend = (direction_ == TransferDirection::Send);

    auto* outer = new wxBoxSizer(wxVERTICAL);

    // --- File list ---
    auto* filesLabel = new wxStaticText(this, wxID_ANY,
                                        isSend ? "Local Files:" : "Remote Files:");
    outer->Add(filesLabel, 0, wxLEFT | wxTOP, 10);

    auto* listRow = new wxBoxSizer(wxHORIZONTAL);
    fileList_ = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                              0, nullptr, wxLB_EXTENDED);
    listRow->Add(fileList_, 1, wxEXPAND | wxRIGHT, 6);

    auto* btnCol = new wxBoxSizer(wxVERTICAL);
    wxButton* addBtn = nullptr;
    if (isSend) {
        addBtn = new wxButton(this, wxID_ANY, "Add Files...");
        addBtn->Bind(wxEVT_BUTTON, &FileTransferDialog::OnAddFiles, this);
    } else {
        addBtn = new wxButton(this, wxID_ANY, "Browse Remote...");
        addBtn->Bind(wxEVT_BUTTON, &FileTransferDialog::OnBrowseRemote, this);
    }
    removeBtn_ = new wxButton(this, wxID_ANY, "Remove");
    removeBtn_->Disable();
    btnCol->Add(addBtn,     0, wxBOTTOM, 4);
    btnCol->Add(removeBtn_, 0);
    listRow->Add(btnCol, 0, wxALIGN_TOP);

    outer->Add(listRow, 1, wxEXPAND | wxALL, 10);

    // --- Destination ---
    if (isSend) {
        auto* destLabel = new wxStaticText(this, wxID_ANY, "Remote Destination Folder:");
        outer->Add(destLabel, 0, wxLEFT | wxBOTTOM, 10);
        destCtrl_ = new wxTextCtrl(this, wxID_ANY);
        destCtrl_->SetHint("Leave blank to use remote current directory");
        outer->Add(destCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    } else {
        auto* destLabel = new wxStaticText(this, wxID_ANY, "Local Destination Folder:");
        outer->Add(destLabel, 0, wxLEFT | wxBOTTOM, 10);
        auto* destRow = new wxBoxSizer(wxHORIZONTAL);
        destCtrl_ = new wxTextCtrl(this, wxID_ANY);
        destCtrl_->SetHint("Leave blank to use current directory");
        auto* browseBtn = new wxButton(this, wxID_ANY, "Browse...",
                                       wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        browseBtn->Bind(wxEVT_BUTTON, &FileTransferDialog::OnBrowseLocalDest, this);
        destRow->Add(destCtrl_, 1, wxRIGHT, 4);
        destRow->Add(browseBtn, 0);
        outer->Add(destRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    }

    // --- Status ---
    statusLabel_ = new wxStaticText(this, wxID_ANY, "Ready");
    outer->Add(statusLabel_, 0, wxLEFT | wxBOTTOM, 10);

    // --- Buttons ---
    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    btnRow->AddStretchSpacer();
    transferBtn_ = new wxButton(this, wxID_ANY, isSend ? "Transfer" : "Receive");
    transferBtn_->Disable();
    auto* closeBtn = new wxButton(this, wxID_CANCEL, "Close");
    btnRow->Add(transferBtn_, 0, wxRIGHT, 6);
    btnRow->Add(closeBtn,     0);
    outer->Add(btnRow, 0, wxALL, 10);

    SetSizerAndFit(outer);
    SetMinSize(wxSize(400, 300));

    removeBtn_->Bind(wxEVT_BUTTON, &FileTransferDialog::OnRemove,              this);
    transferBtn_->Bind(wxEVT_BUTTON, &FileTransferDialog::OnTransfer,          this);
    fileList_->Bind(wxEVT_LISTBOX,   &FileTransferDialog::OnListSelectionChanged, this);
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

void FileTransferDialog::OnBrowseRemote(wxCommandEvent&)
{
    const std::string remote = sm_.GetRemoteDescription(sessionId_);
    RemoteFileBrowserDialog dlg(this, sessionId_, sm_, remote);
    if (dlg.ShowModal() != wxID_OK) return;

    for (const auto& path : dlg.GetSelectedPaths()) {
        const wxString wp = wxString::FromUTF8(path);
        if (fileList_->FindString(wp) == wxNOT_FOUND)
            fileList_->Append(wp);
    }
    transferBtn_->Enable(!fileList_->IsEmpty());
}

void FileTransferDialog::OnBrowseLocalDest(wxCommandEvent&)
{
    wxDirDialog dlg(this, "Select Local Destination Folder",
                    destCtrl_->GetValue(), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK)
        destCtrl_->SetValue(dlg.GetPath());
}

void FileTransferDialog::OnRemove(wxCommandEvent&)
{
    wxArrayInt selected;
    fileList_->GetSelections(selected);
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
        statusLabel_->SetLabel(direction_ == TransferDirection::Send
                               ? "Transfer complete." : "Receive complete.");
        transferBtn_->Enable(!fileList_->IsEmpty());
        return;
    }

    const int idx = transferIndex_;
    const wxString itemLabel = fileList_->GetString(static_cast<unsigned int>(idx));
    statusLabel_->SetLabel(
        wxString::Format("%s %s... (%d / %d)",
                         direction_ == TransferDirection::Send
                             ? "Transferring" : "Receiving",
                         itemLabel, idx + 1, transferTotal_));

    const std::string itemPath = itemLabel.ToStdString();
    wxString dest = destCtrl_->GetValue().Trim();
    if (dest.IsEmpty()) dest = ".";
    const std::string destPath = dest.ToStdString();

    const auto callback = [this, idx](bool success, std::string error) {
        wxTheApp->CallAfter([this, success, idx, err = std::move(error)]() mutable {
            if (!success) {
                const wxString op = (direction_ == TransferDirection::Send)
                                    ? "transfer" : "receive";
                statusLabel_->SetLabel(
                    wxString::Format("%s failed.",
                                     direction_ == TransferDirection::Send
                                     ? "Transfer" : "Receive"));
                transferBtn_->Enable();
                wxMessageBox(
                    wxString::Format("Failed to %s '%s':\n\n%s",
                                     op,
                                     fileList_->GetString(
                                         static_cast<unsigned int>(idx)),
                                     wxString::FromUTF8(err)),
                    direction_ == TransferDirection::Send
                        ? "Transfer Failed" : "Receive Failed",
                    wxOK | wxICON_ERROR, this);
                return;
            }
            ++transferIndex_;
            TransferNext();
        });
    };

    if (direction_ == TransferDirection::Send)
        sm_.SendFile(sessionId_, itemPath, destPath, callback);
    else
        sm_.ReceiveFile(sessionId_, itemPath, destPath, callback);
}

} // namespace ui
