#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/project_service.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

using aowis::dvmdb::test::commitMetadata;

namespace {
using aowis::dvmdb::Key;
using aowis::dvmdb::ModelChange;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::ProjectId;
using aowis::dvmdb::ProjectInfo;
using aowis::dvmdb::ProjectService;
using aowis::dvmdb::Repository;
using aowis::dvmdb::RevisionId;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

struct TemporaryRoot {
    std::filesystem::path directory;
    TemporaryRoot() {
        char name[] = "/tmp/aowis-uuid-backup-XXXXXXXX";
        const char* created = ::mkdtemp(name);
        require(created != nullptr, "mkdtemp failed");
        this->directory = created;
    }
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(this->directory, ignored);
    }
};

Key key(std::uint8_t value) {
    Key result{};
    result[0] = value;
    return result;
}

ModelRecord model(std::uint16_t kind, std::size_t length, std::uint8_t fill) {
    ModelRecord result;
    result.kind = kind;
    result.payload.assign(length, fill);
    return result;
}

int offline(const std::string& python, const std::string& script,
            const std::string& verifier, const std::string& command,
            const std::filesystem::path& source, const std::filesystem::path& destination = {}) {
    const pid_t child = ::fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        std::vector<std::string> args{python, script, command, source.string()};
        if (!destination.empty()) args.push_back(destination.string());
        args.push_back("--verify-binary");
        args.push_back(verifier);
        std::vector<char*> pointers;
        for (std::string& item : args) pointers.push_back(item.data());
        pointers.push_back(nullptr);
        ::execvp(pointers.front(), pointers.data());
        ::_exit(127);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status),
            "offline tool terminated unexpectedly");
    return WEXITSTATUS(status);
}

struct BranchSnapshot {
    std::string name;
    std::string id;
    std::vector<RevisionId> ancestry;
    std::optional<ModelRecord> value;
};
struct ProjectSnapshot {
    ProjectInfo info;
    std::vector<BranchSnapshot> branches;
};

