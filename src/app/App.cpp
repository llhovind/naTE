#include "app/App.h"
#include "db/JsonConnectionRepository.h"
#include "ui/MainFrame.h"
#include "ui/wxKeyAdapter.h"
#include "ui/SearchController.h"
#include "input/KeyEvent.hpp"
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <libssh2.h>
#include <cstdlib>
#include <string>
#include <algorithm>

static void GdkEventHandler(GdkEvent* event, gpointer data)
{
    if (event->type == GDK_KEY_PRESS) {
        static_cast<App*>(data)->OnGdkKeyPress(event);
    }
    gtk_main_do_event(event);
}

bool App::OnInit() {
    libssh2_init(0);

    const wxString exeDir =
        wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
    m_cfg = AppConfig::load((exeDir + wxFileName::GetPathSeparator() + "config.ini").ToStdString());

    // Resolve ~/.nate/connections.json (same directory as known_hosts)
    const std::string connectionsPath = [] {
        const char* home = std::getenv("HOME");
        const std::string base = home ? std::string(home) + "/.nate" : std::string(".nate");
        return base + "/connections.json";
    }();
    m_connectionStore = std::make_unique<term::db::ConnectionStore>(
        std::make_unique<term::db::JsonConnectionRepository>(connectionsPath));

    gdk_event_handler_set(GdkEventHandler, this, nullptr);

    CreateNewWindow();
    return true;
}

MainFrame* App::CreateNewWindow()
{
    auto wc = std::make_unique<WindowContext>();
    wc->router         = std::make_unique<term::input::InputRouter>();
    wc->sessionManager = std::make_unique<term::session::SessionManager>(*wc->router);

    auto* frame = new MainFrame(m_cfg, *wc->router, *wc->sessionManager, *m_connectionStore);
    wc->frame = frame;

    wc->uiManager = std::make_unique<ui::UIManager>(
        *wc->sessionManager, frame->GetConnMenu(), frame, m_cfg,
        *wc->router, frame->GetEditMenu());
    wc->sessionManager->SetObserver(wc->uiManager.get());

    frame->Bind(wxEVT_DESTROY, [this, frame](wxWindowDestroyEvent& evt) {
        if (evt.GetEventObject() == frame) {
            auto it = std::find_if(m_windows.begin(), m_windows.end(),
                [frame](const std::unique_ptr<WindowContext>& w) {
                    return w->frame == frame;
                });
            if (it != m_windows.end())
                m_windows.erase(it);
            RebuildWindowMenus();
        }
        evt.Skip();
    });

    frame->Show();
    m_windows.push_back(std::move(wc));

    RebuildWindowMenus();
    return frame;
}

void App::QuitAll()
{
    // Copy frame pointers — Close() will modify m_windows via EVT_DESTROY.
    std::vector<MainFrame*> frames;
    frames.reserve(m_windows.size());
    for (auto& w : m_windows)
        frames.push_back(w->frame);
    for (auto* f : frames)
        f->Close(true);
}

void App::RebuildWindowMenus()
{
    std::vector<std::pair<MainFrame*, std::string>> entries;
    entries.reserve(m_windows.size());
    for (auto& w : m_windows)
        entries.emplace_back(w->frame, w->frame->GetTitle().ToStdString());
    for (auto& w : m_windows)
        w->frame->RebuildWindowMenu(entries);
}

int App::OnExit()
{
    libssh2_exit();
    return wxApp::OnExit();
}

App::WindowContext* App::FindActiveContext()
{
    // Native GTK modal dialogs use gtk_grab_add(); focus tracking unreliable there.
    if (gtk_grab_get_current() != nullptr)
        return nullptr;

    wxWindow* const focused = wxWindow::FindFocus();
    if (!focused)
        return nullptr;

    auto* topLevel = dynamic_cast<MainFrame*>(wxGetTopLevelParent(focused));
    if (!topLevel)
        return nullptr;

    for (auto& w : m_windows) {
        if (w->frame == topLevel)
            return w.get();
    }
    return nullptr;
}

