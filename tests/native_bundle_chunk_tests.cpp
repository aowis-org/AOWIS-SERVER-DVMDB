// Size-bounded revision bundle export (Repository::exportRevisionBundleChunk):
// a branch larger than a transfer ceiling is copied as a sequence of bundles
// along HEAD's first-parent chain, each one fitting the ceiling, each tip the
// newest that fits, and the recipient ends at exactly the source HEAD.
#include "aowis/dvmdb/native_store.h"
#include "project_repository.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using aowis::dvmdb::BundleImportStatus;
using aowis::dvmdb::BundleLimits;
using aowis::dvmdb::Bytes;
using aowis::dvmdb::Change;
using aowis::dvmdb::Key;
using aowis::dvmdb::MergeStatus;
using aowis::dvmdb::RevisionBundleChunk;
using aowis::dvmdb::RevisionBundleImport;
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::StorageError;
using aowis::dvmdb::StorageErrorKind;
using aowis::dvmdb::test::ProjectRepository;

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <class Callable>
void rejects(StorageErrorKind kind, Callable operation, std::string_view message) {
    try {
        operation();
    } catch (const StorageError& error) {
        if (error.kind() != kind)
            throw std::runtime_error(std::string(message) + ": wrong error kind: " + error.what());
        return;
    }
    throw std::runtime_error(std::string(message) + ": accepted");
}

class TemporaryRoot {
public:
    TemporaryRoot() {
        char path[] = "/tmp/aowis-dvmdb-bundle-chunks-XXXXXX";
        const char* created = ::mkdtemp(path);
        check(created != nullptr, "temporary directory creation failed");
        this->root_ = created;
    }
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(this->root_, ignored);
    }
    const std::filesystem::path& path() const { return this->root_; }
private:
    std::filesystem::path root_;
};

// An empty caller-owned regular file, as the HTTP layer's scratch file.
class BundleFile {
public:
    explicit BundleFile(const std::filesystem::path& path) {
        this->fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        check(this->fd_ >= 0, "cannot create bundle file");
    }
    ~BundleFile() { if (this->fd_ >= 0) ::close(this->fd_); }
    BundleFile(const BundleFile&) = delete;
    BundleFile& operator=(const BundleFile&) = delete;
    int fd() const { return this->fd_; }
    std::uint64_t size() const {
        struct stat st{};
        check(::fstat(this->fd_, &st) == 0, "cannot stat bundle file");
        return static_cast<std::uint64_t>(st.st_size);
    }
private:
    int fd_ = -1;
};

Key testKey(unsigned sequence) {
    Key result{};
    result[12] = static_cast<std::uint8_t>(sequence >> 24U);
    result[13] = static_cast<std::uint8_t>(sequence >> 16U);
    result[14] = static_cast<std::uint8_t>(sequence >> 8U);
    result[15] = static_cast<std::uint8_t>(sequence);
    return result;
}

// Distinct, incompressible-looking payloads so every revision adds bytes.
Bytes payload(unsigned sequence, std::size_t size = 900) {
    Bytes bytes(size, 0);
    std::uint32_t state = 2166136261U ^ sequence;
    for (std::uint8_t& byte : bytes) {
        state = state * 16777619U + 0x9e3779b9U;
        byte = static_cast<std::uint8_t>(state >> 24U);
    }
    return bytes;
}

// Linear history: returns the revisions oldest first.
std::vector<RevisionId> linearHistory(ProjectRepository& repository, std::string_view branch,
                                      unsigned first, unsigned count) {
    std::vector<RevisionId> revisions;
    for (unsigned index = first; index < first + count; ++index)
        revisions.push_back(repository.commit(branch, {Change{testKey(index), payload(index)}}));
    return revisions;
}

struct Transfer {
    std::vector<RevisionBundleChunk> chunks;
    std::vector<std::uint64_t> sizes;
    std::vector<RevisionBundleImport> imports;
};

