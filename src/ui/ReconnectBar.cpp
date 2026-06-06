#include "ui/ReconnectBar.h"
#include "ui/ColorUtils.h"
#include "config/Config.h"
#include <wx/button.h>
#include <wx/dcclient.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

ReconnectBar::ReconnectBar(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetBackgroundColour(toWx(UiColors{}.reconnectBarBg));

    label_        = new wxStaticText(this, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxDefaultSize,
                                     wxST_ELLIPSIZE_END);
    reconnectBtn_ = new wxButton(this, wxID_ANY, "Reconnect",
                                  wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    saveBtn_      = new wxButton(this, wxID_ANY, "Save...",
                                  wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    closeBtn_     = new wxButton(this, wxID_ANY, "Close",
                                  wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);

    label_->SetForegroundColour(toWx(UiColors{}.reconnectBarText));

    auto* sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->AddSpacer(6);
    sizer->Add(label_,        1, wxALIGN_CENTER_VERTICAL);
    sizer->AddSpacer(6);
    sizer->Add(reconnectBtn_, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 2);
    sizer->AddSpacer(4);
    sizer->Add(saveBtn_,      0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 2);
    sizer->AddSpacer(4);
    sizer->Add(closeBtn_,     0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 2);
    sizer->AddSpacer(6);
    SetSizerAndFit(sizer);

    reconnectBtn_->Bind(wxEVT_BUTTON, &ReconnectBar::OnReconnect, this);
    saveBtn_->Bind(wxEVT_BUTTON,      &ReconnectBar::OnSave,      this);
    closeBtn_->Bind(wxEVT_BUTTON,     &ReconnectBar::OnClose,     this);

    Hide();
}

void ReconnectBar::ApplyConfig(const AppConfig& cfg)
{
    SetBackgroundColour(toWx(cfg.uiColors.reconnectBarBg));
    label_->SetForegroundColour(toWx(cfg.uiColors.reconnectBarText));
    Refresh();
}

void ReconnectBar::ShowBar(const wxString& message)
{
    label_->SetLabel(message);
    Layout();
    Show();
}

void ReconnectBar::HideBar()
{
    Hide();
    label_->SetLabel(wxEmptyString);
}

void ReconnectBar::OnReconnect(wxCommandEvent&)
{
    if (reconnectCb_) reconnectCb_();
}

void ReconnectBar::OnSave(wxCommandEvent&)
{
    if (saveCb_) saveCb_();
}

void ReconnectBar::OnClose(wxCommandEvent&)
{
    if (closeCb_) closeCb_();
}
