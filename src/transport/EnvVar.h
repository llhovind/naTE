#pragma once
#include <string>

namespace term::transport {

struct EnvVar {
    std::string key;
    std::string value;
};

} // namespace term::transport
