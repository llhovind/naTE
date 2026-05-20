#include "ui/ConnectionFactory.h"
#include <algorithm>
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
            conn.sessionInit.workingDir  = p.workingDir;
            conn.sessionInit.envVars     = p.envVars;
            conn.sessionInit.envFilePath = p.envFilePath;
            conn.sessionInit.loginShell  = p.loginShell;
            conn.profileTitle            = p.profileTitle;
            conn.useProfileTitle         = p.useProfileTitle;

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
            d.x11Forwarding     = p.x11Forwarding;
            d.agentForwarding   = p.agentForwarding;
            d.agentIdentityHint = p.agentIdentityHint;
            d.password          = p.password;
            d.privateKeyPath    = p.privateKeyPath;
            d.passphrase        = p.passphrase;
            switch (p.authMethod) {
                case SshAuthChoice::Password:
                    d.authMethod = term::session::SshAuthMethod::Password; break;
                case SshAuthChoice::PrivateKey:
                    d.authMethod = term::session::SshAuthMethod::PrivateKey; break;
                case SshAuthChoice::KeyboardInteractive:
                    d.authMethod = term::session::SshAuthMethod::KbdInteractive; break;
                default:
                    d.authMethod = term::session::SshAuthMethod::Agent; break;
            }
            conn.transport               = d;
            conn.wrapMode                = p.wrapMode;
            conn.columnWidth             = p.columnWidth;
            conn.rows                    = p.rows;
            conn.sessionInit.workingDir  = p.workingDir;
            conn.sessionInit.envVars     = p.envVars;
            conn.sessionInit.envFilePath = p.envFilePath;
            conn.profileTitle            = p.profileTitle;
            conn.useProfileTitle         = p.useProfileTitle;

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
            d.dialScript                 = p.dialScript;
            conn.transport               = d;
            conn.wrapMode                = p.wrapMode;
            conn.columnWidth             = p.columnWidth;
            conn.rows                    = p.rows;
            conn.sessionInit.envVars     = p.envVars;
            conn.sessionInit.envFilePath = p.envFilePath;
            conn.profileTitle            = p.profileTitle;
            conn.useProfileTitle         = p.useProfileTitle;
        }
    }, params);

    return conn;
}

void SaveProfile(term::db::ConnectionStore& store,
                 const term::session::Connection& conn,
                 const std::string& name,
                 const std::string& existingId)
{
    if (!existingId.empty()) {
        const auto& profiles = store.GetAll();
        auto it = std::find_if(profiles.begin(), profiles.end(),
                               [&](const term::db::ConnectionProfile& p){
                                   return p.id == existingId; });
        if (it == profiles.end()) return;

        term::db::ConnectionProfile upd = *it;
        upd.name            = name;
        upd.transport       = conn.transport;
        upd.wrapMode        = conn.wrapMode;
        upd.columnWidth     = conn.columnWidth;
        upd.rows            = conn.rows;
        upd.sessionInit     = conn.sessionInit;
        upd.profileTitle    = conn.profileTitle;
        upd.useProfileTitle = conn.useProfileTitle;
        store.Update(upd);
    } else {
        store.Add(name, conn.transport, conn.wrapMode, conn.columnWidth, conn.rows,
                  conn.sessionInit, conn.profileTitle, conn.useProfileTitle);
    }
}

} // namespace ui
