#pragma once

#include <wx/event.h>
#include "input/KeyEvent.hpp"

namespace term::ui::wx {

term::input::KeyEvent ConvertKey(const wxKeyEvent& e);

}

