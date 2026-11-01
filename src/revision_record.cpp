#include "aowis/dvmdb/revision_record.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <utility>

namespace aowis::dvmdb {
namespace {

void check(bool valid, StorageErrorKind kind, const char* message) {
    if (!valid) throw StorageError(kind, message);
}

UuidV7 readUuid(const Bytes& bytes, std::size_t offset) {
    std::array<std::uint8_t, 16> value{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), value.size(), value.begin());
    check((value[6] & 0xf0U) == 0x70U && (value[8] & 0xc0U) == 0x80U,
          StorageErrorKind::Corruption, "non-UUIDv7 revision identifier");
    return UuidV7{value};
}

void writeUuid(Bytes& out, std::size_t offset, const UuidV7& value) {
    std::copy(value.bytes.begin(), value.bytes.end(),
              out.begin() + static_cast<std::ptrdiff_t>(offset));
}

void writeHash(Bytes& out, std::size_t offset, const Hash& value) {
    std::copy(value.begin(), value.end(),
              out.begin() + static_cast<std::ptrdiff_t>(offset));
}

Hash readHash(const Bytes& bytes, std::size_t offset) {
    Hash value{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), value.size(), value.begin());
    return value;
}

bool isUuidV7(const UuidV7& value) {
    return (value.bytes[6] & 0xf0U) == 0x70U &&
           (value.bytes[8] & 0xc0U) == 0x80U;
}

void checkMetadata(const CommitMetadata& metadata, StorageErrorKind kind);

void validateRecord(const RevisionRecord& record, StorageErrorKind kind) {
    check(isUuidV7(record.id.uuid()) && isUuidV7(record.project.uuid()), kind,
          "invalid revision or project UUIDv7");
    check(record.parents.size() <= 2, kind, "revision has too many parents");
    check(record.modelIndexed || record.referencesRoot == Hash{}, kind,
          "raw revision cannot contain model references");
    std::set<UuidV7> seen;
    for (const RevisionParent& parent : record.parents) {
        check(isUuidV7(parent.id.uuid()), kind, "invalid parent UUIDv7");
        check(parent.id != record.id, kind, "revision is its own parent");
        check(seen.insert(parent.id.uuid()).second, kind, "duplicate revision parent");
        check(parent.expectedRecordHash != Hash{}, kind, "zero parent revision digest");
    }
    checkMetadata(record.metadata, kind);
    check(record.committedAtMs != 0, kind, "revision has no commit time");
}

bool validUtf8(std::string_view text, bool allowLineControls) {
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[i]);
        if (first < 0x80U) {
            const bool control = first < 0x20U || first == 0x7fU;
            if (control && !(allowLineControls && (first == '\n' || first == '\t'))) return false;
            ++i;
            continue;
        }
        unsigned count = 0;
        if (first >= 0xc2U && first <= 0xdfU) count = 2;
        else if (first >= 0xe0U && first <= 0xefU) count = 3;
        else if (first >= 0xf0U && first <= 0xf4U) count = 4;
        else return false;
        if (i + count > text.size()) return false;
        for (unsigned step = 1; step < count; ++step)
            if ((static_cast<unsigned char>(text[i + step]) & 0xc0U) != 0x80U) return false;
        const unsigned char second = static_cast<unsigned char>(text[i + 1]);
        if ((first == 0xe0U && second < 0xa0U) || (first == 0xedU && second > 0x9fU) ||
            (first == 0xf0U && second < 0x90U) || (first == 0xf4U && second > 0x8fU))
            return false;
        // C1 controls (U+0080..U+009F) are controls too.
        if (first == 0xc2U && second < 0xa0U) return false;
        i += count;
    }
    return true;
}

void checkMetadata(const CommitMetadata& metadata, StorageErrorKind kind) {
    check(isUuidV7(metadata.authorId), kind, "commit author ID must be a UUIDv7");
    check(!metadata.authorName.empty() && metadata.authorName.size() <= maxAuthorNameBytes &&
          validUtf8(metadata.authorName, false), kind,
          "commit author name must be 1..200 bytes of UTF-8 without control characters");
    check(!metadata.message.empty() && metadata.message.size() <= maxCommitMessageBytes &&
          validUtf8(metadata.message, true), kind,
          "commit message must be 1..65536 bytes of UTF-8; tab and newline are the only "
          "allowed control characters");
}

void writeBig(Bytes& out, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (8 * (width - 1 - i))));
}

std::uint64_t readBig(const Bytes& bytes, std::size_t offset, unsigned width) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value = (value << 8U) | bytes[offset + i];
    return value;
}

struct Frame {
    RevisionRecord record;
    Hash digest;
    std::size_t nextParent = 0;
};

} // namespace

void validateCommitMetadata(const CommitMetadata& metadata) {
    checkMetadata(metadata, StorageErrorKind::InvalidInput);
}

