#include "transport/SshAuthenticator.h"
#include "transport/ITransportTarget.h"   // ITransportTarget, KbdIntChallenge
#include "transport/SshConfig.h"          // QuerySshConfigIdentities
#include "transport/SshPublicKey.h"       // LoadPublicKeyBlob
#include "transport/SshSession.h"         // ssh::PollUntilReady, ssh::LastSshError, kPollTimeoutMs

#include <libssh2.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace term::transport {

namespace {

using Cat = TransportError::Category;

// Returns preferred public-key blobs for agent auth derived from the
// connection's agentIdentityHint (first priority) or ~/.ssh/config lookup
// (second priority).
std::vector<std::vector<uint8_t>> LoadPreferredBlobs(const SshDesc& desc)
{
    std::vector<std::filesystem::path> paths;

    if (!desc.agentIdentityHint.empty()) {
        std::filesystem::path p(desc.agentIdentityHint);
        if (p.extension() != ".pub") p += ".pub";
        paths.push_back(std::move(p));
    } else {
        paths = QuerySshConfigIdentities(desc.host, desc.port, desc.username);
        for (auto& p : paths)
            if (p.extension() != ".pub") p += ".pub";
    }

    std::vector<std::vector<uint8_t>> blobs;
    blobs.reserve(paths.size());
    for (const auto& p : paths) {
        auto blob = LoadPublicKeyBlob(p);
        if (!blob.empty())
            blobs.push_back(std::move(blob));
    }
    return blobs;
}

// Builds a KbdIntChallenge from the libssh2 prompt arrays, delegates to target
// for user responses, then fills in libssh2's response structs. libssh2 owns
// the response buffers and frees them with free(), so strdup() is correct here.
void ApplyKbdIntResponses(
    const char* name,        int name_len,
    const char* instruction, int instruction_len,
    int num_prompts,
    const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts,
    LIBSSH2_USERAUTH_KBDINT_RESPONSE*     responses,
    ITransportTarget&                     target)
{
    KbdIntChallenge challenge;
    challenge.name        = std::string(name,        static_cast<size_t>(name_len));
    challenge.instruction = std::string(instruction, static_cast<size_t>(instruction_len));
    for (int i = 0; i < num_prompts; ++i)
        challenge.prompts.push_back({
            std::string(reinterpret_cast<const char*>(prompts[i].text),
                        prompts[i].length),
            prompts[i].echo != 0
        });

    const auto answers = target.OnKbdIntChallenge(challenge);

    for (int i = 0; i < num_prompts; ++i) {
        const std::string& ans =
            (i < static_cast<int>(answers.size())) ? answers[i] : "";
        responses[i].text   = strdup(ans.c_str());
        responses[i].length = static_cast<unsigned int>(ans.size());
    }
}

// Keyboard-interactive callback. abstract is the session user pointer, set to
// the SshAuthenticator for the duration of AuthViaKbdInteractive.
void KbdIntCallback(
    const char* name,        int name_len,
    const char* instruction, int instruction_len,
    int num_prompts,
    const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts,
    LIBSSH2_USERAUTH_KBDINT_RESPONSE*     responses,
    void**                                abstract)
{
    auto* self = static_cast<SshAuthenticator*>(*abstract);
    ApplyKbdIntResponses(name, name_len, instruction, instruction_len,
                         num_prompts, prompts, responses, self->Target());
}

} // namespace

SshAuthenticator::SshAuthenticator(const SshDesc& desc, _LIBSSH2_SESSION*& session,
                                   int& sockFd, const std::atomic<bool>& running,
                                   _LIBSSH2_AGENT*& agent, ITransportTarget& target)
    : desc_(desc), session_(session), sockFd_(sockFd), running_(running),
      agent_(agent), target_(target)
{}

bool SshAuthenticator::PollUntilReady()
{
    return ssh::PollUntilReady(session_, sockFd_, ssh::kPollTimeoutMs, running_);
}

SshAuthenticator::Result SshAuthenticator::Authenticate()
{
    switch (desc_.authMethod) {
        case SshAuthMethod::Agent:          return AuthViaAgent();
        case SshAuthMethod::Password:       return AuthViaPassword();
        case SshAuthMethod::PrivateKey:     return AuthViaPrivateKey();
        case SshAuthMethod::KbdInteractive: return AuthViaKbdInteractive();
    }
    return {false, Cat::Authentication, "SSH: unknown authentication method"};
}

std::vector<std::vector<uint8_t>> SshAuthenticator::PreferredAgentKeyBlobs() const
{
    return LoadPreferredBlobs(desc_);
}

