#pragma once
#include "transport/ITransportTarget.h"

#include <string>
#include <vector>
#include <wx/dialog.h>
#include <wx/textctrl.h>

namespace ui {

// Modal dialog shown during SSH keyboard-interactive authentication.
// Displays the server-supplied name, instruction, and one text field per prompt.
// Prompts with echo=false render as password fields.
class KbdIntDialog : public wxDialog {
public:
    KbdIntDialog(wxWindow*                                    parent,
                 const term::transport::KbdIntChallenge&     challenge);

    // Valid after ShowModal() == wxID_OK.
    std::vector<std::string> GetResponses() const;

private:
    std::vector<wxTextCtrl*> inputs_;
};

} // namespace ui
