#pragma once
#include <wx/dialog.h>
#include <string>
#include <vector>

class wxButton;
class wxListBox;

namespace ui {

class SnapshotManagerDialog : public wxDialog {
public:
    SnapshotManagerDialog(wxWindow* parent, std::vector<std::string> names);

    std::string              GetSelectedName() const;
    std::vector<std::string> GetDeletedNames() const { return m_deletedNames; }

private:
    void OnLoad(wxCommandEvent&);
    void OnDelete(wxCommandEvent&);
    void OnSelectionChanged(wxCommandEvent&);

    wxListBox*               m_listBox   = nullptr;
    wxButton*                m_loadBtn   = nullptr;
    wxButton*                m_deleteBtn = nullptr;
    std::vector<std::string> m_deletedNames;
};

} // namespace ui
