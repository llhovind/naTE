#pragma once

#include "config/Config.h"
#include "transport/IRemoteFileSystem.h"

#include <atomic>
#include <memory>
#include <string>

#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace ui {

// Metadata for one remote file, with its permission bits editable.
//
// Everything except the symlink target comes from the directory listing that
// is already in hand, so the dialog opens populated. A symlink's target needs
// one readlink round trip, which resolves into the open dialog rather than
// delaying it — and which is why listings do not resolve links themselves.
//
// The dialog does not apply the permission change itself. It reports the mode
// the user chose and the caller writes it, so the write and the refresh that
// must follow it stay in one place.
class FilePropertiesDialog : public wxDialog {
public:
    FilePropertiesDialog(wxWindow* parent,
                         const AppConfig& cfg,
                         const term::transport::FileInfo& info,
                         const std::string& fullPath,
                         term::transport::IRemoteFileSystem& remote);

    ~FilePropertiesDialog() override;

    // The permission bits the user selected. Meaningful after wxID_OK.
    uint32_t SelectedMode() const noexcept { return selectedMode_; }
    // True when those differ from what the file had on open, so a caller can
    // skip a pointless round trip.
    bool PermissionsChanged() const noexcept { return selectedMode_ != originalMode_; }

private:
    void BuildPermissionEditor(wxSizer* outer);
    // The octal field and the checkbox grid are two views of one value; each
    // handler rewrites the other, and syncing_ stops that from echoing.
    void SyncFromOctal();
    void SyncFromCheckboxes();
    void PushModeToControls();
    // Appends a label/value pair to the grid; returns the value control so a
    // caller can update it once an asynchronous answer arrives.
    wxStaticText* AddRow(wxWindow* parent, wxSizer* grid,
                         const wxString& label, const wxString& value);

    wxStaticText* linkTargetValue_ = nullptr;

    wxTextCtrl* octalCtrl_ = nullptr;
    // Row-major: owner rwx, group rwx, other rwx.
    wxCheckBox* permBoxes_[9] = {};
    wxColour    termBg_;
    wxColour    termFg_;
    uint32_t    originalMode_ = 0;
    uint32_t    selectedMode_ = 0;
    bool        syncing_      = false;

    // Cleared on destruction; the readlink continuation checks it before
    // touching this dialog, which may have been closed in the meantime.
    std::shared_ptr<std::atomic<bool>> alive_;
};

} // namespace ui
