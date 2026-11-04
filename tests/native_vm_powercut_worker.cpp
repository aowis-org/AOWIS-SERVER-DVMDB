#include "aowis/dvmdb/native_store.h"
#include "project_repository.h"
#include "aowis/dvmdb/project_service.h"
#include "commit_metadata_fixture.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <sstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using aowis::dvmdb::Bytes;
using aowis::dvmdb::Change;
using aowis::dvmdb::Key;
using aowis::dvmdb::test::ProjectRepository;

Key counterKey() {
    Key result{};
    result.back() = 0x71;
    return result;
}

Bytes counterData(std::uint64_t sequence) {
    Bytes result(131072U);
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<std::uint8_t>((sequence * 127U + i * 53U + (i >> 7U)) & 0xffU);
    }
    for (unsigned i = 0; i < 8; ++i)
        result[i] = static_cast<std::uint8_t>(sequence >> (8U * i));
    return result;
}

std::uint64_t readCounter(ProjectRepository& repository) {
    const std::optional<Bytes> value = repository.get("main", counterKey());
    if (!value.has_value() || value->size() != 131072U)
        throw std::runtime_error("missing or incorrectly sized counter");
    std::uint64_t sequence = 0;
    for (unsigned i = 0; i < 8; ++i)
        sequence |= static_cast<std::uint64_t>((*value)[i]) << (8U * i);
    if (*value != counterData(sequence))
        throw std::runtime_error("counter checksum/payload mismatch");
    return sequence;
}

void check(ProjectRepository& repository) {
    repository.verifyRepository();
    const std::uint64_t sequence = readCounter(repository);
    std::cout << "STATE " << sequence << " "
              << aowis::dvmdb::toHex(repository.headRevision("main").recordHash) << std::endl;
}
// A durable fixture manifest is written before the host starts a destructive
// operation. Its parent directory is fsynced so a power cut cannot lose the
// verifier's expected identities while preserving the database mutation.
struct DeleteFixture {
    std::string scenario;
    std::string project;
    std::string doomed;
    std::string survivor;
    std::string doomedDraft;
    std::string survivorDraft;
    std::string expectedHead;
    std::string priorHead;
};

std::filesystem::path fixturePath(const std::filesystem::path& root) {
    return root.parent_path() / (root.filename().string() + ".fixture");
}

void writeFixture(const std::filesystem::path& root, const DeleteFixture& fixture) {
    const std::string content = fixture.scenario + "\n" + fixture.project + "\n" +
        fixture.doomed + "\n" + fixture.survivor + "\n" +
        fixture.doomedDraft + "\n" + fixture.survivorDraft + "\n" +
        fixture.expectedHead + "\n" + fixture.priorHead + "\n";
    const std::filesystem::path path = fixturePath(root);
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) throw std::runtime_error("cannot create fixture manifest");
    const ssize_t written = ::write(fd, content.data(), content.size());
    const bool synced = written == static_cast<ssize_t>(content.size()) && ::fsync(fd) == 0;
    ::close(fd);
    if (!synced) throw std::runtime_error("cannot sync fixture manifest");
    const int dir = ::open(root.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (dir < 0) throw std::runtime_error("cannot open fixture parent directory");
    const bool dirSynced = ::fsync(dir) == 0;
    ::close(dir);
    if (!dirSynced) throw std::runtime_error("cannot sync fixture parent directory");
}

DeleteFixture readFixture(const std::filesystem::path& root) {
    std::ifstream input(fixturePath(root));
    DeleteFixture fixture;
    for (std::string* value : {&fixture.scenario, &fixture.project, &fixture.doomed,
             &fixture.survivor, &fixture.doomedDraft, &fixture.survivorDraft,
             &fixture.expectedHead, &fixture.priorHead})
        if (!std::getline(input, *value) || value->empty())
            throw std::runtime_error("incomplete deletion fixture");
    if (fixture.scenario != "branch" && fixture.scenario != "truncate")
        throw std::runtime_error("invalid deletion scenario");
    return fixture;
}

