#pragma once
#include <string>
#include <variant>
#include <vector>

namespace term::session {

struct PtyDesc      { std::string shell; };
struct LoopbackDesc {};

using TransportDesc = std::variant<PtyDesc, LoopbackDesc>;

struct Connection {
    std::string   label;
    TransportDesc transport;

    // Returns the hard-coded set of available connection templates.
    static std::vector<Connection> Defaults(const std::string& shell);
};

} // namespace term::session
