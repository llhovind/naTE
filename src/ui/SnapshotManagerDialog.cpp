#include "ui/SnapshotManagerDialog.h"
#include <wx/button.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>

namespace ui {

SnapshotManagerDialog::SnapshotManagerDialog(wxWindow* parent, std::vector<std::string> names)
    : wxDialog(parent, wxID_ANY, "Open Saved Snapshot",
               wxDefaultPosition, wxSize(380, 280))
{
    wxArrayString items;
    for (const auto& n : names)
        items.Add(wxString::FromUTF8(n));

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    m_listBox = new wxListBox(this, wxID_ANY,
                              wxDefaultPosition, wxDefaultSize,
                              items, wxLB_SINGLE);
    sizer->Add(m_listBox, 1, wxEXPAND | wxALL, 8);

    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    m_loadBtn   = new wxButton(this, wxID_OK,     "Load");
    m_deleteBtn = new wxButton(this, wxID_ANY,    "Delete");
    auto* closeBtn = new wxButton(this, wxID_CANCEL, "Close");
    m_loadBtn->Enable(false);
    m_deleteBtn->Enable(false);
    btnSizer->AddStretchSpacer();
    btnSizer->Add(m_loadBtn,   0, wxRIGHT, 4);
    btnSizer->Add(m_deleteBtn, 0, wxRIGHT, 4);
    btnSizer->Add(closeBtn,    0);
    sizer->Add(btnSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    SetSizer(sizer);

    m_listBox->Bind(wxEVT_LISTBOX,  &SnapshotManagerDialog::OnSelectionChanged, this);
    m_loadBtn->Bind(wxEVT_BUTTON,   &SnapshotManagerDialog::OnLoad,             this);
    m_deleteBtn->Bind(wxEVT_BUTTON, &SnapshotManagerDialog::OnDelete,           this);
}

std::string SnapshotManagerDialog::GetSelectedName() const
{
    const int sel = m_listBox->GetSelection();
    if (sel == wxNOT_FOUND) return {};
    return m_listBox->GetString(sel).ToStdString();
}

void SnapshotManagerDialog::OnSelectionChanged(wxCommandEvent&)
{
    const bool hasSel = m_listBox->GetSelection() != wxNOT_FOUND;
    m_loadBtn->Enable(hasSel);
    m_deleteBtn->Enable(hasSel);
}

void SnapshotManagerDialog::OnLoad(wxCommandEvent&)
{
    if (m_listBox->GetSelection() != wxNOT_FOUND)
        EndModal(wxID_OK);
}

void SnapshotManagerDialog::OnDelete(wxCommandEvent&)
{
    const int sel = m_listBox->GetSelection();
    if (sel == wxNOT_FOUND) return;
    const std::string name = m_listBox->GetString(sel).ToStdString();
    if (wxMessageBox("Delete snapshot '" + name + "'?",
                     "Confirm Delete", wxYES_NO | wxICON_QUESTION, this) != wxYES)
        return;
    m_deletedNames.push_back(name);
    m_listBox->Delete(static_cast<unsigned>(sel));
    m_loadBtn->Enable(false);
    m_deleteBtn->Enable(false);
}

} // namespace ui
