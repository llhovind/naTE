#include "ui/RemoteFileBrowserDialog.h"

#include "ui/ColorUtils.h"
#include "ui/StringUtils.h"

#include <wx/app.h>
#include <wx/sizer.h>

namespace ui {

namespace {
constexpr int kInitialWidth  = 620;
constexpr int kInitialHeight = 460;
constexpr int kMinWidth      = 420;
constexpr int kMinHeight     = 320;
} // namespace

RemoteFileBrowserDialog::RemoteFileBrowserDialog(
    wxWindow* parent,
    term::session::SessionId sessionId,
    term::session::SessionManager& sm,
    const std::string& remoteDescription,
    const wxString& confirmLabel,
    const std::string& initialPath)
    : wxDialog(parent, wxID_ANY,
               remoteDescription.empty()
                   ? wxString("Browse Remote Files")
                   : wxString::Format("Browse %s",
                                      wxString::FromUTF8(remoteDescription)),
               wxDefaultPosition, wxSize(kInitialWidth, kInitialHeight),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , sm_(sm)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* pathRow = new wxBoxSizer(wxHORIZONTAL);
    upBtn_ = new wxButton(this, wxID_ANY, "Up", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    pathCtrl_ = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(initialPath),
                               wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    auto* goBtn = new wxButton(this, wxID_ANY, "Go", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    pathRow->Add(upBtn_,    0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 4);
    pathRow->Add(pathCtrl_, 1, wxRIGHT | wxALIGN_CENTER_VERTICAL, 4);
    pathRow->Add(goBtn,     0, wxALIGN_CENTER_VERTICAL);
    outer->Add(pathRow, 0, wxEXPAND | wxALL, 10);

    fileList_ = new RemoteFileListCtrl(
        this,
        [this]() -> const term::fs::DirModel* {
            return controller_ ? &controller_->Model() : nullptr;
        },
        wxLC_SINGLE_SEL);
    fileList_->InsertStandardColumns();
    outer->Add(fileList_, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    statusLabel_ = new wxStaticText(this, wxID_ANY, "Loading...");
    outer->Add(statusLabel_, 0, wxLEFT | wxTOP | wxBOTTOM, 10);

    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    btnRow->AddStretchSpacer();
    confirmBtn_ = new wxButton(this, wxID_ANY, confirmLabel);
    auto* closeBtn = new wxButton(this, wxID_CANCEL, "Close");
    btnRow->Add(confirmBtn_, 0, wxRIGHT, 6);
    btnRow->Add(closeBtn,    0);
    outer->Add(btnRow, 0, wxALL, 10);

    SetSizerAndFit(outer);
    SetMinSize(wxSize(kMinWidth, kMinHeight));

    upBtn_->Bind(wxEVT_BUTTON,        &RemoteFileBrowserDialog::OnUp, this);
    goBtn->Bind(wxEVT_BUTTON,         &RemoteFileBrowserDialog::OnGo, this);
    pathCtrl_->Bind(wxEVT_TEXT_ENTER, &RemoteFileBrowserDialog::OnGo, this);
    confirmBtn_->Bind(wxEVT_BUTTON,   &RemoteFileBrowserDialog::OnConfirm, this);
    fileList_->Bind(wxEVT_LIST_ITEM_ACTIVATED,
                    &RemoteFileBrowserDialog::OnItemActivated, this);
    const auto onSelect = [this](wxListEvent& evt) { UpdateControls(); evt.Skip(); };
    fileList_->Bind(wxEVT_LIST_ITEM_SELECTED,   onSelect);
    fileList_->Bind(wxEVT_LIST_ITEM_DESELECTED, onSelect);

    term::transport::IRemoteFileSystem* remote = sm_.GetRemoteFileSystem(sessionId);
    if (!remote) {
        statusLabel_->SetLabel("This session has no remote filesystem.");
        UpdateControls();
        return;
    }

    controller_ = std::make_unique<term::fs::ExplorerController>(
        *remote,
        [](std::function<void()> fn) { wxTheApp->CallAfter(std::move(fn)); });
    controller_->SetListener(this);
    // Dotfiles stay visible here, unlike in the explorer. This dialog exists
    // largely to pick a file to edit, and .bashrc, .vimrc and friends are
    // exactly the files an administrator reaches for — hiding them would make
    // the common case impossible rather than merely tidier.
    controller_->Model().SetShowHidden(true);
    controller_->NavigateTo(initialPath.empty() ? "." : initialPath);
}

// ---------------------------------------------------------------------------
// Controller notifications
// ---------------------------------------------------------------------------

void RemoteFileBrowserDialog::OnExplorerLoadingChanged(bool loading)
{
    if (loading) statusLabel_->SetLabel("Loading...");
    UpdateControls();
}

void RemoteFileBrowserDialog::OnExplorerPathChanged(const std::string& path)
{
    pathCtrl_->ChangeValue(wxString::FromUTF8(path));
}

void RemoteFileBrowserDialog::OnExplorerContentsChanged()
{
    if (!controller_) return;
    const auto& model = controller_->Model();

    const long count = static_cast<long>(model.VisibleCount());
    fileList_->SetItemCount(count);
    if (count > 0) fileList_->RefreshItems(0, count - 1);
    else           fileList_->Refresh();

    // A listing that failed partway still shows its rows, so the status line
    // is where the incompleteness has to be admitted.
    if (model.IsPartial())
        statusLabel_->SetLabel(
            wxString::Format("%zu item(s) - partial: %s", model.VisibleCount(),
                             DecodeForDisplay(model.Error().message)));
    else if (model.HasError())
        statusLabel_->SetLabel("Error: " + DecodeForDisplay(model.Error().message));
    else
        statusLabel_->SetLabel(wxString::Format("%zu item(s)", model.VisibleCount()));

    UpdateControls();
}

void RemoteFileBrowserDialog::UpdateControls()
{
    const bool live    = controller_ != nullptr;
    const bool loading = live && controller_->IsLoading();

    upBtn_->Enable(live && !loading && controller_->CurrentPath() != "/");
    pathCtrl_->Enable(live);
    confirmBtn_->Enable(live && !loading &&
                        fileList_->GetSelectedItemCount() > 0);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void RemoteFileBrowserDialog::OnGo(wxCommandEvent&)
{
    if (controller_) controller_->NavigateTo(pathCtrl_->GetValue().Trim().ToStdString());
}

void RemoteFileBrowserDialog::OnUp(wxCommandEvent&)
{
    if (controller_) controller_->NavigateUp();
}

void RemoteFileBrowserDialog::OnItemActivated(wxListEvent& evt)
{
    if (!controller_) return;
    const auto row = static_cast<size_t>(evt.GetIndex());

    // The controller decides what activation means, including resolving a
    // symlink to find out whether it is a doorway or a file.
    controller_->Activate(row,
        [this](term::fs::ActivationResult result, std::string path,
               term::transport::FsError err) {
            switch (result) {
                case term::fs::ActivationResult::Navigated:
                    break;
                case term::fs::ActivationResult::IsFile:
                    // Double-clicking a file picks it outright.
                    selectedPath_ = std::move(path);
                    EndModal(wxID_OK);
                    break;
                case term::fs::ActivationResult::Failed:
                    statusLabel_->SetLabel(
                        wxString::Format("Cannot open '%s': %s",
                                         DecodeForDisplay(path),
                                         DecodeForDisplay(err.message)));
                    break;
            }
        });
}

void RemoteFileBrowserDialog::OnConfirm(wxCommandEvent&)
{
    if (!controller_) return;

    const long item = fileList_->GetNextItem(-1, wxLIST_NEXT_ALL,
                                             wxLIST_STATE_SELECTED);
    if (item == wxNOT_FOUND) return;
    const auto row = static_cast<size_t>(item);
    if (row >= controller_->Model().VisibleCount()) return;
    // Directories are navigated, not picked.
    if (controller_->Model().At(row).isDir) return;

    selectedPath_ = controller_->PathOf(row);
    EndModal(wxID_OK);
}

} // namespace ui
