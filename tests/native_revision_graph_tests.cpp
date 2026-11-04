#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/project_service.h"

#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

using aowis::dvmdb::test::commitMetadata;

namespace {
using namespace aowis::dvmdb;
namespace fs = std::filesystem;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <typename Operation>
void invalid(Operation operation, StorageErrorKind expected, const char* message) {
    try { operation(); }
    catch (const StorageError& error) {
        if (error.kind() == expected) return;
        throw std::runtime_error(std::string(message) + ": unexpected error kind");
    }
    throw std::runtime_error(message);
}
struct Temporary {
    fs::path path = fs::temp_directory_path() / ("aowis-graph-v2-" + UuidV7::generate().toString());
    ~Temporary() { std::error_code error; fs::remove_all(this->path, error); }
};
Key key(unsigned value) {
    Key result{};
    result[15] = static_cast<std::uint8_t>(value);
    return result;
}

void runUnbornGraph() {
    Temporary tmp;
    const ProjectId project = ProjectId::generate();
    Repository repo(tmp.path / "unborn");
    repo.createUnbornBranch(project, "main");
    const RevisionGraphPage graph = repo.revisionGraph(project);
    check(graph.heads.size() == 1 && graph.heads[0].name == "main" &&
          !graph.heads[0].revision && graph.heads[0].recordHash == Hash{} &&
          graph.revisions.empty() && !graph.next,
          "unborn graph must contain a null HEAD and no revisions");
    Repository reopened(tmp.path / "unborn");
    const RevisionGraphPage persisted = reopened.revisionGraph(project);
    check(persisted.heads.size() == 1 && !persisted.heads[0].revision &&
          persisted.revisions.empty(), "unborn graph did not survive reopen");
    RevisionGraphCursor forged{{graph.heads.front()}, 1};
    invalid([&] { (void)repo.revisionGraph(project, 1, forged); },
            StorageErrorKind::InvalidInput, "unborn graph cursor beyond history accepted");
}

void runRepository() {
    Temporary tmp;
    const ProjectId project = ProjectId::generate();
    Repository repo(tmp.path / "repository");
    const RevisionId root = repo.createRootBranch(project, "main", commitMetadata());
    const RevisionId first = repo.commit(project, "main", {{key(1), Bytes{1}}}, commitMetadata(), root);
    check(repo.createBranch(project, "feature", "main") == first, "fork HEAD should be shared");
    check(repo.createBranch(project, "alias", "main") == first, "alias HEAD should be shared");
    const RevisionId main = repo.commit(project, "main", {{key(2), Bytes{2}}}, commitMetadata(), first);
    const RevisionId feature = repo.commit(project, "feature", {{key(3), Bytes{3}}}, commitMetadata(), first);
    const MergeResult merged = repo.mergeBranches(project, "main", "feature", commitMetadata(), main, feature);
    check(merged.status == MergeStatus::Merged && merged.resultHead.has_value(),
          "expected divergent merge");
    const RevisionId merge = *merged.resultHead;
    const RevisionGraphPage start = repo.revisionGraph(project, 1);
    check(start.heads.size() == 3 && start.revisions.size() == 1 && start.next,
          "graph did not return page and snapshot");
    check(start.heads[0].name == "alias" && start.heads[0].revision == first &&
          start.heads[1].name == "feature" && start.heads[1].revision == feature &&
          start.heads[2].name == "main" && start.heads[2].revision == merge,
          "HEAD snapshot incorrect");
    check(start.heads[0].branchId != start.heads[1].branchId &&
          start.heads[1].branchId != start.heads[2].branchId,
          "branch UUIDs were duplicated");
    check(start.heads[0].originBranchId == start.heads[2].branchId &&
          start.heads[1].originBranchId == start.heads[2].branchId &&
          !start.heads[2].originBranchId,
          "creation provenance does not point at main's branch UUID");
    check(start.revisions[0].id == first || start.revisions[0].id == feature ||
          start.revisions[0].id == merge, "unexpected first graph node");
    // After first page, HEAD moves; the old cursor still represents the
    // immutable earlier snapshot, and a new first page sees the descendant.
    const RevisionId newer = repo.commit(project, "main", {{key(4), Bytes{4}}}, commitMetadata(), merge);
    std::set<RevisionId> observed;
    std::optional<RevisionGraphCursor> cursor = start.next;
    for (const RevisionGraphRevision& item : start.revisions) observed.insert(item.id);
    for (unsigned count = 0; cursor; ++count) {
        check(count < 15, "pagination failed to terminate");
        const RevisionGraphPage next = repo.revisionGraph(project, 1, cursor);
        check(next.heads[2].revision == merge && next.revisions.size() == 1,
              "graph mixed changed HEADs into pinned snapshot");
        const RevisionGraphRevision& item = next.revisions.front();
        check(item.recordHash != Hash{} && observed.insert(item.id).second,
              "page returned invalid hash or duplicate UUID");
        cursor = next.next;
    }
    check(observed == std::set<RevisionId>({root, first, main, feature, merge}),
          "paginated DAG missed a revision or included a later commit");
    const RevisionGraphPage fresh = repo.revisionGraph(project, 100);
    check(!fresh.next && fresh.revisions.size() == 6 && fresh.heads[2].revision == newer,
          "fresh graph did not see current HEAD");
    bool foundMerge = false;
    for (const RevisionGraphRevision& item : fresh.revisions) {
        if (item.id == merge) {
            foundMerge = item.parents.size() == 2 && item.parents[0].id == main &&
                         item.parents[1].id == feature && item.recordHash != Hash{};
        }
    }
    check(foundMerge, "two authenticated merge parents missing from graph");
    check(fresh.heads[0].revision == first, "non-leaf alias HEAD changed");
    const std::vector<RevisionGraphRevision> mainHistory = repo.history(project, "main", 100);
    check(mainHistory.size() == 5 && mainHistory[0].id == newer &&
          mainHistory[1].id == merge && mainHistory[2].id == main &&
          mainHistory[3].id == first && mainHistory[4].id == root,
          "history did not follow first parents");
    const std::vector<RevisionGraphRevision> historyPage = repo.history(project, "main", 2, merge);
    check(historyPage.size() == 2 && historyPage[0].id == main &&
          historyPage[1].id == first, "exclusive history cursor incorrect");
    invalid([&] { (void)repo.history(project, "main", 10, feature); },
            StorageErrorKind::InvalidInput, "second-parent history cursor accepted");
    invalid([&] { (void)repo.revisionGraph(project, 0); },
            StorageErrorKind::InvalidInput, "zero page size accepted");
    invalid([&] {
        RevisionGraphCursor forged = *start.next;
        forged.heads[0].recordHash[0] ^= 1;
        (void)repo.revisionGraph(project, 1, forged);
    }, StorageErrorKind::Corruption, "forged HEAD digest accepted");
    invalid([&] {
        RevisionGraphCursor forged = *start.next;
        forged.heads[0].name = "../traversal";
        (void)repo.revisionGraph(project, 1, forged);
    }, StorageErrorKind::InvalidInput, "invalid cursor branch name accepted");
    invalid([&] { (void)repo.revisionGraph(ProjectId::generate(), 100); },
            StorageErrorKind::Conflict, "wrong project UUID accepted");
    {
        RevisionGraphCursor bogus = *start.next;
        bogus.skip = 250000;
        invalid([&] { (void)repo.revisionGraph(project, 2, bogus); },
                StorageErrorKind::InvalidInput, "cursor beyond history accepted");
    }
    Repository reopened(tmp.path / "repository");
    check(reopened.revisionGraph(project, 100).revisions.size() == 6,
          "graph lost revisions after reopening");
}

void runBranchOriginsAndNames() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    const std::string unicodeLabel = "Feature Branch – Süd";
    BranchId mainId = BranchId::generate();
    BranchId featureId = BranchId::generate();
    RevisionId featureRevision = RevisionId::generate();
    {
        Repository repository(temp.path / "repository");
        const RevisionId root = repository.createRootBranch(project, "main", commitMetadata());
        mainId = *repository.branchId("main");
        check(repository.createBranch(project, unicodeLabel, "main") == root,
              "branch names with spaces and UTF-8 were rejected");
        featureId = *repository.branchId(unicodeLabel);
        check(repository.createBranch(project, "Feature Follow Up", unicodeLabel) == root,
              "second-level branch creation failed");
        (void)repository.commit(project, "main", {{key(10), Bytes{10}}}, commitMetadata(), root);
        const RevisionGraphPage initial = repository.revisionGraph(project, 1);
        check(initial.heads.size() == 3 && initial.next.has_value(),
              "branch ancestry fixture did not produce a pinned cursor");
        check(initial.heads[0].originBranchId == mainId &&
              initial.heads[1].originBranchId == featureId &&
              !initial.heads[2].originBranchId,
              "branch creation origins were not persisted in the first graph page");
        check(repository.revisionGraph(project, 1, initial.next).heads[1].originBranchId == featureId,
              "pinned pagination lost branch origin");
        repository.renameBranch(project, unicodeLabel, "Renamed Feature Branch", root);
        featureRevision = repository.commit(project, "Renamed Feature Branch",
                                               {{key(11), Bytes{11}}}, commitMetadata(), root);
        const RevisionGraphPage changed = repository.revisionGraph(project, 100);
        check(changed.heads[1].name == "Renamed Feature Branch" &&
              changed.heads[1].branchId == featureId &&
              changed.heads[1].originBranchId == mainId &&
              changed.heads[0].originBranchId == featureId,
              "rename or commit destroyed immutable branch creation provenance");
        invalid([&] { repository.createBranch(project, " bad", "main"); },
                StorageErrorKind::InvalidInput, "leading spaces accepted");
        invalid([&] { repository.createBranch(project, "bad\nname", "main"); },
                StorageErrorKind::InvalidInput, "embedded newline accepted");
        invalid([&] { repository.createBranch(project, "bad/name", "main"); },
                StorageErrorKind::InvalidInput, "unsafe route separators accepted");
    }
    {
        Repository reopened(temp.path / "repository");
        const RevisionGraphPage persisted = reopened.revisionGraph(project, 100);
        check(persisted.heads.size() == 3 && persisted.heads[0].originBranchId == featureId
              && persisted.heads[1].originBranchId == mainId
              && persisted.heads[1].revision == featureRevision,
              "reopen lost branch provenance or renamed UTF-8 branch");
    }
}

