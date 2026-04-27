#include "parser/Parser.h"
#include <vector>

namespace term::parser {

Parser::Parser(IParserTarget& target)
    : target_(target)
{}

void Parser::Process(const std::string& data)
{
    for (unsigned char byte : data) {
        switch (state_) {
        case State::Normal:   HandleNormal(byte);   break;
        case State::Escape:   HandleEscape(byte);   break;
        case State::Ss3:      HandleSs3(byte);      break;
        case State::Csi:      HandleCsi(byte);      break;
        case State::Osc:      HandleOsc(byte);      break;
        case State::OscEsc:   HandleOscEsc(byte);   break;
        case State::SkipOne:  HandleSkipOne(byte);  break;
        }
    }
}

void Parser::HandleNormal(unsigned char byte)
{
    if (byte == '\x1b') {
        state_ = State::Escape;
        return;
    }

    if (byte == '\x07')
        return; // BEL — discard

    if (byte == '\b' || byte == '\x7f') {
        target_.OnCursorLeft(1);
        return;
    }

    if (byte == '\n') {
        target_.OnNewLine();
        return;
    }

    if (byte == '\r') {
        target_.OnCarriageReturn();
        return;
    }

    // UTF-8 multi-byte decode
    if (byte < 0x80) {
        target_.OnAppendInsertChar(static_cast<char32_t>(byte));
    } else if ((byte & 0xE0) == 0xC0) {
        utf8_codepoint_ = byte & 0x1F;
        utf8_remaining_ = 1;
    } else if ((byte & 0xF0) == 0xE0) {
        utf8_codepoint_ = byte & 0x0F;
        utf8_remaining_ = 2;
    } else if ((byte & 0xF8) == 0xF0) {
        utf8_codepoint_ = byte & 0x07;
        utf8_remaining_ = 3;
    } else if ((byte & 0xC0) == 0x80) {
        utf8_codepoint_ = (utf8_codepoint_ << 6) | (byte & 0x3F);
        if (--utf8_remaining_ == 0) {
            target_.OnAppendInsertChar(utf8_codepoint_);
            utf8_codepoint_ = 0;
        }
    }
    // Lone continuation bytes with no preceding lead byte are silently dropped
}

void Parser::HandleEscape(unsigned char byte)
{
    if (byte == '[') {
        params_.clear();
        privateMode_ = false;
        state_ = State::Csi;
    } else if (byte == ']') {
        osc_payload_.clear();
        state_ = State::Osc;
    } else if (byte == 'O') {
        state_ = State::Ss3;
    } else if (byte == '(' || byte == ')') {
        // Designate Character Set (G0/G1) — the next byte is the charset selector.
        // We don't need to act on it in a Unicode-aware emulator; just skip it.
        state_ = State::SkipOne;
    } else {
        switch (byte) {
        case 'M': target_.OnReverseIndex();  break;  // Reverse Index
        case '7': target_.OnSaveCursor();    break;  // DECSC
        case '8': target_.OnRestoreCursor(); break;  // DECRC
        default:  break;
        }
        state_ = State::Normal;
    }
}

void Parser::HandleSkipOne(unsigned char /*byte*/)
{
    state_ = State::Normal;
}

// SS3 sequences: ESC O <byte>
// Used for F1–F4 and application-mode cursor/home/end keys.
void Parser::HandleSs3(unsigned char byte)
{
    switch (byte) {
    case 'P': target_.OnFunctionKey(1);       break;
    case 'Q': target_.OnFunctionKey(2);       break;
    case 'R': target_.OnFunctionKey(3);       break;
    case 'S': target_.OnFunctionKey(4);       break;
    case 'A': target_.OnCursorUp(1);          break;
    case 'B': target_.OnCursorDown(1);        break;
    case 'C': target_.OnCursorRight(1);       break;
    case 'D': target_.OnCursorLeft(1);        break;
    case 'H': target_.OnCursorPosition(1, 1); break;
    case 'F': target_.OnCursorEnd();          break;
    default:  break;
    }
    state_ = State::Normal;
}

void Parser::HandleCsi(unsigned char byte)
{
    // Private-mode prefix (e.g. ESC[?25h, ESC[?1049h)
    if (byte == '?' && params_.empty() && !privateMode_) {
        privateMode_ = true;
        return;
    }

    if ((byte >= '0' && byte <= '9') || byte == ';') {
        params_.push_back(static_cast<char>(byte));
    } else if (byte >= 0x40 && byte <= 0x7E) {
        if (privateMode_)
            HandleCsiPrivate(byte);
        else
            HandleCsiFinal(byte);
        params_.clear();
        privateMode_ = false;
        state_ = State::Normal;
    }
    // Intermediate bytes (0x20–0x3F except '?', digits, ';') are silently dropped
}

void Parser::HandleCsiFinal(unsigned char byte)
{
    switch (byte) {
    case 'm': DispatchSgr();                                               break;
    case 'A': target_.OnCursorUp(ParseFirstParam(1));                      break;
    case 'B': target_.OnCursorDown(ParseFirstParam(1));                    break;
    case 'C': target_.OnCursorRight(ParseFirstParam(1));                   break;
    case 'D': target_.OnCursorLeft(ParseFirstParam(1));                    break;
    case 'K': target_.OnEraseInLine(ParseFirstParam(0));                   break;
    case 'H': // fall-through: ESC[H = home, ESC[row;colH = position
    case 'f': target_.OnCursorPosition(ParseParam(0, 1), ParseParam(1, 1)); break;
    case 'F': target_.OnCursorEnd();                                         break;
    case 'J': target_.OnEraseInDisplay(ParseParam(0, 0));                  break;
    case '@': target_.OnInsertChar(ParseFirstParam(1));                     break;
    case 'P': target_.OnDeleteChar(ParseParam(0, 1));                      break;
    case 'r': target_.OnSetScrollRegion(ParseParam(0, 0), ParseParam(1, 0)); break;
    case 'G': target_.OnCursorColumnAbsolute(ParseFirstParam(1));            break;
    case 'd': target_.OnCursorRowAbsolute(ParseFirstParam(1));               break;
    case 's': target_.OnSaveCursor();                                        break;
    case 'u': target_.OnRestoreCursor();                                     break;
    case 'X': target_.OnEraseChar(ParseFirstParam(1));                       break;
    case 'h': if (ParseFirstParam(0) == 4) target_.OnSetInsertMode(true);   break;
    case 'l': if (ParseFirstParam(0) == 4) target_.OnSetInsertMode(false);  break;
    case '~': {
        const int code = ParseParam(0, 0);
        switch (code) {
        case 1: case 7:  target_.OnCursorToLineStart(); break;
        case 2:          /* insert mode toggle — no-op */ break;
        case 3:          target_.OnDeleteChar(1);        break;
        case 4: case 8:  target_.OnCursorEnd();          break;
        case 5:          target_.OnScrollUp(1);          break;
        case 6:          target_.OnScrollDown(1);        break;
        case 11:         target_.OnFunctionKey(1);       break;
        case 12:         target_.OnFunctionKey(2);       break;
        case 13:         target_.OnFunctionKey(3);       break;
        case 14:         target_.OnFunctionKey(4);       break;
        case 15:         target_.OnFunctionKey(5);       break;
        case 17:         target_.OnFunctionKey(6);       break;
        case 18:         target_.OnFunctionKey(7);       break;
        case 19:         target_.OnFunctionKey(8);       break;
        case 20:         target_.OnFunctionKey(9);       break;
        case 21:         target_.OnFunctionKey(10);      break;
        case 23:         target_.OnFunctionKey(11);      break;
        case 24:         target_.OnFunctionKey(12);      break;
        default: break;
        }
        break;
    }
    default: break;
    }
}

// Private-mode CSI: ESC[?<param>h or ESC[?<param>l
void Parser::HandleCsiPrivate(unsigned char byte)
{
    const int code = ParseFirstParam(0);
    if (byte == 'h') {
        switch (code) {
        case 25:   target_.OnSetCursorVisibility(true); break;
        case 1049: target_.OnEnterAltScreen();           break;
        default:   break;
        }
    } else if (byte == 'l') {
        switch (code) {
        case 25:   target_.OnSetCursorVisibility(false); break;
        case 1049: target_.OnExitAltScreen();             break;
        default:   break;
        }
    }
}

void Parser::HandleOsc(unsigned char byte)
{
    if (byte == '\x07') {
        DispatchOsc();
        state_ = State::Normal;
    } else if (byte == '\x1b') {
        state_ = State::OscEsc;
    } else {
        osc_payload_.push_back(static_cast<char>(byte));
    }
}

void Parser::HandleOscEsc(unsigned char byte)
{
    if (byte == '\\')
        DispatchOsc();
    state_ = State::Normal;
}

void Parser::DispatchOsc()
{
    const auto sep = osc_payload_.find(';');
    if (sep == std::string::npos)
        return;

    int code = 0;
    for (size_t i = 0; i < sep; ++i) {
        if (osc_payload_[i] < '0' || osc_payload_[i] > '9') return;
        code = code * 10 + (osc_payload_[i] - '0');
    }

    if (code == 0 || code == 1 || code == 2)
        target_.OnSetTitle(osc_payload_.substr(sep + 1));
}

// Returns the n-th ';'-delimited parameter (0-indexed).
// Returns defaultVal if the segment is absent, empty, or zero
// (VT100 convention: missing/zero param means "use default").
int Parser::ParseParam(int index, int defaultVal) const
{
    int  current = 0;
    int  count   = 0;

    for (size_t i = 0; i <= params_.size(); ++i) {
        if (i == params_.size() || params_[i] == ';') {
            if (count == index)
                return (current <= 0) ? defaultVal : current;
            ++count;
            current = 0;
        } else {
            current = current * 10 + (params_[i] - '0');
        }
    }
    return defaultVal;
}

int Parser::ParseFirstParam(int defaultVal) const
{
    return ParseParam(0, defaultVal);
}

void Parser::DispatchSgr()
{
    // Parse ';'-delimited parameter string into a flat integer list.
    // An empty or absent segment is treated as 0 (VT100 convention).
    std::vector<int> params;
    {
        int  current  = 0;
        bool hasDigit = false;
        for (char c : params_) {
            if (c == ';') {
                params.push_back(hasDigit ? current : 0);
                current  = 0;
                hasDigit = false;
            } else {
                current  = current * 10 + (c - '0');
                hasDigit = true;
            }
        }
        params.push_back(hasDigit ? current : 0);
    }

    Style style;
    bool  reset   = false;
    bool  have_fg = false;
    bool  have_bg = false;
    bool  bold    = false;

    for (size_t i = 0; i < params.size(); ++i) {
        const int p = params[i];
        if (p == 0) {
            reset = true;
        } else if (p == 1) {
            bold = true;
        } else if (p == 22) {
            bold = false;
        } else if (p >= 30 && p <= 37) {
            style.fg = p - 30;   // palette indices 0–7
            have_fg  = true;
        } else if (p == 39) {
            style.fg = -1;       // default fg
            have_fg  = true;
        } else if (p >= 40 && p <= 47) {
            style.bg = p - 40;   // palette indices 0–7
            have_bg  = true;
        } else if (p == 49) {
            style.bg = -1;       // default bg
            have_bg  = true;
        } else if (p >= 90 && p <= 97) {
            style.fg = p - 90 + 8;   // bright palette indices 8–15
            have_fg  = true;
        } else if (p >= 100 && p <= 107) {
            style.bg = p - 100 + 8;  // bright palette indices 8–15
            have_bg  = true;
        } else if ((p == 38 || p == 48) && i + 2 < params.size() && params[i + 1] == 5) {
            const int index = params[i + 2];
            if (index >= 0 && index <= 255) {
                if (p == 38) { style.fg = index; have_fg = true; }
                else         { style.bg = index; have_bg = true; }
            }
            i += 2;
        }
    }

    if (reset) {
        style = Style{};
    } else {
        style.bold = bold;
        if (!have_fg) style.fg = -1;
        if (!have_bg) style.bg = -1;
    }

    target_.OnSetStyle(style);
}

} // namespace term::parser
