#pragma once

#include "config/Config.h"
#include "transport/IRemoteFileSystem.h"

#include <atomic>
#include <memory>
#include <string>

#include <wx/dialog.h>
#include <wx/stattext.h>

namespace ui {

// Read-only metadata for one remote file.
//
// Everything except the symlink target comes from the directory listing that
// is already in hand, so the dialog opens populated. A symlink's target needs
// one readlink round trip, which resolves into the open dialog rather than
// delaying it — and which is why listings do not resolve links themselves.
class FilePropertiesDialog : public wxDialog {
public:
    FilePropertiesDialog(wxWindow* parent,
                         const AppConfig& cfg,
                         const term::transport::FileInfo& info,
                         const std::string& fullPath,
                         term::transport::IRemoteFileSystem& remote);

    ~FilePropertiesDialog() override;

private:
    // Appends a label/value pair to the grid; returns the value control so a
    // caller can update it once an asynchronous answer arrives.
    wxStaticText* AddRow(wxWindow* parent, wxSizer* grid,
                         const wxString& label, const wxString& value);

    wxStaticText* linkTargetValue_ = nullptr;

    // Cleared on destruction; the readlink continuation checks it before
    // touching this dialog, which may have been closed in the meantime.
    std::shared_ptr<std::atomic<bool>> alive_;
};

} // namespace ui