void runImmutableForkProvenance() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    const std::string sourceName = "Source – Süd";
    const std::string forkName = "Historical Fork";
    BranchId sourceId = BranchId::generate();
    BranchId forkId = BranchId::generate();
    RevisionId original = RevisionId::generate();
    RevisionId later = RevisionId::generate();
    {
        Repository repository(temp.path / "repository");
        original = repository.createRootBranch(project, sourceName, commitMetadata());
        sourceId = *repository.branchId(sourceName);
        later = repository.commit(project, sourceName, {{key(20), Bytes{20}}}, commitMetadata(), original);
        const std::size_t before = repository.revisionGraph(project).revisions.size();
        const BranchCreationResult createdFork = repository.createBranchAtRevisionWithIdentity(
            project, forkName, sourceName, original);
        check(createdFork.revisionId == original && createdFork.originBranchId == sourceId &&
              createdFork.forkRevisionId == original,
              "historical fork creation did not return exact revision provenance");
        forkId = createdFork.branchId;
        check(repository.branchId(forkName) == forkId,
              "historical fork creation returned the wrong persistent branch UUID");
        const RevisionGraphPage created = repository.revisionGraph(project);
        check(created.revisions.size() == before,
              "creating a historical branch introduced a synthetic revision");
        check(repository.head(project, sourceName) == later &&
              repository.head(project, forkName) == original,
              "historical fork moved the source HEAD or forked from the wrong revision");
        bool found = false;
        for (const RevisionGraphHead& head : created.heads) {
            if (head.branchId != forkId) continue;
            check(head.originBranchId == sourceId && head.forkRevisionId == original &&
                  head.revision == original,
                  "created branch lost its source UUID or selected historical HEAD");
            found = true;
        }
        check(found, "historical fork is missing from revision graph");
        repository.renameBranch(project, sourceName, "Renamed Source", later);
        (void)repository.commit(project, "Renamed Source", {{key(21), Bytes{21}}}, commitMetadata(), later);
        (void)repository.commit(project, forkName, {{key(22), Bytes{22}}}, commitMetadata(), original);
    }
    {
        Repository reopened(temp.path / "repository");
        const RevisionGraphPage persisted = reopened.revisionGraph(project);
        bool found = false;
        for (const RevisionGraphHead& head : persisted.heads) {
            if (head.branchId != forkId) continue;
            check(head.name == forkName && head.originBranchId == sourceId &&
                  head.forkRevisionId == original && head.revision.has_value() &&
                  head.revision != original,
                  "restart lost source identity or updated fork HEAD");
            found = true;
        }
        check(found && reopened.branchId("Renamed Source") == sourceId,
              "restart lost branch identity or original source after rename");
    }
}