void initDeletion(const std::filesystem::path& root, std::string_view scenario) {
    using namespace aowis::dvmdb;
    if (scenario != "branch" && scenario != "truncate")
        throw std::runtime_error("expected branch or truncate scenario");
    ProjectService service(root);
    const ProjectInfo project = service.createProject("powercut deletion", test::commitMetadata());
    service.configureRepositoryRole(RepositoryRole::Private);
    std::string doomed;
    std::optional<RevisionId> expected;
    std::string prior = "-";
    if (scenario == "branch") {
        doomed = service.createUnbornBranch(project.id, "doomed").toString();
    } else {
        const BranchCreationResult branch = service.createBranchWithIdentity(
            project.id, "doomed", "main");
        doomed = branch.branchId.toString();
        RevisionId first = RevisionId::generate();
        service.withRepository(project.id, [&](Repository& repository) {
            first = repository.commit(*ProjectId::parse(project.id), "doomed",
                {Change{Key{5}, Bytes{1, 2, 3}}}, test::commitMetadata(), branch.revisionId);
            expected = repository.commit(*ProjectId::parse(project.id), "doomed",
                {Change{Key{6}, Bytes{4, 5, 6}}}, test::commitMetadata(), first);
        });
        prior = first.toString();
    }
    const std::string survivor = service.createUnbornBranch(project.id, "survivor").toString();
    const WorkingCopyInfo doomedDraft = service.createWorkingCopyForBranch(
        project.id, doomed, expected, UuidV7::generate().toString());
    const WorkingCopyInfo survivorDraft = service.createWorkingCopyForBranch(
        project.id, survivor, std::nullopt, UuidV7::generate().toString());
    writeFixture(root, DeleteFixture{std::string(scenario), project.id, doomed, survivor,
        doomedDraft.id, survivorDraft.id, expected ? expected->toString() : "-", prior});
    std::cout << "READY " << scenario << std::endl;
}

void writeDeletion(const std::filesystem::path& root) {
    using namespace aowis::dvmdb;
    const DeleteFixture fixture = readFixture(root);
    ProjectService service(root);
    if (fixture.scenario == "branch") {
        service.deleteBranch(fixture.project, fixture.doomed, std::nullopt, true);
    } else {
        service.truncateBranch(fixture.project, fixture.doomed,
            *RevisionId::parse(fixture.expectedHead), *RevisionId::parse(fixture.expectedHead), true);
    }
    std::cout << "DELETE-COMPLETE" << std::endl;
}

void verifyDeletion(const std::filesystem::path& root, bool beforeIntent) {
    using namespace aowis::dvmdb;
    const DeleteFixture fixture = readFixture(root);
    ProjectService service(root); // executes durable-intent recovery on opening
    const std::optional<RevisionGraphHead> doomed = service.branchById(fixture.project, fixture.doomed);
    const std::optional<RevisionGraphHead> survivor = service.branchById(fixture.project, fixture.survivor);
    if (!survivor || survivor->revision.has_value())
        throw std::runtime_error("unrelated survivor branch lost or changed");
    const std::vector<WorkingCopyInfo> drafts = service.listWorkingCopies();
    bool hasDoomed = false;
    bool hasSurvivor = false;
    for (const WorkingCopyInfo& draft : drafts) {
        if (draft.id == fixture.doomedDraft) hasDoomed = true;
        if (draft.id == fixture.survivorDraft) hasSurvivor = true;
    }
    if (!hasSurvivor || hasDoomed != beforeIntent)
        throw std::runtime_error("incorrect working-copy recovery or unrelated draft removed");
    if (beforeIntent) {
        if (!doomed || (fixture.scenario == "branch" && doomed->revision.has_value()) ||
            (fixture.scenario == "truncate" &&
             (!doomed->revision || doomed->revision->toString() != fixture.expectedHead)))
            throw std::runtime_error("unconfirmed operation modified the branch");
    } else if (fixture.scenario == "branch") {
        if (doomed) throw std::runtime_error("confirmed deletion resurrected branch");
    } else if (!doomed || !doomed->revision ||
               doomed->revision->toString() != fixture.priorHead) {
        throw std::runtime_error("confirmed truncation did not restore previous revision");
    }
    if (std::filesystem::exists(root / "history-deletions" / fixture.doomed))
        throw std::runtime_error("recovery left published deletion intent");
    std::cout << "DELETE-VERIFIED " << fixture.scenario << " "
              << (beforeIntent ? "original" : "deleted") << std::endl;
}

// Continuous workload for probabilistic mid-operation VM interruption. The
// deterministic deletion scenario separately covers every named boundary.
void stressInit(const std::filesystem::path& root) {
    using namespace aowis::dvmdb;
    ProjectService service(root);
    const ProjectInfo project = service.createProject("powercut deletion stress", test::commitMetadata());
    service.configureRepositoryRole(RepositoryRole::Private);
    (void)service.createUnbornBranch(project.id, "survivor");
    std::cout << "STRESS-READY" << std::endl;
}

