#include "session/InputEncoder.h"

namespace term::session
{

static const char* const kFnSeqs[12] = {
    "\x1bOP",   // F1
    "\x1bOQ",   // F2
    "\x1bOR",   // F3
    "\x1bOS",   // F4
    "\x1b[15~", // F5
    "\x1b[17~", // F6  (no 16)
    "\x1b[18~", // F7
    "\x1b[19~", // F8
    "\x1b[20~", // F9
    "\x1b[21~", // F10
    "\x1b[23~", // F11 (no 22)
    "\x1b[24~", // F12
};

std::string InputEncoder::Encode(const ::term::input::KeyEvent &evt)
{
    if (evt.ctrl && evt.key == term::input::Key::Character) {
        const uint32_t lower = (evt.code >= 'A' && evt.code <= 'Z')
            ? evt.code + 32 : evt.code;
        if (lower >= 'a' && lower <= 'z')
            return std::string(1, static_cast<char>(lower - 'a' + 1));
    }

    if (evt.IsPrintable())
        return evt.text;

    switch (evt.key)
    {
    case term::input::Key::Enter:     return "\r";
    case term::input::Key::Backspace: return "\x7f";
    case term::input::Key::Tab:       return "\t";
    case term::input::Key::Escape:    return "\x1b";
    case term::input::Key::ArrowUp:   return appCursorKeys_ ? "\x1bOA" : "\x1b[A";
    case term::input::Key::ArrowDown: return appCursorKeys_ ? "\x1bOB" : "\x1b[B";
    case term::input::Key::ArrowRight:return appCursorKeys_ ? "\x1bOC" : "\x1b[C";
    case term::input::Key::ArrowLeft: return appCursorKeys_ ? "\x1bOD" : "\x1b[D";
    case term::input::Key::Home:      return "\x1b[H";
    case term::input::Key::End:       return "\x1b[F";
    case term::input::Key::PageUp:    return "\x1b[5~";
    case term::input::Key::PageDown:  return "\x1b[6~";
    case term::input::Key::Insert:    return "\x1b[2~";
    case term::input::Key::Delete:    return "\x1b[3~";
    case term::input::Key::FunctionKey: {
        const uint32_t idx = evt.code - 1;
        return (idx < 12) ? kFnSeqs[idx] : "";
    }
    default:
        return "";
    }
}

}