void runDestructiveBranchHistory() {
    Temporary tmp;
    const ProjectId project = ProjectId::generate();
    const fs::path folder = tmp.path / "delete";
    {
        Repository repo(folder);
        const BranchId unborn = repo.createUnbornBranch(project, "unborn");
        invalid([&] { repo.deleteBranch(project, unborn, RevisionId::generate()); },
                StorageErrorKind::Conflict, "stale unborn deletion must fail");
        repo.deleteBranch(project, unborn, std::nullopt);
        check(!repo.hasBranch("unborn"), "unborn branch not removed");
        const RevisionId root = repo.createRootBranch(project, "main", commitMetadata());
        const RevisionId a = repo.commit(project, "main", {{key(1), Bytes{1}}}, commitMetadata(), root);
        const BranchId mainBranch = *repo.branchId("main");
        const BranchId forkBranch = repo.createBranchWithIdentity(project, "feature", "main").branchId;
        const RevisionId b = repo.commit(project, "main", {{key(2), Bytes{2}}}, commitMetadata(), a);
        const RevisionId feature = repo.commit(project, "feature", {{key(3), Bytes{3}}}, commitMetadata(), a);
        const MergeResult merged = repo.mergeBranches(project, "feature", "main",
            commitMetadata(), feature, b);
        check(merged.resultHead.has_value(), "merge for branch deletion failed");
        invalid([&] { (void)repo.truncateBranch(project, mainBranch, a, b); },
                StorageErrorKind::Conflict, "stale deletion CAS accepted");
        invalid([&] { (void)repo.truncateBranch(project, mainBranch, b, feature); },
                StorageErrorKind::InvalidInput, "second-parent merge ancestry was truncated");
        check(!repo.truncateBranch(project, mainBranch, b, b),
              "removing latest commit unexpectedly deleted main");
        check(repo.head(project, "main") == a, "branch did not rewind to surviving parent");
        check(repo.head(project, "feature") == *merged.resultHead,
              "other branch merge HEAD was changed");
        check(repo.revisionGraph(project).revisions.size() >= 4,
              "retained merge ancestry lost its referenced deleted commit");
        check(repo.truncateBranch(project, mainBranch, a, root),
              "removing root revision should delete entire branch");
        check(!repo.hasBranch("main") && repo.hasBranch("feature"),
              "root deletion damaged surviving feature");
        repo.deleteBranch(project, forkBranch, *merged.resultHead);
        check(repo.listBranches().empty(), "explicit deletion left a ref");
    }
    {
        Repository reopened(folder);
        check(reopened.listBranches().empty(), "deleted branch resurrected on restart");
    }
}

