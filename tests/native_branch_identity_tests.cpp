#include "aowis/dvmdb/native_store.h"
#include "project_repository.h"
#include "aowis/dvmdb/project_service.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

using aowis::dvmdb::test::commitMetadata;

namespace {
using aowis::dvmdb::BranchId;
using aowis::dvmdb::Bytes;
using aowis::dvmdb::Change;
using aowis::dvmdb::Key;
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::ProjectService;
using aowis::dvmdb::Repository;
using aowis::dvmdb::test::ProjectRepository;
using aowis::dvmdb::StorageError;
using aowis::dvmdb::StorageErrorKind;

void check(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}

struct TemporaryRoot {
    TemporaryRoot() {
        std::string name = (std::filesystem::temp_directory_path() /
                            "aowis-uuid-branches-XXXXXX").string();
        const char* result = ::mkdtemp(name.data());
        check(result != nullptr, "unable to make temporary repository");
        this->directory = result;
    }
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(this->directory, ignored);
    }
    std::filesystem::path directory;
};

// The first branch starts unborn; give it one committed revision.
RevisionId firstCommit(ProjectRepository& repository, const char* branch = "main") {
    Key key{};
    key[15] = 1;
    return repository.commit(branch, {Change{key, Bytes{1}}});
}

template<class Function>
void expectKind(Function&& task, StorageErrorKind kind, const char* message) {
    bool rejected = false;
    try { task(); }
    catch (const StorageError& error) { rejected = error.kind() == kind; }
    check(rejected, message);
}

void testFilenameAndAtomicRename() {
    TemporaryRoot folder;
    const std::filesystem::path directory = folder.directory / "repository";
    std::string identity;
    RevisionId revision = RevisionId::generate();
    {
        ProjectRepository repository(directory);
        repository.createBranch("main");
        identity = repository.branchId("main")->toString();
        check(!repository.optionalHead("main"), "new branch is not unborn");
        revision = firstCommit(repository);
        check(repository.branchId("main")->toString() == identity,
              "first commit changed branch UUID");
        check(BranchId::parse(identity).has_value(), "branch UUID is invalid");
        const std::filesystem::path heads = directory / "refs" / "heads";
        check(std::filesystem::exists(heads / identity), "branch UUID filename missing");
        check(!std::filesystem::exists(heads / "main"), "user name leaked into filesystem path");
        check(!std::filesystem::exists(directory / "refs" / "branch-ids"), "old sidecar directory created");
        repository.createBranch("feature", "main");
        check(repository.head("feature") == revision, "branch creation invented a revision");
        repository.renameBranch("main", "renamed", revision);
        check(!repository.hasBranch("main") && repository.hasBranch("renamed"),
              "rename failed to update advertised branch name");
        check(repository.branchId("renamed")->toString() == identity,
              "rename replaced branch UUID");
        check(repository.head("renamed") == revision, "rename changed immutable branch HEAD");
        check(std::filesystem::exists(heads / identity), "rename moved UUID-named branch file");
        check(repository.verifyRepository().branches == 2, "renamed repository did not verify");
        expectKind([&]() { repository.renameBranch("renamed", "feature", revision); },
                   StorageErrorKind::Conflict, "duplicate branch name accepted");
        const RevisionId wrong = RevisionId::generate();
        expectKind([&]() { repository.renameBranch("renamed", "different", wrong); },
                   StorageErrorKind::Conflict, "stale HEAD accepted");
        expectKind([&]() { repository.renameBranch("renamed", "../escape", revision); },
                   StorageErrorKind::InvalidInput, "path-like user name accepted");
        check(repository.branchId("renamed")->toString() == identity,
              "rejected rename affected branch identity");
    }
    {
        ProjectRepository repository(directory);
        check(repository.branchId("renamed")->toString() == identity,
              "reopening changed UUID after rename");
        check(repository.head("renamed") == revision, "reopening changed HEAD");
        repository.createBranchAt("history", "renamed", revision);
        check(repository.branchId("history") != repository.branchId("renamed"),
              "historical branching recycled UUID");
        check(repository.head("history") == revision, "history fork changed source HEAD");
        (void)repository.compact();
        check(repository.branchId("renamed")->toString() == identity,
              "compaction changed stable branch identity");
    }
}

