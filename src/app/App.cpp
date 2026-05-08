#include "app/App.h"
#include "ui/MainFrame.h"
#include "ui/wxKeyAdapter.h"
#include "ui/SearchController.h"
#include "input/KeyEvent.hpp"
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <libssh2.h>

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

    m_router         = std::make_unique<term::input::InputRouter>();
    m_sessionManager = std::make_unique<term::session::SessionManager>(*m_router);

    gdk_event_handler_set(GdkEventHandler, this, nullptr);

    auto* frame = new MainFrame(m_cfg, *m_router, *m_sessionManager);

    m_uiManager = std::make_unique<ui::UIManager>(
        *m_sessionManager, frame->GetConnMenu(), frame, m_cfg, *m_router,
        frame->GetEditMenu());
    m_sessionManager->SetObserver(m_uiManager.get());

    frame->Show();
    return true;
}

int App::OnExit()
{
    libssh2_exit();
    return wxApp::OnExit();
}

void App::OnGdkKeyPress(GdkEvent* event)
{
    // Native GTK modal dialogs (e.g. wxFileDialog) use gtk_grab_add() internally,
    // so gtk_grab_get_current() is non-null while they are active. Check this first
    // because wx focus tracking does not update for native GTK windows, making
    // wxWindow::FindFocus() unreliable in that case.
    if (gtk_grab_get_current() != nullptr)
        return;

    // Pure wx dialogs (e.g. NewConnectionDialog) don't use GTK grabs, but wx
    // focus tracking does work for them: the focused widget's top-level parent
    // will be the dialog itself, not a MainFrame.
    wxWindow* const focused = wxWindow::FindFocus();
    if (!focused || !dynamic_cast<MainFrame*>(wxGetTopLevelParent(focused)))
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
        gtk_clipboard_request_text(
            gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
            [](GtkClipboard*, const gchar* text, gpointer data) {
                if (!text) return;
                auto* self = static_cast<App*>(data);
                self->m_router->Paste(std::string(text));
                self->m_uiManager->EnsureCursorVisibleForActive();
            },
            this);
        return;
    }

    // Ctrl+F — open/focus the search bar (never send to PTY).
    // If text is selected, pre-populate the bar with the selection.
    if (evt.ctrl && evt.key == term::input::Key::Character && evt.code == 'f') {
        m_uiManager->ShowSearchBarForActive(true, m_uiManager->GetActiveSelectedText());
        return;
    }

    // When the search bar has focus, intercept F3/Shift+F3/Escape and suppress
    // PTY routing for all other keys (wxWidgets still dispatches them to the
    // focused text control via the gtk_main_do_event() call in GdkEventHandler).
    if (m_uiManager->SearchBarHasFocus()) {
        if (evt.key == term::input::Key::Escape) {
            if (auto* sc = m_uiManager->GetActiveSearchController())
                sc->Clear();
            m_uiManager->ShowSearchBarForActive(false);
            m_uiManager->EnsureCursorVisibleForActive();
        } else if (evt.key == term::input::Key::FunctionKey && evt.code == 3) {
            if (auto* sc = m_uiManager->GetActiveSearchController()) {
                if (evt.shift) sc->PrevMatch(); else sc->NextMatch();
            }
        }
        return;
    }

    m_router->Send(evt);
    m_uiManager->EnsureCursorVisibleForActive();
}
