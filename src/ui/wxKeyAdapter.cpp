#include "ui/wxKeyAdapter.h"

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
    case WXK_RETURN: evt.key = Key::Enter; break;
    case WXK_BACK:   evt.key = Key::Backspace; break;
    case WXK_TAB:    evt.key = Key::Tab; break;
    case WXK_ESCAPE: evt.key = Key::Escape; break;

    case WXK_UP:    evt.key = Key::ArrowUp; break;
    case WXK_DOWN:  evt.key = Key::ArrowDown; break;
    case WXK_LEFT:  evt.key = Key::ArrowLeft; break;
    case WXK_RIGHT: evt.key = Key::ArrowRight; break;

    default:
        if (code >= 32 && code < 127) {
            evt.key = Key::Character;
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

