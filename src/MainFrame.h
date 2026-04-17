#pragma once
#include <wx/frame.h>
#include "Config.h"

class TerminalPanel;

class MainFrame : public wxFrame {
public:
    explicit MainFrame(const AppConfig& cfg);

    TerminalPanel* GetPanel() const { return m_panel; }

private:
    void OnQuit(wxCommandEvent&);

    TerminalPanel* m_panel;
};