// The client loop: start from `base` (nullopt for a full first chunk) and
// continue from each chunk's head until a chunk is complete.
Transfer transfer(ProjectRepository& source, ProjectRepository& destination,
                  const std::filesystem::path& scratch, std::string_view label, std::string_view branch,
                  std::optional<RevisionId> base, std::uint64_t maxBytes,
                  BundleLimits limits = {}, std::optional<RevisionId> upTo = std::nullopt) {
    Transfer result;
    for (unsigned step = 0; step < 1000; ++step) {
        const std::filesystem::path path =
            scratch / (std::string(label) + "-" + std::string(branch) + "-" + std::to_string(step) + ".bundle");
        BundleFile file(path);
        const RevisionBundleChunk chunk = source.exportBundleChunk(branch, base, maxBytes, file.fd(), limits, upTo);
        result.chunks.push_back(chunk);
        result.sizes.push_back(file.size());
        result.imports.push_back(destination.importBundle(file.fd(), branch));
        check(result.imports.back().head == chunk.head, "imported head differs from chunk head");
        if (chunk.complete) return result;
        check(!base || chunk.head != *base, "an incomplete chunk made no progress");
        base = chunk.head;
    }
    throw std::runtime_error("chunked transfer did not complete");
}

std::uint64_t fullBundleSize(ProjectRepository& repository, const std::filesystem::path& path,
                             std::string_view branch) {
    BundleFile file(path);
    repository.exportBundle(branch, file.fd());
    return file.size();
}

std::uint64_t deltaBundleSize(ProjectRepository& repository, const std::filesystem::path& path,
                              std::string_view branch, const RevisionId& base) {
    BundleFile file(path);
    repository.exportIncrementalBundle(branch, base, file.fd());
    return file.size();
}

void testWholeBranchFitsInOneChunk() {
    TemporaryRoot temporary;
    ProjectRepository source(temporary.path() / "source");
    ProjectRepository destination(temporary.path() / "destination");
    source.createBranch("main");
    (void)linearHistory(source, "main", 1, 5);
    const std::uint64_t full = fullBundleSize(source, temporary.path() / "full.bundle", "main");

    const Transfer exact = transfer(source, destination, temporary.path(), "exact", "main", std::nullopt, full);
    check(exact.chunks.size() == 1 && exact.chunks.front().complete &&
          exact.chunks.front().head == source.head("main"),
          "a branch that fits exactly was split");
    check(exact.sizes.front() == full, "a complete chunk differs from the plain full bundle");
    check(exact.imports.front().status == BundleImportStatus::Created, "first chunk did not create the branch");
    check(destination.head("main") == source.head("main"), "destination did not reach source HEAD");
}

void testLinearHistorySplitsIntoNewestFittingChunks() {
    TemporaryRoot temporary;
    ProjectRepository source(temporary.path() / "source");
    ProjectRepository destination(temporary.path() / "destination");
    source.createBranch("main");
    const std::vector<RevisionId> revisions = linearHistory(source, "main", 1, 24);
    const std::uint64_t full = fullBundleSize(source, temporary.path() / "full.bundle", "main");
    const std::uint64_t limit = full / 5;

    const Transfer chunks = transfer(source, destination, temporary.path(), "linear", "main", std::nullopt, limit);
    check(chunks.chunks.size() >= 5, "a fifth of the history did not need at least five chunks");
    for (std::uint64_t size : chunks.sizes) check(size <= limit, "a chunk exceeds the byte limit");
    check(chunks.imports.front().status == BundleImportStatus::Created, "first chunk did not create the branch");
    for (std::size_t index = 1; index < chunks.imports.size(); ++index)
        check(chunks.imports[index].status == BundleImportStatus::FastForward,
              "a later chunk did not fast-forward the branch");
    for (std::size_t index = 0; index + 1 < chunks.chunks.size(); ++index)
        check(!chunks.chunks[index].complete, "only the last chunk may be complete");
    check(chunks.chunks.back().complete && chunks.chunks.back().head == revisions.back(),
          "the last chunk is not the source HEAD");
    check(destination.head("main") == source.head("main"), "destination did not reach source HEAD");
    for (unsigned index = 1; index <= 24; ++index)
        check(destination.get("main", testKey(index)) == std::optional<Bytes>(payload(index)),
              "a value is missing or wrong after the chunked transfer");

    // Every tip is the newest that fits: one revision more would not.
    std::optional<RevisionId> base;
    for (std::size_t index = 0; index + 1 < chunks.chunks.size(); ++index) {
        const RevisionId tip = chunks.chunks[index].head;
        std::size_t position = 0;
        while (revisions[position] != tip) ++position;
        check(position + 1 < revisions.size(), "an incomplete chunk ended at HEAD");
        // Export the next revision's bundle from a throwaway branch at it.
        const std::string probe = "probe-" + std::to_string(index);
        source.createBranchAt(probe, "main", revisions[position + 1]);
        const std::filesystem::path probePath = temporary.path() / (probe + ".bundle");
        const std::uint64_t larger = base ? deltaBundleSize(source, probePath, probe, *base)
                                          : fullBundleSize(source, probePath, probe);
        // A probe branch has fork provenance, which adds 32 header bytes.
        check(larger - 32 > limit, "a chunk stopped before the newest fitting revision");
        base = tip;
    }
}

