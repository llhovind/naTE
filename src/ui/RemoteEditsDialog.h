#pragma once
#include "ui/RemoteEditManager.h"

#include <vector>

#include <wx/dialog.h>

class wxButton;
class wxListBox;
class wxStaticText;

namespace ui {

// Lists the remote edits in progress and lets the user end one.
//
// This exists because nothing else can decide when an edit is over. The editor
// is launched detached, and its exit means "the user closed the buffer" for
// `code --wait` but "the file was handed to an already-running instance" for
// plain `code` — so acting on it would delete a working copy out from under an
// editor that has only just opened it. The user is the only reliable source of
// that signal, and this is where they give it.
class RemoteEditsDialog : public wxDialog {
public:
    RemoteEditsDialog(wxWindow* parent, RemoteEditManager& mgr);

private:
    void Repopulate();
    void OnSelectionChanged(wxCommandEvent&);
    void OnFinish(wxCommandEvent&);

    RemoteEditManager&      mgr_;
    wxListBox*              list_      = nullptr;
    wxButton*               finishBtn_ = nullptr;
    wxStaticText*           hint_      = nullptr;
    // Parallel to the list box: the row the user picks has to resolve back to
    // the local path, which is the edit's identity and is not on screen.
    std::vector<ActiveEdit>  edits_;
};

} // namespace ui
