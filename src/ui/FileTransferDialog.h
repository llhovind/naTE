#pragma once

#include "session/SessionManager.h"
#include "session/Session.h"

#include <string>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/listbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace ui {

enum class TransferDirection { Send, Receive };

class FileTransferDialog : public wxDialog {
public:
    FileTransferDialog(wxWindow* parent,
                       term::session::SessionId sessionId,
                       term::session::SessionManager& sm,
                       const std::string& remoteDescription,
                       TransferDirection direction = TransferDirection::Send);

private:
    void OnAddFiles(wxCommandEvent&);
    void OnBrowseRemote(wxCommandEvent&);
    void OnBrowseLocalDest(wxCommandEvent&);
    void OnRemove(wxCommandEvent&);
    void OnTransfer(wxCommandEvent&);
    void OnListSelectionChanged(wxCommandEvent&);

    // Starts the next pending transfer; called after each successful file.
    void TransferNext();

    term::session::SessionId       sessionId_;
    term::session::SessionManager& sm_;
    TransferDirection              direction_;

    wxListBox*    fileList_       = nullptr;
    wxTextCtrl*   destCtrl_       = nullptr;
    wxButton*     transferBtn_    = nullptr;
    wxButton*     removeBtn_      = nullptr;
    wxStaticText* statusLabel_    = nullptr;

    // Indices into fileList_ for the in-progress transfer sequence.
    int transferIndex_  = 0;
    int transferTotal_  = 0;
};

} // namespace ui
