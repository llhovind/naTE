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
        default:
            return "";
        }
    }

}
