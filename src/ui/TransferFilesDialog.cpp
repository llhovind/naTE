#include "ui/TransferFilesDialog.h"
#include "ui/RemoteFileBrowserDialog.h"

#include <wx/app.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/string.h>

namespace ui {

static const wxString kLocalLabel = "Local (this computer)";

TransferFilesDialog::TransferFilesDialog(
    wxWindow*                   parent,
    term::session::SessionManager& sm,
    std::vector<std::pair<term::session::SessionId, std::string>> sessions,
    term::session::SessionId    preSelectedSrcId)
    : wxDialog(parent, wxID_ANY, "Transfer Files",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , sm_(sm)
    , alive_(std::make_shared<std::atomic<bool>>(true))
{
    // Build parallel id + label arrays: index 0 is always Local.
    sessionIds_.push_back(0);
    wxArrayString choices;
    choices.Add(kLocalLabel);
    for (auto& [id, label] : sessions) {
        sessionIds_.push_back(id);
        choices.Add(wxString::FromUTF8(label));
    }

    auto* outer = new wxBoxSizer(wxVERTICAL);

    // --- Source panel ---
    auto* srcBox = new wxStaticBoxSizer(wxVERTICAL, this, "Source");

    auto* srcSessionRow = new wxBoxSizer(wxHORIZONTAL);
    srcSessionRow->Add(new wxStaticText(srcBox->GetStaticBox(), wxID_ANY, "Session:"),
                       0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
    srcChoice_ = new wxChoice(srcBox->GetStaticBox(), wxID_ANY,
                              wxDefaultPosition, wxDefaultSize, choices);
    srcChoice_->SetSelection(0);
    srcSessionRow->Add(srcChoice_, 1);
    srcBox->Add(srcSessionRow, 0, wxEXPAND | wxALL, 6);

    auto* fileRow = new wxBoxSizer(wxHORIZONTAL);
    fileList_ = new wxListBox(srcBox->GetStaticBox(), wxID_ANY,
                              wxDefaultPosition, wxDefaultSize,
                              0, nullptr, wxLB_EXTENDED);
    fileRow->Add(fileList_, 1, wxEXPAND | wxRIGHT, 6);
    auto* fileCol = new wxBoxSizer(wxVERTICAL);
    addBtn_    = new wxButton(srcBox->GetStaticBox(), wxID_ANY, "Add Files...", wxDefaultPosition, wxDefaultSize);
    addBtn_->SetMinSize(wxSize(140, -1));
    removeBtn_ = new wxButton(srcBox->GetStaticBox(), wxID_ANY, "Remove", wxDefaultPosition, wxDefaultSize);
    removeBtn_->Disable();
    fileCol->Add(addBtn_,    0, wxBOTTOM, 4);
    fileCol->Add(removeBtn_, 0);
    fileRow->Add(fileCol, 0, wxALIGN_TOP);
    srcBox->Add(fileRow, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    outer->Add(srcBox, 1, wxEXPAND | wxALL, 10);

    // --- Destination panel ---
    auto* dstBox = new wxStaticBoxSizer(wxVERTICAL, this, "Destination");

    auto* dstSessionRow = new wxBoxSizer(wxHORIZONTAL);
    dstSessionRow->Add(new wxStaticText(dstBox->GetStaticBox(), wxID_ANY, "Session:"),
                       0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
    dstChoice_ = new wxChoice(dstBox->GetStaticBox(), wxID_ANY,
                              wxDefaultPosition, wxDefaultSize, choices);
    dstChoice_->SetSelection(0);
    dstSessionRow->Add(dstChoice_, 1);
    dstBox->Add(dstSessionRow, 0, wxEXPAND | wxALL, 6);

    auto* dstDirRow = new wxBoxSizer(wxHORIZONTAL);
    dstDirRow->Add(new wxStaticText(dstBox->GetStaticBox(), wxID_ANY, "Directory:"),
                   0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
    dstDirCtrl_ = new wxTextCtrl(dstBox->GetStaticBox(), wxID_ANY, wxGetCwd());
    browseDstBtn_ = new wxButton(dstBox->GetStaticBox(), wxID_ANY, "Browse...",
                                 wxDefaultPosition, wxDefaultSize);
    dstDirRow->Add(dstDirCtrl_, 1, wxRIGHT, 4);
    dstDirRow->Add(browseDstBtn_, 0);
    dstBox->Add(dstDirRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    outer->Add(dstBox, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // --- Status ---
    statusLabel_ = new wxStaticText(this, wxID_ANY, "Ready");
    outer->Add(statusLabel_, 0, wxLEFT | wxTOP | wxBOTTOM, 10);

    // --- Action buttons ---
    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    btnRow->AddStretchSpacer();
    transferBtn_ = new wxButton(this, wxID_ANY, "Transfer");
    transferBtn_->Disable();
    auto* closeBtn = new wxButton(this, wxID_CANCEL, "Close");
    btnRow->Add(transferBtn_, 0, wxRIGHT, 6);
    btnRow->Add(closeBtn, 0);
    outer->Add(btnRow, 0, wxALL, 10);

    SetSizerAndFit(outer);
    SetMinSize(wxSize(440, 360));

    // Pre-select source session if provided.
    if (preSelectedSrcId != 0) {
        for (size_t i = 0; i < sessionIds_.size(); ++i) {
            if (sessionIds_[i] == preSelectedSrcId) {
                srcChoice_->SetSelection(static_cast<int>(i));
                break;
            }
        }
    }

    // Bindings
    srcChoice_->Bind(wxEVT_CHOICE,  &TransferFilesDialog::OnSourceSessionChanged, this);
    dstChoice_->Bind(wxEVT_CHOICE,  &TransferFilesDialog::OnDestSessionChanged,   this);
    addBtn_->Bind(wxEVT_BUTTON,     &TransferFilesDialog::OnAddSourceFiles,        this);
    removeBtn_->Bind(wxEVT_BUTTON,  &TransferFilesDialog::OnRemoveSource,          this);
    browseDstBtn_->Bind(wxEVT_BUTTON, &TransferFilesDialog::OnBrowseDest,          this);
    transferBtn_->Bind(wxEVT_BUTTON, &TransferFilesDialog::OnTransfer,             this);
    fileList_->Bind(wxEVT_LISTBOX,  &TransferFilesDialog::OnListSelectionChanged,  this);

    // Initialise add button label to match the resolved source selection.
    addBtn_->SetLabel(SelectedSrcId() == 0 ? "Add Files..." : "Browse Remote...");
}

TransferFilesDialog::~TransferFilesDialog()
{
    // Invalidate before destruction so any in-flight transfer callback no-ops
    // instead of touching this freed dialog (see alive_).
    alive_->store(false, std::memory_order_release);
}

term::session::SessionId TransferFilesDialog::SelectedSrcId() const
{
    const int sel = srcChoice_->GetSelection();
    if (sel < 0 || sel >= static_cast<int>(sessionIds_.size())) return 0;
    return sessionIds_[static_cast<size_t>(sel)];
}

term::session::SessionId TransferFilesDialog::SelectedDstId() const
{
    const int sel = dstChoice_->GetSelection();
    if (sel < 0 || sel >= static_cast<int>(sessionIds_.size())) return 0;
    return sessionIds_[static_cast<size_t>(sel)];
}

void TransferFilesDialog::OnSourceSessionChanged(wxCommandEvent&)
{
    const bool isLocal = (SelectedSrcId() == 0);
    addBtn_->SetLabel(isLocal ? "Add Files..." : "Browse Remote...");
    fileList_->Clear();
    transferBtn_->Disable();
    removeBtn_->Disable();
}

void TransferFilesDialog::OnDestSessionChanged(wxCommandEvent&)
{
    const bool isLocal = (SelectedDstId() == 0);
    dstDirCtrl_->SetValue(isLocal ? wxGetCwd() : ".");
}

void TransferFilesDialog::OnAddSourceFiles(wxCommandEvent&)
{
    const term::session::SessionId srcId = SelectedSrcId();

    if (srcId == 0) {
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
    } else {
        const std::string remote = sm_.GetRemoteDescription(srcId);
        const std::string cwd    = sm_.GetCurrentWorkingDir(srcId);
        RemoteFileBrowserDialog dlg(this, srcId, sm_, remote, "Add Selected",
                                    cwd.empty() ? "." : cwd, BrowseMode::Files);
        const int rc = dlg.ShowModal();
        wxTheApp->CallAfter([this]{ Raise(); SetFocus(); });
        if (rc != wxID_OK) return;
        for (const auto& path : dlg.GetSelectedPaths()) {
            const wxString wp = wxString::FromUTF8(path);
            if (fileList_->FindString(wp) == wxNOT_FOUND)
                fileList_->Append(wp);
        }
    }

    transferBtn_->Enable(!fileList_->IsEmpty());
}

void TransferFilesDialog::OnRemoveSource(wxCommandEvent&)
{
    wxArrayInt selected;
    fileList_->GetSelections(selected);
    for (int i = static_cast<int>(selected.size()) - 1; i >= 0; --i)
        fileList_->Delete(static_cast<unsigned int>(selected[i]));
    removeBtn_->Disable();
    transferBtn_->Enable(!fileList_->IsEmpty());
}

void TransferFilesDialog::OnListSelectionChanged(wxCommandEvent&)
{
    wxArrayInt sel;
    fileList_->GetSelections(sel);
    removeBtn_->Enable(!sel.IsEmpty());
}

void TransferFilesDialog::OnBrowseDest(wxCommandEvent&)
{
    const term::session::SessionId dstId = SelectedDstId();

    if (dstId == 0) {
        wxDirDialog dlg(this, "Select Destination Folder",
                        dstDirCtrl_->GetValue(),
                        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK)
            dstDirCtrl_->SetValue(dlg.GetPath());
    } else {
        const std::string remote  = sm_.GetRemoteDescription(dstId);
        const std::string current = dstDirCtrl_->GetValue().Trim().ToStdString();
        const std::string cwd    = sm_.GetCurrentWorkingDir(dstId);
        const std::string initial = (!current.empty() && current != ".")
                                    ? current
                                    : (!cwd.empty() ? cwd : ".");
        RemoteFileBrowserDialog dlg(this, dstId, sm_, remote, "",
                                    initial, BrowseMode::Directory);
        const int rc = dlg.ShowModal();
        wxTheApp->CallAfter([this]{ Raise(); SetFocus(); });
        if (rc != wxID_OK) return;
        const auto& paths = dlg.GetSelectedPaths();
        if (!paths.empty())
            dstDirCtrl_->SetValue(wxString::FromUTF8(paths.front()));
    }
}

void TransferFilesDialog::OnTransfer(wxCommandEvent&)
{
    transferBtn_->Disable();
    transferIndex_ = 0;
    transferTotal_ = static_cast<int>(fileList_->GetCount());
    TransferNext();
}

void TransferFilesDialog::TransferNext()
{
    if (transferIndex_ >= transferTotal_) {
        statusLabel_->SetLabel("Transfer complete.");
        transferBtn_->Enable(!fileList_->IsEmpty());
        return;
    }

    const int idx = transferIndex_;
    const wxString itemLabel =
        fileList_->GetString(static_cast<unsigned int>(idx));
    statusLabel_->SetLabel(
        wxString::Format("Transferring %s... (%d / %d)",
                         itemLabel, idx + 1, transferTotal_));

    const std::string srcPath = itemLabel.ToStdString();
    wxString dest = dstDirCtrl_->GetValue().Trim();
    if (dest.IsEmpty()) dest = ".";
    const std::string dstDir = dest.ToStdString();

    const term::session::SessionId srcId = SelectedSrcId();
    const term::session::SessionId dstId = SelectedDstId();

    std::weak_ptr<std::atomic<bool>> weakAlive = alive_;
    const auto callback = [this, weakAlive, idx](bool success, std::string error) {
        wxTheApp->CallAfter(
            [this, weakAlive, success, idx, err = std::move(error)]() mutable {
            auto alive = weakAlive.lock();
            if (!alive || !alive->load(std::memory_order_acquire))
                return;
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
    };

    sm_.TransferFileBetweenSessions(srcId, srcPath, dstId, dstDir, callback);
}

} // namespace ui