void testCorruptAndObsoleteFilesRejected() {
    TemporaryRoot folder;
    const std::filesystem::path path = folder.directory / "repo";
    {
        ProjectRepository repository(path);
        repository.createBranch("main");
        (void)firstCommit(repository);
        const std::string id = repository.branchId("main")->toString();
        const std::filesystem::path file = path / "refs" / "heads" / id;
        {
            std::ofstream corrupt(file, std::ios::binary | std::ios::trunc);
            corrupt << "garbage\n";
        }
        expectKind([&]() { (void)repository.verifyRepository(); },
                   StorageErrorKind::Corruption, "corrupt branch record accepted");
    }
    expectKind([&]() { ProjectRepository repository(path); }, StorageErrorKind::Corruption,
               "corrupt branch accepted on reopen");
    std::filesystem::remove_all(path);
    {
        ProjectRepository repository(path);
        repository.createBranch("main");
        const std::string id = repository.branchId("main")->toString();
        const std::filesystem::path dir = path / "refs" / "heads";
        std::filesystem::rename(dir / id, dir / "main");
        expectKind([&]() { (void)repository.listBranches(); }, StorageErrorKind::Corruption,
                   "obsolete name-keyed branch accepted");
    }
    expectKind([&]() { ProjectRepository repository(path); }, StorageErrorKind::Corruption,
               "obsolete repository accepted on reopen");
}

void testDuplicateNamesAndSymlinksRejected() {
    TemporaryRoot folder;
    const std::filesystem::path path = folder.directory / "repo";
    ProjectRepository repository(path);
    repository.createBranch("main");
    (void)firstCommit(repository);
    repository.createBranch("feature", "main");
    const std::filesystem::path heads = path / "refs" / "heads";
    const std::string first = repository.branchId("main")->toString();
    const std::string second = repository.branchId("feature")->toString();
    std::filesystem::copy_file(heads / first, heads / second,
                               std::filesystem::copy_options::overwrite_existing);
    expectKind([&]() { (void)repository.verifyRepository(); }, StorageErrorKind::Corruption,
               "duplicate name in UUID records accepted");
    std::filesystem::remove(heads / second);
    std::filesystem::create_symlink(heads / first, heads / second);
    expectKind([&]() { (void)repository.verifyRepository(); }, StorageErrorKind::Corruption,
               "symlinked UUID branch file accepted");
}

void testService() {
    TemporaryRoot folder;
    std::string project;
    std::string id;
    {
        ProjectService service(folder.directory);
        project = service.createUnbornProject("Inspect branch rename").id;
        id = service.branchId(project, "main")->toString();
        Key key{};
        key[15] = 2;
        aowis::dvmdb::ModelRecord record;
        record.kind = 1;
        record.payload = {2};
        const RevisionId head = service.commitModel(project, "main", {{key, record}}, commitMetadata());
        service.renameBranch(project, "main", "develop", head);
        check(service.branchId(project, "develop")->toString() == id,
              "service rename changed branch UUID");
    }
    {
        ProjectService service(folder.directory);
        check(service.branchId(project, "develop")->toString() == id,
              "service reopen forgot branch rename");
    }
}
}

int main() {
    try {
        testFilenameAndAtomicRename();
        testCorruptAndObsoleteFilesRejected();
        testDuplicateNamesAndSymlinksRejected();
        testService();
        std::cout << "UUIDv7 branch storage, atomic rename, and rejection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "branch UUID storage tests FAILED: " << error.what() << '\n';
        return 1;
    }
}
