#pragma once
#include <wx/app.h>
#include <memory>
#include <vector>
#include "config/Config.h"
#include "db/ConnectionStore.h"
#include "input/InputRouter.h"
#include "session/SessionManager.h"
#include "ui/UIManager.h"

#include <gdk/gdk.h>

class MainFrame;

class App : public wxApp {
public:
    bool OnInit() override;
    int  OnExit() override;
    void OnGdkKeyPress(GdkEvent* event);

    // Creates a new independent window with its own session stack.
    MainFrame* CreateNewWindow();

    // Closes every open window and exits the application.
    void QuitAll();

    // Notifies all open windows to rebuild their Window menu.
    void RebuildWindowMenus();

private:
    struct WindowContext {
        std::unique_ptr<term::input::InputRouter>      router;
        std::unique_ptr<term::session::SessionManager> sessionManager;
        std::unique_ptr<ui::UIManager>                 uiManager;
        MainFrame*                                     frame = nullptr; // wx-owned
    };

    WindowContext* FindActiveContext();

    AppConfig                                      m_cfg;
    std::unique_ptr<term::db::ConnectionStore>     m_connectionStore;
    std::vector<std::unique_ptr<WindowContext>>    m_windows;
};
