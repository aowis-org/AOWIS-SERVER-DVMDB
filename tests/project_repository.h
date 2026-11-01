#pragma once

// Test support: a Repository bound to its project UUIDv7, with convenience
// reads at a branch HEAD and bundle export/import through regular files.
// Every call goes through the public UUIDv7 Repository API; nothing here is
// a second storage path. Tests that need an API detail use native().

#include "aowis/dvmdb/native_store.h"
#include "aowis/dvmdb/revision_record.h"
#include "commit_metadata_fixture.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <string>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace aowis::dvmdb::test {

// A decoded immutable revision record and its SHA-256 record digest.
struct CommittedRevision {
    RevisionId id;
    Hash recordHash{};
    Hash root{};
    Hash referencesRoot{};
    bool modelIndexed = false;
    std::vector<RevisionId> parents;
    CommitMetadata metadata;
    std::uint64_t committedAtMs = 0;
};

// Layout of a UUIDv7 revision bundle file: header size by format marker,
// then the big-endian object count at offset 88 and the sorted object
// entries (32-byte SHA-256, 8-byte big-endian size, payload).
struct BundleLayout {
    std::uint64_t headerBytes = 0;
    std::uint64_t objects = 0;
    bool incremental = false;
};

inline BundleLayout bundleLayout(const std::filesystem::path& file) {
    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throwSystemError("open bundle");
    std::array<std::uint8_t, 96> header{};
    const ssize_t read = ::pread(fd, header.data(), header.size(), 0);
    ::close(fd);
    if (read != static_cast<ssize_t>(header.size()))
        throw StorageError(StorageErrorKind::InvalidInput, "truncated bundle header");
    const std::string magic(header.begin(), header.begin() + 8);
    BundleLayout layout;
    if (magic == "AOWREV21") layout.headerBytes = 96;
    else if (magic == "AOWREV22") layout.headerBytes = 144;
    else if (magic == "AOWREV23") layout.headerBytes = 128;
    else if (magic == "AOWREV24") layout.headerBytes = 176;
    else throw StorageError(StorageErrorKind::InvalidInput, "not a revision bundle");
    layout.incremental = magic == "AOWREV22" || magic == "AOWREV24";
    for (unsigned i = 0; i < 8; ++i) layout.objects = (layout.objects << 8U) | header[88 + i];
    return layout;
}

class ProjectRepository final {
public:
    // Reopening an existing repository keeps its pinned project. A fresh
    // repository uses `project` when given, else a new UUIDv7; a fresh one
    // without an explicit project adopts the project of the first bundle it
    // imports, like a clone target.
    explicit ProjectRepository(std::filesystem::path directory,
                               std::optional<ProjectId> project = std::nullopt)
        : repository_(std::move(directory)),
          project_(this->repository_.projectId().value_or(
              project.value_or(ProjectId::generate()))),
          adoptsBundleProject_(!project && !this->repository_.projectId()) {}

    ProjectRepository(const ProjectRepository&) = delete;
    ProjectRepository& operator=(const ProjectRepository&) = delete;

    Repository& native() noexcept { return this->repository_; }
    const ProjectId& project() const noexcept { return this->project_; }

    // Repository-wide operations (no project qualification).
    VerificationReport verifyRepository() { return this->repository_.verifyRepository(); }
    void verifyPack() { this->repository_.verifyPack(); }
    CompactionStats compact() { return this->repository_.compact(); }
    std::uint64_t packSize() { return this->repository_.packSize(); }
    std::uint64_t objectCount() { return this->repository_.objectCount(); }
    std::uint64_t recoveredPackTailBytes() { return this->repository_.recoveredPackTailBytes(); }
    ScratchFile createScratchFile() { return this->repository_.createScratchFile(); }
    const std::filesystem::path& scratchDirectory() const noexcept {
        return this->repository_.scratchDirectory();
    }
    std::vector<std::string> listBranches() { return this->repository_.listBranches(); }
    bool hasBranch(std::string_view branch) { return this->repository_.hasBranch(branch); }
    std::optional<BranchId> branchId(std::string_view branch) {
        return this->repository_.branchId(branch);
    }

