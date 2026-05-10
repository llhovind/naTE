#include "ui/wxKeyAdapter.h"
#include <cctype>

namespace term::ui::wx {

using namespace term::input;

KeyEvent ConvertKey(const wxKeyEvent& e)
{
    KeyEvent evt;

    evt.ctrl  = e.ControlDown();
    evt.alt   = e.AltDown();
    evt.shift = e.ShiftDown();

    int code = e.GetKeyCode();

    switch (code) {
    case WXK_RETURN:   evt.key = Key::Enter;     break;
    case WXK_BACK:     evt.key = Key::Backspace; break;
    case WXK_TAB:      evt.key = Key::Tab;       break;
    case WXK_ESCAPE:   evt.key = Key::Escape;    break;

    case WXK_UP:       evt.key = Key::ArrowUp;    break;
    case WXK_DOWN:     evt.key = Key::ArrowDown;  break;
    case WXK_LEFT:     evt.key = Key::ArrowLeft;  break;
    case WXK_RIGHT:    evt.key = Key::ArrowRight; break;

    case WXK_HOME:     evt.key = Key::Home;     break;
    case WXK_END:      evt.key = Key::End;      break;
    case WXK_PAGEUP:   evt.key = Key::PageUp;   break;
    case WXK_PAGEDOWN: evt.key = Key::PageDown; break;
    case WXK_INSERT:   evt.key = Key::Insert;   break;
    case WXK_DELETE:   evt.key = Key::Delete;   break;

    case WXK_F1:  case WXK_F2:  case WXK_F3:  case WXK_F4:
    case WXK_F5:  case WXK_F6:  case WXK_F7:  case WXK_F8:
    case WXK_F9:  case WXK_F10: case WXK_F11: case WXK_F12:
        evt.key  = Key::FunctionKey;
        evt.code = static_cast<uint32_t>(code - WXK_F1 + 1);
        break;

    default:
        // wx KEY_DOWN delivers uppercase letters regardless of shift; normalize to
        // lowercase to match GDK's natural keysym values (e.g. Ctrl+V → code='v').
        if (code >= 'A' && code <= 'Z')
            code = std::tolower(code);
        if (code >= 32 && code < 127) {
            evt.key  = Key::Character;
            evt.code = static_cast<uint32_t>(code);
            evt.text = std::string(1, static_cast<char>(code));
        } else {
            evt.key = Key::Unknown;
        }
        break;
    }

    return evt;
}

}

