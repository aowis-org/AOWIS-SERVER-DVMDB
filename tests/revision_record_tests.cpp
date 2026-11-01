#include "aowis/dvmdb/revision_record.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using aowis::dvmdb::Bytes;
using aowis::dvmdb::CommitMetadata;
using aowis::dvmdb::UuidV7;
using aowis::dvmdb::Hash;
using aowis::dvmdb::ProjectId;
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::RevisionParent;
using aowis::dvmdb::RevisionRecord;
using aowis::dvmdb::StorageError;
using aowis::dvmdb::StorageErrorKind;
using aowis::dvmdb::decodeRevisionRecord;
using aowis::dvmdb::encodeRevisionRecord;
using aowis::dvmdb::hashRevisionRecord;
using aowis::dvmdb::sha256;
using aowis::dvmdb::toHex;
using aowis::dvmdb::verifyRevisionAncestry;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

RevisionId revisionId(std::string_view text) {
    const std::optional<RevisionId> parsed = RevisionId::parse(text);
    check(parsed.has_value(), "invalid fixture revision UUID");
    return *parsed;
}
ProjectId projectId(std::string_view text) {
    const std::optional<ProjectId> parsed = ProjectId::parse(text);
    check(parsed.has_value(), "invalid fixture project UUID");
    return *parsed;
}

constexpr std::uint64_t fixtureTime = 1793491823009ULL;  // 2026-11-01, Unix ms

CommitMetadata fixtureMetadata() {
    return CommitMetadata{*UuidV7::parse("018f84c0-0000-7000-8000-00000000a001"),
                          "Jürgen Ö", "Fix pump curve\n\nTab:\there"};
}

RevisionRecord makeRecord(std::string_view id, std::string_view project,
                            bool indexed = false) {
    RevisionRecord record{revisionId(id), projectId(project), Hash{}, Hash{}, indexed, {},
                          fixtureMetadata(), fixtureTime};
    record.root.fill(0x22);
    if (indexed) record.referencesRoot.fill(0x33);
    return record;
}

std::uint64_t readBig(const Bytes& bytes, std::size_t offset, unsigned width) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value = (value << 8U) | bytes[offset + i];
    return value;
}

void expectFailure(const std::function<void()>& call, StorageErrorKind kind, const char* caseName) {
    try { call(); }
    catch (const StorageError& error) {
        check(error.kind() == kind, caseName);
        return;
    }
    throw std::runtime_error(caseName);
}

void testGoldenVectors() {
    RevisionRecord root = makeRecord("018f84c0-0000-7000-8000-000000000001",
                                       "018f84c0-0000-7000-8000-000000000002", true);
    const Bytes rootBytes = encodeRevisionRecord(root);
    // Layout, checked byte by byte: identity and roots, then the commit time,
    // the author UUID, the u16-length author name and the u32-length message.
    const CommitMetadata metadata = fixtureMetadata();
    const std::size_t tail = 100;
    check(rootBytes[0] == 'T' && rootBytes[1] == 3, "golden root record marker incorrect");
    check(rootBytes.size() == tail + 8 + 16 + 2 + metadata.authorName.size() + 4 +
              metadata.message.size(), "golden root record length incorrect");
    check(readBig(rootBytes, tail, 8) == fixtureTime, "commit time not at offset 100");
    check(std::equal(metadata.authorId.bytes.begin(), metadata.authorId.bytes.end(),
                     rootBytes.begin() + tail + 8), "author UUID not after the commit time");
    check(readBig(rootBytes, tail + 24, 2) == metadata.authorName.size() &&
          std::string(rootBytes.begin() + tail + 26,
                      rootBytes.begin() + tail + 26 +
                          static_cast<std::ptrdiff_t>(metadata.authorName.size())) ==
              metadata.authorName, "author name not length-prefixed UTF-8");
    const std::size_t messageAt = tail + 26 + metadata.authorName.size();
    check(readBig(rootBytes, messageAt, 4) == metadata.message.size() &&
          std::string(rootBytes.begin() + static_cast<std::ptrdiff_t>(messageAt) + 4,
                      rootBytes.end()) == metadata.message,
          "message not length-prefixed UTF-8 at the end");
    check(toHex(sha256(rootBytes)) ==
          "c9507fd8d8e88db0a5d6a472c0a23c10c59b76633e395ef0f584d40206dbc3ad",
          "root does not match frozen SHA-256 vector");
    const RevisionRecord decoded = decodeRevisionRecord(rootBytes);
    check(encodeRevisionRecord(decoded) == rootBytes && decoded.project == root.project &&
          decoded.metadata.authorId == metadata.authorId &&
          decoded.metadata.authorName == metadata.authorName &&
          decoded.metadata.message == metadata.message && decoded.committedAtMs == fixtureTime,
          "root decode did not preserve canonical bytes and metadata");

    RevisionRecord child = makeRecord("018f84c0-0000-7000-8000-000000000003",
                                        "018f84c0-0000-7000-8000-000000000002", true);
    child.parents.push_back({root.id, sha256(rootBytes)});
    check(toHex(hashRevisionRecord(child)) ==
          "ad226427dc3f3953915087e387291027ad40bb1dd350151875bc04a8895b27f6",
          "child does not match frozen SHA-256 vector");
    const Bytes childBytes = encodeRevisionRecord(child);
    check(childBytes.size() == rootBytes.size() + 48 &&
          encodeRevisionRecord(decodeRevisionRecord(childBytes)) == childBytes,
          "child was not canonically decoded");

    const std::map<std::string, Bytes> records{{root.id.toString(), rootBytes}};
    const auto lookup = [&](const RevisionId& id) -> std::optional<Bytes> {
        const auto iter = records.find(id.toString());
        if (iter == records.end()) return std::nullopt;
        return iter->second;
    };
    check(verifyRevisionAncestry(childBytes, lookup) == sha256(childBytes),
          "verified two-record DAG produced wrong tip digest");
    expectFailure([&]{ (void)verifyRevisionAncestry(childBytes, lookup, 1); },
                  StorageErrorKind::LimitExceeded, "ancestry traversal budget was ignored");
}