    // Without a source: the repository's first branch, unborn. With a
    // source: a new branch at the source's HEAD.
    void createBranch(std::string_view name, std::optional<std::string_view> from = std::nullopt) {
        if (from) (void)this->repository_.createBranch(this->project_, name, from);
        else this->repository_.createUnbornBranch(this->project_, name);
    }
    void createBranchAt(std::string_view name, std::string_view source, const RevisionId& revision) {
        (void)this->repository_.createBranchAtRevision(this->project_, name, source, revision);
    }
    void renameBranch(std::string_view previous, std::string_view renamed,
                      const RevisionId& expectedHead) {
        this->repository_.renameBranch(this->project_, previous, renamed, expectedHead);
    }

    // Every revision records the fixture's test author and message.
    RevisionId commit(std::string_view branch, const std::vector<Change>& changes,
                      std::optional<RevisionId> expectedHead = std::nullopt) {
        return this->repository_.commit(this->project_, branch, changes, commitMetadata(),
                                        expectedHead);
    }
    RevisionId commitModel(std::string_view branch, std::vector<ModelChange> changes,
                           std::optional<RevisionId> expectedHead = std::nullopt) {
        return this->repository_.commitModel(this->project_, branch, std::move(changes),
                                               commitMetadata(), expectedHead);
    }

    // Commit and return the decoded immutable record.
    CommittedRevision commitRevision(std::string_view branch, const std::vector<Change>& changes,
                                     std::optional<RevisionId> expectedHead = std::nullopt) {
        return revision(commit(branch, changes, expectedHead));
    }
    CommittedRevision commitModelRevision(std::string_view branch, std::vector<ModelChange> changes,
                                          std::optional<RevisionId> expectedHead = std::nullopt) {
        return revision(commitModel(branch, std::move(changes), expectedHead));
    }

    RevisionId head(std::string_view branch) {
        return this->repository_.head(this->project_, branch);
    }
    std::optional<RevisionId> optionalHead(std::string_view branch) {
        return this->repository_.optionalHead(this->project_, branch);
    }
    CommittedRevision revision(const RevisionId& id) {
        const std::optional<Bytes> bytes = this->repository_.readRevision(this->project_, id);
        if (!bytes) throw StorageError(StorageErrorKind::NotFound, "revision UUID not found");
        const RevisionRecord record = decodeRevisionRecord(*bytes);
        CommittedRevision result{record.id, sha256(*bytes), record.root, record.referencesRoot,
                                 record.modelIndexed, {}, record.metadata, record.committedAtMs};
        for (const RevisionParent& parent : record.parents) result.parents.push_back(parent.id);
        return result;
    }
    CommittedRevision headRevision(std::string_view branch) { return revision(head(branch)); }
    std::vector<RevisionGraphRevision> history(std::string_view branch, std::size_t maxCount = 100,
                                                 std::optional<RevisionId> after = std::nullopt) {
        return this->repository_.history(this->project_, branch, maxCount, after);
    }
    bool reachable(const RevisionId& tip, const RevisionId& candidate) {
        return this->repository_.revisionReachableFrom(this->project_, tip, candidate);
    }

