#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/revision_record.h"

#include <atomic>
#include <filesystem>
#include <thread>
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
void rejects(Callback callback, ProjectErrorKind kind, const char* message) {
    try { callback(); }
    catch (const ProjectError& error) {
        if (error.kind() == kind) return;
        throw std::runtime_error(std::string(message) + ": incorrect error kind");
    }
    throw std::runtime_error(message);
}

struct Temporary {
    fs::path path = fs::temp_directory_path() / ("aowis-project-model-v2-" + UuidV7::generate().toString());
    ~Temporary() { std::error_code ec; fs::remove_all(this->path, ec); }
};

Key key(std::uint8_t value) {
    Key id{};
    id[15] = value;
    return id;
}

ModelRecord record(std::uint8_t value, std::vector<Key> references = {}) {
    ModelRecord result;
    result.kind = 31;
    result.payload = {value};
    result.references = std::move(references);
    return result;
}

void testModelWorkflow() {
    Temporary temp;
    fs::create_directories(temp.path);
    ProjectId project = ProjectId::generate();
    RevisionId root = RevisionId::generate();
    RevisionId first = RevisionId::generate();
    RevisionId second = RevisionId::generate();
    RevisionId feature = RevisionId::generate();
    const Key a = key(1);
    const Key b = key(2);
    BranchId mainBranch = BranchId::generate();
    BranchId featureBranch = BranchId::generate();
    {
        ProjectService service(temp.path);
        const ProjectInfo info = service.createProject("New UUID model project", commitMetadata());
        project = *ProjectId::parse(info.id);
        root = service.head(info.id, "main");
        check(service.optionalHead(info.id, "main") == std::optional<RevisionId>{root},
              "optional HEAD disagrees with committed HEAD");
        check(service.listBranches(info.id) == std::vector<std::string>{"main"},
              "initial branch missing");
        rejects([&] { (void)service.getModelAtRevision(info.id, root, a); },
                ProjectErrorKind::InvalidInput, "raw initial revision exposed model reads");
        service.withRepository(info.id, [&](Repository& repo) {
            mainBranch = *repo.branchId("main");
            const RevisionRecord initial = decodeRevisionRecord(*repo.readRevision(project, root));
            check(initial.parents.empty() && !initial.modelIndexed, "initial revision not raw empty");
        });
        first = service.commitModel(info.id, "main", {{a, record(11)}}, commitMetadata(), root);
        check(first != root && service.head(info.id, "main") == first,
              "model commit did not advance UUID HEAD");
        check(service.getModelAtRevision(info.id, first, a)->payload == Bytes{11},
              "model payload was not stored");
        service.withRepository(info.id, [&](Repository& repo) {
            const std::vector<Difference> changes = repo.diffRevisions(project, root, first, 1);
            check(changes.size() == 1 && changes.front().key == a &&
                  !changes.front().before && changes.front().after.has_value(),
                  "UUIDv7 immutable diff did not report model addition");
            const std::vector<Difference> replay = repo.diffRevisions(project, first, first, 0);
            check(replay.empty(), "same-revision UUID diff was not empty");
        });
        rejects([&] { (void)service.getModelAtRevision(info.id, root, a); },
                ProjectErrorKind::InvalidInput, "initial revision was mutated");
        const RevisionId shared = service.createBranch(info.id, "feature", "main");
        check(shared == first, "branch creation duplicated revision");
        service.withRepository(info.id, [&](Repository& repo) {
            featureBranch = *repo.branchId("feature");
        });
        check(mainBranch != featureBranch, "branch UUIDs are not unique");
        second = service.commitModel(info.id, "main", {{b, record(22, {a, a})}}, commitMetadata(), first);
        feature = service.commitModel(info.id, "feature", {{b, record(33)}}, commitMetadata(), first);
        check(second != feature && service.head(info.id, "main") == second &&
              service.head(info.id, "feature") == feature,
              "forked UUID revisions did not advance independently");
        service.withRepository(info.id, [&](Repository& repo) {
            const std::vector<Difference> changes = repo.diffRevisions(project, root, second, 2);
            check(changes.size() == 2 && changes[0].key == a && changes[1].key == b,
                  "UUIDv7 immutable diff missed changed model records");
            bool limited = false;
            try { (void)repo.diffRevisions(project, root, second, 1); }
            catch (const StorageError& error) {
                limited = error.kind() == StorageErrorKind::LimitExceeded;
            }
            check(limited, "UUIDv7 immutable diff ignored its result limit");
        });
        check(service.getModelAtRevision(info.id, second, b)->payload == Bytes{22} &&
              service.getModelAtRevision(info.id, feature, b)->payload == Bytes{33},
              "branch snapshots not isolated");
        rejects([&] { (void)service.commitModel(info.id, "main", {}, commitMetadata(), first); },
                ProjectErrorKind::Conflict, "stale HEAD was accepted");
        rejects([&] { (void)service.commitModel(info.id, "main", {{a, std::nullopt}}, commitMetadata(), second); },
                ProjectErrorKind::InvalidInput, "deletion of referenced model was accepted");
        rejects([&] { (void)service.commitModel(info.id, "main", {{key(3), record(44, {key(9)})}}, commitMetadata(), second); },
                ProjectErrorKind::InvalidInput, "dangling reference was accepted");
        check(service.head(info.id, "main") == second,
              "rejected commit advanced HEAD");
        service.withRepository(info.id, [&](Repository& repo) {
            const RevisionRecord current = decodeRevisionRecord(*repo.readRevision(project, second));
            check(current.modelIndexed && current.parents.size() == 1 &&
                  current.parents.front().id == first,
                  "model index or UUID parent lost");
            check(current.referencesRoot != Hash{}, "reverse index not persisted");
        });
    }
    {
        ProjectService service(temp.path);
        check(service.listProjects().size() == 1, "reopen lost project catalogue");
        const std::string id = project.toString();
        check(service.head(id, "main") == second &&
              service.head(id, "feature") == feature,
              "reopen changed branch HEAD identities");
        check(service.getModelAtRevision(id, second, b)->references == std::vector<Key>{a},
              "reopen lost deduplicated model references");
        service.withRepository(id, [&](Repository& repo) {
            check(*repo.branchId("main") == mainBranch &&
                  *repo.branchId("feature") == featureBranch,
                  "reopen changed branch UUIDs");
        });
    }
}