void testMalformedBytes() {
    RevisionRecord record = makeRecord("018f84c0-0000-7000-8000-000000000001",
                                         "018f84c0-0000-7000-8000-000000000002", true);
    const Bytes canonical = encodeRevisionRecord(record);
    for (std::size_t i = 0; i < canonical.size(); ++i) {
        const Bytes shortRecord(canonical.begin(), canonical.begin() + static_cast<std::ptrdiff_t>(i));
        expectFailure([&]{ (void)decodeRevisionRecord(shortRecord); },
                      StorageErrorKind::Corruption, "truncated record was accepted");
    }
    const auto mutate = [&](std::size_t offset, std::uint8_t newValue) {
        Bytes corrupted = canonical;
        corrupted[offset] = newValue;
        expectFailure([&]{ (void)decodeRevisionRecord(corrupted); },
                      StorageErrorKind::Corruption, "invalid field was accepted");
    };
    mutate(0, 'S'); mutate(1, 2); mutate(8, 0x40); mutate(24, 0x40);
    mutate(10, 0x00); mutate(26, 0x00); mutate(98, 2); mutate(99, 3);
    // Metadata: zero commit time, non-UUIDv7 author, empty or overlong
    // lengths, and text that is not valid UTF-8 or contains controls.
    {
        Bytes zeroTime = canonical;
        std::fill(zeroTime.begin() + 100, zeroTime.begin() + 108, 0);
        expectFailure([&]{ (void)decodeRevisionRecord(zeroTime); },
                      StorageErrorKind::Corruption, "zero commit time accepted");
    }
    mutate(108 + 6, 0x40);                      // author UUID version nibble
    mutate(124, 0xff);                          // author name length high byte
    mutate(125, 0);                             // author name length low byte
    mutate(126, 0x01);                          // control character in the name
    mutate(126, 0xc0);                          // overlong UTF-8 lead byte
    const std::size_t message = 126 + fixtureMetadata().authorName.size() + 4;
    mutate(message - 4, 0x01);                  // message length disagrees with the record
    mutate(message, 0x0d);                      // carriage return in the message
    mutate(message, 0xed);                      // start of a UTF-16 surrogate
    Bytes extra = canonical;
    extra.push_back(0);
    expectFailure([&]{ (void)decodeRevisionRecord(extra); },
                  StorageErrorKind::Corruption, "extra bytes were accepted");

    RevisionRecord invalid = record;
    invalid.modelIndexed = false;
    expectFailure([&]{ (void)encodeRevisionRecord(invalid); },
                  StorageErrorKind::InvalidInput, "raw record had nonzero reverse root");
    invalid = record;
    aowis::dvmdb::UuidV7 malformed = invalid.id.uuid();
    malformed.bytes[6] = 0x40;
    invalid.id = RevisionId(malformed);
    expectFailure([&]{ (void)encodeRevisionRecord(invalid); },
                  StorageErrorKind::InvalidInput, "invalid generated revision UUID accepted");
    invalid = record;
    invalid.parents.push_back({record.id, sha256(canonical)});
    expectFailure([&]{ (void)encodeRevisionRecord(invalid); },
                  StorageErrorKind::InvalidInput, "self-parent accepted");
    invalid = record;
    invalid.parents.push_back({revisionId("018f84c0-0000-7000-8000-000000000003"), sha256(canonical)});
    invalid.parents.push_back(invalid.parents.front());
    expectFailure([&]{ (void)encodeRevisionRecord(invalid); },
                  StorageErrorKind::InvalidInput, "duplicate parent accepted");
    invalid.parents.emplace_back(RevisionParent{revisionId("018f84c0-0000-7000-8000-000000000004"), sha256(canonical)});
    expectFailure([&]{ (void)encodeRevisionRecord(invalid); },
                  StorageErrorKind::InvalidInput, "three parents accepted");
}

