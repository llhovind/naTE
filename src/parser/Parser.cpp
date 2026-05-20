#include "parser/Parser.h"
#include <vector>

namespace term::parser {

Parser::Parser(IDocumentTarget& docTarget, IScreenTarget& screenTarget)
    : doc_(&docTarget), screen_(screenTarget)
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
        doc_->MoveCursorLeft(1);
        return;
    }

    if (byte == '\n') {
        doc_->NewLine();
        return;
    }

    if (byte == '\r') {
        doc_->CarriageReturn();
        return;
    }

    // UTF-8 multi-byte decode
    if (byte < 0x80) {
        doc_->AppendInsertChar(static_cast<char32_t>(byte));
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
            doc_->AppendInsertChar(utf8_codepoint_);
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
        case 'M': doc_->ReverseIndex();  break;  // Reverse Index
        case '7': doc_->SaveCursor();    break;  // DECSC
        case '8': doc_->RestoreCursor(); break;  // DECRC
        case 'c':                                // RIS — Reset to Initial State
            screen_.OnResetTerminal();
            Reset();
            break;
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
    case 'P': screen_.OnFunctionKey(1);            break;
    case 'Q': screen_.OnFunctionKey(2);            break;
    case 'R': screen_.OnFunctionKey(3);            break;
    case 'S': screen_.OnFunctionKey(4);            break;
    case 'A': doc_->MoveCursorUp(1);               break;
    case 'B': doc_->MoveCursorDown(1);             break;
    case 'C': doc_->MoveCursorRight(1);            break;
    case 'D': doc_->MoveCursorLeft(1);             break;
    case 'H': doc_->MoveCursorToPosition(1, 1);    break;
    case 'F': doc_->MoveCursorToLineEnd();         break;
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
    case 'm': DispatchSgr();                                                        break;
    case 'A': doc_->MoveCursorUp(ParseFirstParam(1));                               break;
    case 'B': doc_->MoveCursorDown(ParseFirstParam(1));                             break;
    case 'C': doc_->MoveCursorRight(ParseFirstParam(1));                            break;
    case 'D': doc_->MoveCursorLeft(ParseFirstParam(1));                             break;
    case 'K': doc_->EraseInLine(ParseFirstParam(0));                                break;
    case 'H': // fall-through: ESC[H = home, ESC[row;colH = position
    case 'f': doc_->MoveCursorToPosition(ParseParam(0, 1), ParseParam(1, 1));       break;
    case 'F': doc_->MoveCursorToLineEnd();                                          break;
    case 'J': doc_->EraseInDisplay(ParseParam(0, 0));                               break;
    case '@': doc_->InsertChar(ParseFirstParam(1));                                 break;
    case 'P': doc_->DeleteChar(ParseParam(0, 1));                                   break;
    case 'r': doc_->SetScrollRegion(ParseParam(0, 0), ParseParam(1, 0));            break;
    case 'G': doc_->MoveCursorToColumn(ParseFirstParam(1));                         break;
    case 'd': doc_->MoveCursorToRow(ParseFirstParam(1));                            break;
    case 's': doc_->SaveCursor();                                                   break;
    case 'u': doc_->RestoreCursor();                                                break;
    case 'L': doc_->InsertLines(ParseFirstParam(1));                                break;
    case 'M': doc_->DeleteLines(ParseFirstParam(1));                                break;
    case 'X': doc_->EraseChar(ParseFirstParam(1));                                  break;
    case 'h': if (ParseFirstParam(0) == 4) doc_->SetInsertMode(true);              break;
    case 'l': if (ParseFirstParam(0) == 4) doc_->SetInsertMode(false);             break;
    case '~': {
        const int code = ParseParam(0, 0);
        switch (code) {
        case 1: case 7:  doc_->MoveCursorToLineStart();  break;
        case 2:          /* insert mode toggle — no-op */ break;
        case 3:          doc_->DeleteChar(1);             break;
        case 4: case 8:  doc_->MoveCursorToLineEnd();     break;
        case 5:          screen_.OnScrollUp(1);           break;
        case 6:          screen_.OnScrollDown(1);         break;
        case 11:         screen_.OnFunctionKey(1);        break;
        case 12:         screen_.OnFunctionKey(2);        break;
        case 13:         screen_.OnFunctionKey(3);        break;
        case 14:         screen_.OnFunctionKey(4);        break;
        case 15:         screen_.OnFunctionKey(5);        break;
        case 17:         screen_.OnFunctionKey(6);        break;
        case 18:         screen_.OnFunctionKey(7);        break;
        case 19:         screen_.OnFunctionKey(8);        break;
        case 20:         screen_.OnFunctionKey(9);        break;
        case 21:         screen_.OnFunctionKey(10);       break;
        case 23:         screen_.OnFunctionKey(11);       break;
        case 24:         screen_.OnFunctionKey(12);       break;
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
        case 1:    screen_.OnSetApplicationCursorKeys(true);  break;
        case 25:   screen_.OnSetCursorVisibility(true);       break;
        case 1049: screen_.OnEnterAltScreen();                break;
        default:   break;
        }
    } else if (byte == 'l') {
        switch (code) {
        case 1:    screen_.OnSetApplicationCursorKeys(false); break;
        case 25:   screen_.OnSetCursorVisibility(false);      break;
        case 1049: screen_.OnExitAltScreen();                 break;
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
        doc_->SetTitle(osc_payload_.substr(sep + 1));
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

    // Mutate currentStyle_ incrementally so that separate SGR sequences
    // (e.g. ESC[1m then ESC[32m) correctly accumulate bold + green.
    for (size_t i = 0; i < params.size(); ++i) {
        const int p = params[i];
        if (p == 0) {
            currentStyle_ = Style{};                    // full reset
        } else if (p == 1) {
            currentStyle_.bold = true;
        } else if (p == 2) {
            currentStyle_.dim  = true;
        } else if (p == 3) {
            currentStyle_.italic = true;
        } else if (p == 4) {
            currentStyle_.underline = true;
        } else if (p == 5 || p == 6) {
            // blink — no-op
        } else if (p == 7) {
            currentStyle_.reverse = true;
        } else if (p == 22) {
            currentStyle_.bold = false;
            currentStyle_.dim  = false;
        } else if (p == 23) {
            currentStyle_.italic = false;
        } else if (p == 24) {
            currentStyle_.underline = false;
        } else if (p == 25) {
            // blink off — no-op
        } else if (p == 27) {
            currentStyle_.reverse = false;
        } else if (p >= 30 && p <= 37) {
            currentStyle_.fg    = p - 30;
            currentStyle_.fgRgb = std::nullopt;
        } else if (p == 39) {
            currentStyle_.fg    = -1;
            currentStyle_.fgRgb = std::nullopt;
        } else if (p >= 40 && p <= 47) {
            currentStyle_.bg    = p - 40;
            currentStyle_.bgRgb = std::nullopt;
        } else if (p == 49) {
            currentStyle_.bg    = -1;
            currentStyle_.bgRgb = std::nullopt;
        } else if (p >= 90 && p <= 97) {
            currentStyle_.fg    = p - 90 + 8;
            currentStyle_.fgRgb = std::nullopt;
        } else if (p >= 100 && p <= 107) {
            currentStyle_.bg    = p - 100 + 8;
            currentStyle_.bgRgb = std::nullopt;
        } else if ((p == 38 || p == 48) && i + 1 < params.size()) {
            if (params[i + 1] == 5 && i + 2 < params.size()) {
                // 256-colour palette: ESC[38;5;Nm
                const int index = params[i + 2];
                if (index >= 0 && index <= 255) {
                    if (p == 38) { currentStyle_.fg = index; currentStyle_.fgRgb = std::nullopt; }
                    else         { currentStyle_.bg = index; currentStyle_.bgRgb = std::nullopt; }
                }
                i += 2;
            } else if (params[i + 1] == 2 && i + 4 < params.size()) {
                // 24-bit true colour: ESC[38;2;r;g;bm
                Rgb rgb{ static_cast<uint8_t>(params[i + 2]),
                         static_cast<uint8_t>(params[i + 3]),
                         static_cast<uint8_t>(params[i + 4]) };
                if (p == 38) { currentStyle_.fgRgb = rgb; currentStyle_.fg = -1; }
                else         { currentStyle_.bgRgb = rgb; currentStyle_.bg = -1; }
                i += 4;
            }
        }
    }

    doc_->SetCurrentStyle(currentStyle_);
}

void Parser::Reset()
{
    state_          = State::Normal;
    params_.clear();
    privateMode_    = false;
    osc_payload_.clear();
    utf8_codepoint_ = 0;
    utf8_remaining_ = 0;
    currentStyle_   = Style{};
    doc_->SetCurrentStyle(currentStyle_);
}

} // namespace term::parser
