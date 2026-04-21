#include "app/App.h"
#include "ui/MainFrame.h"
#include "ui/wxKeyAdapter.h"
#include "input/KeyEvent.hpp"
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>

static void GdkEventHandler(GdkEvent* event, gpointer data)
{
    if (event->type == GDK_KEY_PRESS) {
        static_cast<App*>(data)->OnGdkKeyPress(event);
    }
    gtk_main_do_event(event);
}

bool App::OnInit() {
    const wxString exeDir =
        wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
    m_cfg = AppConfig::load(exeDir + wxFileName::GetPathSeparator() + "config.ini");

    m_router = std::make_unique<term::input::InputRouter>();
    gdk_event_handler_set(GdkEventHandler, this, nullptr);

    auto* frame = new MainFrame(m_cfg, *m_router);
    frame->Show();
    return true;
}

void App::OnGdkKeyPress(GdkEvent* event)
{
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

    m_router->Send(evt);
}
