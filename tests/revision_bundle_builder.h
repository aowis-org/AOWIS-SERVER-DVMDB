#pragma once

// Test support: byte-exact UUIDv7 revision bundles (AOWREV21 full, AOWREV22
// delta) holding arbitrary, possibly malformed objects, for exercising the
// importer's verifier. Revision records may be forged without validation.

#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/native_store.h"
#include "aowis/dvmdb/revision_record.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace aowis::dvmdb::test {

// A revision placed in a bundle (or held by the recipient): its UUID and the
// SHA-256 of its record.
struct BuiltRevision {
    RevisionId id;
    Hash hash{};
};

class RevisionBundleBuilder {
public:
    explicit RevisionBundleBuilder(ProjectId project) : project_(project) {}

    const ProjectId& project() const noexcept { return this->project_; }
    std::map<Hash, Bytes>& objects() noexcept { return this->objects_; }

    Hash add(const Bytes& payload) {
        const Hash hash = sha256(payload);
        this->objects_.emplace(hash, payload);
        return hash;
    }

    // Canonical record; `modelFlag` other than 0/1 forges invalid metadata.
    BuiltRevision revision(const Hash& root, const Hash& references, bool model,
                           const std::vector<BuiltRevision>& parents = {},
                           std::optional<std::uint8_t> modelFlag = std::nullopt) {
        const RevisionId id = RevisionId::generate();
        Bytes bytes{'T', 3};
        for (const UuidV7* uuid : {&id.uuid(), &this->project_.uuid()})
            bytes.insert(bytes.end(), uuid->bytes.begin(), uuid->bytes.end());
        bytes.insert(bytes.end(), root.begin(), root.end());
        bytes.insert(bytes.end(), references.begin(), references.end());
        bytes.push_back(modelFlag.value_or(model ? 1 : 0));
        bytes.push_back(static_cast<std::uint8_t>(parents.size()));
        for (const BuiltRevision& parent : parents) {
            bytes.insert(bytes.end(), parent.id.uuid().bytes.begin(), parent.id.uuid().bytes.end());
            bytes.insert(bytes.end(), parent.hash.begin(), parent.hash.end());
        }
        appendRecordMetadata(bytes);
        return BuiltRevision{id, add(bytes)};
    }

    // `base` makes a delta (AOWREV22) for a recipient whose HEAD is `base`.
    void write(const std::filesystem::path& path, const BuiltRevision& tip,
               const std::optional<BuiltRevision>& base = std::nullopt,
               const std::optional<BranchId>& branch = std::nullopt) const {
        Bytes bundle{'A', 'O', 'W', 'R', 'E', 'V', '2', static_cast<std::uint8_t>(base ? '2' : '1')};
        const BranchId id = branch.value_or(BranchId::generate());
        for (const UuidV7* uuid : {&this->project_.uuid(), &id.uuid(), &tip.id.uuid()})
            bundle.insert(bundle.end(), uuid->bytes.begin(), uuid->bytes.end());
        bundle.insert(bundle.end(), tip.hash.begin(), tip.hash.end());
        appendU64(bundle, static_cast<std::uint64_t>(this->objects_.size()));
        if (base) {
            bundle.insert(bundle.end(), base->id.uuid().bytes.begin(), base->id.uuid().bytes.end());
            bundle.insert(bundle.end(), base->hash.begin(), base->hash.end());
        }
        for (const std::pair<const Hash, Bytes>& entry : this->objects_) {
            bundle.insert(bundle.end(), entry.first.begin(), entry.first.end());
            appendU64(bundle, static_cast<std::uint64_t>(entry.second.size()));
            bundle.insert(bundle.end(), entry.second.begin(), entry.second.end());
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bundle.data()),
                     static_cast<std::streamsize>(bundle.size()));
        if (!output) throw std::runtime_error("cannot write test bundle");
    }

private:
    static void appendU64(Bytes& bytes, std::uint64_t value) {
        for (unsigned position = 0; position < 8; ++position)
            bytes.push_back(static_cast<std::uint8_t>(value >> (56U - 8U * position)));
    }

    ProjectId project_;
    std::map<Hash, Bytes> objects_;
};

} // namespace aowis::dvmdb::test
