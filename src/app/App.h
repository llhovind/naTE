#pragma once
#include <wx/app.h>
#include <memory>
#include "config/Config.h"
#include "input/InputRouter.h"

#include <gdk/gdk.h>

class App : public wxApp {
public:
    bool OnInit() override;
    void OnGdkKeyPress(GdkEvent* event);

private:
    AppConfig                                 m_cfg;
    std::unique_ptr<term::input::InputRouter> m_router;
};
