#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace term::transport {

// Read an OpenSSH public key file ("keytype BASE64BLOB [comment]") and return
// the raw decoded key blob. Returns an empty vector on any parse or I/O failure.
std::vector<uint8_t> LoadPublicKeyBlob(const std::filesystem::path& pubKeyPath);

} // namespace term::transport
