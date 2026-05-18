#include "ui/RemoteFileBrowserDialog.h"

#include <wx/app.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>

namespace ui {

RemoteFileBrowserDialog::RemoteFileBrowserDialog(
    wxWindow* parent,
    term::session::SessionId sessionId,
    term::session::SessionManager& sm,
    const std::string& remoteDescription)
    : wxDialog(parent, wxID_ANY,
               remoteDescription.empty()
                   ? wxString("Browse Remote Files")
                   : wxString::Format("Browse %s",
                                      wxString::FromUTF8(remoteDescription)),
               wxDefaultPosition, wxSize(560, 420),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , sessionId_(sessionId)
    , sm_(sm)
    , currentPath_("~")
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    // --- Path bar ---
    auto* pathRow = new wxBoxSizer(wxHORIZONTAL);
    upBtn_ = new wxButton(this, wxID_ANY, "Up", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    pathCtrl_ = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(currentPath_),
                               wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    auto* goBtn = new wxButton(this, wxID_ANY, "Go", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    pathRow->Add(upBtn_,    0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 4);
    pathRow->Add(pathCtrl_, 1, wxRIGHT | wxALIGN_CENTER_VERTICAL, 4);
    pathRow->Add(goBtn,     0, wxALIGN_CENTER_VERTICAL);
    outer->Add(pathRow, 0, wxEXPAND | wxALL, 10);

    // --- File list ---
    fileList_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxLC_REPORT | wxLC_VRULES | wxLC_HRULES);
    fileList_->InsertColumn(0, "Name",        wxLIST_FORMAT_LEFT,  260);
    fileList_->InsertColumn(1, "Size",        wxLIST_FORMAT_RIGHT,  80);
    fileList_->InsertColumn(2, "Permissions", wxLIST_FORMAT_LEFT,  120);
    outer->Add(fileList_, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // --- Status ---
    statusLabel_ = new wxStaticText(this, wxID_ANY, "Loading...");
    outer->Add(statusLabel_, 0, wxLEFT | wxTOP | wxBOTTOM, 10);

    // --- Buttons ---
    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    btnRow->AddStretchSpacer();
    addBtn_      = new wxButton(this, wxID_ANY, "Add Selected");
    auto* closeBtn = new wxButton(this, wxID_CANCEL, "Close");
    addBtn_->Disable();
    btnRow->Add(addBtn_,   0, wxRIGHT, 6);
    btnRow->Add(closeBtn,  0);
    outer->Add(btnRow, 0, wxALL, 10);

    SetSizerAndFit(outer);
    SetMinSize(wxSize(400, 300));

    upBtn_->Bind(wxEVT_BUTTON,       &RemoteFileBrowserDialog::OnUp,         this);
    goBtn->Bind(wxEVT_BUTTON,        &RemoteFileBrowserDialog::OnGo,         this);
    pathCtrl_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& evt){ OnGo(evt); });
    fileList_->Bind(wxEVT_LIST_ITEM_ACTIVATED, &RemoteFileBrowserDialog::OnItemActivated, this);
    fileList_->Bind(wxEVT_LIST_ITEM_SELECTED,
        [this](wxListEvent&){ addBtn_->Enable(fileList_->GetSelectedItemCount() > 0); });
    fileList_->Bind(wxEVT_LIST_ITEM_DESELECTED,
        [this](wxListEvent&){ addBtn_->Enable(fileList_->GetSelectedItemCount() > 0); });
    addBtn_->Bind(wxEVT_BUTTON, &RemoteFileBrowserDialog::OnAdd, this);

    Navigate(currentPath_);
}

void RemoteFileBrowserDialog::Navigate(const std::string& path)
{
    if (loading_) return;
    loading_ = true;
    currentPath_ = path;
    pathCtrl_->SetValue(wxString::FromUTF8(currentPath_));
    upBtn_->Disable();
    addBtn_->Disable();
    fileList_->DeleteAllItems();
    statusLabel_->SetLabel("Loading...");

    sm_.ListRemoteDirectory(sessionId_, currentPath_,
        [this](std::vector<term::transport::RemoteDirEntry> entries, std::string error) {
            wxTheApp->CallAfter([this,
                                 es  = std::move(entries),
                                 err = std::move(error)]() mutable {
                loading_ = false;
                if (!err.empty()) {
                    statusLabel_->SetLabel(wxString::Format("Error: %s",
                                           wxString::FromUTF8(err)));
                    upBtn_->Enable(currentPath_ != "/");
                    return;
                }
                currentEntries_ = std::move(es);
                PopulateList(currentEntries_);
                upBtn_->Enable(currentPath_ != "/");
            });
        });
}

void RemoteFileBrowserDialog::PopulateList(
    const std::vector<term::transport::RemoteDirEntry>& entries)
{
    fileList_->DeleteAllItems();
    for (const auto& e : entries) {
        const long idx = fileList_->InsertItem(
            fileList_->GetItemCount(),
            wxString::FromUTF8(e.isDir ? e.name + "/" : e.name));
        fileList_->SetItem(idx, 1,
            e.isDir ? wxString("-") : wxString::Format("%llu",
                static_cast<unsigned long long>(e.size)));
        fileList_->SetItem(idx, 2, wxString::FromUTF8(e.permissions));
    }
    statusLabel_->SetLabel(wxString::Format("%zu item(s)",
                                            entries.size()));
}

std::string RemoteFileBrowserDialog::FullPath(const std::string& name) const
{
    if (currentPath_.empty() || currentPath_.back() == '/')
        return currentPath_ + name;
    return currentPath_ + "/" + name;
}

std::string RemoteFileBrowserDialog::ParentPath() const
{
    const auto& p = currentPath_;
    if (p == "~" || p == "/") return "/";
    const auto pos = p.rfind('/');
    if (pos == std::string::npos) return "~";
    if (pos == 0) return "/";
    return p.substr(0, pos);
}

void RemoteFileBrowserDialog::OnGo(wxCommandEvent&)
{
    Navigate(pathCtrl_->GetValue().Trim().ToStdString());
}

void RemoteFileBrowserDialog::OnUp(wxCommandEvent&)
{
    Navigate(ParentPath());
}

void RemoteFileBrowserDialog::OnItemActivated(wxListEvent& evt)
{
    const long idx = evt.GetIndex();
    if (idx < 0 || idx >= static_cast<long>(currentEntries_.size())) return;
    const auto& entry = currentEntries_[static_cast<size_t>(idx)];
    if (entry.isDir) Navigate(FullPath(entry.name));
}

void RemoteFileBrowserDialog::OnAdd(wxCommandEvent&)
{
    long item = -1;
    while (true) {
        item = fileList_->GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (item == wxNOT_FOUND) break;
        if (item >= static_cast<long>(currentEntries_.size())) continue;
        const auto& entry = currentEntries_[static_cast<size_t>(item)];
        if (!entry.isDir) {
            const std::string path = FullPath(entry.name);
            if (std::find(selectedPaths_.begin(), selectedPaths_.end(), path)
                == selectedPaths_.end())
                selectedPaths_.push_back(path);
        }
    }
    if (!selectedPaths_.empty()) EndModal(wxID_OK);
}

} // namespace ui
