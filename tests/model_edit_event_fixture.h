#pragma once
#include "aowis/dvmdb/native_store.h"
#include "aowis/dvmdb/object_id.h"
#include <algorithm>
#include <functional>
#include <string_view>

// A MODEL format-2 compatible hydraulic edit frame used by native-only storage tests;
// production encoding/decoding remains exclusively in AOWIS-SERVER-MODEL.
namespace aowis::dvmdb::test {
// One entity edit with explicit before/after images (both present: an
// in-place modification), keyed by the entity's 16 UUID bytes.
inline Bytes editEvent(const UuidV7& id, const Key& entity,
                       const Bytes& before, const Bytes& after) {
    Bytes payload;
    const std::function<void(std::uint16_t)> put16 = [&](std::uint16_t value) {
        payload.push_back(static_cast<std::uint8_t>(value >> 8));
        payload.push_back(static_cast<std::uint8_t>(value));
    };
    const std::function<void(std::uint32_t)> put32 = [&](std::uint32_t value) {
        for (unsigned step = 0; step < 4; ++step)
            payload.push_back(static_cast<std::uint8_t>(value >> (24 - step * 8)));
    };
    const std::function<void(std::string_view)> putString = [&](std::string_view string) {
        put16(static_cast<std::uint16_t>(string.size()));
        payload.insert(payload.end(), string.begin(), string.end());
    };
    payload.insert(payload.end(), id.bytes.begin(), id.bytes.end());
    putString("hydraulic.junction.move");
    putString("");
    put16(1);
    payload.insert(payload.end(), entity.begin(), entity.end());
    putString("HydraulicNodeJunction");
    put32(static_cast<std::uint32_t>(before.size()));
    payload.insert(payload.end(), before.begin(), before.end());
    put32(static_cast<std::uint32_t>(after.size()));
    payload.insert(payload.end(), after.begin(), after.end());
    Bytes result{'A', 'O', 'W', 'E', 2};
    const std::uint32_t size = static_cast<std::uint32_t>(payload.size());
    for (unsigned step = 0; step < 4; ++step)
        result.push_back(static_cast<std::uint8_t>(size >> (24 - step * 8)));
    result.insert(result.end(), payload.begin(), payload.end());
    const Hash digest = sha256(payload);
    result.insert(result.end(), digest.begin(), digest.end());
    return result;
}

inline Bytes editEvent(const UuidV7& id, const UuidV7& entity,
                       std::uint8_t before = 1, std::uint8_t after = 2) {
    Key key{};
    std::copy(entity.bytes.begin(), entity.bytes.end(), key.begin());
    return editEvent(id, key, Bytes{before}, Bytes{after});
}
}
