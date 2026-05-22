#pragma once

namespace term::transport {

enum class DisconnectReason {
    Clean,       // Remote process exited normally (e.g. user typed "exit")
    Interrupted, // Network error, socket drop, or device removed
    Deliberate,  // Transport::Stop() was called — session is being closed intentionally
};

} // namespace term::transport
