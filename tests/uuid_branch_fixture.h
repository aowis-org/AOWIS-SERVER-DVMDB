#pragma once

// Test-only forgery of locally stored UUIDv7 history: byte-exact revision
// records (including invalid ones), their UUID->SHA bindings and branch
// records. Simulates on-disk damage that bypassed the normal write path.

#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/native_store.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aowis::dvmdb::test {

inline std::filesystem::path branchFile(Repository& repository,
                                        const std::filesystem::path& root,
                                        std::string_view name) {
    const std::optional<BranchId> id = repository.branchId(name);
    if (!id) throw std::runtime_error("test branch missing UUIDv7");
    return root / "refs" / "heads" / id->toString();
}

// Replace a branch record with arbitrary bytes (e.g. a malformed record).
inline void overwriteBranch(Repository& repository, const std::filesystem::path& root,
                            std::string_view name, std::string_view content) {
    std::ofstream output(branchFile(repository, root, name), std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("test branch record cannot open");
    output << content;
    if (!output) throw std::runtime_error("test branch record cannot write");
}

// Point a branch (without fork provenance) at a revision UUID and digest.
inline void repointBranch(Repository& repository, const std::filesystem::path& root,
                          std::string_view name, const RevisionId& revision, const Hash& digest) {
    overwriteBranch(repository, root, name,
                    "AOWIS-BRANCH-2\n" + std::string(name) + "\n" + revision.toString() + "\n" +
                        toHex(digest) + "\n");
}

// Canonical revision record layout ('T',3), written without the
// encoder's validation so tests can store records the encoder would refuse.
inline Bytes forgedRevisionRecord(const RevisionId& id, const ProjectId& project, const Hash& root,
                                  const Hash& references, std::uint8_t modelFlag,
                                  const std::vector<std::pair<RevisionId, Hash>>& parents) {
    Bytes bytes{'T', 3};
    for (const UuidV7* uuid : {&id.uuid(), &project.uuid()})
        bytes.insert(bytes.end(), uuid->bytes.begin(), uuid->bytes.end());
    bytes.insert(bytes.end(), root.begin(), root.end());
    bytes.insert(bytes.end(), references.begin(), references.end());
    bytes.push_back(modelFlag);
    bytes.push_back(static_cast<std::uint8_t>(parents.size()));
    for (const std::pair<RevisionId, Hash>& parent : parents) {
        bytes.insert(bytes.end(), parent.first.uuid().bytes.begin(), parent.first.uuid().bytes.end());
        bytes.insert(bytes.end(), parent.second.begin(), parent.second.end());
    }
    appendRecordMetadata(bytes);
    return bytes;
}

// Publish a UUID->SHA binding directly, as RevisionResolver stores it:
// 'R','I','D',2 + revision UUID + project UUID + record SHA-256.
inline void forgeBinding(const std::filesystem::path& root, const ProjectId& project,
                         const RevisionId& revision, const Hash& digest) {
    Bytes bytes{'R', 'I', 'D', 2};
    for (const UuidV7* uuid : {&revision.uuid(), &project.uuid()})
        bytes.insert(bytes.end(), uuid->bytes.begin(), uuid->bytes.end());
    bytes.insert(bytes.end(), digest.begin(), digest.end());
    std::ofstream output(root / "revision-v2" / "ids" / revision.toString(),
                         std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("test revision binding cannot write");
}

} // namespace aowis::dvmdb::test
