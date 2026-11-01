#pragma once

// Test support: the author and message every test revision records. A fixed
// author UUIDv7 keeps test records deterministic apart from the commit time.

#include "aowis/dvmdb/native_store.h"

#include <cstdint>
#include <string>
#include <utility>

namespace aowis::dvmdb::test {

inline UuidV7 testAuthorId() {
    return *UuidV7::parse("018f84c0-0000-7000-8000-00000000a001");
}

inline CommitMetadata commitMetadata(std::string message = "test revision") {
    return CommitMetadata{testAuthorId(), "DVMDB test", std::move(message)};
}

// The metadata tail of a hand-forged `T` 0x03 revision record: commit time
// (u64), author UUID, u16-length author name, u32-length message.
inline void appendRecordMetadata(Bytes& record, std::uint64_t committedAtMs = 1793491823009ULL,
                                 const CommitMetadata& metadata = commitMetadata()) {
    for (unsigned i = 0; i < 8; ++i)
        record.push_back(static_cast<std::uint8_t>(committedAtMs >> (56 - 8 * i)));
    record.insert(record.end(), metadata.authorId.bytes.begin(), metadata.authorId.bytes.end());
    record.push_back(static_cast<std::uint8_t>(metadata.authorName.size() >> 8));
    record.push_back(static_cast<std::uint8_t>(metadata.authorName.size()));
    record.insert(record.end(), metadata.authorName.begin(), metadata.authorName.end());
    for (unsigned i = 0; i < 4; ++i)
        record.push_back(static_cast<std::uint8_t>(metadata.message.size() >> (24 - 8 * i)));
    record.insert(record.end(), metadata.message.begin(), metadata.message.end());
}

} // namespace aowis::dvmdb::test
