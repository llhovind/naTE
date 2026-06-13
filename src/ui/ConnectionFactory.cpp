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
            conn.transport   = term::transport::PtyDesc{ p.shell, p.command };
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
            term::transport::SshDesc d;
            d.host              = p.host;
            d.port              = p.port;
            d.username          = p.username;
            d.connectTimeoutSec = p.connectTimeoutSec;
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
                    d.authMethod = term::transport::SshAuthMethod::Password; break;
                case SshAuthChoice::PrivateKey:
                    d.authMethod = term::transport::SshAuthMethod::PrivateKey; break;
                case SshAuthChoice::KeyboardInteractive:
                    d.authMethod = term::transport::SshAuthMethod::KbdInteractive; break;
                default:
                    d.authMethod = term::transport::SshAuthMethod::Agent; break;
            }
            if (p.proxyJump) {
                term::transport::ProxyJumpDesc pj;
                pj.host              = p.proxyJump->host;
                pj.port              = p.proxyJump->port;
                pj.user              = p.proxyJump->user;
                pj.password          = p.proxyJump->password;
                pj.privateKeyPath    = p.proxyJump->privateKeyPath;
                pj.passphrase        = p.proxyJump->passphrase;
                pj.agentIdentityHint = p.proxyJump->agentIdentityHint;
                switch (p.proxyJump->authMethod) {
                    case SshAuthChoice::Password:
                        pj.authMethod = term::transport::SshAuthMethod::Password; break;
                    case SshAuthChoice::PrivateKey:
                        pj.authMethod = term::transport::SshAuthMethod::PrivateKey; break;
                    case SshAuthChoice::KeyboardInteractive:
                        pj.authMethod = term::transport::SshAuthMethod::KbdInteractive; break;
                    default:
                        pj.authMethod = term::transport::SshAuthMethod::Agent; break;
                }
                d.proxyJump = std::move(pj);
            }
            d.portForwards               = p.portForwards;
            conn.transport               = d;
            conn.wrapMode                = p.wrapMode;
            conn.columnWidth             = p.columnWidth;
            conn.rows                    = p.rows;
            conn.sessionInit.workingDir  = p.workingDir;
            conn.sessionInit.envVars     = p.envVars;
            conn.sessionInit.envFilePath = p.envFilePath;
            conn.sessionInit.loginShell  = p.loginShell;
            conn.profileTitle            = p.profileTitle;
            conn.useProfileTitle         = p.useProfileTitle;

        } else if constexpr (std::is_same_v<T, SerialParams>) {
            conn.label       = labelIdx
                ? wxString::Format("Serial %s #%d", p.device, labelIdx).ToStdString()
                : p.device;
            term::transport::SerialDesc d;
            d.device      = p.device;
            d.baudRate    = p.baudRate;
            d.dataBits    = p.dataBits;
            d.stopBits    = p.stopBits == "2" ? term::transport::SerialStopBits::Two
                                               : term::transport::SerialStopBits::One;
            if      (p.parity == "Even") d.parity = term::transport::SerialParity::Even;
            else if (p.parity == "Odd")  d.parity = term::transport::SerialParity::Odd;
            else                         d.parity = term::transport::SerialParity::None;
            if      (p.flowControl == "Hardware") d.flowControl = term::transport::SerialFlowControl::Hardware;
            else if (p.flowControl == "Software") d.flowControl = term::transport::SerialFlowControl::Software;
            else                                  d.flowControl = term::transport::SerialFlowControl::None;
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
