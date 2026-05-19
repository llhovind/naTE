#pragma once
#include "config/Config.h"
#include "config/ColorScheme.h"
#include <vector>
#include <wx/dialog.h>

class wxChoice;
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

    AppConfig                    result_;
    std::vector<ColorScheme>     themes_;   // parallel to m_themeChoice items

    wxChoice*   m_themeChoice   = nullptr;
    wxTextCtrl* m_familyCtrl    = nullptr;
    wxSpinCtrl* m_sizeCtrl      = nullptr;
    wxSpinCtrl* m_paddingCtrl   = nullptr;

    wxTextCtrl* m_shellCtrl     = nullptr;
    wxTextCtrl* m_workDirCtrl   = nullptr;
    wxChoice*   m_wrapModeChoice = nullptr;
};
