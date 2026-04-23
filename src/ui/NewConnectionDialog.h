#pragma once

#include <wx/dialog.h>
#include <string>
#include <variant>

class wxRadioButton;
class wxTextCtrl;

namespace ui
{

struct LoopbackParams {};
struct PtyParams { std::string shell; };

using ConnectionParams = std::variant<LoopbackParams, PtyParams>;

class NewConnectionDialog : public wxDialog
{
public:
    NewConnectionDialog(wxWindow* parent, const std::string& defaultShell);

    // Valid only after ShowModal() == wxID_OK
    ConnectionParams GetParams() const;

private:
    void OnTransportChanged(wxCommandEvent&);

    wxRadioButton* m_rbLoopback = nullptr;
    wxRadioButton* m_rbPty      = nullptr;
    wxTextCtrl*    m_shellCtrl  = nullptr;
};

} // namespace ui
