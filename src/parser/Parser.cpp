#include "parser/Parser.h"
#include <sstream>

namespace term::parser {

Parser::Parser(IParserTarget& target)
    : target_(target)
{}

void Parser::Process(const std::string& data)
{
    for (unsigned char byte : data) {
        switch (state_) {
        case State::Normal: HandleNormal(byte); break;
        case State::Escape: HandleEscape(byte); break;
        case State::Csi:    HandleCsi(byte);    break;
        }
    }
}

void Parser::HandleNormal(unsigned char byte)
{
    if (byte == '\x1b') {
        state_ = State::Escape;
        return;
    }

    if (byte == '\b' || byte == '\x7f') {
        target_.OnBackspace();
        return;
    }

    if (byte == '\n') {
        target_.OnNewLine();
        return;
    }

    if (byte == '\r') {
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
        state_ = State::Csi;
    } else {
        state_ = State::Normal;
    }
}

void Parser::HandleCsi(unsigned char byte)
{
    if ((byte >= '0' && byte <= '9') || byte == ';') {
        params_.push_back(static_cast<char>(byte));
    } else if (byte >= 0x40 && byte <= 0x7E) {
        if (byte == 'm')
            DispatchSgr();
        params_.clear();
        state_ = State::Normal;
    }
}

void Parser::DispatchSgr()
{
    Style style;
    bool  reset   = false;
    bool  have_fg = false;
    bool  have_bg = false;
    bool  bold    = false;

    std::istringstream ss(params_.empty() ? "0" : params_);
    std::string token;
    while (std::getline(ss, token, ';')) {
        int p = token.empty() ? 0 : std::stoi(token);
        if      (p == 0)               { reset = true; }
        else if (p == 1)               { bold = true; }
        else if (p == 22)              { bold = false; }
        else if (p >= 30 && p <= 37)   { style.fg = p; have_fg = true; }
        else if (p >= 40 && p <= 47)   { style.bg = p; have_bg = true; }
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
