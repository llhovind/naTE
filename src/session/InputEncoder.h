#pragma once

#include <string>
#include "input/KeyEvent.hpp"

namespace term::session
{

    class InputEncoder
    {
    public:
        std::string Encode(const ::term::input::KeyEvent &evt);

        // Mirrors DECCKM (?1h / ?1l): when true, arrow keys emit ESC O{A,B,C,D}
        // instead of ESC [{A,B,C,D}.
        void SetApplicationCursorKeys(bool enabled) noexcept { appCursorKeys_ = enabled; }
        bool ApplicationCursorKeys() const noexcept          { return appCursorKeys_; }

    private:
        bool appCursorKeys_ = false;
    };

}
