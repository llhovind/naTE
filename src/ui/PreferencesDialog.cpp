#include "ui/PreferencesDialog.h"
#include "ui/GeometryDialog.h"
#include <regex>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/fontdlg.h>
#include <wx/gbsizer.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace {
    constexpr int kBrowseFontId   = wxID_HIGHEST + 600;
    constexpr int kGeoListId      = wxID_HIGHEST + 601;
    constexpr int kGeoAddId       = wxID_HIGHEST + 602;
    constexpr int kGeoEditId      = wxID_HIGHEST + 603;
    constexpr int kGeoRemoveId    = wxID_HIGHEST + 604;
}

PreferencesDialog::PreferencesDialog(wxWindow* parent,
                                     const AppConfig& current,
                                     const std::vector<ColorScheme>& themes)
    : wxDialog(parent, wxID_ANY, "Preferences",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      result_(current),
      themes_(themes),
      m_geometryPresets(current.geometryPresets)
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
                                wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                wxSP_ARROW_KEYS, 6, 72, current.fontSize);
    appSizer->Add(m_sizeCtrl, {row, 1}, {1, 1});
    ++row;

    // Padding
    appSizer->Add(new wxStaticText(appPage, wxID_ANY, "Padding (px):"),
                  {row, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    m_paddingCtrl = new wxSpinCtrl(appPage, wxID_ANY,
                                   wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                   wxSP_ARROW_KEYS, 0, 32, current.padding);
    appSizer->Add(m_paddingCtrl, {row, 1}, {1, 1});
    ++row;

    // Cursor style
    appSizer->Add(new wxStaticText(appPage, wxID_ANY, "Cursor style:"),
                  {row, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    {
        wxArrayString styleOpts;
        styleOpts.Add("Block");
        styleOpts.Add("Bar");
        styleOpts.Add("Underline");
        m_cursorStyleChoice = new wxChoice(appPage, wxID_ANY,
                                           wxDefaultPosition, wxDefaultSize, styleOpts);
        int preselect = 0;
        switch (current.cursorStyle) {
            case CursorStyle::Bar:       preselect = 1; break;
            case CursorStyle::Underline: preselect = 2; break;
            default:                     preselect = 0; break;
        }
        m_cursorStyleChoice->SetSelection(preselect);
    }
    appSizer->Add(m_cursorStyleChoice, {row, 1}, {1, 1});

    // Cursor blink — on the same row as cursor style
    m_cursorBlinkChk = new wxCheckBox(appPage, wxID_ANY, "Blinking cursor");
    m_cursorBlinkChk->SetValue(current.cursorBlink);
    appSizer->Add(m_cursorBlinkChk, {row, 2}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    ++row;

    // Tile layout
    appSizer->Add(new wxStaticText(appPage, wxID_ANY, "Tile layout:"),
                  {row, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    {
        wxArrayString layoutOpts;
        layoutOpts.Add("Row Ordered");
        layoutOpts.Add("Column Ordered");
        m_tileLayoutChoice = new wxChoice(appPage, wxID_ANY,
                                          wxDefaultPosition, wxDefaultSize, layoutOpts);
        const int preselect = (current.tileLayout == TileLayout::ColumnFirst) ? 1 : 0;
        m_tileLayoutChoice->SetSelection(preselect);
    }
    appSizer->Add(m_tileLayoutChoice, {row, 1}, {1, 1});
    ++row;

    // Geometry presets — drives the Terminal > Set Geometry menu and the New
    // Connection dialog's geometry combo.
    appSizer->Add(new wxStaticText(appPage, wxID_ANY, "Geometry presets:"),
                  {row, 0}, {1, 1}, wxALIGN_TOP);
    m_geoList = new wxListBox(appPage, kGeoListId,
                              wxDefaultPosition, {-1, 96});
    appSizer->Add(m_geoList, {row, 1}, {1, 1}, wxEXPAND);
    {
        auto* btnCol = new wxBoxSizer(wxVERTICAL);
        auto* addBtn   = new wxButton(appPage, kGeoAddId,    "Add...");
        m_geoEditBtn   = new wxButton(appPage, kGeoEditId,   "Edit...");
        m_geoRemoveBtn = new wxButton(appPage, kGeoRemoveId, "Remove");
        btnCol->Add(addBtn,         0, wxEXPAND | wxBOTTOM, 4);
        btnCol->Add(m_geoEditBtn,   0, wxEXPAND | wxBOTTOM, 4);
        btnCol->Add(m_geoRemoveBtn, 0, wxEXPAND);
        appSizer->Add(btnCol, {row, 2}, {1, 1}, wxALIGN_TOP);
    }
    RefreshGeometryList();
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

    behSizer->Add(new wxStaticText(behPage, wxID_ANY, "Web search URL:"),
                  {1, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    m_webSearchCtrl = new wxTextCtrl(behPage, wxID_ANY,
                                     wxString::FromUTF8(current.webSearchUrl),
                                     wxDefaultPosition, {320, -1});
    m_webSearchCtrl->SetHint("e.g. https://duckduckgo.com/?q=");
    behSizer->Add(m_webSearchCtrl, {1, 1}, {1, 1}, wxEXPAND);

    behSizer->Add(new wxStaticText(behPage, wxID_ANY, "Word selection pattern:"),
                  {2, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    m_wordSelectCtrl = new wxTextCtrl(behPage, wxID_ANY,
                                      wxString::FromUTF8(current.wordSelectRegex),
                                      wxDefaultPosition, {320, -1});
    m_wordSelectCtrl->SetHint("e.g. [^\\s]+ or [a-zA-Z0-9_-]+");
    behSizer->Add(m_wordSelectCtrl, {2, 1}, {1, 1}, wxEXPAND);

    behSizer->Add(new wxStaticText(behPage, wxID_ANY, "Bell mode:"),
                  {3, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    {
        wxArrayString bellOpts;
        bellOpts.Add("Visual flash");
        bellOpts.Add("Audible");
        bellOpts.Add("None");
        m_bellModeChoice = new wxChoice(behPage, wxID_ANY,
                                        wxDefaultPosition, wxDefaultSize, bellOpts);
        int preselect = 0;
        switch (current.bellMode) {
            case BellMode::Audible: preselect = 1; break;
            case BellMode::None:    preselect = 2; break;
            default:                preselect = 0; break;
        }
        m_bellModeChoice->SetSelection(preselect);
    }
    behSizer->Add(m_bellModeChoice, {3, 1}, {1, 1});

    m_copyOnSelectChk = new wxCheckBox(behPage, wxID_ANY, "Copy to primary selection on select (X11)");
    m_copyOnSelectChk->SetValue(current.copyOnSelect);
    behSizer->Add(m_copyOnSelectChk, {4, 0}, {1, 2}, wxALIGN_CENTER_VERTICAL);

    m_confirmCloseChk = new wxCheckBox(behPage, wxID_ANY,
        "Ask for confirmation when closing windows with active sessions");
    m_confirmCloseChk->SetValue(current.confirmCloseWindow);
    behSizer->Add(m_confirmCloseChk, {5, 0}, {1, 2}, wxALIGN_CENTER_VERTICAL);

    behSizer->Add(new wxStaticText(behPage, wxID_ANY, "External editor:"),
                  {6, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    m_externalEditorCtrl = new wxTextCtrl(behPage, wxID_ANY,
                                          wxString::FromUTF8(current.externalEditorCommand),
                                          wxDefaultPosition, {320, -1});
    m_externalEditorCtrl->SetHint("e.g. code --wait   (empty = $EDITOR)");
    behSizer->Add(m_externalEditorCtrl, {6, 1}, {1, 1}, wxEXPAND);

    behSizer->AddGrowableCol(1);
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

    sesSizer->Add(new wxStaticText(sesPage, wxID_ANY, "Default wrap mode:"),
                  {2, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    {
        wxArrayString wrapOpts;
        wrapOpts.Add("OFF");
        wrapOpts.Add("ON");
        m_wrapModeChoice = new wxChoice(sesPage, wxID_ANY,
                                        wxDefaultPosition, wxDefaultSize, wrapOpts);
        m_wrapModeChoice->SetSelection(current.defaultWrapMode ? 1 : 0);
    }
    sesSizer->Add(m_wrapModeChoice, {2, 1}, {1, 1});

    // Login shell
    m_loginShellChk = new wxCheckBox(sesPage, wxID_ANY, "Login shell");
    m_loginShellChk->SetValue(current.defaultLoginShell);
    sesSizer->Add(m_loginShellChk, {3, 0}, {1, 2});

    // Scrollback lines
    sesSizer->Add(new wxStaticText(sesPage, wxID_ANY, "Scrollback lines:"),
                  {4, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    m_scrollbackCtrl = new wxSpinCtrl(sesPage, wxID_ANY,
                                      wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                      wxSP_ARROW_KEYS, 1000, 1'000'000,
                                      current.scrollbackLines);
    m_scrollbackCtrl->SetIncrement(10'000);
    sesSizer->Add(m_scrollbackCtrl, {4, 1}, {1, 1});

    // Auto-restore session
    m_autoRestoreChk = new wxCheckBox(sesPage, wxID_ANY, "Auto-restore session on launch");
    m_autoRestoreChk->SetValue(current.autoRestoreSession);
    sesSizer->Add(m_autoRestoreChk, {5, 0}, {1, 2});

    // Save interval (enabled only when auto-restore is on)
    sesSizer->Add(new wxStaticText(sesPage, wxID_ANY, "Save interval (s):"),
                  {6, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    m_saveIntervalCtrl = new wxSpinCtrl(sesPage, wxID_ANY,
                                        wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                        wxSP_ARROW_KEYS, 10, 3600,
                                        current.sessionSaveInterval);
    sesSizer->Add(m_saveIntervalCtrl, {6, 1}, {1, 1});

    // Save scrollback buffer with workspace
    m_saveScrollbackChk = new wxCheckBox(sesPage, wxID_ANY,
                                          "Save scrollback buffer with workspace");
    m_saveScrollbackChk->SetValue(current.saveScrollbackWithWorkspace);
    sesSizer->Add(m_saveScrollbackChk, {7, 0}, {1, 2});

    sesSizer->Add(new wxStaticText(sesPage, wxID_ANY, "Lines to save:"),
                  {8, 0}, {1, 1}, wxALIGN_CENTER_VERTICAL);
    m_scrollbackLinesCtrl = new wxSpinCtrl(sesPage, wxID_ANY,
                                           wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                           wxSP_ARROW_KEYS, 1000, 500'000,
                                           current.scrollbackSaveLines);
    m_scrollbackLinesCtrl->SetIncrement(1000);
    sesSizer->Add(m_scrollbackLinesCtrl, {8, 1}, {1, 1});

    m_scrollbackStylesChk = new wxCheckBox(sesPage, wxID_ANY, "Save styles (color/bold)");
    m_scrollbackStylesChk->SetValue(current.scrollbackSaveStyles);
    sesSizer->Add(m_scrollbackStylesChk, {9, 0}, {1, 2});

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
    SetMinClientSize({440, 320});

    Bind(wxEVT_BUTTON, &PreferencesDialog::OnBrowseFont, this, kBrowseFontId);
    Bind(wxEVT_BUTTON, &PreferencesDialog::OnOk,         this, wxID_OK);
    Bind(wxEVT_BUTTON,  &PreferencesDialog::OnGeometryAdd,      this, kGeoAddId);
    Bind(wxEVT_BUTTON,  &PreferencesDialog::OnGeometryEdit,     this, kGeoEditId);
    Bind(wxEVT_BUTTON,  &PreferencesDialog::OnGeometryRemove,   this, kGeoRemoveId);
    Bind(wxEVT_LISTBOX, &PreferencesDialog::OnGeometrySelected, this, kGeoListId);
    Bind(wxEVT_LISTBOX_DCLICK, &PreferencesDialog::OnGeometryEdit, this, kGeoListId);
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
    // Validate the word selection regex before accepting.
    const std::string wordRegex = m_wordSelectCtrl->GetValue().ToStdString();
    try {
        std::regex{wordRegex};
    } catch (const std::regex_error& ex) {
        wxMessageBox(wxString::FromUTF8("Invalid word selection pattern: ") +
                     wxString::FromUTF8(ex.what()),
                     "Preferences", wxOK | wxICON_ERROR, this);
        return;
    }

    const int sel = m_themeChoice->GetSelection();
    if (sel >= 0 && sel < static_cast<int>(themes_.size())) {
        result_.themeName    = themes_[sel].stem;
        result_.textColour   = themes_[sel].foreground;
        result_.bgColour     = themes_[sel].background;
        result_.cursorColour = themes_[sel].cursor;
        if (themes_[sel].hasPalette)
            result_.ansiColors = themes_[sel].ansiColors;
        result_.uiColors = themes_[sel].deriveUiColors();
    }

    result_.fontFamily         = m_familyCtrl->GetValue().ToStdString();
    result_.fontSize           = m_sizeCtrl->GetValue();
    result_.padding            = m_paddingCtrl->GetValue();
    switch (m_cursorStyleChoice->GetSelection()) {
        case 1:  result_.cursorStyle = CursorStyle::Bar;       break;
        case 2:  result_.cursorStyle = CursorStyle::Underline; break;
        default: result_.cursorStyle = CursorStyle::Block;     break;
    }
    result_.cursorBlink  = m_cursorBlinkChk->IsChecked();
    result_.tileLayout   = (m_tileLayoutChoice->GetSelection() == 1)
                           ? TileLayout::ColumnFirst : TileLayout::RowFirst;
    // Mirror parseGeometryPresets' empty-fallback so the menu always has entries.
    result_.geometryPresets = m_geometryPresets.empty()
                              ? std::vector<GeometryPreset>{{80, 24}, {132, 24}}
                              : m_geometryPresets;
    switch (m_bellModeChoice->GetSelection()) {
        case 1:  result_.bellMode = BellMode::Audible; break;
        case 2:  result_.bellMode = BellMode::None;    break;
        default: result_.bellMode = BellMode::Visual;  break;
    }
    result_.defaultShell       = m_shellCtrl->GetValue().ToStdString();
    result_.defaultWorkingDir  = m_workDirCtrl->GetValue().ToStdString();
    result_.defaultWrapMode    = m_wrapModeChoice->GetSelection() == 1;
    result_.defaultLoginShell  = m_loginShellChk->IsChecked();
    result_.scrollbackLines              = m_scrollbackCtrl->GetValue();
    result_.autoRestoreSession           = m_autoRestoreChk->IsChecked();
    result_.sessionSaveInterval          = m_saveIntervalCtrl->GetValue();
    result_.saveScrollbackWithWorkspace  = m_saveScrollbackChk->IsChecked();
    result_.scrollbackSaveLines          = m_scrollbackLinesCtrl->GetValue();
    result_.scrollbackSaveStyles         = m_scrollbackStylesChk->IsChecked();
    result_.webSearchUrl       = m_webSearchCtrl->GetValue().ToStdString();
    result_.wordSelectRegex    = wordRegex;
    result_.copyOnSelect          = m_copyOnSelectChk->IsChecked();
    result_.confirmCloseWindow    = m_confirmCloseChk->IsChecked();
    result_.externalEditorCommand = m_externalEditorCtrl->GetValue().ToStdString();

    evt.Skip();
}

void PreferencesDialog::RefreshGeometryList()
{
    if (!m_geoList) return;
    const int prevSel = m_geoList->GetSelection();
    m_geoList->Clear();
    for (const auto& g : m_geometryPresets)
        m_geoList->Append(wxString::Format("%u x %u", g.cols, g.rows));

    if (!m_geometryPresets.empty()) {
        const int count = static_cast<int>(m_geometryPresets.size());
        m_geoList->SetSelection(prevSel >= 0 && prevSel < count ? prevSel : count - 1);
    }
    wxCommandEvent dummy;
    OnGeometrySelected(dummy);
}

void PreferencesDialog::OnGeometrySelected(wxCommandEvent&)
{
    const bool hasSel = m_geoList && m_geoList->GetSelection() != wxNOT_FOUND;
    if (m_geoEditBtn)   m_geoEditBtn->Enable(hasSel);
    // Always keep at least one preset so the menu/combo are never empty.
    if (m_geoRemoveBtn) m_geoRemoveBtn->Enable(hasSel && m_geometryPresets.size() > 1);
}

void PreferencesDialog::OnGeometryAdd(wxCommandEvent&)
{
    GeometryDialog dlg(this, 80, 24);
    if (dlg.ShowModal() != wxID_OK) return;
    m_geometryPresets.push_back({dlg.GetCols(), dlg.GetRows()});
    m_geoList->SetSelection(wxNOT_FOUND);   // select the newly appended row
    RefreshGeometryList();
}

void PreferencesDialog::OnGeometryEdit(wxCommandEvent&)
{
    const int sel = m_geoList ? m_geoList->GetSelection() : wxNOT_FOUND;
    if (sel == wxNOT_FOUND || sel >= static_cast<int>(m_geometryPresets.size())) return;
    auto& preset = m_geometryPresets[static_cast<std::size_t>(sel)];
    GeometryDialog dlg(this, preset.cols, preset.rows);
    if (dlg.ShowModal() != wxID_OK) return;
    preset = {dlg.GetCols(), dlg.GetRows()};
    RefreshGeometryList();
}

void PreferencesDialog::OnGeometryRemove(wxCommandEvent&)
{
    const int sel = m_geoList ? m_geoList->GetSelection() : wxNOT_FOUND;
    if (sel == wxNOT_FOUND || m_geometryPresets.size() <= 1) return;
    m_geometryPresets.erase(m_geometryPresets.begin() + sel);
    RefreshGeometryList();
}
