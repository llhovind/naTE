#pragma once
#include <functional>
#include <wx/panel.h>
#include <wx/textctrl.h>
#include <wx/stattext.h>
#include <wx/button.h>

class SearchController;

class SearchBar : public wxPanel {
public:
    using CloseCallback = std::function<void()>;

    SearchBar(wxWindow* parent, SearchController& ctrl);

    void FocusInput();
    void UpdateStatus(size_t current, size_t total);
    void SetCloseCallback(CloseCallback cb) { onClose_ = std::move(cb); }

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
