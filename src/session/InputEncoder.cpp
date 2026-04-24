#include "session/InputEncoder.h"

namespace term::session
{

    std::string InputEncoder::Encode(const ::term::input::KeyEvent &evt)
    {
        if (evt.ctrl && evt.key == term::input::Key::Character) {
            const uint32_t lower = (evt.code >= 'A' && evt.code <= 'Z')
                ? evt.code + 32 : evt.code;
            if (lower >= 'a' && lower <= 'z')
                return std::string(1, static_cast<char>(lower - 'a' + 1));
        }

        if (evt.IsPrintable())
        {
            return evt.text;
        }

        switch (evt.key)
        {
        case term::input::Key::Enter:
            return "\r";
        case term::input::Key::Backspace:
            return "\x7f";
        case term::input::Key::Tab:
            return "\t";
        case term::input::Key::ArrowUp:
            return "\x1b[A";
        case term::input::Key::ArrowDown:
            return "\x1b[B";
        case term::input::Key::ArrowRight:
            return "\x1b[C";
        case term::input::Key::ArrowLeft:
            return "\x1b[D";
        default:
            return "";
        }
    }

}
