#pragma once
#include <string>

namespace term::transport {

struct TransportError {
    enum class Category { Connection, Authentication, HostKey, Protocol };
    Category    category;
    std::string message;
};

} // namespace term::transport
