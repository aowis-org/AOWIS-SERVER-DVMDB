#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/revision_record.h"

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
template <typename Callback>
void rejects(Callback callback, StorageErrorKind kind, const char* message) {
    try { callback(); }
    catch (const StorageError& error) {
        if (error.kind() == kind) return;
        throw std::runtime_error(std::string(message) + ": unexpected storage error");
    }
    throw std::runtime_error(message);
}
struct Temporary {
    fs::path path = fs::temp_directory_path() / ("aowis-v2-merge-" + UuidV7::generate().toString());
    ~Temporary() { std::error_code ignored; fs::remove_all(this->path, ignored); }
};
Key key(std::uint8_t value) {
    Key result{};
    result[15] = value;
    return result;
}
ModelRecord model(std::uint8_t value, std::vector<Key> references = {}) {
    ModelRecord result;
    result.kind = 31;
    result.payload = {value};
    result.references = std::move(references);
    return result;
}
void testRawAndHistoricalFork() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    RevisionId root = RevisionId::generate();
    RevisionId first = RevisionId::generate();
    RevisionId feature = RevisionId::generate();
    RevisionId merged = RevisionId::generate();
    BranchId mainBranch = BranchId::generate();
    BranchId featureBranch = BranchId::generate();
    {
        Repository repository(temp.path);
        root = repository.createRootBranch(project, "main", commitMetadata());
        mainBranch = *repository.branchId("main");
        first = repository.commit(project, "main", {{key(1), Bytes{11}}}, commitMetadata(), root);
        check(repository.createBranchAtRevision(project, "feature", "main", root) == root,
              "historical fork changed source revision");
        featureBranch = *repository.branchId("feature");
        check(mainBranch != featureBranch, "fork reused a branch ID");
        rejects([&] { (void)repository.createBranchAtRevision(
                       project, "invalid", "feature", first); },
                StorageErrorKind::InvalidInput, "unreachable fork accepted");
        check(!repository.hasBranch("invalid"), "rejected fork still published");
        feature = repository.commit(project, "feature", {{key(2), Bytes{22}}}, commitMetadata(), root);
        check(repository.revisionReachableFrom(project, feature, root),
              "ancestry cannot find root");
        check(!repository.revisionReachableFrom(project, feature, first),
              "unrelated history incorrectly reachable");
        rejects([&] { (void)repository.mergeBranches(project, "main", "feature", commitMetadata(), root, feature); },
                StorageErrorKind::Conflict, "stale target merge accepted");
        const MergeResult result = repository.mergeBranches(project, "main", "feature", commitMetadata(), first, feature);
        check(result.status == MergeStatus::Merged && result.resultHead &&
              repository.head(project, "main") == *result.resultHead &&
              repository.head(project, "feature") == feature,
              "divergent merge did not publish target only");
        merged = *result.resultHead;
        const RevisionRecord record = decodeRevisionRecord(*repository.readRevision(project, merged));
        check(record.parents.size() == 2 && record.parents[0].id == first &&
              record.parents[1].id == feature, "two parent UUID ancestry lost");
        check(repository.revisionReachableFrom(project, merged, feature),
              "second parent ancestry not traversed");
        check(repository.mergeBranches(project, "main", "feature", commitMetadata()).status == MergeStatus::UpToDate,
              "remerging incorporated history changed HEAD");
        check(repository.createBranchAtRevision(project, "historic", "main", feature) == feature,
              "historical fork from second merge parent failed");
        const RevisionId historical = repository.head(project, "historic");
        check(historical == feature, "historic branch head incorrect");
    }
    {
        Repository repository(temp.path);
        check(repository.head(project, "main") == merged &&
              repository.head(project, "feature") == feature &&
              *repository.branchId("main") == mainBranch &&
              *repository.branchId("feature") == featureBranch,
              "reopened merge lost HEAD or identity");
    }
}
void testFastForwardAndConflicts() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    Repository repo(temp.path);
    const RevisionId root = repo.createRootBranch(project, "main", commitMetadata());
    check(repo.createBranch(project, "feature", "main") == root, "shared head not created");
    const RevisionId f1 = repo.commit(project, "feature", {{key(1), Bytes{1}}}, commitMetadata(), root);
    const MergeResult fast = repo.mergeBranches(project, "main", "feature", commitMetadata(), root, f1);
    check(fast.status == MergeStatus::FastForward && fast.resultHead == f1 &&
          repo.head(project, "main") == f1, "fast-forward did not advance target");
    check(repo.mergeBranches(project, "feature", "main", commitMetadata()).status == MergeStatus::UpToDate,
          "source-containing-target should be up to date");
    const RevisionId main2 = repo.commit(project, "main", {{key(2), Bytes{5}}}, commitMetadata(), f1);
    const RevisionId feature2 = repo.commit(project, "feature", {{key(2), Bytes{6}}}, commitMetadata(), f1);
    const MergeResult conflict = repo.mergeBranches(project, "main", "feature", commitMetadata(), main2, feature2);
    check(conflict.status == MergeStatus::Conflicts && conflict.conflicts.size() == 1 &&
          repo.head(project, "main") == main2 &&
          repo.head(project, "feature") == feature2,
          "concurrent edits were not rejected atomically");
}
void testModelMergeAndInvalidReferences() {
    Temporary temp;
    ProjectService service(temp.path);
    const ProjectInfo project = service.createProject("model-merge", commitMetadata());
    const RevisionId root = service.head(project.id, "main");
    const RevisionId base = service.commitModel(project.id, "main", {{key(1), model(1)}}, commitMetadata(), root);
    check(service.createBranch(project.id, "feature", "main") == base,
          "model fork did not share base");
    const RevisionId main = service.commitModel(project.id, "main", {{key(2), model(2, {key(1)})}}, commitMetadata(), base);
    const RevisionId feature = service.commitModel(project.id, "feature", {{key(3), model(3)}}, commitMetadata(), base);
    const MergeResult merge = service.mergeBranches(project.id, "main", "feature", commitMetadata(), main, feature);
    check(merge.status == MergeStatus::Merged && merge.resultHead.has_value(),
          "model merge did not publish a two-parent revision");
    check(service.head(project.id, "feature") == feature, "model merge rewound source");
    check(service.getModelAtRevision(project.id, *merge.resultHead, key(1)).has_value() &&
          service.getModelAtRevision(project.id, *merge.resultHead, key(2)).has_value() &&
          service.getModelAtRevision(project.id, *merge.resultHead, key(3)).has_value(),
          "model merge lost disjoint records");
    const RevisionId current = *merge.resultHead;
    service.withRepository(project.id, [&](Repository& repo) {
        const VerificationReport report = repo.verifyRepository();
        check(report.branches == 2 && report.revisions == 5 &&
              report.modelSnapshots >= 3,
              "verifier missed model-indexed merge ancestry");
        const CompactionStats stats = repo.compact();
        check(stats.objectsAfter <= stats.objectsBefore,
              "compaction grew the immutable pack object count");
        check(repo.verifyRepository().revisions == 5,
              "compacted model merge failed integrity verification");
    });
    check(service.getModelAtRevision(project.id, current, key(2)).has_value(),
          "compaction lost model-indexed merge content");
    check(service.createBranchAtRevision(project.id, "deleter", "main", base) == base,
          "service historical branch creation failed");
    const RevisionId removed = service.commitModel(project.id, "deleter", {{key(1), std::nullopt}}, commitMetadata(), base);
    const MergeResult conflict = service.mergeBranches(project.id, "main", "deleter", commitMetadata(), current, removed);
    check(conflict.status == MergeStatus::Conflicts && !conflict.conflicts.empty() &&
          service.head(project.id, "main") == current &&
          service.head(project.id, "deleter") == removed,
          "merge with dangling reference did not preserve HEADs");
}
void testTypedConflictResolutionAndValidation() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    Repository repository(temp.path);
    const RevisionId root = repository.createRootBranch(project, "main", commitMetadata());
    const RevisionId base = repository.commitModel(project, "main", {{key(1), model(1)}}, commitMetadata(), root);
    check(repository.createBranch(project, "feature", "main") == base,
          "typed test failed to share its base revision");
    const RevisionId target = repository.commitModel(project, "main", {{key(1), model(2)}}, commitMetadata(), base);
    const RevisionId source = repository.commitModel(project, "feature", {{key(1), model(3)}}, commitMetadata(), base);
    const std::uint64_t bytes = repository.packSize();
    const MergeResult rejected = repository.mergeBranches(project, "main", "feature", commitMetadata(), target, source);
    check(rejected.status == MergeStatus::Conflicts && rejected.conflicts.size() == 1 &&
          repository.packSize() == bytes, "conflict wrote objects before resolution");

    MergeOptions choices;
    choices.resolve = [](const Key& changed, const ModelRecord& original,
                         const ModelRecord& left, const ModelRecord& right) {
        check(changed == key(1) && original.payload == Bytes{1} &&
              left.payload == Bytes{2} && right.payload == Bytes{3},
              "resolver observed wrong immutable revisions");
        return MergeResolution{model(4), {}};
    };
    choices.checkSelections = []() -> std::optional<std::string> { return std::nullopt; };
    choices.validate = [](const MergeCandidate& candidate) -> std::optional<std::string> {
        const std::optional<ModelRecord> merged = candidate.get(key(1));
        if (!merged || merged->payload != Bytes{4}) return "bad resolved candidate";
        std::size_t counted = 0;
        candidate.forEach([&counted](const Key&, const ModelRecord&) { ++counted; });
        return counted == 1 ? std::nullopt : std::optional<std::string>("bad merged traversal");
    };
    const MergeResult accepted = repository.mergeBranches(
        project, "main", "feature", commitMetadata(), target, source, choices);
    check(accepted.status == MergeStatus::Merged && accepted.resultHead &&
          *accepted.resultHead != target && repository.head(project, "feature") == source,
          "typed conflict resolution failed");
    const std::optional<ModelRecord> merged = repository.getModelAtRevision(
        project, *accepted.resultHead, key(1));
    check(merged && merged->payload == Bytes{4}, "typed merge lost selected field payload");
    const RevisionRecord revision = decodeRevisionRecord(
        *repository.readRevision(project, *accepted.resultHead));
    check(revision.parents.size() == 2 && revision.parents[0].id == target &&
          revision.parents[1].id == source, "typed merge lost its two UUIDv7 parents");

    check(repository.createBranchAtRevision(project, "reject-left", "main", base) == base,
          "cannot create test branch from common ancestor");
    check(repository.createBranchAtRevision(project, "reject-right", "main", base) == base,
          "cannot create second test branch from common ancestor");
    const RevisionId left = repository.commitModel(project, "reject-left", {{key(1), model(5)}}, commitMetadata(), base);
    const RevisionId right = repository.commitModel(project, "reject-right", {{key(1), model(6)}}, commitMetadata(), base);
    MergeOptions rejectedCandidate;
    rejectedCandidate.resolve = [](const Key&, const ModelRecord&, const ModelRecord&,
                                   const ModelRecord&) { return MergeResolution{model(7), {}}; };
    rejectedCandidate.validate = [](const MergeCandidate&) -> std::optional<std::string> {
        return "domain validation rejected this hydraulic candidate";
    };
    const std::uint64_t sizeBefore = repository.packSize();
    const MergeResult invalid = repository.mergeBranches(
        project, "reject-left", "reject-right", commitMetadata(), left, right, rejectedCandidate);
    check(invalid.status == MergeStatus::Conflicts && !invalid.conflicts.empty() &&
          repository.head(project, "reject-left") == left &&
          repository.head(project, "reject-right") == right &&
          repository.packSize() == sizeBefore,
          "invalid typed candidate modified immutable storage or branch tips");

    MergeOptions unused;
    unused.checkSelections = []() -> std::optional<std::string> { return "unused selection"; };
    rejects([&] { (void)repository.mergeBranches(
        project, "reject-left", "reject-right", commitMetadata(), left, right, unused); },
        StorageErrorKind::InvalidInput, "unused explicit choice was accepted");
    rejects([&] { (void)repository.mergeBranches(
        project, "reject-left", "reject-right", commitMetadata(), base, right, choices); },
        StorageErrorKind::Conflict, "stale selection was accepted");

    check(repository.createBranchAtRevision(project, "ff-target", "main", base) == base,
          "cannot create fast-forward target");
    check(repository.createBranchAtRevision(project, "ff-source", "main", base) == base,
          "cannot create fast-forward source");
    const RevisionId ff = repository.commitModel(project, "ff-source", {{key(1), model(8)}}, commitMetadata(), base);
    const std::uint64_t ffBytes = repository.packSize();
    MergeOptions invalidFastForward;
    invalidFastForward.validate = [](const MergeCandidate& candidate) -> std::optional<std::string> {
        const std::optional<ModelRecord> current = candidate.get(key(1));
        return current && current->payload == Bytes{8} ? std::optional<std::string>("bad hydraulic tip")
                                                     : std::nullopt;
    };
    const MergeResult rejectedFF = repository.mergeBranches(
        project, "ff-target", "ff-source", commitMetadata(), base, ff, invalidFastForward);
    check(rejectedFF.status == MergeStatus::Conflicts &&
          repository.head(project, "ff-target") == base &&
          repository.packSize() == ffBytes,
          "fast forward bypassed semantic validation or wrote pack objects");
    const MergeResult acceptedFF = repository.mergeBranches(project, "ff-target", "ff-source", commitMetadata(), base, ff);
    check(acceptedFF.status == MergeStatus::FastForward &&
          repository.head(project, "ff-target") == ff,
          "safe fast-forward did not reuse the existing UUIDv7 revision");
    // Two branches independently add a record with the *same* UUID. A typed
    // concurrent-addition resolver must be able to resolve it and reconcile
    // the candidate while retaining correct reverse references.
    check(repository.createBranchAtRevision(project, "add-left", "main", base) == base &&
          repository.createBranchAtRevision(project, "add-right", "main", base) == base,
          "cannot create concurrent-addition branches");
    const RevisionId addLeft = repository.commitModel(
        project, "add-left", {{key(4), model(12)}}, commitMetadata(), base);
    const RevisionId addRight = repository.commitModel(
        project, "add-right", {{key(4), model(13)}}, commitMetadata(), base);
    bool resolvedAddition = false;
    bool reconciledCandidate = false;
    MergeOptions addition;
    addition.resolveConcurrentAddition = [&resolvedAddition](
        const Key& record, const ModelRecord& left, const ModelRecord& right) {
        check(record == key(4) && left.payload == Bytes{12} && right.payload == Bytes{13},
              "concurrent addition resolver saw the wrong records");
        resolvedAddition = true;
        return MergeResolution{model(14), {}};
    };
    addition.reconcile = [&reconciledCandidate](
        const MergeCandidate& candidate,
        const std::function<void(const Key&, std::optional<ModelRecord>)>& set,
        const std::function<std::optional<ModelRecord>(const Key&)>& targetSnapshot,
        const std::function<std::optional<ModelRecord>(const Key&)>& sourceSnapshot)
        -> std::optional<std::string> {
        const std::optional<ModelRecord> choice = candidate.get(key(4));
        const std::optional<ModelRecord> originalLeft = targetSnapshot(key(4));
        const std::optional<ModelRecord> originalRight = sourceSnapshot(key(4));
        if (!choice || choice->payload != Bytes{14} ||
            !originalLeft || originalLeft->payload != Bytes{12} ||
            !originalRight || originalRight->payload != Bytes{13})
            return "invalid reconciled concurrent addition";
        set(key(5), model(15, {key(4)}));
        reconciledCandidate = true;
        return std::nullopt;
    };
    addition.validate = [](const MergeCandidate& candidate) -> std::optional<std::string> {
        const std::optional<ModelRecord> dependent = candidate.get(key(5));
        return dependent && dependent->references == std::vector<Key>{key(4)}
               ? std::nullopt : std::optional<std::string>("candidate misses reference repair");
    };
    const MergeResult added = repository.mergeBranches(
        project, "add-left", "add-right", commitMetadata(), addLeft, addRight, addition);
    check(added.status == MergeStatus::Merged && added.resultHead &&
          resolvedAddition && reconciledCandidate &&
          repository.getModelAtRevision(project, *added.resultHead, key(4))->payload == Bytes{14} &&
          repository.referencingModelAtRevision(project, *added.resultHead, key(4)) ==
              std::vector<Key>{key(5)},
          "concurrent addition and candidate reconciliation were not published atomically");

    // A whole-object choice can accept either side of a genuine divergent
    // record. It must not be offered for an unchanged model UUID.
    check(repository.createBranchAtRevision(project, "object-left", "main", base) == base &&
          repository.createBranchAtRevision(project, "object-right", "main", base) == base,
          "cannot create object-choice branches");
    const RevisionId objectLeft = repository.commitModel(
        project, "object-left", {{key(1), model(21)}}, commitMetadata(), base);
    const RevisionId objectRight = repository.commitModel(
        project, "object-right", {{key(1), model(22)}}, commitMetadata(), base);
    MergeOptions objectChoice;
    objectChoice.chooseConflict = [](const MergeConflict& conflict)
        -> std::optional<MergeChoiceSide> {
        return conflict.key == key(1) ? std::optional<MergeChoiceSide>(MergeChoiceSide::Source)
                                      : std::nullopt;
    };
    const MergeResult selected = repository.mergeBranches(
        project, "object-left", "object-right", commitMetadata(), objectLeft, objectRight, objectChoice);
    check(selected.status == MergeStatus::Merged && selected.resultHead &&
          repository.getModelAtRevision(project, *selected.resultHead, key(1))->payload == Bytes{22},
          "whole-object source choice was ignored");

    check(repository.verifyRepository().revisions >= 8,
          "typed merge graph not verifiable");
}
}
int main() {
    try {
        testRawAndHistoricalFork();
        testFastForwardAndConflicts();
        testModelMergeAndInvalidReferences();
        testTypedConflictResolutionAndValidation();
        std::cout << "UUIDv7 merge ancestry, fast-forward, conflict and model validation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UUIDv7 merge regression FAILED: " << error.what() << '\n';
        return 1;
    }
}
