#pragma once
#include <string>
#include <vector>

#include "transport/TransportError.h"

namespace term::transport {

struct KbdIntPrompt {
    std::string text;
    bool        echo;   // false → mask input (password-like)
};

struct KbdIntChallenge {
    std::string                name;
    std::string                instruction;
    std::vector<KbdIntPrompt>  prompts;
};

class ITransportTarget {
public:
    virtual ~ITransportTarget() = default;
    virtual void OnData(const std::string& data) = 0;
    virtual void OnError(const TransportError& error) = 0;
    virtual void OnDisconnect() = 0;

    // Fired on the worker thread when X11 forwarding becomes active on a channel.
    // Implementations must CallAfter before touching wx objects.
    virtual void OnX11StateChanged(bool /*active*/) {}

    // Fired on the worker thread during keyboard-interactive SSH auth.
    // Implementations must dispatch to the UI thread and block until the user
    // responds (e.g. via std::promise/future + wxCallAfter).
    // Returning an empty vector causes authentication failure.
    virtual std::vector<std::string> OnKbdIntChallenge(
        const KbdIntChallenge& /*challenge*/) { return {}; }
};

} // namespace term::transport