void stressVerify(const std::filesystem::path& root) {
    using namespace aowis::dvmdb;
    ProjectService service(root);
    const std::vector<ProjectInfo> projects = service.listProjects();
    if (projects.size() != 1) throw std::runtime_error("stress project missing");
    const std::string project = projects.front().id;
    const std::optional<BranchId> survivor = service.branchId(project, "survivor");
    if (!survivor || !service.branchById(project, survivor->toString()))
        throw std::runtime_error("stress survivor branch was deleted");
    service.withRepository(project, [&](Repository& repo) { repo.verifyRepository(); });
    const std::vector<WorkingCopyInfo> drafts = service.listWorkingCopies();
    for (const WorkingCopyInfo& draft : drafts)
        if (draft.projectId != project || !service.branchById(project, draft.branchId))
            throw std::runtime_error("orphaned working copy after VM recovery");
    const std::filesystem::path intents = root / "history-deletions";
    if (std::filesystem::exists(intents))
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(intents))
            if (entry.is_regular_file())
                throw std::runtime_error("unrecovered deletion intent after restart");
    std::cout << "STRESS-VERIFIED " << drafts.size() << std::endl;
}

void stressWriter(const std::filesystem::path& root) {
    using namespace aowis::dvmdb;
    ProjectService service(root);
    const std::string project = service.listProjects().at(0).id;
    std::cout << "STRESS-RUNNING" << std::endl;
    std::uint64_t iteration = 0;
    for (;;) {
        const std::string name = "transient-" + UuidV7::generate().toString();
        const BranchId branch = service.createUnbornBranch(project, name);
        const std::string id = branch.toString();
        (void)service.createWorkingCopyForBranch(project, id, std::nullopt,
                                                 UuidV7::generate().toString());
        service.deleteBranch(project, id, std::nullopt, true);
        ++iteration;
        if ((iteration & 31U) == 0)
            std::cout << "STRESS-OPS " << iteration << std::endl;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: aowis-dvmdb-vm-powercut-worker init|writer|verify|delete-init|delete-writer|delete-verify REPOSITORY [scenario]\n";
        return 2;
    }
    try {
        const std::string_view mode(argv[1]);
        if (mode == "stress-init") {
            stressInit(std::filesystem::path(argv[2]));
            return 0;
        }
        if (mode == "stress-writer") {
            stressWriter(std::filesystem::path(argv[2]));
            return 0;
        }
        if (mode == "stress-verify") {
            stressVerify(std::filesystem::path(argv[2]));
            return 0;
        }
        if (mode == "delete-init") {
            if (argc != 4) throw std::runtime_error("delete-init requires branch or truncate");
            initDeletion(std::filesystem::path(argv[2]), argv[3]);
            return 0;
        }
        if (mode == "delete-writer") {
            writeDeletion(std::filesystem::path(argv[2]));
            return 0;
        }
        if (mode == "delete-verify") {
            if (argc != 4 || (std::string_view(argv[3]) != "original" &&
                               std::string_view(argv[3]) != "deleted"))
                throw std::runtime_error("delete-verify requires original or deleted");
            verifyDeletion(std::filesystem::path(argv[2]), std::string_view(argv[3]) == "original");
            return 0;
        }
        if (mode != "init" && mode != "writer" && mode != "verify")
            throw std::runtime_error("unknown mode");
        const std::filesystem::path repositoryPath(argv[2]);
        // 'verify' must never create a missing repository and thereby disguise
        // lost storage or an accidentally unmounted data disk.
        if (mode == "verify" &&
            !std::filesystem::exists(repositoryPath / "objects.pack") &&
            !std::filesystem::exists(repositoryPath / "objects.current"))
            throw std::runtime_error("repository missing; is the test disk mounted?");
        ProjectRepository repository(repositoryPath);
        if (mode == "init") {
            repository.createBranch("main");
            (void)repository.commit("main", {Change{counterKey(), counterData(0)}});
            check(repository);
        } else if (mode == "verify") {
            check(repository);
        } else if (mode == "writer") {
            std::uint64_t next = readCounter(repository);
            // Host acknowledges each commit before allowing another. This
            // bounds crash-publication ambiguity to at most one revision.
            for (;;) {
                ++next;
                const aowis::dvmdb::RevisionId revision = repository.commit(
                    "main", {Change{counterKey(), counterData(next)}});
                // The immutable record digest identifies the acknowledged state.
                std::cout << "ACK " << next << " "
                          << aowis::dvmdb::toHex(repository.revision(revision).recordHash)
                          << std::endl;
                std::string proceed;
                if (!std::getline(std::cin, proceed) || proceed != "GO") break;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "VM power-cut worker failed: " << error.what() << '\n';
        return 1;
    }
}
