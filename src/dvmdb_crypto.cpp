#include "dvmdb_crypto.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <sys/random.h>
#endif

namespace aowis::dvmdb::crypto {

#ifdef AOWIS_DVMDB_WITH_OPENSSL

Sha256::Sha256() : context_(EVP_MD_CTX_new()) {
    if (this->context_ == nullptr || EVP_DigestInit_ex(this->context_, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(this->context_);
        this->context_ = nullptr;
        throw std::runtime_error("initialize SHA-256 failed");
    }
}

Sha256::~Sha256() { EVP_MD_CTX_free(this->context_); }

void Sha256::update(const std::uint8_t* data, std::size_t size) {
    if (this->finished_) throw std::logic_error("SHA-256 updated after finish");
    if (size != 0 && EVP_DigestUpdate(this->context_, data, size) != 1)
        throw std::runtime_error("update SHA-256 failed");
}

Sha256Digest Sha256::finish() {
    if (this->finished_) throw std::logic_error("SHA-256 finished twice");
    this->finished_ = true;
    Sha256Digest result{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(this->context_, result.data(), &length) != 1 || length != result.size())
        throw std::runtime_error("finalize SHA-256 failed");
    return result;
}

#else

namespace {

// FIPS 180-4, section 4.2.2.
constexpr std::array<std::uint32_t, 64> roundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

constexpr std::uint32_t rotateRight(std::uint32_t value, unsigned count) noexcept {
    return (value >> count) | (value << (32U - count));
}

} // namespace

Sha256::Sha256()
    : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
             0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

Sha256::~Sha256() = default;

void Sha256::compress(const std::uint8_t* block) {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t i = 0; i < 16; ++i) {
        schedule[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24U) |
                      (static_cast<std::uint32_t>(block[4 * i + 1]) << 16U) |
                      (static_cast<std::uint32_t>(block[4 * i + 2]) << 8U) |
                      static_cast<std::uint32_t>(block[4 * i + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotateRight(schedule[i - 15], 7) ^
                                 rotateRight(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3U);
        const std::uint32_t s1 = rotateRight(schedule[i - 2], 17) ^
                                 rotateRight(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10U);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }
    std::uint32_t a = this->state_[0];
    std::uint32_t b = this->state_[1];
    std::uint32_t c = this->state_[2];
    std::uint32_t d = this->state_[3];
    std::uint32_t e = this->state_[4];
    std::uint32_t f = this->state_[5];
    std::uint32_t g = this->state_[6];
    std::uint32_t h = this->state_[7];
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t first = h + sum1 + choose + roundConstants[i] + schedule[i];
        const std::uint32_t sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t second = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    this->state_[0] += a;
    this->state_[1] += b;
    this->state_[2] += c;
    this->state_[3] += d;
    this->state_[4] += e;
    this->state_[5] += f;
    this->state_[6] += g;
    this->state_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t size) {
    if (this->finished_) throw std::logic_error("SHA-256 updated after finish");
    this->totalBytes_ += static_cast<std::uint64_t>(size);
    if (this->blockUsed_ != 0) {
        const std::size_t take = std::min(size, this->block_.size() - this->blockUsed_);
        std::memcpy(this->block_.data() + this->blockUsed_, data, take);
        this->blockUsed_ += take;
        data += take;
        size -= take;
        if (this->blockUsed_ < this->block_.size()) return;
        this->compress(this->block_.data());
        this->blockUsed_ = 0;
    }
    // Whole blocks straight from the caller's buffer: no intermediate copy.
    while (size >= this->block_.size()) {
        this->compress(data);
        data += this->block_.size();
        size -= this->block_.size();
    }
    if (size != 0) {
        std::memcpy(this->block_.data(), data, size);
        this->blockUsed_ = size;
    }
}

Sha256Digest Sha256::finish() {
    if (this->finished_) throw std::logic_error("SHA-256 finished twice");
    const std::uint64_t bitLength = this->totalBytes_ * 8U;
    // Padding: 0x80, zeros up to 56 mod 64, then the 64-bit big-endian length.
    this->block_[this->blockUsed_++] = 0x80U;
    if (this->blockUsed_ > 56) {
        std::memset(this->block_.data() + this->blockUsed_, 0, this->block_.size() - this->blockUsed_);
        this->compress(this->block_.data());
        this->blockUsed_ = 0;
    }
    std::memset(this->block_.data() + this->blockUsed_, 0, 56 - this->blockUsed_);
    for (std::size_t i = 0; i < 8; ++i)
        this->block_[56 + i] = static_cast<std::uint8_t>(bitLength >> (56U - 8U * i));
    this->compress(this->block_.data());
    this->finished_ = true;
    Sha256Digest result{};
    for (std::size_t i = 0; i < 8; ++i) {
        result[4 * i] = static_cast<std::uint8_t>(this->state_[i] >> 24U);
        result[4 * i + 1] = static_cast<std::uint8_t>(this->state_[i] >> 16U);
        result[4 * i + 2] = static_cast<std::uint8_t>(this->state_[i] >> 8U);
        result[4 * i + 3] = static_cast<std::uint8_t>(this->state_[i]);
    }
    return result;
}

#endif

Sha256Digest sha256(const std::uint8_t* data, std::size_t size) {
    Sha256 digest;
    digest.update(data, size);
    return digest.finish();
}

bool secureRandomBytes(std::uint8_t* data, std::size_t size) noexcept {
#if defined(_WIN32)
    while (size != 0) {
        const ULONG chunk = size > 0x10000000U ? 0x10000000U : static_cast<ULONG>(size);
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, data, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            return false;
        data += chunk;
        size -= chunk;
    }
    return true;
#elif defined(__APPLE__)
    // Kernel-seeded ChaCha20 generator; cannot fail and needs no descriptor.
    ::arc4random_buf(data, size);
    return true;
#else
    while (size != 0) {
        const ssize_t count = ::getrandom(data, size, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        data += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
#endif
}

} // namespace aowis::dvmdb::crypto