void testLimitTooSmallForOneRevision() {
    TemporaryRoot temporary;
    ProjectRepository source(temporary.path() / "source");
    source.createBranch("main");
    (void)linearHistory(source, "main", 1, 3);
    BundleFile file(temporary.path() / "small.bundle");
    rejects(StorageErrorKind::LimitExceeded, [&] {
        (void)source.exportBundleChunk("main", std::nullopt, 64, file.fd());
    }, "a limit below one revision");
    check(file.size() == 0, "a rejected chunk left bytes in the descriptor");
    rejects(StorageErrorKind::InvalidInput, [&] {
        (void)source.exportBundleChunk("main", std::nullopt, 0, file.fd());
    }, "a zero byte limit");
}

void testConfiguredLimitsAlsoBoundChunks() {
    TemporaryRoot temporary;
    ProjectRepository source(temporary.path() / "source");
    ProjectRepository destination(temporary.path() / "destination");
    source.createBranch("main");
    (void)linearHistory(source, "main", 1, 12);
    const std::uint64_t full = fullBundleSize(source, temporary.path() / "full.bundle", "main");
    // A tighter configured byte limit wins over a generous request.
    BundleLimits bytes;
    bytes.maxBytes = full / 3;
    const Transfer byBytes = transfer(source, destination, temporary.path(), "bytes", "main", std::nullopt,
                                      full * 10, bytes);
    check(byBytes.chunks.size() >= 3, "the configured byte limit was ignored");
    for (std::uint64_t size : byBytes.sizes) check(size <= bytes.maxBytes, "a chunk exceeds the configured byte limit");

    // The object budget is honoured too.
    ProjectRepository second(temporary.path() / "second");
    BundleLimits objects;
    objects.maxObjects = 20;
    const Transfer byObjects = transfer(source, second, temporary.path(), "objects", "main", std::nullopt,
                                        full * 10, objects);
    check(byObjects.chunks.size() > 1, "the object limit did not split the transfer");
    for (const RevisionBundleImport& imported : byObjects.imports)
        check(imported.objectsImported <= objects.maxObjects, "a chunk exceeds the object limit");
    check(second.head("main") == source.head("main"), "object-bounded transfer did not finish");
}

void testIncrementalContinuationAfterExistingBase() {
    TemporaryRoot temporary;
    ProjectRepository source(temporary.path() / "source");
    ProjectRepository destination(temporary.path() / "destination");
    source.createBranch("main");
    (void)linearHistory(source, "main", 1, 4);
    const Transfer seed = transfer(source, destination, temporary.path(), "seed", "main", std::nullopt, 1U << 30U);
    const RevisionId base = source.head("main");
    const std::vector<RevisionId> later = linearHistory(source, "main", 100, 16);
    const std::uint64_t delta = deltaBundleSize(source, temporary.path() / "delta.bundle", "main", base);

    const Transfer rest = transfer(source, destination, temporary.path(), "rest", "main", base, delta / 4);
    check(rest.chunks.size() >= 4, "an incremental continuation was not split");
    for (std::uint64_t size : rest.sizes) check(size <= delta / 4, "an incremental chunk exceeds the limit");
    for (const RevisionBundleChunk& chunk : rest.chunks)
        check(chunk.head != base, "a chunk returned the recipient base");
    check(destination.head("main") == later.back(), "incremental chunks did not reach HEAD");
    (void)seed;
}