Bytes encodeRevisionRecord(const RevisionRecord& record) {
    validateRecord(record, StorageErrorKind::InvalidInput);
    Bytes bytes(100 + 48 * record.parents.size(), 0);
    bytes[0] = static_cast<std::uint8_t>('T');
    bytes[1] = 3;
    writeUuid(bytes, 2, record.id.uuid());
    writeUuid(bytes, 18, record.project.uuid());
    writeHash(bytes, 34, record.root);
    writeHash(bytes, 66, record.referencesRoot);
    bytes[98] = record.modelIndexed ? 1 : 0;
    bytes[99] = static_cast<std::uint8_t>(record.parents.size());
    for (std::size_t i = 0; i < record.parents.size(); ++i) {
        const std::size_t offset = 100 + 48 * i;
        writeUuid(bytes, offset, record.parents[i].id.uuid());
        writeHash(bytes, offset + 16, record.parents[i].expectedRecordHash);
    }
    bytes.reserve(bytes.size() + 30 + record.metadata.authorName.size() +
                  record.metadata.message.size());
    writeBig(bytes, record.committedAtMs, 8);
    bytes.insert(bytes.end(), record.metadata.authorId.bytes.begin(),
                 record.metadata.authorId.bytes.end());
    writeBig(bytes, record.metadata.authorName.size(), 2);
    bytes.insert(bytes.end(), record.metadata.authorName.begin(), record.metadata.authorName.end());
    writeBig(bytes, record.metadata.message.size(), 4);
    bytes.insert(bytes.end(), record.metadata.message.begin(), record.metadata.message.end());
    return bytes;
}

RevisionRecord decodeRevisionRecord(const Bytes& bytes) {
    check(bytes.size() >= 2 && bytes[0] == static_cast<std::uint8_t>('T') && bytes[1] == 3,
          StorageErrorKind::Corruption, "unsupported revision record header");
    check(bytes.size() >= 100, StorageErrorKind::Corruption, "truncated revision record");
    const std::size_t parentCount = bytes[99];
    check(parentCount <= 2, StorageErrorKind::Corruption, "invalid revision parent count");
    check(bytes[98] <= 1, StorageErrorKind::Corruption, "invalid indexing flag");
    std::size_t offset = 100 + 48 * parentCount;
    check(bytes.size() >= offset + 26, StorageErrorKind::Corruption, "truncated revision record");

    RevisionRecord record{
        RevisionId(readUuid(bytes, 2)),
        ProjectId(readUuid(bytes, 18)),
        readHash(bytes, 34),
        readHash(bytes, 66),
        bytes[98] == 1,
        {},
        {},
        0
    };
    record.parents.reserve(parentCount);
    for (std::size_t i = 0; i < parentCount; ++i) {
        const std::size_t at = 100 + 48 * i;
        record.parents.push_back({RevisionId(readUuid(bytes, at)), readHash(bytes, at + 16)});
    }
    record.committedAtMs = readBig(bytes, offset, 8);
    offset += 8;
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), 16,
                record.metadata.authorId.bytes.begin());
    offset += 16;
    const std::size_t nameBytes = static_cast<std::size_t>(readBig(bytes, offset, 2));
    offset += 2;
    check(bytes.size() - offset >= nameBytes + 4, StorageErrorKind::Corruption,
          "truncated revision author");
    record.metadata.authorName.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(offset + nameBytes));
    offset += nameBytes;
    const std::size_t messageBytes = static_cast<std::size_t>(readBig(bytes, offset, 4));
    offset += 4;
    check(bytes.size() - offset == messageBytes, StorageErrorKind::Corruption,
          "invalid revision record length");
    record.metadata.message.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                   bytes.end());
    validateRecord(record, StorageErrorKind::Corruption);
    return record;
}

Hash hashRevisionRecord(const RevisionRecord& record) {
    return sha256(encodeRevisionRecord(record));
}

Hash verifyRevisionAncestry(const Bytes& tipRecord,
                              const RevisionRecordLookup& getRecord,
                              std::size_t maxRevisions,
                              const TrustedRevision& trusted) {
    check(static_cast<bool>(getRecord), StorageErrorKind::InvalidInput,
          "missing revision resolver");
    check(maxRevisions > 0, StorageErrorKind::InvalidInput,
          "zero revision traversal budget");
    const RevisionRecord tip = decodeRevisionRecord(tipRecord);
    const Hash tipHash = sha256(tipRecord);
    std::vector<Frame> stack;
    stack.push_back({tip, tipHash, 0});
    std::set<UuidV7> active{tip.id.uuid()};
    std::map<UuidV7, Hash> verified;
    std::size_t traversed = 1;

    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (frame.nextParent == frame.record.parents.size()) {
            verified.emplace(frame.record.id.uuid(), frame.digest);
            active.erase(frame.record.id.uuid());
            stack.pop_back();
            continue;
        }
        const RevisionParent parent = frame.record.parents[frame.nextParent++];
        check(!active.contains(parent.id.uuid()), StorageErrorKind::Corruption,
              "cycle in revision ancestry");
        const std::map<UuidV7, Hash>::const_iterator known = verified.find(parent.id.uuid());
        if (known != verified.end()) {
            check(known->second == parent.expectedRecordHash,
                  StorageErrorKind::Corruption, "conflicting revision identity binding");
            continue;
        }
        if (trusted && trusted(parent)) {
            verified.emplace(parent.id.uuid(), parent.expectedRecordHash);
            continue;
        }
        check(traversed < maxRevisions, StorageErrorKind::LimitExceeded,
              "revision ancestry budget exceeded");
        const std::optional<Bytes> encoded = getRecord(parent.id);
        check(encoded.has_value(), StorageErrorKind::Corruption, "missing parent revision");
        const Hash parentHash = sha256(*encoded);
        check(parentHash == parent.expectedRecordHash, StorageErrorKind::Corruption,
              "parent revision SHA-256 mismatch");
        const RevisionRecord parentRecord = decodeRevisionRecord(*encoded);
        check(parentRecord.id == parent.id, StorageErrorKind::Corruption,
              "resolver returned wrong revision UUID");
        check(parentRecord.project == tip.project, StorageErrorKind::Corruption,
              "cross-project revision parent");
        active.insert(parent.id.uuid());
        stack.push_back({parentRecord, parentHash, 0});
        ++traversed;
    }
    return tipHash;
}

} // namespace aowis::dvmdb
