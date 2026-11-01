#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/object_id.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using aowis::dvmdb::test::commitMetadata;

namespace {
using aowis::dvmdb::ProjectService;
using aowis::dvmdb::ProjectError;
using aowis::dvmdb::ProjectErrorKind;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TemporaryRoot {
    std::filesystem::path directory;
    TemporaryRoot() {
        char pattern[] = "/tmp/aowis-dvmdb-projects-XXXXXX";
        char* directoryName = ::mkdtemp(pattern);
        require(directoryName != nullptr, "mkdtemp failed");
        this->directory = directoryName;
    }
    ~TemporaryRoot() { std::error_code error; std::filesystem::remove_all(this->directory, error); }
};

void expectError(const std::function<void()>& action, ProjectErrorKind kind) {
    bool rejected = false;
    try { action(); }
    catch (const ProjectError& error) { rejected = true; require(error.kind() == kind, "unexpected project error kind"); }
    require(rejected, "operation was not rejected");
}

aowis::dvmdb::ModelRecord record(std::uint8_t value) {
    aowis::dvmdb::ModelRecord result;
    result.kind = 31;
    result.payload = {value};
    return result;
}

aowis::dvmdb::Key modelKey(std::uint8_t value) {
    aowis::dvmdb::Key result{};
    result[0] = 0x91;
    result[15] = value;
    return result;
}

// Project identities are UUIDv7. A directory named with another UUID
// version is not a project; the catalogue refuses to start, never ignores it.
void exerciseNonV7ProjectRejected() {
    TemporaryRoot root;
    const std::string legacyId = "12345678-1234-4123-8123-1234567890ab";
    const std::filesystem::path directory = root.directory / "projects" / legacyId;
    std::filesystem::create_directories(directory);
    {
        std::ofstream manifest(directory / "project.meta", std::ios::binary);
        manifest << "AOWIS-PROJECT-1\n" << legacyId << "\npack\n4c6567616379\n";
        require(manifest.good(), "cannot create UUIDv4 project manifest");
    }
    require(!ProjectService::validProjectId(legacyId), "UUIDv4 accepted as project identity");
    require(!ProjectService::validProjectId("01234567-89AB-7DEF-8123-456789ABCDEF"),
            "noncanonical uppercase UUIDv7 accepted as project identity");
    bool refused = false;
    try { ProjectService service(root.directory); }
    catch (const std::exception&) { refused = true; }
    require(refused, "catalogue opened with a non-UUIDv7 project directory");
}

void exerciseBackend() {
    TemporaryRoot root;
    std::string firstId;
    std::string secondId;
    aowis::dvmdb::RevisionId firstHead = aowis::dvmdb::RevisionId::generate();
    {
        ProjectService service(root.directory);
        require(service.listProjects().empty(), "new catalogue is not empty");
        const aowis::dvmdb::ProjectInfo first = service.createUnbornProject("Engineering Å");
        const aowis::dvmdb::ProjectInfo second = service.createUnbornProject("Second project");
        firstId = first.id;
        secondId = second.id;
        require(ProjectService::validProjectId(firstId), "invalid generated UUID");
        require(aowis::dvmdb::ProjectId::parse(firstId).has_value(),
                "new project must receive a UUIDv7 identity");
        require(aowis::dvmdb::ProjectId::parse(secondId).has_value(),
                "second project must receive a UUIDv7 identity");
        require(firstId != secondId, "duplicate project IDs");
        require(first.name == "Engineering Å", "bad metadata");
        require(service.project(firstId).name == first.name, "catalogue read differs");
        require(service.listProjects().size() == 2, "missing published project");
        require(service.listBranches(firstId) == std::vector<std::string>{"main"}, "missing initial main branch");
        require(!service.optionalHead(firstId, "main") && service.history(firstId, "main", 10).empty(),
                "new project main is not unborn");
        const aowis::dvmdb::BranchId scratch = service.createUnbornBranch(firstId, "scratch");
        require(service.createUnbornBranch(firstId, "scratch", scratch.toString()) == scratch &&
                !service.optionalHead(firstId, "scratch"),
                "project service did not create or retry independent unborn branch");
        expectError([&] { (void)service.createUnbornBranch(firstId, "scratch"); },
                    ProjectErrorKind::Conflict);
        require(service.listBranches(secondId) == std::vector<std::string>{"main"},
                "unborn branch leaked into another project's repository");
        firstHead = service.commitModel(firstId, "main", {{modelKey(1), record(1)}}, commitMetadata());
        require(service.history(firstId, "main", 10).size() == 1, "missing first revision");
        const aowis::dvmdb::BranchId later = service.createUnbornBranch(firstId, "later");
        require(service.branchId(firstId, "later") == later &&
                !service.optionalHead(firstId, "later"),
                "cannot create a root unborn branch after a committed revision");
        require(service.revision(firstId, firstHead).id == firstHead, "revision lookup failed");
        require(service.history(firstId, "main", 10, firstHead).empty(), "history cursor should be exclusive");
        require(!service.optionalHead(secondId, "main"), "commit leaked across projects");
        const aowis::dvmdb::RevisionId fork = service.createBranchAtRevision(firstId, "design", "main", firstHead);
        require(fork == firstHead, "pinned fork changed revision");
        (void)service.createBranch(firstId, "independent", "main");
        require(service.listBranches(firstId).size() == 5, "missing new branch");
        require(service.listBranches(secondId).size() == 1, "projects are not isolated");
        // The model HTTP adapter's scoped native callback shares the exact
        // project handle and serializes access without exposing its lifetime.
        const aowis::dvmdb::ProjectId project = *aowis::dvmdb::ProjectId::parse(firstId);
        aowis::dvmdb::RevisionId modelRevision = aowis::dvmdb::RevisionId::generate();
        service.withRepository(firstId, [&](aowis::dvmdb::Repository& repository) {
            modelRevision = repository.commitModel(project, "main", {{modelKey(2), record(2)}}, commitMetadata(),
                                                     firstHead);
            require(repository.getModelAtRevision(project, modelRevision, modelKey(2)).has_value(),
                    "scoped repository callback did not persist model record");
        });
        require(modelRevision != firstHead && service.head(firstId, "main") == modelRevision,
                "scoped repository callback lost the model commit");
        require(!service.optionalHead(secondId, "main"),
                "scoped model commit leaked across project boundaries");
        expectError([&] {
            service.withRepository("../../wrong", [](aowis::dvmdb::Repository&) {});
        }, ProjectErrorKind::InvalidInput);
        bool callbackFailure = false;
        try {
            service.withRepository(firstId, [](aowis::dvmdb::Repository&) {
                throw std::runtime_error("intentional callback failure");
            });
        } catch (const std::runtime_error&) { callbackFailure = true; }
        require(callbackFailure && service.head(firstId, "main") == modelRevision,
                "failing scoped callback left catalogue locked or corrupted");
        expectError([&]{ service.createBranch(firstId, "design", "main"); }, ProjectErrorKind::Conflict);
        expectError([&]{ service.project("../../etc/passwd"); }, ProjectErrorKind::InvalidInput);
        expectError([&]{ service.project(aowis::dvmdb::ProjectId::generate().toString()); },
                    ProjectErrorKind::NotFound);
        expectError([&]{ service.head(firstId, "missing"); }, ProjectErrorKind::NotFound);
        expectError([&]{ service.createBranch(firstId, "fork", "missing"); }, ProjectErrorKind::NotFound);
        expectError([&]{ service.revision(firstId, aowis::dvmdb::RevisionId::generate()); },
                    ProjectErrorKind::NotFound);
        expectError([&]{ service.createUnbornProject(""); }, ProjectErrorKind::InvalidInput);
        expectError([&]{ service.createUnbornProject("bad\nname"); }, ProjectErrorKind::InvalidInput);
        expectError([&]{ service.createUnbornProject(std::string("\xc0\xaf", 2)); },
                    ProjectErrorKind::InvalidInput);
        expectError([&]{ service.createUnbornProject(std::string("\xed\xa0\x80", 3)); },
                    ProjectErrorKind::InvalidInput);
        expectError([&]{ service.createBranch(firstId, "../invalid", "main"); }, ProjectErrorKind::InvalidInput);
        expectError([&]{ service.history(firstId, "main", 1001); }, ProjectErrorKind::InvalidInput);
        expectError([&]{ service.history(firstId, "main", 0); }, ProjectErrorKind::InvalidInput);
        // No second service instance (and no separate process) can own the same root.
        expectError([&]{ ProjectService another(root.directory); }, ProjectErrorKind::Conflict);
        const pid_t pid = ::fork();
        require(pid >= 0, "fork failed");
        if (pid == 0) {
            try { ProjectService another(root.directory); _exit(11); }
            catch (const ProjectError& error) {
                _exit(error.kind() == ProjectErrorKind::Conflict ? 0 : 12);
            } catch (...) { _exit(13); }
        }
        int status = 0;
        require(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "project catalogue process lock not exclusive");

        std::atomic<int> completed{0};
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 6; ++worker) {
            workers.emplace_back([&, worker] {
                const aowis::dvmdb::ProjectInfo item =
                    service.createUnbornProject("thread-" + std::to_string(worker));
                require(!item.id.empty(), "concurrent create failed");
                ++completed;
            });
        }
        for (std::thread& worker : workers) worker.join();
        require(completed == 6 && service.listProjects().size() == 8,
                "concurrent catalogue publication lost projects");
    }
    {
        ProjectService reopened(root.directory);
        require(reopened.listProjects().size() == 8, "restart lost catalogue entries");
        require(reopened.project(firstId).name == "Engineering Å", "restart changed project metadata");
        require(reopened.head(firstId, "design") == firstHead, "restart lost branch");
        require(reopened.listBranches(secondId).size() == 1, "restart crossed project boundaries");
    }