void testMetadataValidation() {
    using aowis::dvmdb::validateCommitMetadata;
    const auto rejected = [](CommitMetadata metadata, const char* caseName) {
        expectFailure([&]{ validateCommitMetadata(metadata); },
                      StorageErrorKind::InvalidInput, caseName);
        RevisionRecord record = makeRecord("018f84c0-0000-7000-8000-000000000001",
                                           "018f84c0-0000-7000-8000-000000000002");
        record.metadata = metadata;
        expectFailure([&]{ (void)encodeRevisionRecord(record); },
                      StorageErrorKind::InvalidInput, caseName);
    };
    const CommitMetadata valid = fixtureMetadata();
    validateCommitMetadata(valid);
    CommitMetadata metadata = valid;
    metadata.authorId.bytes[6] = 0x40;  // version 4, not a UUIDv7
    rejected(metadata, "non-UUIDv7 author accepted");
    metadata = valid; metadata.authorName.clear();
    rejected(metadata, "empty author name accepted");
    metadata = valid; metadata.authorName.assign(aowis::dvmdb::maxAuthorNameBytes + 1, 'a');
    rejected(metadata, "201-byte author name accepted");
    metadata = valid; metadata.authorName = "tab\tname";
    rejected(metadata, "control character in author name accepted");
    metadata = valid; metadata.authorName = "line\nname";
    rejected(metadata, "newline in author name accepted");
    metadata = valid; metadata.authorName = "c1\xc2\x85";
    rejected(metadata, "C1 control in author name accepted");
    metadata = valid; metadata.message.clear();
    rejected(metadata, "empty message accepted");
    metadata = valid; metadata.message.assign(aowis::dvmdb::maxCommitMessageBytes + 1, 'm');
    rejected(metadata, "oversized message accepted");
    const std::vector<std::string> badMessages{
        "cr\r\n", std::string("nul\0x", 5), "del\x7f", "\xc0\xaf", "\xed\xa0\x80",
        "\xf4\x90\x80\x80", "truncated\xe2\x82", "\x80"};
    for (const std::string& bad : badMessages) {
        metadata = valid;
        metadata.message = bad;
        rejected(metadata, "invalid message text accepted");
    }
    // Limits are inclusive; tab, newline and any valid UTF-8 are allowed.
    metadata = valid;
    metadata.authorName.assign(aowis::dvmdb::maxAuthorNameBytes, 'a');
    metadata.message.assign(aowis::dvmdb::maxCommitMessageBytes, 'm');
    validateCommitMetadata(metadata);
    metadata.authorName = "名前 \xf0\x9f\x92\xa7";
    metadata.message = "line\n\tindented €";
    validateCommitMetadata(metadata);
    RevisionRecord timeless = makeRecord("018f84c0-0000-7000-8000-000000000001",
                                         "018f84c0-0000-7000-8000-000000000002");
    timeless.committedAtMs = 0;
    expectFailure([&]{ (void)encodeRevisionRecord(timeless); },
                  StorageErrorKind::InvalidInput, "record without commit time accepted");
}

void testMetadataIsPartOfTheDigest() {
    const RevisionRecord base = makeRecord("018f84c0-0000-7000-8000-000000000001",
                                           "018f84c0-0000-7000-8000-000000000002");
    RevisionRecord other = base;
    other.metadata.message = "Another message";
    check(hashRevisionRecord(other) != hashRevisionRecord(base), "message not in the digest");
    other = base;
    other.metadata.authorName = "Someone else";
    check(hashRevisionRecord(other) != hashRevisionRecord(base), "author name not in the digest");
    other = base;
    other.metadata.authorId = *UuidV7::parse("018f84c0-0000-7000-8000-00000000a002");
    check(hashRevisionRecord(other) != hashRevisionRecord(base), "author ID not in the digest");
    other = base;
    other.committedAtMs += 1;
    check(hashRevisionRecord(other) != hashRevisionRecord(base), "commit time not in the digest");
}