void testStrictLocalCommitHeadPin() {
    Temporary temp;
    fs::create_directories(temp.path);
    std::string projectId;
    RevisionId first = RevisionId::generate();
    RevisionId second = RevisionId::generate();
    const Key created = key(61);
    const Key later = key(62);
    {
        ProjectService service(temp.path);
        const ProjectInfo project = service.createUnbornProject("strict local commit CAS");
        projectId = project.id;
        check(!service.optionalHead(projectId, "main"),
              "new project must start with unborn HEAD");
        rejects([&] {
            (void)service.commitModelAtExpectedHead(projectId, "main",
                    {{created, record(1)}}, commitMetadata(), std::nullopt);
        }, ProjectErrorKind::Conflict, "local commit accepted an unconfigured repository");
        service.configureRepositoryRole(RepositoryRole::Private);
        rejects([&] {
            (void)service.commitModelAtExpectedHead(projectId, "main",
                    {{created, record(1)}}, commitMetadata(), RevisionId::generate());
        }, ProjectErrorKind::Conflict, "born expected HEAD accepted on unborn branch");
        check(!service.optionalHead(projectId, "main") &&
              service.revisionGraph(projectId).revisions.empty(),
              "rejected unborn commit wrote revision or HEAD");

        // Two sessions both pinning explicit null must not each create an
        // independent first revision. The repository lock owns the CAS.
        std::atomic<int> successes{0};
        std::atomic<int> conflicts{0};
        std::atomic<bool> unexpected{false};
        const auto attempt = [&]() {
            try {
                (void)service.commitModelAtExpectedHead(projectId, "main",
                         {{created, record(42)}}, commitMetadata(), std::nullopt);
                ++successes;
            } catch (const ProjectError& error) {
                if (error.kind() == ProjectErrorKind::Conflict) ++conflicts;
                else unexpected = true;
            } catch (...) { unexpected = true; }
        };
        std::thread writerA(attempt);
        std::thread writerB(attempt);
        writerA.join();
        writerB.join();
        check(!unexpected && successes == 1 && conflicts == 1,
              "concurrent first commits did not enforce exact unborn HEAD");
        const std::optional<RevisionId> committed = service.optionalHead(projectId, "main");
        check(committed.has_value() &&
              service.getModelAtRevision(projectId, *committed, created)->payload == Bytes{42},
              "strict first commit lost its model data");
        first = *committed;
        rejects([&] {
            (void)service.commitModelAtExpectedHead(projectId, "main",
                     {{later, record(43)}}, commitMetadata(), std::nullopt);
        }, ProjectErrorKind::Conflict, "stale explicit-null commit advanced born HEAD");
        rejects([&] {
            (void)service.commitModelAtExpectedHead(projectId, "main",
                     {{later, record(43)}}, commitMetadata(), RevisionId::generate());
        }, ProjectErrorKind::Conflict, "wrong committed HEAD pin was accepted");
        check(service.optionalHead(projectId, "main") == first,
              "rejected local commit moved HEAD");
        second = service.commitModelAtExpectedHead(projectId, "main",
                     {{later, record(43)}}, commitMetadata(), first);
        check(second != first && service.optionalHead(projectId, "main") == second,
              "correct expected HEAD did not advance local branch");
        const ProjectId nativeProject = *ProjectId::parse(projectId);
        service.withRepository(projectId, [&](Repository& repository) {
            const RevisionRecord initial = decodeRevisionRecord(
                    *repository.readRevision(nativeProject, first));
            const RevisionRecord advanced = decodeRevisionRecord(
                    *repository.readRevision(nativeProject, second));
            check(initial.parents.empty() && advanced.parents.size() == 1 &&
                  advanced.parents.front().id == first,
                  "strict local CAS created incorrect immutable ancestry");
        });
    }
    {
        ProjectService reopened(temp.path);
        check(reopened.optionalHead(projectId, "main") == second &&
              reopened.getModelAtRevision(projectId, second, created)->payload == Bytes{42} &&
              reopened.getModelAtRevision(projectId, second, later)->payload == Bytes{43},
              "strict local commit did not survive reopen");
    }
}
}

int main() {
    try {
        testModelWorkflow();
        testStrictLocalCommitHeadPin();
        std::cout << "UUIDv7 project model commit, fork, references and reopen passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UUIDv7 project model test FAILED: " << error.what() << '\n';
        return 1;
    }
}