void runService() {
    Temporary tmp;
    ProjectService service(tmp.path);
    const ProjectInfo project = service.createProject("first", commitMetadata());
    const RevisionId root = service.head(project.id, "main");
    const BranchId sourceId = *service.branchId(project.id, "main");
    const BranchCreationResult created = service.createBranchWithIdentity(
        project.id, "second", "main");
    check(created.revisionId == root && created.originBranchId == sourceId &&
          created.forkRevisionId == root &&
          service.branchId(project.id, "second") == created.branchId,
          "service did not return persistent identity and exact fork provenance");
    const RevisionGraphPage graph = service.revisionGraph(project.id, 2);
    check(graph.heads.size() == 2 && graph.revisions.size() == 1 && !graph.next &&
          graph.heads[0].revision == root && graph.heads[1].revision == root,
          "ProjectService graph adapter not correct");
    check(service.history(project.id, "second").size() == 1,
          "ProjectService first-parent reader missing");
    const ProjectInfo other = service.createProject("second", commitMetadata());
    const RevisionId otherRoot = service.head(other.id, "main");
    check(root != otherRoot, "two empty projects received same UUIDv7 revision");
    check(service.revisionGraph(other.id).revisions.front().id == otherRoot,
          "project graph scoped to incorrect project");
    service.deleteBranch(project.id, created.branchId.toString(), root);
    check(!service.branchById(project.id, created.branchId.toString()).has_value(),
          "service deletion did not remove UUID-keyed branch ref");
    service.configureRepositoryRole(RepositoryRole::Private);
    const BranchId withDraft = service.createUnbornBranch(project.id, "draft-delete");
    const WorkingCopyInfo draft = service.createWorkingCopyForBranch(
        project.id, withDraft.toString(), std::nullopt, UuidV7::generate().toString());
    bool refused = false;
    try { service.deleteBranch(project.id, withDraft.toString(), std::nullopt); }
    catch (const ProjectError& error) { refused = error.kind() == ProjectErrorKind::Conflict; }
    check(refused, "draft must block branch deletion without explicit consent");
    check(service.workingCopy(draft.id).id == draft.id,
          "refused deletion must preserve working copy");
    service.deleteBranch(project.id, withDraft.toString(), std::nullopt, true);
    check(!service.branchById(project.id, withDraft.toString()).has_value(),
          "explicit discard must delete branch");
    check(service.listWorkingCopies().empty(),
          "explicit discard must remove saved working copy and journal");

    const BranchCreationResult truncated = service.createBranchWithIdentity(
        project.id, "draft-truncate", "main");
    const WorkingCopyInfo draft2 = service.createWorkingCopyForBranch(
        project.id, truncated.branchId.toString(), root, UuidV7::generate().toString());
    refused = false;
    try { (void)service.truncateBranch(project.id, truncated.branchId.toString(),
                                       root, root); }
    catch (const ProjectError& error) { refused = error.kind() == ProjectErrorKind::Conflict; }
    check(refused, "draft must block history truncation without explicit consent");
    refused = false;
    try { (void)service.truncateBranch(project.id, truncated.branchId.toString(),
                root, RevisionId::generate(), true); }
    catch (const ProjectError& error) { refused = error.kind() == ProjectErrorKind::InvalidInput; }
    check(refused && service.workingCopy(draft2.id).id == draft2.id,
          "invalid truncation must not discard working copy");
    (void)service.truncateBranch(project.id, truncated.branchId.toString(), root, root, true);
    check(service.listWorkingCopies().empty(), "truncation must discard branch draft");
    const BranchId unborn = service.createUnbornBranch(project.id, "empty");
    service.deleteBranch(project.id, unborn.toString(), std::nullopt);
    check(!service.branchById(project.id, unborn.toString()).has_value(),
          "service deletion did not remove unborn HEAD");
    // Deleting one branch must never discard another branch's saved edits.
    const BranchId retained = service.createUnbornBranch(project.id, "retained-draft");
    const WorkingCopyInfo retainedDraft = service.createWorkingCopyForBranch(
        project.id, retained.toString(), std::nullopt, UuidV7::generate().toString());
    const BranchId removed = service.createUnbornBranch(project.id, "removed-draft");
    const WorkingCopyInfo removedDraft = service.createWorkingCopyForBranch(
        project.id, removed.toString(), std::nullopt, UuidV7::generate().toString());
    bool staleRejected = false;
    try { service.deleteBranch(project.id, removed.toString(), RevisionId::generate(), true); }
    catch (const ProjectError& error) { staleRejected = error.kind() == ProjectErrorKind::Conflict; }
    check(staleRejected && service.workingCopy(removedDraft.id).id == removedDraft.id,
          "stale confirmed deletion discarded an unrelated draft");
    service.deleteBranch(project.id, removed.toString(), std::nullopt, true);
    check(service.workingCopy(retainedDraft.id).id == retainedDraft.id,
          "deleting a branch discarded the surviving branch's working copy");
    check(service.listWorkingCopies().size() == 1,
          "branch deletion did not clean up exactly the selected draft");
    check(service.branchById(project.id, retained.toString()).has_value(),
          "branch deletion removed another branch's HEAD");
}
}

int main() {
    try {
        runUnbornGraph();
        runRepository();
        runBranchOriginsAndNames();
        runImmutableForkProvenance();
        runService();
        runDestructiveBranchHistory();
        std::cout << "UUIDv7 first-parent history and pinned all-parent graph passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UUIDv7 graph test FAILED: " << error.what() << '\n';
        return 1;
    }
}
