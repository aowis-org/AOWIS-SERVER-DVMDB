#pragma once

// Qt/MODEL-facing conversions shared by independent DVMDB persistence adapters.
// The core native store remains Qt-free.
#include "aowis/dvmdb/native_store.h"
#include "aowis/dvmdb/storage_error.h"

#include <QByteArray>
#include <QUuid>

#include <cstddef>
#include <limits>

namespace aowis::dvmdb::record {

inline Key keyFromUuid(const QUuid& uuid) {
    const QByteArray raw = uuid.toRfc4122();
    if (uuid.isNull() || raw.size() != 16)
        throw StorageError(StorageErrorKind::InvalidInput, "AOWIS model record requires a non-null UUID");
    Key key{};
    for (std::size_t index = 0; index < key.size(); ++index)
        key[index] = static_cast<std::uint8_t>(static_cast<unsigned char>(raw.at(static_cast<qsizetype>(index))));
    return key;
}

inline QUuid uuidFromKey(const Key& key) {
    return QUuid::fromRfc4122(QByteArray(reinterpret_cast<const char*>(key.data()),
                                        static_cast<qsizetype>(key.size())));
}

inline Bytes bytesFromQByteArray(const QByteArray& source) {
    Bytes result;
    result.reserve(static_cast<std::size_t>(source.size()));
    for (const char value : source)
        result.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(value)));
    return result;
}

inline QByteArray qByteArrayFromBytes(const Bytes& source) {
    if (source.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
        throw StorageError(StorageErrorKind::LimitExceeded, "canonical model payload too large");
    return QByteArray(reinterpret_cast<const char*>(source.data()),
                      static_cast<qsizetype>(source.size()));
}

} // namespace aowis::dvmdb::record
