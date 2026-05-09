#pragma once
#include <wx/app.h>
#include <memory>
#include "config/Config.h"
#include "db/ConnectionStore.h"
#include "input/InputRouter.h"
#include "session/SessionManager.h"
#include "ui/UIManager.h"

#include <gdk/gdk.h>

class App : public wxApp {
public:
    bool OnInit() override;
    int  OnExit() override;
    void OnGdkKeyPress(GdkEvent* event);

private:
    AppConfig                                      m_cfg;
    std::unique_ptr<term::db::ConnectionStore>     m_connectionStore;
    std::unique_ptr<term::input::InputRouter>      m_router;
    std::unique_ptr<term::session::SessionManager> m_sessionManager;
    std::unique_ptr<ui::UIManager>                 m_uiManager;
};
