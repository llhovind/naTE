#pragma once
#include <functional>
#include <wx/panel.h>
#include <wx/textctrl.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include "config/Config.h"

class SearchController;

class SearchBar : public wxPanel {
public:
    using CloseCallback = std::function<void()>;

    SearchBar(wxWindow* parent, SearchController& ctrl);

    // Apply theme-derived colors.  Called by TerminalPanel::ApplyConfig.
    void ApplyConfig(const AppConfig& cfg);

    void FocusInput();
    void Reset();
    void UpdateStatus(size_t current, size_t total);
    void SetCloseCallback(CloseCallback cb) { onClose_ = std::move(cb); }

    // Pre-populate the text field and trigger a search, as if the user had typed it.
    void SetInitialQuery(const std::u32string& query);

private:
    void OnText(wxCommandEvent&);
    void OnNext(wxCommandEvent&);
    void OnPrev(wxCommandEvent&);
    void OnClose(wxCommandEvent&);
    void OnKeyDown(wxKeyEvent&);

    wxTextCtrl*   input_;
    wxStaticText* status_;
    wxButton*     prevBtn_;
    wxButton*     nextBtn_;
    wxButton*     closeBtn_;
    SearchController& ctrl_;
    CloseCallback onClose_;
};
