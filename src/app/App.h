#pragma once
#include <wx/app.h>
#include <vector>
#include "config/Config.h"

class TerminalPanel;

class App : public wxApp {
public:
    bool OnInit() override;

private:
    AppConfig                   m_cfg;
    std::vector<TerminalPanel*> m_panels;  // non-owning; lifetime managed by parent frames
};
