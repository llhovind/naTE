#include "transport/SshPublicKey.h"
#include <array>
#include <fstream>
#include <string>

namespace term::transport {

namespace {

// Standard base64 alphabet.
constexpr std::array<int8_t, 256> BuildDecodeTable()
{
    std::array<int8_t, 256> t{};
    t.fill(-1);
    for (int i = 0; i < 26; ++i) { t['A' + i] = static_cast<int8_t>(i);      }
    for (int i = 0; i < 26; ++i) { t['a' + i] = static_cast<int8_t>(26 + i); }
    for (int i = 0; i < 10; ++i) { t['0' + i] = static_cast<int8_t>(52 + i); }
    t['+'] = 62; t['/'] = 63; t['='] = 0;
    return t;
}

constexpr auto kDecodeTable = BuildDecodeTable();

std::vector<uint8_t> Base64Decode(const std::string& b64)
{
    std::vector<uint8_t> out;
    out.reserve(b64.size() * 3 / 4);
    uint32_t accum = 0;
    int      bits  = 0;
    for (unsigned char c : b64) {
        int8_t v = kDecodeTable[c];
        if (v < 0) continue;  // skip whitespace / non-alphabet
        accum = (accum << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(accum >> bits));
            accum &= (1u << bits) - 1u;
        }
    }
    return out;
}

} // namespace

std::vector<uint8_t> LoadPublicKeyBlob(const std::filesystem::path& pubKeyPath)
{
    std::ifstream f(pubKeyPath);
    if (!f) return {};

    std::string line;
    if (!std::getline(f, line)) return {};

    // Format: "keytype BASE64BLOB [optional comment]"
    const auto first = line.find(' ');
    if (first == std::string::npos) return {};
    const auto second = line.find(' ', first + 1);
    const auto blobEnd = (second == std::string::npos) ? line.size() : second;

    const std::string b64 = line.substr(first + 1, blobEnd - (first + 1));
    if (b64.empty()) return {};

    return Base64Decode(b64);
}

} // namespace term::transport
