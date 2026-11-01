#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/native_store.h"
#include "aowis/dvmdb/revision_record.h"

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>
#include <iostream>
#include <stdexcept>
#include <string>

using aowis::dvmdb::test::commitMetadata;

namespace {
using namespace aowis::dvmdb;
namespace fs = std::filesystem;

void check(bool ok, const char* context) {
    if (!ok) throw std::runtime_error(context);
}

template<class Callable>
void expected(Callable&& operation, StorageErrorKind kind, const char* context) {
    try { operation(); }
    catch (const StorageError& error) {
        if (error.kind() == kind) return;
        std::cerr << context << ": unexpected error kind " << StorageError::kindName(error.kind()) << '\n';
        throw;
    }
    throw std::runtime_error(context);
}

struct Sandbox {
    fs::path path;
    Sandbox() {
        this->path = fs::temp_directory_path() / ("aowis-repo-v2-" + UuidV7::generate().toString());
        fs::create_directories(this->path);
    }
    ~Sandbox() { std::error_code ec; fs::remove_all(this->path, ec); }
};

void testLivePackStorage() {
    Sandbox tmp;
    const ProjectId project = ProjectId::generate();
    const ProjectId other = ProjectId::generate();
    RevisionId root = RevisionId::generate();
    RevisionId main = RevisionId::generate();
    RevisionId feature = RevisionId::generate();
    RevisionId merged = RevisionId::generate();
    {
        Repository repo(tmp.path);
        check(!repo.readRevision(other, RevisionId::generate()),
              "empty read unexpectedly resolved a revision");
        check(!fs::exists(tmp.path / "revision-v2"),
              "empty read pinned a caller-chosen project UUID");
        root = repo.writeRevision(project, Hash{}, Hash{}, false, commitMetadata());
        main = repo.writeRevision(project, Hash{}, Hash{}, false, commitMetadata(), {root});
        feature = repo.writeRevision(project, Hash{}, Hash{}, false, commitMetadata(), {root});
        merged = repo.writeRevision(project, Hash{}, Hash{}, false, commitMetadata(), {main, feature});
        check(root != main && root != feature && main != feature && feature != merged,
              "equal snapshots were given identical revision identities");
        const std::optional<Bytes> bytes = repo.readRevision(project, merged);
        check(bytes.has_value(), "could not retrieve merge from real content pack");
        const RevisionRecord record = decodeRevisionRecord(*bytes);
        check(record.id == merged && record.project == project && record.parents.size() == 2,
              "wrong project or revision identity for packed merge");
        check(record.parents[0].id == main && record.parents[1].id == feature,
              "merge parent ordering was lost");
        const std::optional<Bytes> first = repo.readRevision(project, main);
        check(first.has_value() && record.parents[0].expectedRecordHash == sha256(*first),
              "merge parent SHA-256 not bound to stored bytes");
        expected([&] { (void)repo.readRevision(other, merged); }, StorageErrorKind::Conflict,
                 "foreign project lookup was not rejected");
        expected([&] { (void)repo.writeRevision(project, Hash{}, Hash{}, false, commitMetadata(),
                                                    {RevisionId::generate()}); },
                 StorageErrorKind::NotFound, "unknown revision parent accepted");
        expected([&] { (void)repo.writeRevision(project, Hash{}, sha256(Bytes{'x'}), false, commitMetadata()); },
                 StorageErrorKind::InvalidInput, "raw record with reference index accepted");
        expected([&] { (void)repo.writeRevision(project, sha256(Bytes{'x'}), Hash{}, false, commitMetadata()); },
                 StorageErrorKind::Corruption, "unpacked content root accepted");
        // Published UUID->SHA bindings are durable even before a branch
        // points to them; the whole repository verifier and compactor must
        // preserve the complete ancestry, including merge second parents.
        repo.verifyPack();
        const VerificationReport report = repo.verifyRepository();
        check(report.revisions == 4 && report.branches == 0 &&
              report.reachableObjects == 4,
              "whole-repository verification missed unanchored UUIDv7 revisions");
        const CompactionStats compacted = repo.compact();
        check(compacted.objectsAfter == 4,
              "compaction did not retain every published revision");
        const std::optional<Bytes> retained = repo.readRevision(project, merged);
        check(retained && decodeRevisionRecord(*retained).parents.size() == 2,
              "merge revision disappeared during compaction");
        check(repo.verifyRepository().revisions == 4,
              "post-compaction repository verification missed ancestry");
    }
    {
        Repository repo(tmp.path);
        const std::optional<Bytes> saved = repo.readRevision(project, merged);
        check(saved.has_value(), "revision not recoverable after repository reopen");
        check(decodeRevisionRecord(*saved).id == merged, "reopened revision changed identity");
        check(!repo.readRevision(project, RevisionId::generate()),
              "unknown revision unexpectedly resolved");
        expected([&] { (void)repo.writeRevision(other, Hash{}, Hash{}, false, commitMetadata()); },
                 StorageErrorKind::Conflict, "repository accepted second project UUID");
    }
    check(fs::exists(tmp.path / "revision-v2" / "ids" / merged.toString()),
          "expected UUID-only identity filename missing");
}

RevisionRecord recordOf(Repository& repo, const ProjectId& project, const RevisionId& id) {
    const std::optional<Bytes> bytes = repo.readRevision(project, id);
    check(bytes.has_value(), "revision record not found");
    return decodeRevisionRecord(*bytes);
}

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

bool sameMetadata(const CommitMetadata& left, const CommitMetadata& right) {
    return left.authorId == right.authorId && left.authorName == right.authorName &&
           left.message == right.message;
}

// Every write path records the caller's author and message and the time the
// revision was published; history, the graph, bundles and reopening keep them.
void testCommitMetadataIsRecorded() {
    Sandbox tmp;
    const ProjectId project = ProjectId::generate();
    const CommitMetadata alice{*UuidV7::parse("018f84c0-0000-7000-8000-00000000a0a1"), "Alice",
                               "First edits\n\nwith a body"};
    const CommitMetadata bob{*UuidV7::parse("018f84c0-0000-7000-8000-00000000a0b2"), "Bob Ü",
                             "Pump settings"};
    const CommitMetadata merger{alice.authorId, alice.authorName, "Merge feature into main"};
    RevisionId root = RevisionId::generate();
    RevisionId first = RevisionId::generate();
    RevisionId modelled = RevisionId::generate();
    RevisionId merged = RevisionId::generate();
    {
        Repository repo(tmp.path / "raw");
        const std::uint64_t before = nowMs();
        root = repo.createRootBranch(project, "main", commitMetadata("root"));
        first = repo.commit(project, "main", {{Key{1}, Bytes{1}}}, alice, root);
        repo.createBranch(project, "feature", "main");
        const RevisionId side = repo.commit(project, "feature", {{Key{2}, Bytes{2}}}, bob, first);
        const RevisionId later = repo.commit(project, "main", {{Key{3}, Bytes{3}}}, alice, first);
        const MergeResult result = repo.mergeBranches(project, "main", "feature", merger,
                                                      later, side);
        check(result.status == MergeStatus::Merged && result.resultHead, "merge failed");
        merged = *result.resultHead;
        const std::uint64_t after = nowMs();

        const RevisionRecord rootRecord = recordOf(repo, project, root);
        check(sameMetadata(rootRecord.metadata, commitMetadata("root")),
              "root revision lost its metadata");
        const RevisionRecord firstRecord = recordOf(repo, project, first);
        check(sameMetadata(firstRecord.metadata, alice) &&
              firstRecord.committedAtMs >= before && firstRecord.committedAtMs <= after,
              "commit did not record its author, message and publication time");
        check(sameMetadata(recordOf(repo, project, side).metadata, bob), "second author lost");
        const RevisionRecord mergeRecord = recordOf(repo, project, merged);
        check(sameMetadata(mergeRecord.metadata, merger) && mergeRecord.parents.size() == 2,
              "merge revision did not record the merge metadata");

        const std::vector<RevisionGraphRevision> history = repo.history(project, "main");
        check(!history.empty() && history.front().id == merged &&
              sameMetadata(history.front().metadata, merger) &&
              history.front().committedAtMs == mergeRecord.committedAtMs,
              "history did not return revision metadata");
        const RevisionGraphPage graph = repo.revisionGraph(project);
        bool foundBob = false;
        for (const RevisionGraphRevision& revision : graph.revisions)
            if (revision.id == side) foundBob = sameMetadata(revision.metadata, bob);
        check(foundBob, "revision graph did not return revision metadata");

        // Invalid metadata is rejected before anything is written.
        const std::uint64_t packBefore = repo.packSize();
        CommitMetadata invalid = alice;
        invalid.message.clear();
        expected([&] { (void)repo.commit(project, "main", {{Key{4}, Bytes{4}}}, invalid); },
                 StorageErrorKind::InvalidInput, "empty commit message accepted");
        invalid = alice;
        invalid.authorName = "bad\nname";
        expected([&] { (void)repo.mergeBranches(project, "main", "feature", invalid); },
                 StorageErrorKind::InvalidInput, "invalid merge author accepted");
        invalid = alice;
        invalid.authorId.bytes[6] = 0x40;  // version 4, not a UUIDv7
        expected([&] { (void)repo.createBranch(project, "unsourced"); },
                 StorageErrorKind::InvalidInput, "a second root without a source was accepted");
        check(repo.packSize() == packBefore && repo.head(project, "main") == merged,
              "a rejected write changed the repository");

        // A model commit records metadata the same way.
        const ProjectId model = ProjectId::generate();
        Repository modelRepo(tmp.path / "model");
        modelRepo.createUnbornBranch(model, "main");
        ModelRecord record;
        record.kind = 7;
        record.payload = {1, 2, 3};
        expected([&] { (void)modelRepo.commitModel(model, "main", {{Key{9}, record}}, invalid); },
                 StorageErrorKind::InvalidInput, "non-UUIDv7 author accepted");
        modelled = modelRepo.commitModel(model, "main", {{Key{9}, record}}, bob, std::nullopt, true);
        check(sameMetadata(recordOf(modelRepo, model, modelled).metadata, bob),
              "model commit lost its metadata");

        // Bundles carry the records unchanged.
        repo.exportRevisionBundle(project, "main", tmp.path / "main.aowbundle");
    }
    {
        Repository reopened(tmp.path / "raw");
        check(sameMetadata(recordOf(reopened, project, first).metadata, alice),
              "reopening lost revision metadata");
        Repository clone(tmp.path / "clone");
        const int fd = ::open((tmp.path / "main.aowbundle").c_str(), O_RDONLY | O_CLOEXEC);
        check(fd >= 0, "cannot open bundle");
        (void)clone.importRevisionBundle(project, fd, "main", BundleLimits{});
        ::close(fd);
        const RevisionRecord original = recordOf(reopened, project, merged);
        const RevisionRecord copied = recordOf(clone, project, merged);
        check(sameMetadata(copied.metadata, merger) &&
              copied.committedAtMs == original.committedAtMs &&
              encodeRevisionRecord(copied) == encodeRevisionRecord(original),
              "bundle import changed revision metadata");
        check(reopened.verifyRepository().revisions == 5 && clone.verifyRepository().revisions == 5,
              "repositories with metadata do not verify");
    }
}

} // namespace

int main() {
    try {
        testLivePackStorage();
        testCommitMetadataIsRecorded();
        std::cout << "Packed UUIDv7 revision storage and recovery passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Packed revision test failed:" << error.what() << '\n';
        return 1;
    }
}