void testDivergenceAndMerge() {
    const std::string project = "018f84c0-0000-7000-8000-000000000002";
    RevisionRecord root = makeRecord("018f84c0-0000-7000-8000-000000000001", project);
    RevisionRecord left = makeRecord("018f84c0-0000-7000-8000-000000000003", project);
    RevisionRecord right = makeRecord("018f84c0-0000-7000-8000-000000000004", project);
    left.parents.push_back({root.id, hashRevisionRecord(root)});
    right.parents.push_back({root.id, hashRevisionRecord(root)});
    RevisionRecord merge = makeRecord("018f84c0-0000-7000-8000-000000000005", project);
    merge.parents.push_back({left.id, hashRevisionRecord(left)});
    merge.parents.push_back({right.id, hashRevisionRecord(right)});
    const Bytes merged = encodeRevisionRecord(merge);
    std::map<std::string, Bytes> records{{root.id.toString(), encodeRevisionRecord(root)},
                                         {left.id.toString(), encodeRevisionRecord(left)},
                                         {right.id.toString(), encodeRevisionRecord(right)}};
    std::size_t fetches = 0;
    const auto lookup = [&](const RevisionId& id) -> std::optional<Bytes> {
        ++fetches;
        const auto it = records.find(id.toString());
        if (it == records.end()) return std::nullopt;
        return it->second;
    };
    check(verifyRevisionAncestry(merged, lookup) == sha256(merged), "merge verification failed");
    check(fetches == 3, "shared ancestors were not deduplicated");

    records.erase(root.id.toString());
    expectFailure([&]{ (void)verifyRevisionAncestry(merged, lookup); },
                  StorageErrorKind::Corruption, "missing parent was accepted");
    records[root.id.toString()] = encodeRevisionRecord(root);
    records[left.id.toString()][34] ^= 1;
    expectFailure([&]{ (void)verifyRevisionAncestry(merged, lookup); },
                  StorageErrorKind::Corruption, "altered parent snapshot was accepted");
    records[left.id.toString()] = encodeRevisionRecord(left);
    RevisionRecord wrongProject = root;
    wrongProject.project = projectId("018f84c0-0000-7000-8000-000000000099");
    left.parents[0].expectedRecordHash = hashRevisionRecord(wrongProject);
    records[left.id.toString()] = encodeRevisionRecord(left);
    records[root.id.toString()] = encodeRevisionRecord(wrongProject);
    merge.parents[0].expectedRecordHash = hashRevisionRecord(left);
    expectFailure([&]{ (void)verifyRevisionAncestry(encodeRevisionRecord(merge), lookup); },
                  StorageErrorKind::Corruption, "cross-project parent accepted");
    // A reused UUID with distinct SHA bindings on the two merge arms is invalid.
    right.parents[0].expectedRecordHash.fill(0x44);
    records[right.id.toString()] = encodeRevisionRecord(right);
    merge.parents[1].expectedRecordHash = hashRevisionRecord(right);
    expectFailure([&]{ (void)verifyRevisionAncestry(encodeRevisionRecord(merge), lookup); },
                  StorageErrorKind::Corruption, "conflicting repeated parent UUID accepted");
}

void testIdentitySeparateFromSnapshot() {
    const std::string project = "018f84c0-0000-7000-8000-000000000002";
    const RevisionRecord one = makeRecord("018f84c0-0000-7000-8000-000000000001", project);
    const RevisionRecord two = makeRecord("018f84c0-0000-7000-8000-000000000003", project);
    check(one.root == two.root && hashRevisionRecord(one) != hashRevisionRecord(two),
          "two revisions with the same snapshot must have distinct record hashes");
    RevisionRecord foreign = one;
    foreign.project = projectId("018f84c0-0000-7000-8000-000000000099");
    check(hashRevisionRecord(one) != hashRevisionRecord(foreign),
          "project identity was not included in the revision digest");
}
} // namespace

int main() {
    try {
        testGoldenVectors();
        testMalformedBytes();
        testMetadataValidation();
        testMetadataIsPartOfTheDigest();
        testDivergenceAndMerge();
        testIdentitySeparateFromSnapshot();
        std::cout << "UUIDv7 revision record golden vectors and integrity tests passed\n";
    } catch (const std::exception& exception) {
        std::cerr << "UUIDv7 revision record FAILED:" << exception.what() << '\n';
        return 1;
    }
    return 0;
}