    // Reads at the branch HEAD; an unborn branch has no values.
    std::optional<Bytes> get(std::string_view branch, const Key& key) {
        const std::optional<RevisionId> tip = optionalHead(branch);
        return tip ? getAt(*tip, key) : std::nullopt;
    }
    std::optional<Bytes> getAt(const RevisionId& revision, const Key& key) {
        return this->repository_.getAtRevision(this->project_, revision, key);
    }
    std::vector<std::pair<Key, Bytes>> scan(std::string_view branch) {
        const std::optional<RevisionId> tip = optionalHead(branch);
        if (!tip) return {};
        return this->repository_.scanAtRevision(this->project_, *tip);
    }
    std::optional<ModelRecord> getModel(std::string_view branch, const Key& key) {
        const std::optional<RevisionId> tip = optionalHead(branch);
        return tip ? getModelAt(*tip, key) : std::nullopt;
    }
    std::optional<ModelRecord> getModelAt(const RevisionId& revision, const Key& key) {
        return this->repository_.getModelAtRevision(this->project_, revision, key);
    }
    std::vector<Key> referencing(std::string_view branch, const Key& key) {
        return referencingAt(head(branch), key);
    }
    std::vector<Key> referencingAt(const RevisionId& revision, const Key& key) {
        return this->repository_.referencingModelAtRevision(this->project_, revision, key);
    }
    std::vector<Difference> diff(const RevisionId& from, const RevisionId& to,
                                 std::size_t maxDifferences = std::numeric_limits<std::size_t>::max()) {
        return this->repository_.diffRevisions(this->project_, from, to, maxDifferences);
    }
    std::vector<Difference> diffBranches(std::string_view from, std::string_view to) {
        return diff(head(from), head(to));
    }
    MergeResult merge(std::string_view target, std::string_view source,
                        const MergeOptions& options = {},
                        std::optional<RevisionId> expectedTarget = std::nullopt,
                        std::optional<RevisionId> expectedSource = std::nullopt) {
        return this->repository_.mergeBranches(this->project_, target, source,
                                                 commitMetadata("merge"), expectedTarget,
                                                 expectedSource, options);
    }

    // Named bundle files are published atomically by the engine: export never
    // replaces an existing file and never leaves a partial one.
    void exportBundle(std::string_view branch, const std::filesystem::path& file,
                      BundleLimits limits = {}) {
        this->repository_.exportRevisionBundle(this->project_, branch, file, limits);
    }
    void exportIncrementalBundle(std::string_view branch, const RevisionId& recipientBase,
                                 const std::filesystem::path& file, BundleLimits limits = {}) {
        this->repository_.exportIncrementalRevisionBundle(this->project_, branch, recipientBase,
                                                            file, limits);
    }
    // Descriptor forms: the caller owns `fd` (an empty regular file for export).
    void exportBundle(std::string_view branch, int fd, BundleLimits limits = {}) {
        this->repository_.exportRevisionBundle(this->project_, branch, fd, limits);
    }
    void exportIncrementalBundle(std::string_view branch, const RevisionId& recipientBase, int fd,
                                 BundleLimits limits = {}) {
        this->repository_.exportIncrementalRevisionBundle(this->project_, branch, recipientBase,
                                                            fd, limits);
    }
    RevisionBundleImport importBundle(int fd, std::string_view localBranch,
                                        BundleLimits limits = {},
                                        BundleImportProfile* profile = nullptr) {
        adoptProjectOf(fd);
        return this->repository_.importRevisionBundle(this->project_, fd, localBranch, limits,
                                                        profile);
    }
    RevisionBundleImport importBundle(const std::filesystem::path& file,
                                        std::string_view localBranch, BundleLimits limits = {},
                                        BundleImportProfile* profile = nullptr) {
        const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) throwSystemError("open bundle for import");
        try {
            adoptProjectOf(fd);
            RevisionBundleImport result = this->repository_.importRevisionBundle(
                this->project_, fd, localBranch, limits, profile);
            ::close(fd);
            return result;
        } catch (...) {
            ::close(fd);
            throw;
        }
    }

private:
    // Bundle header: 8-byte magic, then the project UUIDv7.
    void adoptProjectOf(int fd) {
        if (!this->adoptsBundleProject_ || this->repository_.projectId()) return;
        UuidV7 uuid{};
        if (::pread(fd, uuid.bytes.data(), uuid.bytes.size(), 8) !=
            static_cast<ssize_t>(uuid.bytes.size())) return;
        if (const std::optional<ProjectId> named = ProjectId::parse(uuid.toString()))
            this->project_ = *named;
    }

    Repository repository_;
    ProjectId project_;
    bool adoptsBundleProject_ = false;
};

} // namespace aowis::dvmdb::test