std::vector<ProjectSnapshot> capture(const std::filesystem::path& root,
                                     const std::vector<ProjectInfo>& expected) {
    ProjectService service(root);
    require(service.listProjects().size() == expected.size(), "project count mismatch");
    std::vector<ProjectSnapshot> snapshots;
    for (const ProjectInfo& info : expected) {
        require(service.project(info.id).name == info.name, "project name changed");
        const ProjectId project = *ProjectId::parse(info.id);
        ProjectSnapshot snapshot;
        snapshot.info = info;
        service.withRepository(info.id, [&](Repository& repository) {
            require(repository.verifyRepository().branches == 2, "invalid branch verification count");
            for (const std::string& name : repository.listBranches()) {
                BranchSnapshot branch;
                branch.name = name;
                const std::optional<aowis::dvmdb::BranchId> id = repository.branchId(name);
                require(id.has_value(), "branch lost its UUIDv7 identity");
                branch.id = id->toString();
                for (const aowis::dvmdb::RevisionGraphRevision& record :
                     repository.history(project, name, 100)) branch.ancestry.push_back(record.id);
                require(branch.ancestry.size() >= 2, "UUIDv7 first-parent history is incomplete");
                const Key lookup = name == "main" ? key(0x21) : key(0x31);
                branch.value = repository.getModelAtRevision(project, branch.ancestry.front(), lookup);
                require(branch.value.has_value(), "model record missing from UUIDv7 tip");
                snapshot.branches.push_back(std::move(branch));
            }
        });
        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

void compare(const std::filesystem::path& root, const std::vector<ProjectSnapshot>& expected) {
    std::vector<ProjectInfo> projects;
    for (const ProjectSnapshot& snapshot : expected) projects.push_back(snapshot.info);
    const std::vector<ProjectSnapshot> found = capture(root, projects);
    require(found.size() == expected.size(), "restored project count differs");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(found[i].branches.size() == expected[i].branches.size(), "restored branches differ");
        for (std::size_t j = 0; j < expected[i].branches.size(); ++j) {
            const BranchSnapshot& before = expected[i].branches[j];
            const BranchSnapshot& after = found[i].branches[j];
            require(after.name == before.name && after.id == before.id,
                    "restored branch name or UUID changed");
            require(after.ancestry == before.ancestry, "restored UUIDv7 ancestry differs");
            require(after.value->kind == before.value->kind &&
                    after.value->payload == before.value->payload &&
                    after.value->references == before.value->references,
                    "restored model bytes differ");
        }
    }
}

void exercise(const std::string& python, const std::string& script,
              const std::string& verifier) {
    TemporaryRoot temporary;
    const std::filesystem::path source = temporary.directory / "source";
    const std::filesystem::path backup = temporary.directory / "backup";
    const std::filesystem::path restored = temporary.directory / "restored";
    const std::filesystem::path secondBackup = temporary.directory / "second-backup";
    const std::filesystem::path secondRestored = temporary.directory / "second-restored";

    std::vector<ProjectInfo> projects;
    std::vector<RevisionId> historical;
    {
        ProjectService service(source);
        for (int index = 0; index < 4; ++index) {
            const ProjectInfo info = service.createProject("Current project " + std::to_string(index), commitMetadata());
            projects.push_back(info);
            const ProjectId project = *ProjectId::parse(info.id);
            const RevisionId initial = service.head(info.id, "main");
            const RevisionId fork = service.createBranchAtRevision(info.id, "feature", "main", initial);
            require(fork == initial, "fork invented a revision");
            const ModelRecord first = model(100, 16, static_cast<std::uint8_t>(index));
            historical.push_back(service.commitModel(info.id, "main", {{key(0x21), first}}, commitMetadata(), initial));
            const ModelRecord feature = model(101, 19, static_cast<std::uint8_t>(index + 1));
            (void)service.commitModel(info.id, "feature", {{key(0x31), feature}}, commitMetadata(), initial);
            const std::size_t length = index == 3 ? 2 * 1024 * 1024 : 32 + static_cast<std::size_t>(index);
            const ModelRecord latest = model(102, length, static_cast<std::uint8_t>(index + 2));
            (void)service.commitModel(info.id, "main", {{key(0x21), latest}}, commitMetadata(), historical.back());
            service.withRepository(info.id, [&](Repository& repository) {
                require(repository.getModelAtRevision(project, historical.back(), key(0x21))->payload == first.payload,
                        "historical immutable model snapshot changed");
            });
        }
    }
    const std::vector<ProjectSnapshot> before = capture(source, projects);
    require(offline(python, script, verifier, "backup", source, backup) == 0,
            "UUIDv7 project backup failed");
    require(offline(python, script, verifier, "restore", backup, restored) == 0,
            "UUIDv7 project restore failed");
    compare(restored, before);
    require(offline(python, script, verifier, "verify", restored) == 0,
            "offline verification of restored UUIDv7 projects failed");

    {
        ProjectService service(restored);
        const ProjectId project = *ProjectId::parse(projects[0].id);
        const RevisionId expected = service.head(projects[0].id, "main");
        (void)service.commitModel(projects[0].id, "main",
                                  {{key(0x61), model(103, 18, 0x59)}}, commitMetadata(), expected);
        service.withRepository(projects[0].id, [&](Repository& repository) {
            const std::optional<ModelRecord> old =
                repository.getModelAtRevision(project, historical[0], key(0x21));
            require(old.has_value() && old->payload.size() == 16,
                    "historical UUIDv7 data changed after restore and commit");
            repository.verifyRepository();
        });
    }
    const std::vector<ProjectSnapshot> after = capture(restored, projects);
    require(after[0].branches[1].ancestry.size() == before[0].branches[1].ancestry.size() + 1,
            "post-restore commit was not durable");
    compare(backup, before); // independently restored copy did not mutate the first backup
    require(offline(python, script, verifier, "backup", restored, secondBackup) == 0,
            "second backup after UUIDv7 commit failed");
    require(offline(python, script, verifier, "restore", secondBackup, secondRestored) == 0,
            "second UUIDv7 restore failed");
    compare(secondRestored, after);
    compare(source, before);
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 4, "expected Python interpreter, backup script and verifier");
        exercise(argv[1], argv[2], argv[3]);
        std::cout << "UUIDv7 multi-project upgrade/restart/restore tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UUIDv7 multi-project backup regression failed: " << error.what() << '\n';
        return 1;
    }
}