void App::OnGdkKeyPress(GdkEvent* event)
{
    // Pure wx dialogs (e.g. NewConnectionDialog) don't use GTK grabs, but wx
    // focus tracking does work for them: the focused widget's top-level parent
    // will be the dialog itself, not a MainFrame.
    WindowContext* wc = FindActiveContext();
    if (!wc)
        return;

    GdkEventKey* ke = reinterpret_cast<GdkEventKey*>(event);

    term::input::KeyEvent evt;
    evt.ctrl  = (ke->state & GDK_CONTROL_MASK) != 0;
    evt.alt   = (ke->state & GDK_MOD1_MASK)    != 0;
    evt.shift = (ke->state & GDK_SHIFT_MASK)   != 0;

    switch (ke->keyval) {
    case GDK_KEY_Return:    evt.key = term::input::Key::Enter;      break;
    case GDK_KEY_BackSpace: evt.key = term::input::Key::Backspace;  break;
    case GDK_KEY_Tab:       evt.key = term::input::Key::Tab;        break;
    case GDK_KEY_Escape:    evt.key = term::input::Key::Escape;     break;
    case GDK_KEY_Up:        evt.key = term::input::Key::ArrowUp;    break;
    case GDK_KEY_Down:      evt.key = term::input::Key::ArrowDown;  break;
    case GDK_KEY_Left:      evt.key = term::input::Key::ArrowLeft;  break;
    case GDK_KEY_Right:     evt.key = term::input::Key::ArrowRight; break;
    case GDK_KEY_Home:      evt.key = term::input::Key::Home;       break;
    case GDK_KEY_End:       evt.key = term::input::Key::End;        break;
    case GDK_KEY_Page_Up:   evt.key = term::input::Key::PageUp;     break;
    case GDK_KEY_Page_Down: evt.key = term::input::Key::PageDown;   break;
    case GDK_KEY_Insert:    evt.key = term::input::Key::Insert;     break;
    case GDK_KEY_Delete:    evt.key = term::input::Key::Delete;     break;
    case GDK_KEY_F1:  case GDK_KEY_F2:  case GDK_KEY_F3:  case GDK_KEY_F4:
    case GDK_KEY_F5:  case GDK_KEY_F6:  case GDK_KEY_F7:  case GDK_KEY_F8:
    case GDK_KEY_F9:  case GDK_KEY_F10: case GDK_KEY_F11: case GDK_KEY_F12:
        evt.key  = term::input::Key::FunctionKey;
        evt.code = ke->keyval - GDK_KEY_F1 + 1;
        break;
    default:
        if (ke->keyval >= 32 && ke->keyval < 127) {
            evt.key  = term::input::Key::Character;
            evt.code = ke->keyval;
            evt.text = std::string(1, static_cast<char>(ke->keyval));
        } else {
            evt.key = term::input::Key::Unknown;
        }
        break;
    }

    // Ctrl+V — paste clipboard contents through the router (respects broadcast mode).
    // Uses GTK's async gtk_clipboard_request_text rather than wxTheClipboard: wx's
    // synchronous clipboard API pumps the GTK event loop via gtk_main_iteration_do(),
    // which corrupts GTK focus/grab state and silently kills all subsequent keyboard input.
    if (evt.ctrl && evt.key == term::input::Key::Character && evt.code == 'v') {
        struct PasteCtx { ui::UIManager* ui; term::input::InputRouter* router; };
        auto* ctx = new PasteCtx{ wc->uiManager.get(), wc->router.get() };
        gtk_clipboard_request_text(
            gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
            [](GtkClipboard*, const gchar* text, gpointer data) {
                auto* ctx = static_cast<PasteCtx*>(data);
                if (text) {
                    ctx->router->Paste(std::string(text));
                    ctx->ui->EnsureCursorVisibleForActive();
                }
                delete ctx;
            },
            ctx);
        return;
    }

    // Ctrl+F — open/focus the search bar (never send to PTY).
    if (evt.ctrl && evt.key == term::input::Key::Character && evt.code == 'f') {
        wc->uiManager->ShowSearchBarForActive(true, wc->uiManager->GetActiveSelectedText());
        return;
    }

    // When the search bar has focus, intercept F3/Shift+F3/Escape and suppress
    // PTY routing for all other keys.
    if (wc->uiManager->SearchBarHasFocus()) {
        if (evt.key == term::input::Key::Escape) {
            if (auto* sc = wc->uiManager->GetActiveSearchController())
                sc->Clear();
            wc->uiManager->ShowSearchBarForActive(false);
            wc->uiManager->EnsureCursorVisibleForActive();
        } else if (evt.key == term::input::Key::FunctionKey && evt.code == 3) {
            if (auto* sc = wc->uiManager->GetActiveSearchController()) {
                if (evt.shift) sc->PrevMatch(); else sc->NextMatch();
            }
        }
        return;
    }

    wc->router->Send(evt);
    wc->uiManager->EnsureCursorVisibleForActive();
}