void testMergedHistoryCarriesSecondParents() {
    TemporaryRoot temporary;
    ProjectRepository source(temporary.path() / "source");
    ProjectRepository destination(temporary.path() / "destination");
    source.createBranch("main");
    (void)linearHistory(source, "main", 1, 4);
    source.createBranch("side", "main");
    (void)linearHistory(source, "side", 200, 6);
    (void)linearHistory(source, "main", 10, 6);
    check(source.merge("main", "side").status == MergeStatus::Merged, "fixture merge failed");
    (void)linearHistory(source, "main", 20, 4);
    const std::uint64_t full = fullBundleSize(source, temporary.path() / "full.bundle", "main");

    // A merge and the side history it brings in are one first-parent step.
    const Transfer chunks = transfer(source, destination, temporary.path(), "merged", "main", std::nullopt, full / 2);
    check(chunks.chunks.size() >= 2, "merged history was not split");
    for (std::uint64_t size : chunks.sizes) check(size <= full / 2, "a merged-history chunk exceeds the limit");
    check(destination.head("main") == source.head("main"), "merged history did not reach HEAD");
    for (unsigned index = 200; index < 206; ++index)
        check(destination.get("main", testKey(index)) == std::optional<Bytes>(payload(index)),
              "a merged side revision is missing after the chunked transfer");
}

void testForkedBranchNeverPrecedesItsForkRevision() {
    TemporaryRoot temporary;
    ProjectRepository source(temporary.path() / "source");
    ProjectRepository destination(temporary.path() / "destination");
    source.createBranch("main");
    const std::vector<RevisionId> trunk = linearHistory(source, "main", 1, 12);
    source.createBranch("feature", "main");
    const RevisionId fork = source.head("feature");
    const std::vector<RevisionId> feature = linearHistory(source, "feature", 300, 10);
    const std::uint64_t full = fullBundleSize(source, temporary.path() / "feature-full.bundle", "feature");
    const std::uint64_t atFork = [&] {
        source.createBranchAt("at-fork", "main", fork);
        return fullBundleSize(source, temporary.path() / "at-fork.bundle", "at-fork");
    }();
    check(atFork < full, "invalid fork fixture");

    // Without a base the first chunk must contain the fork revision.
    const Transfer chunks = transfer(source, destination, temporary.path(), "fork", "feature", std::nullopt,
                                     atFork + (full - atFork) / 3);
    check(chunks.chunks.size() > 1, "the forked branch was not split");
    check(chunks.chunks.front().head == fork || [&] {
              for (const RevisionId& revision : feature) if (revision == chunks.chunks.front().head) return true;
              return false;
          }(), "the first chunk of a forked branch ends before its fork revision");
    check(destination.head("feature") == feature.back(), "forked branch did not reach HEAD");
    const std::optional<aowis::dvmdb::BranchId> sourceId = source.branchId("feature");
    const std::optional<aowis::dvmdb::BranchId> destinationId = destination.branchId("feature");
    check(sourceId && destinationId && *sourceId == *destinationId, "branch UUIDv7 changed");
    const std::optional<aowis::dvmdb::RevisionGraphHead> copied =
        destination.native().branchById(destination.project(), *destinationId);
    check(copied && copied->forkRevisionId == std::optional<RevisionId>(fork),
          "fork provenance was not preserved");

    // Below the fork revision there is nothing to split: a limit that only
    // fits earlier trunk revisions is rejected, not answered with a tip that
    // would violate provenance.
    ProjectRepository other(temporary.path() / "other");
    BundleFile file(temporary.path() / "too-small-for-fork.bundle");
    rejects(StorageErrorKind::LimitExceeded, [&] {
        (void)source.exportBundleChunk("feature", std::nullopt, atFork - 1, file.fd());
    }, "a chunk below the fork revision");
    (void)trunk;
    (void)other;
}

