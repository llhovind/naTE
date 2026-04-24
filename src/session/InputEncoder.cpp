#include "session/InputEncoder.h"

namespace term::session
{

    std::string InputEncoder::Encode(const ::term::input::KeyEvent &evt)
    {
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