    // Staging debris from an interrupted project creation is never advertised.
    std::filesystem::create_directory(root.directory / "projects" / ".creating-orphan");
    {
        ProjectService service(root.directory);
        require(service.listProjects().size() == 8, "incomplete staging project was published");
    }
    // A published but damaged manifest is not silently ignored.
    {
        std::ofstream damaged(root.directory / "projects" / firstId / "project.meta", std::ios::trunc);
        damaged << "bad manifest\n";
    }
    bool failed = false;
    try { ProjectService service(root.directory); }
    catch (const std::exception&) { failed = true; }
    require(failed, "corrupt catalogue should fail startup");
}

// A growing branch catalogue must not make a single-branch request depend on
// enumerating every other ref. Use real, immutable ref files to build the
// directory without spending thousands of fsyncs creating test-only branches.
void exerciseLargeBranchCatalogue() {
    TemporaryRoot root;
    ProjectService service(root.directory);
    const aowis::dvmdb::ProjectInfo project = service.createUnbornProject("Branch scaling");
    const aowis::dvmdb::RevisionId mainHead =
        service.commitModel(project.id, "main", {{modelKey(1), record(1)}}, commitMetadata());
    const aowis::dvmdb::Hash mainDigest = service.revision(project.id, mainHead).recordHash;
    const std::filesystem::path heads = root.directory / "projects" / project.id /
                                        "repository" / "refs" / "heads";
    constexpr unsigned branchCount = 4096;
    for (unsigned number = 0; number < branchCount; ++number) {
        const std::string branch = "bulk_" + std::to_string(number);
        const aowis::dvmdb::BranchId id = aowis::dvmdb::BranchId::generate();
        std::ofstream record(heads / id.toString(), std::ios::binary);
        require(record.good(), "cannot create UUID-named bulk branch");
        record << "AOWIS-BRANCH-2\n" << branch << "\n" << mainHead.toString() << "\n"
               << aowis::dvmdb::toHex(mainDigest) << "\n";
        require(record.good(), "cannot write UUID-named bulk branch");
    }
    require(service.listBranches(project.id).size() == branchCount + 1U,
            "large branch catalogue is missing entries");
    for (const unsigned number : {0U, 1024U, 2048U, 4095U}) {
        const std::string branch = "bulk_" + std::to_string(number);
        require(service.head(project.id, branch) == mainHead,
                "targeted branch read changed revision");
        require(service.history(project.id, branch, 1).at(0).id == mainHead,
                "targeted branch history changed revision");
    }
    (void)service.createBranch(project.id, "new_from_bulk", "bulk_2048");
    require(service.head(project.id, "new_from_bulk") == mainHead,
            "fork from large branch catalogue lost head");
    expectError([&] { service.head(project.id, "absent"); }, ProjectErrorKind::NotFound);
    expectError([&] { service.history(project.id, "absent", 1); }, ProjectErrorKind::NotFound);
    expectError([&] { service.createBranch(project.id, "missing_parent", "absent"); },
                ProjectErrorKind::NotFound);
    expectError([&] { service.createBranch(project.id, "bulk_2048", "main"); },
                ProjectErrorKind::Conflict);

    // UUID-only storage rejects unknown filenames rather than treating
    // user-named path entries as valid branch refs or silently skipping them.
    std::filesystem::create_symlink("main", heads / "symlink_ref");
    {
        bool rejected = false;
        try { (void)service.listBranches(project.id); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "unknown branch filename was not rejected");
    }
    std::filesystem::remove(heads / "symlink_ref");
    std::filesystem::create_directory(heads / "directory_ref");
    {
        bool rejected = false;
        try { (void)service.listBranches(project.id); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "unknown branch filename was not rejected");
    }
    std::filesystem::remove(heads / "directory_ref");
    require(service.listBranches(project.id).size() == branchCount + 2U,
            "invalid branch fixtures affected valid UUID branch references");
}

void exerciseUnsafeCatalogue() {
    TemporaryRoot root;
    const std::filesystem::path directory = root.directory / "projects";
    std::filesystem::create_directory(directory);
    // A valid UUIDv7 name, so the refusal is about the symlink itself.
    std::filesystem::create_symlink("/tmp", directory / aowis::dvmdb::ProjectId::generate().toString());
    bool rejected = false;
    try { ProjectService service(root.directory); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "symlinked project must fail startup");
}
} // namespace

int main() {
    try {
        exerciseBackend();
        exerciseNonV7ProjectRejected();
        exerciseUnsafeCatalogue();
        exerciseLargeBranchCatalogue();
        std::cout << "Project catalogue and lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Project service test failed: " << error.what() << '\n';
        return 1;
    }
}