void testBaseOffTheFirstParentChainOnlyOffersHead() {
    TemporaryRoot temporary;
    ProjectRepository source(temporary.path() / "source");
    source.createBranch("main");
    (void)linearHistory(source, "main", 1, 3);
    source.createBranch("side", "main");
    (void)linearHistory(source, "side", 50, 3);
    const RevisionId sideTip = source.head("side");
    (void)linearHistory(source, "main", 10, 3);
    check(source.merge("main", "side").status == MergeStatus::Merged, "fixture merge failed");
    (void)linearHistory(source, "main", 20, 3);
    // sideTip is an ancestor of HEAD only through a second parent.
    const std::uint64_t delta = deltaBundleSize(source, temporary.path() / "delta.bundle", "main", sideTip);
    {
        BundleFile file(temporary.path() / "head-only.bundle");
        const RevisionBundleChunk chunk = source.exportBundleChunk("main", sideTip, delta, file.fd());
        check(chunk.complete && chunk.head == source.head("main") && file.size() == delta,
              "a second-parent base did not fall back to the HEAD export");
    }
    BundleFile file(temporary.path() / "head-only-too-small.bundle");
    rejects(StorageErrorKind::LimitExceeded, [&] {
        (void)source.exportBundleChunk("main", sideTip, delta - 1, file.fd());
    }, "a second-parent base below the HEAD bundle size");
}

// Push of part of a branch: everything up to and including the selected
// revision is sent, in chunks, and the recipient ends exactly there.
void testPushUpToARevisionStopsThere() {
    TemporaryRoot temporary;
    ProjectRepository source(temporary.path() / "source");
    ProjectRepository destination(temporary.path() / "destination");
    source.createBranch("main");
    const std::vector<RevisionId> revisions = linearHistory(source, "main", 1, 20);

    // First push creates the branch at revision 5, in one bundle.
    const Transfer first = transfer(source, destination, temporary.path(), "push-first", "main",
                                    std::nullopt, 1U << 30U, {}, revisions[4]);
    check(first.chunks.size() == 1 && first.chunks.front().complete &&
          first.chunks.front().head == revisions[4] &&
          first.imports.front().status == BundleImportStatus::Created,
          "a first push did not create the branch at the pushed revision");
    check(destination.head("main") == revisions[4], "the recipient is not at the pushed revision");
    check(!destination.get("main", testKey(6)), "a revision after the pushed one was sent");

    // A second push up to revision 13 continues from the recipient, split.
    const std::uint64_t delta = deltaBundleSize(source, temporary.path() / "push-delta.bundle", "main", revisions[4]);
    const Transfer middle = transfer(source, destination, temporary.path(), "push-middle", "main",
                                     revisions[4], delta / 6, {}, revisions[12]);
    check(middle.chunks.size() > 1, "a partial push was not split into chunks");
    for (std::size_t index = 0; index + 1 < middle.chunks.size(); ++index)
        check(!middle.chunks[index].complete, "a chunk before the pushed revision is complete");
    for (std::uint64_t size : middle.sizes) check(size <= delta / 6, "a push chunk exceeds the limit");
    check(middle.chunks.back().head == revisions[12] && destination.head("main") == revisions[12],
          "a partial push did not end at the pushed revision");
    for (unsigned index = 1; index <= 13; ++index)
        check(destination.get("main", testKey(index)) == std::optional<Bytes>(payload(index)),
              "a pushed value is missing");
    check(!destination.get("main", testKey(14)), "a partial push sent newer history");

    // Pushing HEAD sends the rest; upTo == HEAD is the same as no upTo.
    const Transfer rest = transfer(source, destination, temporary.path(), "push-rest", "main",
                                   revisions[12], 1U << 30U, {}, revisions.back());
    check(rest.chunks.size() == 1 && rest.chunks.front().complete &&
          destination.head("main") == source.head("main"), "pushing HEAD did not send the rest");
}

