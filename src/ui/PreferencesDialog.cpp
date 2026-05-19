#include "ui/PreferencesDialog.h"
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/fontdlg.h>
#include <wx/gbsizer.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace {
    constexpr int kBrowseFontId = wxID_HIGHEST + 600;
}

PreferencesDialog::PreferencesDialog(wxWindow* parent,
                                     const AppConfig& current,
                                     const std::vector<ColorScheme>& themes)
    : wxDialog(parent, wxID_ANY, "Preferences",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      result_(current),
      themes_(themes)
{
    auto* nb = new wxNotebook(this, wxID_ANY);

    // ---- Appearance page -------------------------------------------------------
    auto* appPage  = new wxPanel(nb);
    auto* appSizer = new wxGridBagSizer(6, 8);
    appSizer->SetEmptyCellSize({0, 0});

    int row = 0;

    // Color theme — populated from the scanned themes list.
    appSizer->Add(new wxStaticText(appPage, wxID_ANY, "Color theme:"),
                  {row, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    {
        wxArrayString labels;
        int preselect = 0;
        for (int i = 0; i < static_cast<int>(themes_.size()); ++i) {
            labels.Add(wxString::FromUTF8(themes_[i].displayName));
            if (themes_[i].stem == current.themeName) preselect = i;
        }
        if (labels.IsEmpty()) labels.Add("(no themes found)");
        m_themeChoice = new wxChoice(appPage, wxID_ANY,
                                     wxDefaultPosition, wxDefaultSize, labels);
        m_themeChoice->SetSelection(preselect);
    }
    appSizer->Add(m_themeChoice, {row, 1}, {1, 2}, wxEXPAND);
    ++row;

    // Font family
    appSizer->Add(new wxStaticText(appPage, wxID_ANY, "Font family:"),
                  {row, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    m_familyCtrl = new wxTextCtrl(appPage, wxID_ANY,
                                  wxString::FromUTF8(current.fontFamily),
                                  wxDefaultPosition, {200, -1});
    appSizer->Add(m_familyCtrl, {row, 1}, {1, 1}, wxEXPAND);
    appSizer->Add(new wxButton(appPage, kBrowseFontId, "Browse..."), {row, 2}, {1, 1});
    ++row;

    // Font size
    appSizer->Add(new wxStaticText(appPage, wxID_ANY, "Font size (pt):"),
                  {row, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    m_sizeCtrl = new wxSpinCtrl(appPage, wxID_ANY,
                                wxEmptyString, wxDefaultPosition, {80, -1},
                                wxSP_ARROW_KEYS, 6, 72, current.fontSize);
    appSizer->Add(m_sizeCtrl, {row, 1}, {1, 1});
    ++row;

    // Padding
    appSizer->Add(new wxStaticText(appPage, wxID_ANY, "Padding (px):"),
                  {row, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    m_paddingCtrl = new wxSpinCtrl(appPage, wxID_ANY,
                                   wxEmptyString, wxDefaultPosition, {80, -1},
                                   wxSP_ARROW_KEYS, 0, 32, current.padding);
    appSizer->Add(m_paddingCtrl, {row, 1}, {1, 1});
    ++row;

    appSizer->AddGrowableCol(1);
    auto* appOuter = new wxBoxSizer(wxVERTICAL);
    appOuter->Add(appSizer, 1, wxEXPAND | wxALL, 12);
    appPage->SetSizer(appOuter);
    nb->AddPage(appPage, "Appearance");

    // ---- Behavior page --------------------------------------------------------
    auto* behPage  = new wxPanel(nb);
    auto* behSizer = new wxGridBagSizer(6, 8);
    behSizer->Add(new wxStaticText(behPage, wxID_ANY, "Encoding:"),
                  {0, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    behSizer->Add(new wxStaticText(behPage, wxID_ANY,
                                   wxString::FromUTF8(current.encoding) +
                                   "  (only UTF-8 supported currently)"),
                  {0, 1}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    auto* behOuter = new wxBoxSizer(wxVERTICAL);
    behOuter->Add(behSizer, 1, wxEXPAND | wxALL, 12);
    behPage->SetSizer(behOuter);
    nb->AddPage(behPage, "Behavior");

    // ---- Session page ---------------------------------------------------------
    auto* sesPage  = new wxPanel(nb);
    auto* sesSizer = new wxGridBagSizer(6, 8);
    sesSizer->SetEmptyCellSize({0, 0});

    sesSizer->Add(new wxStaticText(sesPage, wxID_ANY, "Default shell:"),
                  {0, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    m_shellCtrl = new wxTextCtrl(sesPage, wxID_ANY,
                                 wxString::FromUTF8(current.defaultShell),
                                 wxDefaultPosition, {260, -1});
    m_shellCtrl->SetHint("e.g. /bin/bash  (empty = $SHELL)");
    sesSizer->Add(m_shellCtrl, {0, 1}, {1, 1}, wxEXPAND);

    sesSizer->Add(new wxStaticText(sesPage, wxID_ANY, "Default working dir:"),
                  {1, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    m_workDirCtrl = new wxTextCtrl(sesPage, wxID_ANY,
                                   wxString::FromUTF8(current.defaultWorkingDir),
                                   wxDefaultPosition, {260, -1});
    m_workDirCtrl->SetHint("e.g. ~  (empty = inherit launcher cwd)");
    sesSizer->Add(m_workDirCtrl, {1, 1}, {1, 1}, wxEXPAND);

    sesSizer->AddGrowableCol(1);
    auto* sesOuter = new wxBoxSizer(wxVERTICAL);
    sesOuter->Add(sesSizer, 1, wxEXPAND | wxALL, 12);
    sesPage->SetSizer(sesOuter);
    nb->AddPage(sesPage, "Session");

    // ---- Dialog layout --------------------------------------------------------
    auto* dlgSizer = new wxBoxSizer(wxVERTICAL);
    dlgSizer->Add(nb, 1, wxEXPAND | wxALL, 8);
    dlgSizer->Add(CreateButtonSizer(wxOK | wxCANCEL), 0,
                  wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    SetSizerAndFit(dlgSizer);
    SetMinClientSize({440, 260});

    Bind(wxEVT_BUTTON, &PreferencesDialog::OnBrowseFont, this, kBrowseFontId);
    Bind(wxEVT_BUTTON, &PreferencesDialog::OnOk,         this, wxID_OK);
}

void PreferencesDialog::OnBrowseFont(wxCommandEvent&)
{
    wxFontData data;
    data.SetAllowSymbols(false);
    data.SetInitialFont(wxFont(m_sizeCtrl->GetValue(),
                               wxFONTFAMILY_TELETYPE,
                               wxFONTSTYLE_NORMAL,
                               wxFONTWEIGHT_NORMAL,
                               false,
                               wxString::FromUTF8(m_familyCtrl->GetValue().ToStdString())));
    wxFontDialog dlg(this, data);
    if (dlg.ShowModal() == wxID_OK) {
        const wxFont selected = dlg.GetFontData().GetChosenFont();
        m_familyCtrl->SetValue(selected.GetFaceName());
        m_sizeCtrl->SetValue(selected.GetPointSize());
    }
}

void PreferencesDialog::OnOk(wxCommandEvent& evt)
{
    const int sel = m_themeChoice->GetSelection();
    if (sel >= 0 && sel < static_cast<int>(themes_.size())) {
        result_.themeName  = themes_[sel].stem;
        result_.textColour = themes_[sel].foreground;
        result_.bgColour   = themes_[sel].background;
    }

    result_.fontFamily       = m_familyCtrl->GetValue().ToStdString();
    result_.fontSize         = m_sizeCtrl->GetValue();
    result_.padding          = m_paddingCtrl->GetValue();
    result_.defaultShell     = m_shellCtrl->GetValue().ToStdString();
    result_.defaultWorkingDir = m_workDirCtrl->GetValue().ToStdString();

    evt.Skip();
}
