#include "ui/SearchBar.h"
#include "ui/ColorUtils.h"
#include "ui/SearchController.h"
#include "ui/StringUtils.h"
#include <wx/sizer.h>
#include <wx/settings.h>

static constexpr int kBarPad = 4;

SearchBar::SearchBar(wxWindow *parent, SearchController &ctrl)
    : wxPanel(parent, wxID_ANY), ctrl_(ctrl)
{
    SetBackgroundColour(toWx(UiColors{}.searchBarBg));

    wxFont btnfont(
        14,
        wxFONTFAMILY_DEFAULT,
        wxFONTSTYLE_NORMAL,
        wxFONTWEIGHT_NORMAL,
        false);

    const wxColour btnFg = toWx(UiColors{}.controlActive);

    input_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(200, -1),
                            wxTE_PROCESS_ENTER);
    status_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxSize(80, -1));
    status_->SetForegroundColour(btnFg);
    prevBtn_ = new wxButton(this, wxID_ANY, L"\u2BC7", wxDefaultPosition, wxSize(28, -1), wxBORDER_NONE | wxBU_EXACTFIT);
    prevBtn_->SetFont(btnfont);
    prevBtn_->SetForegroundColour(btnFg);
    nextBtn_ = new wxButton(this, wxID_ANY, L"\u2BC8", wxDefaultPosition, wxSize(28, -1), wxBORDER_NONE | wxBU_EXACTFIT);
    nextBtn_->SetFont(btnfont);
    nextBtn_->SetForegroundColour(btnFg);
    closeBtn_ = new wxButton(this, wxID_ANY, "X", wxDefaultPosition, wxSize(28, -1), wxBORDER_NONE | wxBU_EXACTFIT);
    closeBtn_->SetFont(btnfont);
    closeBtn_->SetForegroundColour(btnFg);

    auto *sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->AddSpacer(kBarPad);
    sizer->Add(input_, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, kBarPad);
    sizer->AddSpacer(kBarPad);
    sizer->Add(prevBtn_, 0, wxALIGN_CENTER_VERTICAL);
    sizer->AddSpacer(2);
    sizer->Add(nextBtn_, 0, wxALIGN_CENTER_VERTICAL);
    sizer->AddSpacer(kBarPad);
    sizer->Add(status_, 0, wxALIGN_CENTER_VERTICAL);
    sizer->AddStretchSpacer();
    sizer->Add(closeBtn_, 0, wxALIGN_CENTER_VERTICAL);
    sizer->AddSpacer(kBarPad);
    SetSizerAndFit(sizer);

    input_->Bind(wxEVT_TEXT, &SearchBar::OnText, this);
    input_->Bind(wxEVT_KEY_DOWN, &SearchBar::OnKeyDown, this);
    nextBtn_->Bind(wxEVT_BUTTON, &SearchBar::OnNext, this);
    prevBtn_->Bind(wxEVT_BUTTON, &SearchBar::OnPrev, this);
    closeBtn_->Bind(wxEVT_BUTTON, &SearchBar::OnClose, this);
}

void SearchBar::ApplyConfig(const AppConfig& cfg)
{
    SetBackgroundColour(toWx(cfg.uiColors.searchBarBg));
    const wxColour btnFg = toWx(cfg.uiColors.controlActive);
    status_->SetForegroundColour(btnFg);
    prevBtn_->SetForegroundColour(btnFg);
    nextBtn_->SetForegroundColour(btnFg);
    closeBtn_->SetForegroundColour(btnFg);
    Refresh();
}

void SearchBar::FocusInput()
{
    input_->SetFocus();
    input_->SelectAll();
}

void SearchBar::Reset()
{
    input_->ChangeValue("");
    status_->SetLabel("");
}

void SearchBar::SetInitialQuery(const std::u32string& query)
{
    // ChangeValue does not fire wxEVT_TEXT, so we drive the search explicitly.
    input_->ChangeValue(ToWxString(query));
    input_->SetFocus();
    input_->SelectAll();
    ctrl_.SetQuery(query);
}

void SearchBar::UpdateStatus(size_t current, size_t total)
{
    if (total == 0)
        status_->SetLabel("No results");
    else
        status_->SetLabel(wxString::Format("%zu / %zu", current, total));
    Layout();
}

void SearchBar::OnText(wxCommandEvent &)
{
    const std::wstring ws = input_->GetValue().ToStdWstring();
    std::u32string u32;
    u32.reserve(ws.size());
    for (wchar_t c : ws)
        u32.push_back(static_cast<char32_t>(c));
    ctrl_.SetQuery(u32);
}

void SearchBar::OnNext(wxCommandEvent &) { ctrl_.NextMatch(); }
void SearchBar::OnPrev(wxCommandEvent &) { ctrl_.PrevMatch(); }

void SearchBar::OnClose(wxCommandEvent &)
{
    ctrl_.Clear();
    if (onClose_)
        onClose_();
}

void SearchBar::OnKeyDown(wxKeyEvent &e)
{
    if (e.GetKeyCode() == WXK_ESCAPE)
    {
        ctrl_.Clear();
        if (onClose_)
            onClose_();
    }
    else if (e.GetKeyCode() == WXK_RETURN || e.GetKeyCode() == WXK_NUMPAD_ENTER)
    {
        ctrl_.NextMatch();
    }
    else if (e.GetKeyCode() == WXK_F3)
    {
        if (e.ShiftDown()) ctrl_.PrevMatch(); else ctrl_.NextMatch();
    }
    else
    {
        e.Skip();
    }
}