bool SshAuthenticator::AgentTryPreferred(_LIBSSH2_AGENT* agent,
                                         const std::vector<std::vector<uint8_t>>& preferred,
                                         bool* anyMatched)
{
    *anyMatched = false;
    libssh2_agent_publickey* identity = nullptr;
    libssh2_agent_publickey* prev     = nullptr;

    while (running_) {
        int rc = libssh2_agent_get_identity(agent, &identity, prev);
        if (rc != 0) break;  // rc==1: exhausted; rc<0: error

        // Check if this identity's blob matches any preferred blob.
        const bool matches = std::any_of(
            preferred.begin(), preferred.end(),
            [&](const std::vector<uint8_t>& b) {
                return b.size() == identity->blob_len &&
                       std::memcmp(b.data(), identity->blob, b.size()) == 0;
            });

        if (!matches) { prev = identity; continue; }
        *anyMatched = true;

        int auth;
        while ((auth = libssh2_agent_userauth(agent, desc_.username.c_str(), identity))
               == LIBSSH2_ERROR_EAGAIN) {
            if (!running_) return false;
            PollUntilReady();
        }
        if (auth == 0) return true;

        prev = identity;
    }
    return false;
}

SshAuthenticator::Result SshAuthenticator::AgentTryAll(_LIBSSH2_AGENT* agent)
{
    libssh2_agent_publickey* identity = nullptr;
    libssh2_agent_publickey* prev     = nullptr;

    while (running_) {
        int rc = libssh2_agent_get_identity(agent, &identity, prev);
        if (rc == 1)
            return {false, Cat::Authentication,
                    "SSH: agent has no identity that was accepted by the server"};
        if (rc < 0)
            return {false, Cat::Authentication,
                    "SSH: agent identity enumeration failed"};

        int auth;
        while ((auth = libssh2_agent_userauth(agent, desc_.username.c_str(), identity))
               == LIBSSH2_ERROR_EAGAIN) {
            if (!running_) return {false, Cat::Authentication, ""};
            PollUntilReady();
        }
        if (auth == 0) return {true};

        prev = identity;
    }
    return {false, Cat::Authentication, ""};
}

SshAuthenticator::Result SshAuthenticator::AuthViaAgent()
{
    agent_ = libssh2_agent_init(session_);
    if (!agent_)
        return {false, Cat::Authentication, "SSH: could not initialise SSH agent"};

    if (libssh2_agent_connect(agent_) != 0)
        return {false, Cat::Authentication,
                "SSH: could not connect to SSH agent — is SSH_AUTH_SOCK set?"};

    if (libssh2_agent_list_identities(agent_) != 0)
        return {false, Cat::Authentication, "SSH: could not list SSH agent identities"};

    const auto preferred = PreferredAgentKeyBlobs();
    if (!preferred.empty()) {
        bool anyMatched = false;
        if (AgentTryPreferred(agent_, preferred, &anyMatched)) return {true};
        if (anyMatched) {
            // The SSH config / hint key was in the agent but was rejected — do not
            // spray the remaining keys at a server with strict MaxAuthTries.
            return {false, Cat::Authentication,
                    "SSH: preferred identity (from SSH config or hint) was not accepted by the server"};
        }
        // Preferred keys weren't in the agent at all — fall back to trying all.
    }

    return AgentTryAll(agent_);
}

SshAuthenticator::Result SshAuthenticator::AuthViaPassword()
{
    int rc;
    while ((rc = libssh2_userauth_password(
                session_,
                desc_.username.c_str(),
                desc_.password.c_str())) == LIBSSH2_ERROR_EAGAIN) {
        if (!running_) return {false, Cat::Authentication, ""};
        PollUntilReady();
    }
    if (rc != 0)
        return {false, Cat::Authentication,
                "SSH: password authentication failed — " + ssh::LastSshError(session_)};
    return {true};
}

SshAuthenticator::Result SshAuthenticator::AuthViaPrivateKey()
{
    const char* pubkey = desc_.publicKeyPath.empty()
                         ? nullptr
                         : desc_.publicKeyPath.c_str();
    const char* passphrase = desc_.passphrase.empty()
                             ? nullptr
                             : desc_.passphrase.c_str();

    int rc;
    while ((rc = libssh2_userauth_publickey_fromfile(
                session_,
                desc_.username.c_str(),
                pubkey,
                desc_.privateKeyPath.c_str(),
                passphrase)) == LIBSSH2_ERROR_EAGAIN) {
        if (!running_) return {false, Cat::Authentication, ""};
        PollUntilReady();
    }
    if (rc != 0)
        return {false, Cat::Authentication,
                "SSH: private key authentication failed — " + ssh::LastSshError(session_)};
    return {true};
}

SshAuthenticator::Result SshAuthenticator::AuthViaKbdInteractive()
{
    // Point the session abstract at us so KbdIntCallback reaches target_; the
    // transport resets it to itself once authentication returns, before it
    // registers the X11/agent channel-open callbacks.
    *libssh2_session_abstract(session_) = this;

    int rc;
    while ((rc = libssh2_userauth_keyboard_interactive(
                session_,
                desc_.username.c_str(),
                &KbdIntCallback)) == LIBSSH2_ERROR_EAGAIN) {
        if (!running_) return {false, Cat::Authentication, ""};
        PollUntilReady();
    }
    if (rc != 0)
        return {false, Cat::Authentication,
                "SSH: keyboard-interactive authentication failed — " + ssh::LastSshError(session_)};
    return {true};
}

} // namespace term::transport
