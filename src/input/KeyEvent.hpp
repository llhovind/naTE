#pragma once

#include <cstdint>
#include <string>

namespace term::input {

enum class Key {
    Unknown = 0,

    Character,

    Enter,
    Backspace,
    Tab,
    Escape,

    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,

    Home,
    End,
    PageUp,
    PageDown,

    Insert,
    Delete,

    FunctionKey  // F1–F12 (use code field)
};

struct KeyEvent {
    Key key = Key::Unknown;

    // For Character or FunctionKey
    uint32_t code = 0;   // Unicode codepoint or function index

    bool ctrl = false;
    bool alt = false;
    bool shift = false;

    // Optional precomputed UTF-8 (useful but not required)
    std::string text;

    // Convenience helpers
    bool IsPrintable() const noexcept {
        return key == Key::Character && !text.empty();
    }

};

} // namespace term::input