void testPushRefusesRevisionsOffTheBranch() {
    TemporaryRoot temporary;
    ProjectRepository source(temporary.path() / "source");
    source.createBranch("main");
    const std::vector<RevisionId> trunk = linearHistory(source, "main", 1, 6);
    source.createBranch("feature", "main");
    const RevisionId fork = source.head("feature");
    const std::vector<RevisionId> feature = linearHistory(source, "feature", 300, 4);
    (void)linearHistory(source, "main", 50, 2);
    BundleFile file(temporary.path() / "off-branch.bundle");
    // Another branch's revision, and history below a fork, are not this branch's.
    rejects(StorageErrorKind::InvalidInput, [&] {
        (void)source.exportBundleChunk("main", std::nullopt, 1U << 30U, file.fd(), {}, feature[1]);
    }, "a push of another branch's revision");
    rejects(StorageErrorKind::InvalidInput, [&] {
        (void)source.exportBundleChunk("feature", std::nullopt, 1U << 30U, file.fd(), {}, trunk[1]);
    }, "a push below a forked branch's fork revision");
    check(file.size() == 0, "a refused push left bytes in the descriptor");
    // The fork revision itself still belongs to the forked branch.
    const RevisionBundleChunk atFork =
        source.exportBundleChunk("feature", std::nullopt, 1U << 30U, file.fd(), {}, fork);
    check(atFork.complete && atFork.head == fork, "a push of the fork revision was refused");
}

void testPushRefusesARecipientAheadOrApart() {
    TemporaryRoot temporary;
    ProjectRepository source(temporary.path() / "source");
    ProjectRepository destination(temporary.path() / "destination");
    source.createBranch("main");
    const std::vector<RevisionId> revisions = linearHistory(source, "main", 1, 10);
    (void)transfer(source, destination, temporary.path(), "ahead", "main", std::nullopt, 1U << 30U);
    // The recipient already has newer history than the pushed revision.
    BundleFile behind(temporary.path() / "behind.bundle");
    rejects(StorageErrorKind::Conflict, [&] {
        (void)source.exportBundleChunk("main", revisions.back(), 1U << 30U, behind.fd(), {}, revisions[3]);
    }, "a push older than the recipient");
    check(behind.size() == 0, "a refused push left bytes in the descriptor");
    // The recipient committed on its own: its head is unknown here.
    const RevisionId own = destination.commit("main", {Change{testKey(900), payload(900)}});
    BundleFile apart(temporary.path() / "apart.bundle");
    rejects(StorageErrorKind::NotFound, [&] {
        (void)source.exportBundleChunk("main", own, 1U << 30U, apart.fd(), {}, revisions.back());
    }, "a push onto history the sender does not have");
}

void testUnbornAndMissingBranches() {
    TemporaryRoot temporary;
    ProjectRepository source(temporary.path() / "source");
    source.createBranch("main");
    BundleFile file(temporary.path() / "unborn.bundle");
    rejects(StorageErrorKind::Unavailable, [&] {
        (void)source.exportBundleChunk("main", std::nullopt, 1U << 20U, file.fd());
    }, "an unborn branch");
    rejects(StorageErrorKind::NotFound, [&] {
        (void)source.exportBundleChunk("missing", std::nullopt, 1U << 20U, file.fd());
    }, "a missing branch");
}
}

int main() {
    try {
        testWholeBranchFitsInOneChunk();
        testLinearHistorySplitsIntoNewestFittingChunks();
        testLimitTooSmallForOneRevision();
        testConfiguredLimitsAlsoBoundChunks();
        testIncrementalContinuationAfterExistingBase();
        testMergedHistoryCarriesSecondParents();
        testForkedBranchNeverPrecedesItsForkRevision();
        testBaseOffTheFirstParentChainOnlyOffersHead();
        testUnbornAndMissingBranches();
        testPushUpToARevisionStopsThere();
        testPushRefusesRevisionsOffTheBranch();
        testPushRefusesARecipientAheadOrApart();
    } catch (const std::exception& error) {
        std::cerr << "native bundle chunk tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "native bundle chunk tests passed\n";
    return 0;
}
