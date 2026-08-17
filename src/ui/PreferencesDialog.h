#pragma once
#include "config/Config.h"
#include "config/ColorScheme.h"
#include <vector>
#include <wx/dialog.h>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxListBox;
class wxSpinCtrl;
class wxTextCtrl;

class PreferencesDialog : public wxDialog {
public:
    // themes — list of available colour schemes (from App::GetThemesDir scan).
    PreferencesDialog(wxWindow* parent,
                      const AppConfig& current,
                      const std::vector<ColorScheme>& themes);

    AppConfig GetResult() const { return result_; }

private:
    void OnBrowseFont(wxCommandEvent&);
    void OnOk(wxCommandEvent&);
    void OnGeometryAdd(wxCommandEvent&);
    void OnGeometryEdit(wxCommandEvent&);
    void OnGeometryRemove(wxCommandEvent&);
    void OnGeometrySelected(wxCommandEvent&);

    // Repopulates m_geoList from m_geometryPresets and syncs button enable state.
    void RefreshGeometryList();

    AppConfig                    result_;
    std::vector<ColorScheme>     themes_;   // parallel to m_themeChoice items

    // Editable working copy of the geometry presets; committed to result_ on OK.
    std::vector<GeometryPreset>  m_geometryPresets;

    wxChoice*   m_themeChoice       = nullptr;
    wxTextCtrl* m_familyCtrl        = nullptr;
    wxSpinCtrl* m_sizeCtrl          = nullptr;
    wxSpinCtrl* m_paddingCtrl       = nullptr;
    wxChoice*   m_cursorStyleChoice  = nullptr;
    wxCheckBox* m_cursorBlinkChk     = nullptr;
    wxChoice*   m_tileLayoutChoice   = nullptr;
    wxListBox*  m_geoList            = nullptr;
    wxButton*   m_geoEditBtn         = nullptr;
    wxButton*   m_geoRemoveBtn       = nullptr;

    wxTextCtrl* m_shellCtrl      = nullptr;
    wxTextCtrl* m_workDirCtrl    = nullptr;
    wxChoice*   m_wrapModeChoice = nullptr;
    wxCheckBox* m_loginShellChk    = nullptr;
    wxSpinCtrl* m_scrollbackCtrl   = nullptr;
    wxCheckBox* m_autoRestoreChk      = nullptr;
    wxSpinCtrl* m_saveIntervalCtrl    = nullptr;
    wxCheckBox* m_saveScrollbackChk   = nullptr;
    wxSpinCtrl* m_scrollbackLinesCtrl = nullptr;
    wxCheckBox* m_scrollbackStylesChk = nullptr;

    wxChoice*   m_bellModeChoice     = nullptr;
    wxTextCtrl* m_webSearchCtrl      = nullptr;
    wxTextCtrl* m_wordSelectCtrl     = nullptr;
    wxCheckBox* m_copyOnSelectChk    = nullptr;
    wxCheckBox* m_confirmCloseChk    = nullptr;
    wxChoice*   m_symlinkPolicyChoice = nullptr;
    wxTextCtrl* m_externalEditorCtrl = nullptr;
};
