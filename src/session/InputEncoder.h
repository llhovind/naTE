#pragma once

#include <string>
#include "input/KeyEvent.hpp"

namespace term::session
{

    class InputEncoder
    {
    public:
        std::string Encode(const ::term::input::KeyEvent &evt);
    };

}
