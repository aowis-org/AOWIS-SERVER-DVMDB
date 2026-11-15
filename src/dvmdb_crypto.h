#pragma once

// Internal cryptographic primitives of the native store.
//
// DVMDB needs exactly two primitives: SHA-256 (content addressing) and a
// cryptographically secure random source (pack generation tokens, access
// UUIDs, UUIDv7 random bits). Linux builds keep OpenSSL's hardware-accelerated
// SHA-256 by default (AOWIS_DVMDB_WITH_OPENSSL). Windows and macOS builds use
// the portable implementation in dvmdb_crypto.cpp so Standalone does not need
// a target-side OpenSSL for those cross toolchains. Both implementations
// produce identical digests; the test suite runs against either backend.

#include <array>
#include <cstddef>
#include <cstdint>

#ifdef AOWIS_DVMDB_WITH_OPENSSL
#include <openssl/evp.h>
#endif

namespace aowis::dvmdb::crypto {

using Sha256Digest = std::array<std::uint8_t, 32>;

class Sha256 final {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(const std::uint8_t* data, std::size_t size);
    // May be called once; the object must not be updated afterwards.
    Sha256Digest finish();

private:
#ifdef AOWIS_DVMDB_WITH_OPENSSL
    EVP_MD_CTX* context_ = nullptr;
#else
    void compress(const std::uint8_t* block);

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> block_{};
    std::size_t blockUsed_ = 0;
    std::uint64_t totalBytes_ = 0;
#endif
    bool finished_ = false;
};

Sha256Digest sha256(const std::uint8_t* data, std::size_t size);

// Fills the buffer from the operating system CSPRNG (getrandom, arc4random_buf
// or BCryptGenRandom). Returns false only if the OS refuses to provide entropy.
bool secureRandomBytes(std::uint8_t* data, std::size_t size) noexcept;

} // namespace aowis::dvmdb::crypto
