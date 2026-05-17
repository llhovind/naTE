#include "ui/ConnectionFactory.h"
#include <wx/string.h>

namespace ui {

term::session::Connection ToConnection(const ConnectionParams& params, int labelIdx)
{
    term::session::Connection conn;

    std::visit([&](const auto& p) {
        using T = std::decay_t<decltype(p)>;

        if constexpr (std::is_same_v<T, PtyParams>) {
            conn.label       = labelIdx ? wxString::Format("Local Shell %d", labelIdx).ToStdString()
                                        : "Local Shell";
            conn.transport   = term::session::PtyDesc{ p.shell };
            conn.wrapMode    = p.wrapMode;
            conn.columnWidth = p.columnWidth;
            conn.rows        = p.rows;

        } else if constexpr (std::is_same_v<T, SshParams>) {
            conn.label       = labelIdx
                ? wxString::Format("SSH %s@%s:%d #%d",
                                   p.username, p.host,
                                   static_cast<int>(p.port), labelIdx).ToStdString()
                : (p.username.empty() ? p.host : p.username + "@" + p.host);
            term::session::SshDesc d;
            d.host              = p.host;
            d.port              = p.port;
            d.username          = p.username;
            d.connectTimeoutSec = p.connectTimeoutSec;
            d.keepaliveSeconds  = p.keepaliveSeconds;
            d.remoteCommand     = p.remoteCommand;
            d.compress          = p.compress;
            d.password          = p.password;
            d.privateKeyPath    = p.privateKeyPath;
            d.passphrase        = p.passphrase;
            switch (p.authMethod) {
                case SshAuthChoice::Password:
                    d.authMethod = term::session::SshAuthMethod::Password; break;
                case SshAuthChoice::PrivateKey:
                    d.authMethod = term::session::SshAuthMethod::PrivateKey; break;
                default:
                    d.authMethod = term::session::SshAuthMethod::Agent; break;
            }
            conn.transport   = d;
            conn.wrapMode    = p.wrapMode;
            conn.columnWidth = p.columnWidth;
            conn.rows        = p.rows;

        } else if constexpr (std::is_same_v<T, SerialParams>) {
            conn.label       = labelIdx
                ? wxString::Format("Serial %s #%d", p.device, labelIdx).ToStdString()
                : p.device;
            term::session::SerialDesc d;
            d.device      = p.device;
            d.baudRate    = p.baudRate;
            d.dataBits    = p.dataBits;
            d.stopBits    = p.stopBits == "2" ? term::session::SerialStopBits::Two
                                               : term::session::SerialStopBits::One;
            if      (p.parity == "Even") d.parity = term::session::SerialParity::Even;
            else if (p.parity == "Odd")  d.parity = term::session::SerialParity::Odd;
            else                         d.parity = term::session::SerialParity::None;
            if      (p.flowControl == "Hardware") d.flowControl = term::session::SerialFlowControl::Hardware;
            else if (p.flowControl == "Software") d.flowControl = term::session::SerialFlowControl::Software;
            else                                  d.flowControl = term::session::SerialFlowControl::None;
            d.dialScript     = p.dialScript;
            conn.transport   = d;
            conn.wrapMode    = p.wrapMode;
            conn.columnWidth = p.columnWidth;
            conn.rows        = p.rows;
        }
    }, params);

    return conn;
}

} // namespace ui
