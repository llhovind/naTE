#pragma once
#include <string>

namespace term::session {

struct EnvVar {
    std::string key;
    std::string value;
};

} // namespace term::session
