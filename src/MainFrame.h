#pragma once
#include <wx/frame.h>
#include "Config.h"

class TextPanel;

class MainFrame : public wxFrame {
public:
    explicit MainFrame(const AppConfig& cfg);

    TextPanel* GetPanel() const { return m_panel; }

private:
    void OnQuit(wxCommandEvent&);

    TextPanel* m_panel;
};
