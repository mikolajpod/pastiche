#pragma once

// SHA-256 (FIPS 180-4), used to verify downloaded model files against the
// checksums in models.json (D8). Written out rather than pulled from a library
// because it is 150 lines and the alternatives are platform-specific (CNG on
// Windows, OpenSSL elsewhere), which would cost more than it saves.
//
// Streaming, so a download can be hashed as it arrives instead of being read
// back from disk afterwards.
#include <cstddef>
#include <cstdint>
#include <string>

namespace pastiche {

class Sha256 {
public:
    Sha256() { reset(); }

    void reset();
    void update(const void* data, std::size_t len);

    // Finishes the digest and returns it as 64 lowercase hex characters.
    // The object must be reset() before it is used again.
    std::string hex_digest();

private:
    void transform(const uint8_t block[64]);

    uint32_t state_[8];
    uint64_t bit_count_;
    uint8_t buffer_[64];
    std::size_t buffer_len_;
};

// Convenience wrapper for data already in memory.
std::string sha256_hex(const void* data, std::size_t len);

// Hashes a file in chunks. Returns the digest, or "" with `error` set.
std::string sha256_file(const std::string& path, std::string& error);

} // namespace pastiche
