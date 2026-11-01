#include "commit_metadata_fixture.h"
#include "model_edit_event_fixture.h"
#include "aowis/dvmdb/project_service.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using aowis::dvmdb::test::commitMetadata;

namespace {
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::ProjectId;
using aowis::dvmdb::Key;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::ProjectInfo;
using aowis::dvmdb::ProjectService;
using aowis::dvmdb::Repository;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

struct TemporaryRoot {
    std::filesystem::path directory;
    TemporaryRoot() {
        char value[] = "/tmp/aowis-backup-restore-XXXXXXXX";
        const char* created = ::mkdtemp(value);
        require(created != nullptr, "mkdtemp failed");
        this->directory = created;
    }
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(this->directory, ignored);
    }
};

int run(const std::string& python, const std::string& script, const std::string& verify,
        const std::string& command, const std::filesystem::path& source,
        const std::filesystem::path& destination = {}) {
    const pid_t pid = ::fork();
    require(pid >= 0, "fork failed");
    if (pid == 0) {
        std::vector<std::string> parameters{python, script, command, source.string()};
        if (!destination.empty()) parameters.push_back(destination.string());
        parameters.push_back("--verify-binary");
        parameters.push_back(verify);
        std::vector<char*> args;
        for (std::string& value : parameters) args.push_back(value.data());
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    int status = 0;
    require(::waitpid(pid, &status, 0) == pid && WIFEXITED(status), "backup process failed abnormally");
    return WEXITSTATUS(status);
}

struct Snapshot {
    std::string id;
    std::string name;
    std::string branch;
    std::optional<RevisionId> main;
    std::optional<RevisionId> alternate;
    std::string mainBranchId;
    std::string alternateBranchId;
};

void compare(const std::filesystem::path& root, const std::vector<Snapshot>& expected,
             const std::string& expectedRepositoryId) {
    ProjectService service(root);
    require(service.repositoryId() == expectedRepositoryId,
            "backup/restore changed the durable repository UUIDv7");
    const std::vector<ProjectInfo> projects = service.listProjects();
    require(projects.size() == expected.size(), "restored project count mismatch");
    const std::vector<aowis::dvmdb::WorkingCopyInfo> drafts = service.listWorkingCopies();
    require(drafts.size() == 1, "restored draft count mismatch");
    const aowis::dvmdb::WorkingCopyInfo draft = service.workingCopy(drafts.front().id);
    require(draft.projectId == expected.front().id &&
            draft.branchId == expected.front().mainBranchId &&
            draft.baseRevision == expected.front().main &&
            draft.generation == 1 && draft.dirty &&
            draft.candidateChanges.size() == 1 &&
            draft.candidateChanges.front().record.has_value() &&
            draft.candidateChanges.front().record->payload ==
                std::vector<std::uint8_t>({42, 43}) &&
            draft.journal.size() == 1 && draft.journalCount == 1 &&
            draft.journal.front().encoded.size() > 32,
            "backup/restore lost the saved private candidate overlay");
    for (const Snapshot& item : expected) {
        const ProjectInfo project = service.project(item.id);
        require(project.name == item.name, "restored project identity mismatch");
        require(service.head(item.id, "main") == item.main, "restored main head mismatch");
        require(service.head(item.id, item.branch) == item.alternate,
                "restored secondary branch head mismatch");
        require(service.history(item.id, "main", 30).size() == 2,
                "restored revision history mismatch");
        const std::optional<aowis::dvmdb::BranchId> mainId = service.branchId(item.id, "main");
        const std::optional<aowis::dvmdb::BranchId> alternateId =
            service.branchId(item.id, item.branch);
        require(mainId.has_value() && mainId->toString() == item.mainBranchId,
                "backup/restore changed the main branch UUIDv7");
        require(alternateId.has_value() && alternateId->toString() == item.alternateBranchId,
                "backup/restore changed the secondary branch UUIDv7");
    }
}

void tests(const std::string& python, const std::string& script, const std::string& verify) {
    TemporaryRoot temporary;
    const std::filesystem::path source = temporary.directory / "source";
    const std::filesystem::path backup = temporary.directory / "backup";
    const std::filesystem::path restored = temporary.directory / "restored";
    const std::filesystem::path reserved = temporary.directory / "reserved";
    std::vector<Snapshot> expected;
    std::string originalRepositoryId;
    std::string remoteRepositoryId;
    {
        ProjectService service(source);
        originalRepositoryId = service.repositoryId();
        // Tracking metadata is part of a verified offline snapshot, but it
        // must never change a local branch's mutable HEAD.
        remoteRepositoryId = aowis::dvmdb::UuidV7::generate().toString();
        service.configureRepositoryRole(aowis::dvmdb::RepositoryRole::Private);
        service.registerRemoteRepository(remoteRepositoryId,
                                            "https://remote.example.test/dvmdb/v1");
        service.recordRemoteTrackingRef({remoteRepositoryId,
              ProjectId::generate().toString(), aowis::dvmdb::BranchId::generate().toString(),
              std::nullopt}, std::nullopt);
        for (int index = 0; index < 3; ++index) {
            const ProjectInfo info = service.createProject("Hydraulic project " + std::to_string(index), commitMetadata());
            const RevisionId before = service.head(info.id, "main");
            const std::string secondary = index == 1 ? "Scénario Süd 2026" : "design";
            service.createBranchAtRevision(info.id, secondary, "main", before);
            Key key{};
            key[0] = static_cast<std::uint8_t>(index + 1);
            ModelRecord value;
            value.kind = static_cast<std::uint16_t>(30 + index);
            value.payload = {1, 2, static_cast<std::uint8_t>(index)};
            std::optional<RevisionId> after;
            service.withRepository(info.id, [&](Repository& repository) {
                const ProjectId project = *ProjectId::parse(info.id);
                after = repository.commitModel(project, "main", {{key, value}}, commitMetadata(), before);
            });
            expected.push_back({info.id, info.name, secondary, *after, before,
                                service.branchId(info.id, "main")->toString(),
                                service.branchId(info.id, secondary)->toString()});
            if (index == 0) {
                const std::string session = aowis::dvmdb::UuidV7::generate().toString();
                const aowis::dvmdb::WorkingCopyInfo draft =
                    service.createWorkingCopy(info.id, "main", after, session);
                require(draft.baseRevision == after, "could not create backup draft fixture");
                const aowis::dvmdb::WorkingCopyLease lease =
                    service.acquireWorkingCopyLease(draft.id, session, 120);
                Key candidateKey{};
                candidateKey[15] = 9;
                ModelRecord candidate;
                candidate.kind = 44;
                candidate.payload = {42, 43};
                const aowis::dvmdb::Bytes event = aowis::dvmdb::test::editEvent(
                    aowis::dvmdb::UuidV7::generate(), aowis::dvmdb::UuidV7::generate());
                const aowis::dvmdb::WorkingCopyInfo saved =
                    service.saveWorkingCopyTransactions(draft.id, session, lease.token,
                                         lease.generation, {{candidateKey, candidate}}, {event});
                require(saved.generation == 1 && saved.dirty,
                        "failed to save private draft before backup");
                service.releaseWorkingCopyLease(draft.id, session, lease.token);
            }
        }
        // The catalogue is open; an offline snapshot must refuse it promptly.
        require(run(python, script, verify, "backup", source, reserved) != 0,
                "backup incorrectly accepted live project service");
        require(!std::filesystem::exists(reserved), "live-root rejection published a backup");
    }
    {
        Repository open(source / "projects" / expected[0].id / "repository");
        require(run(python, script, verify, "backup", source, reserved) != 0,
                "backup incorrectly accepted a direct native-library reader");
        require(!std::filesystem::exists(reserved), "direct handle rejection published backup");
    }
    require(run(python, script, verify, "backup", source, backup) == 0,
            "offline backup creation failed");
    compare(backup, expected, originalRepositoryId);
    {
        ProjectService snapshot(backup);
        require(snapshot.repositoryRole() == aowis::dvmdb::RepositoryRole::Private &&
                snapshot.remoteRepositories().size() == 1 &&
                snapshot.remoteRepositories().front().repositoryId == remoteRepositoryId &&
                snapshot.remoteTrackingRefs().size() == 1 &&
                !snapshot.remoteTrackingRefs().front().observedHead.has_value(),
                "offline backup lost qualified remote tracking or unborn observation");
    }
    require(run(python, script, verify, "verify", backup) == 0,
            "offline backup verification failed");
    require(run(python, script, verify, "restore", backup, restored) == 0,
            "verified restore failed");
    compare(restored, expected, originalRepositoryId);
    {
        ProjectService snapshot(restored);
        require(snapshot.repositoryRole() == aowis::dvmdb::RepositoryRole::Private &&
                snapshot.remoteRepositories().size() == 1 &&
                snapshot.remoteTrackingRefs().size() == 1 &&
                !snapshot.remoteTrackingRefs().front().observedHead.has_value(),
                "offline restore lost remote tracking metadata");
    }
    const std::filesystem::path backupTracking = backup / "repository.tracking";
    std::ifstream trackingInput(backupTracking, std::ios::binary);
    const std::string originalTracking((std::istreambuf_iterator<char>(trackingInput)),
                                        std::istreambuf_iterator<char>());
    {
        std::ofstream corrupted(backupTracking, std::ios::binary | std::ios::trunc);
        corrupted << "AOWIS-TRACKING-1\nrole\tprivate\nref\tinvalid\n";
    }
    const std::filesystem::path badTrackingRestore = temporary.directory / "bad-tracking";
    require(run(python, script, verify, "restore", backup, badTrackingRestore) != 0,
            "offline restore accepted malformed remote tracking metadata");
    require(!std::filesystem::exists(badTrackingRestore),
            "offline restore published malformed remote tracking metadata");
    {
        std::ofstream reset(backupTracking, std::ios::binary | std::ios::trunc);
        reset << originalTracking;
    }
    // A malformed draft must never be backed up or restored as usable private data.
    const aowis::dvmdb::WorkingCopyInfo draftInfo = [&]() {
        ProjectService store(backup);
        return store.listWorkingCopies().front();
    }();
    const std::filesystem::path draftDirectory = backup / "working-copies" / draftInfo.id;
    const std::function<std::string(const std::filesystem::path&)> readAll =
        [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    // The data files the meta names (candidate-<uuid>, journal-<uuid>).
    std::vector<std::filesystem::path> draftFiles{draftDirectory / "meta"};
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(draftDirectory))
        if (entry.path().filename() != "meta") draftFiles.push_back(entry.path());
    require(draftFiles.size() == 3, "backup draft lacks its candidate and journal files");
    for (std::size_t index = 0; index < draftFiles.size(); ++index) {
        const std::filesystem::path& damaged = draftFiles[index];
        const std::string original = readAll(damaged);
        {
            std::string altered = original;
            if (index == 0) altered = "AOWIS-WORKING-COPY-1\nproject\tbroken\n";
            else altered[altered.size() - 1] = static_cast<char>(altered.back() ^ 0x01);
            std::ofstream invalid(damaged, std::ios::binary | std::ios::trunc);
            invalid << altered;
        }
        const std::filesystem::path invalidDraftRestore =
            temporary.directory / ("bad-draft-" + std::to_string(index));
        require(run(python, script, verify, "restore", backup, invalidDraftRestore) != 0,
                "offline restore accepted a corrupt private working-copy file");
        require(!std::filesystem::exists(invalidDraftRestore),
                "offline restore published a corrupt working copy");
        std::ofstream reset(damaged, std::ios::binary | std::ios::trunc);
        reset << original;
    }
    // A backup with a corrupted catalogue-root identity must fail closed;
    // restore must never invent a different UUID or publish such a root.
    const std::filesystem::path backupIdentity = backup / "repository.id";
    {
        std::ofstream corrupted(backupIdentity, std::ios::binary | std::ios::trunc);
        corrupted << "AOWIS-REPOSITORY-1\nnot-a-uuid\n";
    }
    const std::filesystem::path badRepositoryRestore = temporary.directory / "bad-repository-identity";
    require(run(python, script, verify, "restore", backup, badRepositoryRestore) != 0,
            "invalid repository UUIDv7 was accepted during restore");
    require(!std::filesystem::exists(badRepositoryRestore),
            "invalid repository identity was published during restore");
    std::filesystem::copy_file(source / "repository.id", backupIdentity,
                               std::filesystem::copy_options::overwrite_existing);
    // Corrupt/duplicated identity metadata must fail the same full verifier
    // used by offline backup/restore, even though every revision is valid.
    const std::filesystem::path refs = backup / "projects" / expected[0].id /
                                       "repository" / "refs" / "heads";
    const std::filesystem::path sourceId = refs / expected[0].mainBranchId;
    const std::filesystem::path destinationId = refs / expected[0].alternateBranchId;
    // A copied record now duplicates the *name*, while its filename remains
    // the stable original UUID. Full verification must reject this corruption.
    std::filesystem::copy_file(sourceId, destinationId,
                               std::filesystem::copy_options::overwrite_existing);
    const std::filesystem::path badIdentityRestore = temporary.directory / "bad-identity";
    require(run(python, script, verify, "restore", backup, badIdentityRestore) != 0,
            "duplicate branch name survived restore verification");
    require(!std::filesystem::exists(badIdentityRestore),
            "invalid branch identity restore was published");
    // Restore the backup's authoritative identity bytes before later checks.
    std::filesystem::copy_file(source / "projects" / expected[0].id /
                               "repository" / "refs" / "heads" / expected[0].alternateBranchId,
                               destinationId, std::filesystem::copy_options::overwrite_existing);
    require(run(python, script, verify, "backup", source, backup) != 0,
            "existing destination was overwritten");
    compare(backup, expected, originalRepositoryId);
    // Pre-release SHA-head records are not eligible for canonical backups.
    const std::filesystem::path shaRef = source / "projects" / expected[0].id /
                                          "repository" / "refs" / "heads" / expected[0].mainBranchId;
    const std::string originalRef = [&] {
        std::ifstream input(shaRef, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    }();
    {
        std::ofstream output(shaRef, std::ios::binary | std::ios::trunc);
        output << "AOWIS-BRANCH-1\nmain\n" << std::string(64, 'a') << "\n";
    }
    const std::filesystem::path obsolete = temporary.directory / "obsolete";
    require(run(python, script, verify, "backup", source, obsolete) != 0,
            "SHA-revision branch was accepted into UUIDv7-only backup");
    require(!std::filesystem::exists(obsolete), "obsolete backup was published");
    {
        std::ofstream output(shaRef, std::ios::binary | std::ios::trunc);
        output << originalRef;
    }
    // A syntactically valid UUIDv7 HEAD with a forged record digest must
    // fail full immutable-record verification before destination publication.
    {
        std::string forged = originalRef;
        require(forged.size() > 66, "unexpected short BRANCH-2 record");
        const std::size_t lastLine = forged.rfind('\n', forged.size() - 2);
        require(lastLine != std::string::npos && forged.size() - lastLine == 66,
                "unexpected BRANCH-2 digest layout");
        forged[lastLine + 1] = forged[lastLine + 1] == 'a' ? 'b' : 'a';
        std::ofstream output(shaRef, std::ios::binary | std::ios::trunc);
        output << forged;
    }
    const std::filesystem::path forgedDestination = temporary.directory / "forged-checksum";
    require(run(python, script, verify, "backup", source, forgedDestination) != 0,
            "forged UUIDv7 revision checksum was accepted");
    require(!std::filesystem::exists(forgedDestination), "forged backup was published");
    {
        std::ofstream output(shaRef, std::ios::binary | std::ios::trunc);
        output << originalRef;
    }
    // Backups must validate immutable BRANCH-5 fork metadata, not merely
    // recognize its marker. Invalid fork UUIDs must prevent publication.
    const std::filesystem::path forkRef = source / "projects" / expected[0].id /
                                          "repository" / "refs" / "heads" / expected[0].alternateBranchId;
    const std::string originalForkRef = [&] {
        std::ifstream input(forkRef, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    }();
    require(originalForkRef.starts_with("AOWIS-BRANCH-5\n"),
            "new historical fork did not use the durable BRANCH-5 record");
    {
        std::string forged = originalForkRef;
        std::size_t forkStart = 0;
        for (int line = 0; line < 4; ++line) {
            forkStart = forged.find('\n', forkStart);
            require(forkStart != std::string::npos, "incomplete BRANCH-5 fork record");
            ++forkStart;
        }
        require(forkStart + 36 < forged.size() && forged[forkStart + 36] == '\n',
                "unexpected BRANCH-5 fork UUID layout");
        forged.replace(forkStart, 36, std::string(36, 'x'));
        std::ofstream output(forkRef, std::ios::binary | std::ios::trunc);
        output << forged;
    }
    const std::filesystem::path badFork = temporary.directory / "bad-fork";
    require(run(python, script, verify, "backup", source, badFork) != 0,
            "malformed immutable fork UUID was accepted into offline backup");
    require(!std::filesystem::exists(badFork), "invalid fork backup was published");
    {
        std::ofstream output(forkRef, std::ios::binary | std::ios::trunc);
        output << originalForkRef;
    }
    const std::filesystem::path hidden = shaRef.parent_path() / ".unexpected";
    {
        std::ofstream output(hidden, std::ios::binary);
        output << "not a branch";
    }
    const std::filesystem::path hiddenDestination = temporary.directory / "hidden-entry";
    require(run(python, script, verify, "backup", source, hiddenDestination) != 0,
            "unknown hidden branch entry was silently ignored");
    require(!std::filesystem::exists(hiddenDestination), "invalid hidden-entry backup was published");
    std::filesystem::remove(hidden);
    // A source with damaged committed refs must not yield a published backup.
    const std::filesystem::path ref = source / "projects" / expected[0].id / "repository" /
                                      "refs" / "heads" / expected[0].mainBranchId;
    const std::filesystem::path corrupt = temporary.directory / "corrupt";
    std::filesystem::resize_file(ref, 4);
    require(run(python, script, verify, "backup", source, corrupt) != 0,
            "corrupt branch was accepted");
    require(!std::filesystem::exists(corrupt), "corrupt snapshot was published");
    // Reject a symlink anywhere in the root, including non-repository files.
    const std::filesystem::path link = backup / "unsafe-link";
    std::filesystem::create_symlink("/tmp", link);
    const std::filesystem::path unsafe = temporary.directory / "unsafe";
    require(run(python, script, verify, "restore", backup, unsafe) != 0,
            "symlink in backup was accepted");
    require(!std::filesystem::exists(unsafe), "unsafe restore was published");
    std::filesystem::remove(link);
    // A backup with unreadable physical storage must not be restored merely
    // because its catalogue metadata and branch refs still look correct.
    const std::filesystem::path pack = backup / "projects" / expected[1].id /
                                        "repository" / "objects.pack";
    require(std::filesystem::is_regular_file(pack), "expected physical pack is absent");
    std::filesystem::resize_file(pack, 17);
    const std::filesystem::path broken = temporary.directory / "broken";
    require(run(python, script, verify, "restore", backup, broken) != 0,
            "physically corrupted backup was restored");
    require(!std::filesystem::exists(broken), "corrupt restore was published");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 4, "expected Python interpreter, script and verifier arguments");
        tests(argv[1], argv[2], argv[3]);
        std::cout << "Offline multi-project backup and restore tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Backup/restore test failed: " << error.what() << '\n';
        return 1;
    }
}
