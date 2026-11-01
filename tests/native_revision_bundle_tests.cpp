#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/native_store.h"
#include "aowis/dvmdb/storage_error.h"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using aowis::dvmdb::test::commitMetadata;

namespace {
namespace fs = std::filesystem;
using namespace aowis::dvmdb;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void expectError(Function execute, StorageErrorKind kind, const char* message) {
    try { execute(); }
    catch (const StorageError& error) {
        if (error.kind() == kind) return;
        throw std::runtime_error(std::string(message) + ": wrong error category: " + error.what());
    }
    throw std::runtime_error(message);
}

struct Temporary {
    fs::path path = fs::temp_directory_path() / ("aowis-v2-sync-" + UuidV7::generate().toString());
    Temporary() { fs::create_directories(this->path); }
    ~Temporary() { std::error_code ec; fs::remove_all(this->path, ec); }
    fs::path bundle() const { return this->path / (UuidV7::generate().toString() + ".bundle"); }
};

struct File {
    int fd = -1;
    File(const fs::path& path, int flags)
        : fd(::open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW, 0600)) {
        if (this->fd < 0) throw std::runtime_error("unable to open test bundle");
    }
    ~File() { if (this->fd >= 0) (void)::close(this->fd); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};

Key key(unsigned suffix) {
    Key value{};
    value[15] = static_cast<std::uint8_t>(suffix);
    return value;
}

void checkGraph(Repository& repo, const ProjectId& project, std::size_t expectedCount,
                std::size_t expectedBranches = 1) {
    const RevisionGraphPage graph = repo.revisionGraph(project, 100);
    check(!graph.next && graph.revisions.size() == expectedCount, "transfer graph is incomplete");
    check(graph.heads.size() == expectedBranches, "imported branch count is wrong");
}

void run() {
    Temporary files;
    const ProjectId project = ProjectId::generate();
    Repository source(files.path / "source");
    const RevisionId root = source.createRootBranch(project, "main", commitMetadata());
    const RevisionId base = source.commit(project, "main", {{key(1), Bytes{10}}}, commitMetadata(), root);
    check(source.createBranch(project, "feature", "main") == base, "branch must share the same revision");
    const RevisionId main = source.commit(project, "main", {{key(2), Bytes{20}}}, commitMetadata(), base);
    const RevisionId feature = source.commit(project, "feature", {{key(3), Bytes{30}}}, commitMetadata(), base);
    const MergeResult merge = source.mergeBranches(project, "main", "feature", commitMetadata(), main, feature);
    check(merge.status == MergeStatus::Merged && merge.resultHead, "missing two-parent merge");
    const RevisionId merged = *merge.resultHead;
    const std::optional<BranchId> branchId = source.branchId("main");
    check(branchId.has_value(), "source branch UUID missing");

    const fs::path path = files.bundle();
    {
        File transfer(path, O_RDWR | O_CREAT | O_EXCL);
        source.exportRevisionBundle(project, "main", transfer.fd);
    }
    Repository target(files.path / "target");
    {
        File transfer(path, O_RDONLY);
        const RevisionBundleImport result = target.importRevisionBundle(project, transfer.fd, "main");
        check(result.status == BundleImportStatus::Created && result.head == merged &&
              result.objectsImported > 0, "initial full transfer not published");
    }
    check(target.head(project, "main") == merged && target.branchId("main") == branchId,
          "revision or branch identity changed on replica");
    checkGraph(target, project, 5);
    {
        File transfer(path, O_RDONLY);
        const RevisionBundleImport replay = target.importRevisionBundle(project, transfer.fd, "main");
        check(replay.status == BundleImportStatus::UpToDate && replay.objectsImported == 0,
              "idempotent replay is not a no-op");
    }
    {
        Repository reopened(files.path / "target");
        checkGraph(reopened, project, 5);
        check(reopened.verifyRepository().revisions >= 5, "imported repository did not verify");
    }

    (void)target.compact();
    check(target.head(project, "main") == merged && target.branchId("main") == branchId,
          "compaction changed imported branch UUID or HEAD");

    const RevisionId advanced = source.commit(project, "main", {{key(4), Bytes{40}}}, commitMetadata(), merged);
    const fs::path later = files.bundle();
    {
        File transfer(later, O_RDWR | O_CREAT | O_EXCL);
        source.exportRevisionBundle(project, "main", transfer.fd);
        File read(later, O_RDONLY);
        const RevisionBundleImport result = target.importRevisionBundle(project, read.fd, "main");
        check(result.status == BundleImportStatus::FastForward && result.head == advanced,
              "transfer did not fast-forward HEAD");
    }
    checkGraph(target, project, 6);
    // Another branch of the same project transports a distinct stable branch
    // UUID while sharing already-received ancestry through the merge.
    const fs::path featureBundle = files.bundle();
    {
        File file(featureBundle, O_RDWR | O_CREAT | O_EXCL);
        source.exportRevisionBundle(project, "feature", file.fd);
        File read(featureBundle, O_RDONLY);
        const RevisionBundleImport result = target.importRevisionBundle(project, read.fd, "feature");
        check(result.status == BundleImportStatus::Created && result.head == feature,
              "feature identity was not imported");
    }
    check(target.head(project, "feature") == feature &&
          target.branchId("feature") == source.branchId("feature") &&
          target.branchId("feature") != branchId,
          "second branch UUID or shared ancestry was corrupted");
    checkGraph(target, project, 6, 2);

    // AOWREV22 carries only the objects missing from a pinned recipient
    // base, not another copy of the entire merge ancestry. Same UUID and
    // parent hashes must survive an incremental round trip and replay.
    const RevisionId deltaHead = source.commit(project, "main", {{key(5), Bytes{50}}}, commitMetadata(), advanced);
    const fs::path deltaPath = files.bundle();
    {
        File delta(deltaPath, O_RDWR | O_CREAT | O_EXCL);
        source.exportIncrementalRevisionBundle(project, "main", advanced, delta.fd);
        std::array<char, 8> magic{};
        check(::pread(delta.fd, magic.data(), magic.size(), 0) == 8 &&
              std::string(magic.data(), magic.size()) == "AOWREV22",
              "incremental bundle header was not emitted");
        check(fs::file_size(deltaPath) < fs::file_size(later),
              "incremental bundle did not omit old immutable ancestry");
        File incoming(deltaPath, O_RDONLY);
        const RevisionBundleImport imported =
            target.importRevisionBundle(project, incoming.fd, "main");
        check(imported.status == BundleImportStatus::FastForward &&
              imported.head == deltaHead && imported.objectsImported > 0,
              "incremental transfer failed to fast-forward the branch");
        check(target.importRevisionBundle(project, incoming.fd, "main").status ==
              BundleImportStatus::UpToDate, "incremental replay moved the branch");
    }
    check(target.head(project, "main") == deltaHead &&
          target.branchId("main") == branchId,
          "incremental import changed the branch's UUID");
    checkGraph(target, project, 7, 2);
    {
        const fs::path emptyDelta = files.bundle();
        File output(emptyDelta, O_RDWR | O_CREAT | O_EXCL);
        source.exportIncrementalRevisionBundle(project, "main", deltaHead, output.fd);
        check(fs::file_size(emptyDelta) == 144,
              "same-revision incremental replay included redundant objects");
        File input(emptyDelta, O_RDONLY);
        check(target.importRevisionBundle(project, input.fd, "main").status ==
              BundleImportStatus::UpToDate,
              "zero-object same-HEAD incremental replay failed");
    }
    // No source revision mutation or HEAD publication is allowed if the
    // recipient is not at the exact base recorded in the delta.
    {
        const RevisionId future = source.commit(project, "main", {{key(6), Bytes{60}}}, commitMetadata(), deltaHead);
        check(future != deltaHead, "stale-delta test did not advance source");
        const fs::path stalePath = files.bundle();
        File stale(stalePath, O_RDWR | O_CREAT | O_EXCL);
        source.exportIncrementalRevisionBundle(project, "main", merged, stale.fd);
        File input(stalePath, O_RDONLY);
        expectError([&] { (void)target.importRevisionBundle(project, input.fd, "main"); },
                    StorageErrorKind::Conflict, "stale incremental base accepted");
        check(target.head(project, "main") == deltaHead,
              "rejected incremental bundle mutated branch HEAD");
    }
    expectError([&] {
        File output(files.bundle(), O_RDWR | O_CREAT | O_EXCL);
        source.exportIncrementalRevisionBundle(project, "main", RevisionId::generate(), output.fd);
    }, StorageErrorKind::NotFound, "unknown incremental base exported");
    {
        const fs::path tampered = files.bundle();
        fs::copy_file(deltaPath, tampered);
        File output(tampered, O_RDWR);
        std::uint8_t value = 0;
        check(::pread(output.fd, &value, 1, 144 + 40) == 1,
              "failed to read incremental corruption byte");
        value ^= 0x40;
        check(::pwrite(output.fd, &value, 1, 144 + 40) == 1,
              "failed to write incremental corruption byte");
        File input(tampered, O_RDONLY);
        expectError([&] { (void)target.importRevisionBundle(project, input.fd, "main"); },
                    StorageErrorKind::InvalidInput, "tampered incremental payload accepted");
    }
    // An incremental merge can contain a whole new side of the DAG, not
    // merely the new first-parent chain. Neither side gets a cloned UUID.
    const RevisionId side = source.commit(project, "feature", {{key(8), Bytes{80}}}, commitMetadata(), feature);
    const MergeResult sideMerge = source.mergeBranches(
        project, "main", "feature", commitMetadata(), source.head(project, "main"), side);
    check(sideMerge.status == MergeStatus::Merged && sideMerge.resultHead,
          "incremental merge fixture was not divergent");
    const fs::path mergeDelta = files.bundle();
    {
        File output(mergeDelta, O_RDWR | O_CREAT | O_EXCL);
        source.exportIncrementalRevisionBundle(project, "main", deltaHead, output.fd);
        File input(mergeDelta, O_RDONLY);
        const RevisionBundleImport result =
            target.importRevisionBundle(project, input.fd, "main");
        check(result.status == BundleImportStatus::FastForward &&
              result.head == *sideMerge.resultHead && result.objectsImported > 0,
              "incremental two-parent merge was not replicated");
    }
    checkGraph(target, project, 10, 2);
    check(target.head(project, "feature") == feature &&
          target.head(project, "main") == *sideMerge.resultHead,
          "merge delta incorrectly advanced source branch");
    {
        File transfer(path, O_RDONLY);
        check(target.importRevisionBundle(project, transfer.fd, "main").status == BundleImportStatus::UpToDate,
              "older bundle should not rewind the target");
    }
    // A new branch, with the old shared root, receives a different UUID; it
    // cannot overwrite the imported main identity simply because the HEADs share ancestry.
    {
        File transfer(path, O_RDONLY);
        expectError([&] { (void)target.importRevisionBundle(project, transfer.fd, "renamed"); },
                    StorageErrorKind::Conflict, "one branch UUID imported under two names");
    }
    const fs::path corrupt = files.bundle();
    fs::copy_file(later, corrupt);
    {
        File file(corrupt, O_RDWR);
        std::uint8_t changed = 0;
        check(::pread(file.fd, &changed, 1, 96 + 40) == 1, "cannot read corruption test byte");
        changed ^= 0xff;
        check(::pwrite(file.fd, &changed, 1, 96 + 40) == 1, "cannot write corruption test byte");
    }
    {
        Repository clean(files.path / "clean");
        File transfer(corrupt, O_RDONLY);
        expectError([&] { (void)clean.importRevisionBundle(project, transfer.fd, "main"); },
                    StorageErrorKind::InvalidInput, "corrupted bundle accepted");
        check(!clean.hasBranch("main"), "corrupt transfer published branch HEAD");
    }
    {
        Repository untouched(files.path / "wrong-expected");
        File transfer(path, O_RDONLY);
        expectError([&] {
            (void)untouched.importRevisionBundle(ProjectId::generate(), transfer.fd, "main");
        }, StorageErrorKind::Conflict, "unexpected project accepted into fresh repository");
        check(!untouched.hasBranch("main") &&
              !fs::exists(files.path / "wrong-expected" / "revision-v2"),
              "project mismatch pinned an incorrect target identity");
    }
    {
        Repository wrong(files.path / "wrong");
        (void)wrong.createRootBranch(ProjectId::generate(), "other", commitMetadata());
        File transfer(path, O_RDONLY);
        expectError([&] { (void)wrong.importRevisionBundle(project, transfer.fd, "main"); },
                    StorageErrorKind::Conflict, "cross-project transfer was accepted");
    }
    // Full bundles also contain model-indexed state and reverse-reference
    // trees, not just raw byte revisions. Exercise complete semantic transfer.
    {
        const ProjectId modelProject = ProjectId::generate();
        Repository modelSource(files.path / "model-source");
        const RevisionId initial = modelSource.createRootBranch(modelProject, "main", commitMetadata());
        ModelRecord firstModel;
        firstModel.kind = 31;
        firstModel.payload = {7, 8};
        const RevisionId modelHead = modelSource.commitModel(
            modelProject, "main", {{key(7), firstModel}}, commitMetadata(), initial);
        const fs::path modelBundle = files.bundle();
        File output(modelBundle, O_RDWR | O_CREAT | O_EXCL);
        modelSource.exportRevisionBundle(modelProject, "main", output.fd);
        Repository modelTarget(files.path / "model-target");
        File input(modelBundle, O_RDONLY);
        check(modelTarget.importRevisionBundle(modelProject, input.fd, "main").head == modelHead,
              "model-indexed bundle HEAD mismatch");
        check(modelTarget.getModelAtRevision(modelProject, modelHead, key(7))->payload == Bytes({7, 8}),
              "model payload was lost in replication");
        check(modelTarget.verifyRepository().modelSnapshots > 0,
              "model/reverse-reference integrity failed after import");
    }
    {
        File transfer(later, O_RDONLY);
        expectError([&] {
            (void)target.importRevisionBundle(project, transfer.fd, "main", BundleLimits{256, 100, 0});
        }, StorageErrorKind::LimitExceeded, "import byte limit not enforced");
    }
    {
        File transfer(files.bundle(), O_RDWR | O_CREAT | O_EXCL);
        // A configured small output limit must reject before exposing any bytes.
        expectError([&] {
            source.exportRevisionBundle(project, "main", transfer.fd, BundleLimits{256, 100, 0});
        }, StorageErrorKind::LimitExceeded, "export byte limit not enforced");
    }
}

void runForkProvenanceBundle() {
    Temporary files;
    const ProjectId project = ProjectId::generate();
    Repository source(files.path / "source");
    const RevisionId root = source.createRootBranch(project, "main", commitMetadata());
    const RevisionId historical = source.commit(project, "main", {{key(1), Bytes{1}}}, commitMetadata(), root);
    const BranchId origin = *source.branchId("main");
    const RevisionId newer = source.commit(project, "main", {{key(2), Bytes{2}}}, commitMetadata(), historical);
    const BranchCreationResult fork = source.createBranchAtRevisionWithIdentity(
        project, "Prüfung Süd", "main", historical);
    check(fork.originBranchId == origin && fork.forkRevisionId == historical,
          "source historical fork provenance was not recorded");
    const RevisionId advanced = source.commit(
        project, "Prüfung Süd", {{key(3), Bytes{3}}}, commitMetadata(), historical);
    check(advanced != newer && advanced != historical, "fork did not advance independently");

    const fs::path full = files.bundle();
    {
        File out(full, O_RDWR | O_CREAT | O_EXCL);
        source.exportRevisionBundle(project, "Prüfung Süd", out.fd);
        std::array<char, 8> magic{};
        check(::pread(out.fd, magic.data(), magic.size(), 0) == 8 &&
              std::string(magic.data(), magic.size()) == "AOWREV23",
              "full historical fork bundle has no provenance-aware format");
    }
    Repository recipient(files.path / "recipient");
    {
        File in(full, O_RDONLY);
        check(recipient.importRevisionBundle(project, in.fd, "Prüfung Süd").head == advanced,
              "historical fork import did not publish exact HEAD");
    }
    const auto checkProvenance = [&](Repository& repository, const RevisionId& expectedHead) {
        const RevisionGraphPage graph = repository.revisionGraph(project, 50);
        check(graph.heads.size() == 1 && graph.heads[0].branchId == fork.branchId &&
              graph.heads[0].revision == expectedHead &&
              graph.heads[0].originBranchId == origin &&
              graph.heads[0].forkRevisionId == historical,
              "historical fork origin/point changed during bundle round-trip");
    };
    checkProvenance(recipient, advanced);
    {
        Repository reopened(files.path / "recipient");
        checkProvenance(reopened, advanced);
    }
    {
        File in(full, O_RDONLY);
        check(recipient.importRevisionBundle(project, in.fd, "Prüfung Süd").status ==
              BundleImportStatus::UpToDate, "historical fork replay was not idempotent");
        checkProvenance(recipient, advanced);
    }
    const RevisionId later = source.commit(
        project, "Prüfung Süd", {{key(4), Bytes{4}}}, commitMetadata(), advanced);
    const fs::path delta = files.bundle();
    {
        File out(delta, O_RDWR | O_CREAT | O_EXCL);
        source.exportIncrementalRevisionBundle(project, "Prüfung Süd", advanced, out.fd);
        std::array<char, 8> magic{};
        check(::pread(out.fd, magic.data(), magic.size(), 0) == 8 &&
              std::string(magic.data(), magic.size()) == "AOWREV24",
              "incremental historical fork bundle has no provenance-aware format");
        File in(delta, O_RDONLY);
        check(recipient.importRevisionBundle(project, in.fd, "Prüfung Süd").head == later,
              "historical fork incremental bundle did not advance HEAD");
    }
    checkProvenance(recipient, later);
    {
        Repository reopened(files.path / "recipient");
        checkProvenance(reopened, later);
    }
    {
        const fs::path emptyDelta = files.bundle();
        File out(emptyDelta, O_RDWR | O_CREAT | O_EXCL);
        source.exportIncrementalRevisionBundle(project, "Prüfung Süd", later, out.fd);
        check(fs::file_size(emptyDelta) == 176,
              "same-HEAD provenance-aware delta contained unexpected payload");
        File in(emptyDelta, O_RDONLY);
        check(recipient.importRevisionBundle(project, in.fd, "Prüfung Süd").status ==
              BundleImportStatus::UpToDate,
              "same-HEAD provenance-aware delta did not replay cleanly");
        checkProvenance(recipient, later);
    }
    // The transport must not accept a fork UUID that isn't in the true
    // ancestry, and rejection must not publish a branch or new provenance.
    const fs::path forged = files.bundle();
    fs::copy_file(full, forged);
    {
        File out(forged, O_RDWR);
        const RevisionId unrelated = RevisionId::generate();
        check(::pwrite(out.fd, unrelated.uuid().bytes.data(), 16, 112) == 16,
              "unable to forge the bundled fork UUID");
    }
    {
        Repository clean(files.path / "forged-target");
        File in(forged, O_RDONLY);
        expectError([&] { (void)clean.importRevisionBundle(project, in.fd, "Prüfung Süd"); },
                    StorageErrorKind::InvalidInput,
                    "bundle with a non-ancestor fork revision was accepted");
        check(!clean.hasBranch("Prüfung Süd"),
              "invalid fork provenance published a branch");
    }
    // An already imported branch must not accept a different immutable
    // origin, even if all revision objects and the advertised HEAD match.
    const fs::path changedOrigin = files.bundle();
    fs::copy_file(full, changedOrigin);
    {
        File out(changedOrigin, O_RDWR);
        const BranchId unrelatedOrigin = BranchId::generate();
        check(::pwrite(out.fd, unrelatedOrigin.uuid().bytes.data(), 16, 96) == 16,
              "unable to change bundled source branch identity");
    }
    {
        File in(changedOrigin, O_RDONLY);
        expectError([&] { (void)recipient.importRevisionBundle(project, in.fd, "Prüfung Süd"); },
                    StorageErrorKind::Conflict,
                    "replayed bundle replaced immutable origin branch identity");
        checkProvenance(recipient, later);
    }
    // The historical AOWREV21/AOWREV22 formats are still readable, and
    // an old-format bundle cannot guess provenance absent from its header.
    const fs::path legacy = files.bundle();
    {
        File out(legacy, O_RDWR | O_CREAT | O_EXCL);
        source.exportRevisionBundle(project, "main", out.fd);
        std::array<char, 8> magic{};
        check(::pread(out.fd, magic.data(), magic.size(), 0) == 8 &&
              std::string(magic.data(), magic.size()) == "AOWREV21",
              "legacy provenance-free bundle changed format");
        Repository clean(files.path / "legacy-target");
        File in(legacy, O_RDONLY);
        check(clean.importRevisionBundle(project, in.fd, "main").head == newer,
              "legacy branch bundle failed to import");
        const RevisionGraphPage graph = clean.revisionGraph(project, 50);
        check(graph.heads.size() == 1 && !graph.heads[0].originBranchId &&
              !graph.heads[0].forkRevisionId,
              "legacy branch import fabricated fork metadata");
    }
}

} // namespace

int main() {
    try {
        run();
        runForkProvenanceBundle();
        std::cout << "UUIDv7 full-revision bundle replication passed\n";
    } catch (const std::exception& error) {
        std::cerr << "UUIDv7 full-revision bundle replication FAILED: " << error.what() << '\n';
        return 1;
    }
}
