#pragma once
#include "transport/TransportDesc.h"
#include <string>
#include <vector>

namespace term::session {

// Session-domain composition: a labelled, persistable description of how to
// open one terminal session. The transport-specific configuration lives in
// transport/TransportDesc.h — session depends on transport, never the reverse.
// (Types are spelled term::transport:: in full because the `transport` data
// member shadows the namespace inside this class scope.)
struct Connection {
    std::string                     label;
    term::transport::TransportDesc  transport;
    bool                            wrapMode        = false;
    unsigned short                  columnWidth     = 0;    // 0 = use app config default
    unsigned short                  rows            = 0;    // 0 = use app config default
    term::transport::SessionInit    sessionInit;            // pre-launch setup applied by all transports
    std::string                     profileTitle;           // static title; empty = use transport-provided title
    bool                            useProfileTitle = false;// if true, profileTitle overrides OSC title changes

    // Returns the hard-coded set of available connection templates.
    static std::vector<Connection> Defaults(const std::string& shell);
};

} // namespace term::session
