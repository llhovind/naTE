#pragma once
#include <wx/app.h>
#include <vector>
#include "Config.h"

class TextPanel;

class App : public wxApp {
public:
    bool OnInit() override;

private:
    AppConfig               m_cfg;
    std::vector<TextPanel*> m_panels;  // non-owning; lifetime managed by parent frames
};
