#include "aowis/dvmdb/object_id.h"

#include "dvmdb_crypto.h"

#include <chrono>
#include <stdexcept>
#include <utility>
#include "dvmdb_sys.h"

namespace aowis::dvmdb {
namespace {

constexpr std::uint64_t MaxTimestamp = (std::uint64_t{1} << 48) - 1;
constexpr std::uint64_t MaxRandomB = (std::uint64_t{1} << 62) - 1;
constexpr char HexDigits[] = "0123456789abcdef";

std::uint64_t systemMilliseconds() {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (value < 0) throw std::runtime_error("UUIDv7 requires time since Unix epoch");
    return static_cast<std::uint64_t>(value);
}

int hexDigit(char ch) noexcept {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

} // namespace

std::optional<UuidV7> UuidV7::parse(std::string_view value) {
    if (value.size() != 36) return std::nullopt;
    UuidV7 result;
    std::size_t byteIndex = 0;
    for (std::size_t index = 0; index < value.size();) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return std::nullopt;
            ++index;
            continue;
        }
        if (index + 1 >= value.size() || byteIndex >= result.bytes.size()) return std::nullopt;
        const int upper = hexDigit(value[index]);
        const int lower = hexDigit(value[index + 1]);
        if (upper < 0 || lower < 0) return std::nullopt;
        result.bytes[byteIndex++] = static_cast<std::uint8_t>((upper << 4) | lower);
        index += 2;
    }
    if (byteIndex != result.bytes.size() || (result.bytes[6] >> 4) != 7 ||
        (result.bytes[8] & 0xc0U) != 0x80U) return std::nullopt;
    return result;
}

std::string UuidV7::toString() const {
    std::string result;
    result.reserve(36);
    for (std::size_t i = 0; i < this->bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result.push_back('-');
        result.push_back(HexDigits[this->bytes[i] >> 4]);
        result.push_back(HexDigits[this->bytes[i] & 0x0fU]);
    }
    return result;
}

std::uint64_t UuidV7::unixMilliseconds() const noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 6; ++i) value = (value << 8) | this->bytes[i];
    return value;
}

UuidV7Generator::UuidV7Generator(Clock clock) : clock_(std::move(clock)) {
    if (!this->clock_) this->clock_ = systemMilliseconds;
}

UuidV7 UuidV7Generator::generate() {
    std::lock_guard<std::mutex> lock(this->mutex_);
    std::uint64_t milliseconds = this->clock_();
    if (milliseconds > MaxTimestamp) throw std::runtime_error("UUIDv7 timestamp overflow");
    // A fork duplicates the in-process counter and random state. Reseed the
    // child rather than returning the same next UUID in both processes.
    const std::uint64_t processId = static_cast<std::uint64_t>(sys::getpid());
    if (!this->started_ || processId != this->lastProcessId_ || milliseconds > this->lastMs_) {
        unsigned char random[10]{};
        if (!crypto::secureRandomBytes(random, sizeof(random)))
            throw std::runtime_error("cryptographic UUIDv7 randomness unavailable");
        this->randomA_ = static_cast<std::uint16_t>(
            ((static_cast<std::uint16_t>(random[0]) << 8) | random[1]) & 0x0fffU);
        this->randomB_ = 0;
        for (std::size_t i = 2; i < sizeof(random); ++i)
            this->randomB_ = (this->randomB_ << 8) | random[i];
        this->randomB_ &= MaxRandomB;
        this->lastMs_ = milliseconds;
        this->lastProcessId_ = processId;
        this->started_ = true;
    } else {
        // Logical monotonic time when NTP or the system clock moves backwards.
        milliseconds = this->lastMs_;
        if (this->randomB_ != MaxRandomB) ++this->randomB_;
        else if (this->randomA_ != 0x0fffU) {
            ++this->randomA_;
            this->randomB_ = 0;
        } else {
            if (milliseconds == MaxTimestamp) throw std::runtime_error("UUIDv7 space exhausted");
            ++milliseconds;
            this->lastMs_ = milliseconds;
            this->randomA_ = 0;
            this->randomB_ = 0;
        }
    }
    UuidV7 result;
    for (int i = 5; i >= 0; --i) {
        result.bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(milliseconds & 0xffU);
        milliseconds >>= 8;
    }
    result.bytes[6] = static_cast<std::uint8_t>(0x70U | (this->randomA_ >> 8));
    result.bytes[7] = static_cast<std::uint8_t>(this->randomA_ & 0xffU);
    result.bytes[8] = static_cast<std::uint8_t>(0x80U | (this->randomB_ >> 56));
    std::uint64_t randomB = this->randomB_;
    for (int i = 15; i >= 9; --i) {
        result.bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(randomB & 0xffU);
        randomB >>= 8;
    }
    return result;
}

UuidV7 UuidV7::generate() {
    static UuidV7Generator generator;
    return generator.generate();
}

} // namespace aowis::dvmdb
