#include "commit_metadata_fixture.h"
#include "model_edit_event_fixture.h"
#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/object_id.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <chrono>
#include <thread>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

using aowis::dvmdb::test::commitMetadata;

namespace {
using aowis::dvmdb::BranchId;
using aowis::dvmdb::Key;
using aowis::dvmdb::ModelChange;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::ProjectError;
using aowis::dvmdb::ProjectErrorKind;
using aowis::dvmdb::ProjectInfo;
using aowis::dvmdb::ProjectService;
using aowis::dvmdb::RepositoryRole;
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::UuidV7;
using aowis::dvmdb::WorkingCopyInfo;

void require(bool accepted, const char *message) {
    if (!accepted) throw std::runtime_error(message);
}

template <typename Action>
void expectError(Action action, ProjectErrorKind kind) {
    try { action(); }
    catch (const ProjectError& error) {
        require(error.kind() == kind, "wrong working-copy error category");
        return;
    }
    throw std::runtime_error("expected working-copy error");
}

struct TemporaryRoot {
    TemporaryRoot() {
        std::string pattern = (std::filesystem::temp_directory_path() /
            "aowis-working-copy-XXXXXXXX").string();
        std::vector<char> letters(pattern.begin(), pattern.end());
        letters.push_back('\0');
        const char *created = ::mkdtemp(letters.data());
        require(created != nullptr, "failed to create temporary DVMDB root");
        this->path = created;
    }
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(this->path, ignored);
    }
    std::filesystem::path path;
};

// File name the draft's meta record references for `field` (candidate or
// journal); "-" when there is none.
std::string draftFileName(const std::filesystem::path& root, const std::string& draftId,
                          const std::string& field) {
    std::ifstream meta(root / "working-copies" / draftId / "meta", std::ios::binary);
    std::string line;
    while (std::getline(meta, line)) {
        if (!line.starts_with(field + "\t")) continue;
        const std::size_t start = field.size() + 1;
        return line.substr(start, line.find('\t', start) - start);
    }
    throw std::runtime_error("draft meta lacks field " + field);
}

ModelChange simpleChange() {
    Key key{};
    key[15] = 1;
    ModelRecord record;
    record.kind = 1;
    record.payload = {12};
    return {key, record};
}

void testWorkingCopyCreationAndRecovery() {
    TemporaryRoot root;
    std::string projectId;
    std::string branchId;
    std::string firstId;
    std::string secondId;
    std::string committedId;
    RevisionId committed = RevisionId::generate();
    const std::string sessionOne = UuidV7::generate().toString();
    const std::string sessionTwo = UuidV7::generate().toString();
    {
        ProjectService service(root.path);
        const ProjectInfo project = service.createUnbornProject("checkout from unborn");
        projectId = project.id;
        branchId = service.branchId(projectId, "main")->toString();
        require(service.listWorkingCopies().empty(), "new root contains drafts");
        expectError([&]() { service.createWorkingCopy(projectId, "main", std::nullopt, sessionOne); },
                    ProjectErrorKind::Conflict);
        service.configureRepositoryRole(RepositoryRole::Private);
        const WorkingCopyInfo first = service.createWorkingCopy(projectId, "main", std::nullopt, sessionOne);
        const WorkingCopyInfo second = service.createWorkingCopy(projectId, "main", std::nullopt, sessionTwo);
        firstId = first.id;
        secondId = second.id;
        require(first.id != second.id && UuidV7::parse(first.id).has_value(),
                "distinct sessions did not receive distinct draft UUIDv7s");
        require(first.projectId == projectId && first.branchId == branchId &&
                first.sessionId == sessionOne && !first.baseRevision.has_value() &&
                !first.dirty && first.generation == 0,
                "unborn draft metadata incorrect");
        require(service.listWorkingCopies().size() == 2 &&
                !service.optionalHead(projectId, "main").has_value() &&
                service.revisionGraph(projectId).revisions.empty(),
                "draft creation modified committed ancestry");
        expectError([&]() { service.workingCopy(sessionOne); }, ProjectErrorKind::NotFound);
        expectError([&]() { service.workingCopy("not-a-uuid"); }, ProjectErrorKind::InvalidInput);
        committed = service.commitModel(projectId, "main", {simpleChange()}, commitMetadata());
        committedId = service.createWorkingCopy(projectId, "main", committed, sessionOne).id;
        expectError([&]() { service.createWorkingCopy(projectId, "main", std::nullopt, sessionTwo); },
                    ProjectErrorKind::Conflict);
        require(service.optionalHead(projectId, "main") == committed,
                "working-copy creation moved committed HEAD");
    }
    {
        ProjectService reopened(root.path);
        require(reopened.repositoryRole() == RepositoryRole::Private &&
                reopened.listWorkingCopies().size() == 3,
                "working copies not recovered after restart");
        const WorkingCopyInfo first = reopened.workingCopy(firstId);
        const WorkingCopyInfo second = reopened.workingCopy(secondId);
        const WorkingCopyInfo third = reopened.workingCopy(committedId);
        require(first.projectId == projectId && first.branchId == branchId &&
                first.sessionId == sessionOne && !first.baseRevision &&
                second.sessionId == sessionTwo && !second.baseRevision &&
                third.baseRevision == committed && third.generation == 0 &&
                reopened.optionalHead(projectId, "main") == committed,
                "recovered drafts lost pinned base or identity");
        const std::filesystem::path stages = root.path / "working-copies" /
            (".creating-" + UuidV7::generate().toString());
        std::filesystem::create_directory(stages);
        std::ofstream(stages / "meta") << "not-published";
        require(reopened.listWorkingCopies().size() == 3,
                "unpublished draft staging was incorrectly advertised");
    }
    // Corruption must never be silently treated as a missing draft.
    {
        std::ofstream altered(root.path / "working-copies" / firstId / "meta",
                              std::ios::binary | std::ios::trunc);
        altered << "invalid draft contents\n";
    }
    {
        ProjectService reopened(root.path);
        bool rejected = false;
        try { (void)reopened.listWorkingCopies(); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "corrupt draft was silently accepted");
    }
}
void testLeasedTransactionalCandidateSaves() {
    TemporaryRoot root;
    const std::string writerA = UuidV7::generate().toString();
    const std::string writerB = UuidV7::generate().toString();
    std::string projectId;
    std::string draftId;
    std::string staleToken;
    Key firstKey{};
    firstKey[15] = 4;
    Key secondKey{};
    secondKey[15] = 8;
    ModelRecord firstRecord;
    firstRecord.kind = 12;
    firstRecord.payload = {5, 6, 7};
    ModelRecord secondRecord;
    secondRecord.kind = 15;
    secondRecord.payload = {8, 9};
    {
        ProjectService service(root.path);
        service.configureRepositoryRole(RepositoryRole::Private);
        projectId = service.createUnbornProject("private editor saves").id;
        draftId = service.createWorkingCopy(projectId, "main", std::nullopt, writerA).id;
        expectError([&]() { (void)service.saveWorkingCopyChanges(
            draftId, writerA, UuidV7::generate().toString(), 0,
            {ModelChange{firstKey, firstRecord}}); }, ProjectErrorKind::Conflict);
        const aowis::dvmdb::WorkingCopyLease lease =
            service.acquireWorkingCopyLease(draftId, writerA, 120);
        staleToken = lease.token;
        require(lease.generation == 0 && lease.expiresAtMs != 0,
                "initial draft lease invalid");
        expectError([&]() { (void)service.acquireWorkingCopyLease(draftId, writerB, 120); },
                    ProjectErrorKind::Conflict);
        const WorkingCopyInfo saved = service.saveWorkingCopyChanges(
            draftId, writerA, lease.token, lease.generation,
            {ModelChange{firstKey, firstRecord}, ModelChange{secondKey, secondRecord}});
        require(saved.dirty && saved.generation == 1 && saved.candidateChanges.size() == 2 &&
                saved.candidateChanges[0].key == firstKey &&
                saved.candidateChanges[1].record->payload == secondRecord.payload &&
                !service.optionalHead(projectId, "main") &&
                service.revisionGraph(projectId).revisions.empty(),
                "draft save changed committed history or lost candidate changes");
        expectError([&]() { (void)service.saveWorkingCopyChanges(
            draftId, writerA, lease.token, 0, {}); }, ProjectErrorKind::Conflict);
        expectError([&]() { (void)service.saveWorkingCopyChanges(
            draftId, writerB, lease.token, 1, {}); }, ProjectErrorKind::Conflict);
        expectError([&]() { (void)service.saveWorkingCopyChanges(
            draftId, writerA, lease.token, 1,
            {ModelChange{firstKey, firstRecord}, ModelChange{firstKey, firstRecord}}); },
            ProjectErrorKind::InvalidInput);
        require(service.workingCopy(draftId).generation == 1,
                "rejected duplicate-key save changed draft generation");
        const aowis::dvmdb::WorkingCopyLease refreshed =
            service.acquireWorkingCopyLease(draftId, writerA, 120);
        require(refreshed.token != lease.token && refreshed.generation == 1,
                "reacquiring a lease did not rotate its fencing token");
        expectError([&]() { (void)service.saveWorkingCopyChanges(
            draftId, writerA, lease.token, 1, {}); }, ProjectErrorKind::Conflict);
        const WorkingCopyInfo changed = service.saveWorkingCopyChanges(
            draftId, writerA, refreshed.token, 1,
            {ModelChange{firstKey, std::nullopt}});
        require(changed.generation == 2 && changed.candidateChanges.size() == 2 &&
                !changed.candidateChanges[0].record.has_value() &&
                changed.candidateChanges[1].record->payload == secondRecord.payload,
                "incremental Save failed to merge change overlay");
        service.releaseWorkingCopyLease(draftId, writerA, refreshed.token);
        expectError([&]() { (void)service.saveWorkingCopyChanges(
            draftId, writerA, refreshed.token, 2, {}); }, ProjectErrorKind::Conflict);
    }
    {
        ProjectService reopened(root.path);
        const WorkingCopyInfo recovered = reopened.workingCopy(draftId);
        require(recovered.generation == 2 && recovered.candidateChanges.size() == 2 &&
                recovered.dirty && !recovered.leaseToken &&
                !reopened.optionalHead(projectId, "main"),
                "restart lost draft saves or changed committed HEAD");
        const aowis::dvmdb::WorkingCopyLease taken =
            reopened.acquireWorkingCopyLease(draftId, writerB, 120);
        require(taken.generation == 2 && taken.token != staleToken,
                "another session could not acquire a released lease");
        expectError([&]() { (void)reopened.saveWorkingCopyChanges(
            draftId, writerA, staleToken, 2, {}); }, ProjectErrorKind::Conflict);
        const WorkingCopyInfo third = reopened.saveWorkingCopyChanges(
            draftId, writerB, taken.token, 2, {ModelChange{secondKey, secondRecord}});
        require(third.generation == 3 && third.candidateChanges.size() == 2,
                "new writer lost existing candidate state");
        reopened.releaseWorkingCopyLease(draftId, writerB, taken.token);
        const aowis::dvmdb::WorkingCopyLease shortLease =
            reopened.acquireWorkingCopyLease(draftId, writerA, 1);
        expectError([&]() { (void)reopened.acquireWorkingCopyLease(draftId, writerB, 120); },
                    ProjectErrorKind::Conflict);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        const aowis::dvmdb::WorkingCopyLease takeover =
            reopened.acquireWorkingCopyLease(draftId, writerB, 120);
        require(takeover.generation == 3 && takeover.token != shortLease.token,
                "expired lease takeover did not preserve generation/fence");
        expectError([&]() { (void)reopened.saveWorkingCopyChanges(
            draftId, writerA, shortLease.token, 3, {}); }, ProjectErrorKind::Conflict);
        const WorkingCopyInfo final = reopened.saveWorkingCopyChanges(
            draftId, writerB, takeover.token, 3, {});
        require(final.generation == 4 && final.candidateChanges.size() == 2,
                "successful empty save lost stored candidate changes");
    }
}

void testAtomicJournalAndIdempotentReplay() {
    TemporaryRoot root;
    const std::string session = UuidV7::generate().toString();
    const UuidV7 eventId = UuidV7::generate();
    const UuidV7 entityId = UuidV7::generate();
    const aowis::dvmdb::Bytes event = aowis::dvmdb::test::editEvent(eventId, entityId);
    const aowis::dvmdb::Bytes differentEvent =
        aowis::dvmdb::test::editEvent(eventId, entityId, 1, 3);
    Key key = entityId.bytes;
    ModelRecord record;
    record.kind = 5;
    record.payload = {8, 9, 10};
    const std::vector<ModelChange> changes{{key, record}};
    std::string projectId;
    std::string draftId;
    std::string token;
    {
        ProjectService service(root.path);
        service.configureRepositoryRole(RepositoryRole::Private);
        projectId = service.createUnbornProject("atomic edit journal").id;
        draftId = service.createWorkingCopy(projectId, "main", std::nullopt, session).id;
        token = service.acquireWorkingCopyLease(draftId, session, 120).token;
        const WorkingCopyInfo saved = service.saveWorkingCopyTransactions(
            draftId, session, token, 0, changes, {event});
        require(saved.generation == 1 && saved.journal.size() == 1 &&
                saved.journal[0].id == eventId.toString() &&
                saved.journal[0].encoded == event &&
                saved.candidateChanges[0].record->payload == record.payload &&
                !service.optionalHead(projectId, "main").has_value(),
                "atomic event save lost journal/model or advanced HEAD");
        const WorkingCopyInfo retry = service.saveWorkingCopyTransactions(
            draftId, session, token, 0, changes, {event});
        require(retry.generation == 1 && retry.journal.size() == 1,
                "lost-acknowledgement retry duplicated a journal event");
        expectError([&]() { (void)service.saveWorkingCopyTransactions(
            draftId, session, token, 0, changes, {differentEvent}); }, ProjectErrorKind::Conflict);
        expectError([&]() { (void)service.saveWorkingCopyTransactions(
            draftId, session, token, 1, changes, {event}); }, ProjectErrorKind::Conflict);
        expectError([&]() { (void)service.saveWorkingCopyChanges(
            draftId, session, token, 1, changes); }, ProjectErrorKind::Conflict);
        aowis::dvmdb::Bytes corrupt = aowis::dvmdb::test::editEvent(UuidV7::generate(), entityId);
        corrupt.back() ^= 0x80;
        expectError([&]() { (void)service.saveWorkingCopyTransactions(
            draftId, session, token, 1, changes, {corrupt}); }, ProjectErrorKind::InvalidInput);
        // Format-1 frames remain parseable at the transport byte level, but
        // must not be accepted following MODEL's format-2 owner provenance
        // change. The payload digest is unchanged when only byte 4 changes.
        aowis::dvmdb::Bytes obsolete =
            aowis::dvmdb::test::editEvent(UuidV7::generate(), entityId);
        obsolete.at(4) = 1;
        expectError([&]() { (void)service.saveWorkingCopyTransactions(
            draftId, session, token, 1, changes, {obsolete}); }, ProjectErrorKind::InvalidInput);
        require(service.workingCopy(draftId).generation == 1,
                "rejected journal save mutated generation");
    }
    {
        ProjectService reopened(root.path);
        const WorkingCopyInfo persisted = reopened.workingCopy(draftId);
        require(persisted.generation == 1 && persisted.journal.size() == 1 &&
                persisted.journal[0].encoded == event &&
                persisted.candidateChanges.size() == 1,
                "restart lost journal or materialized candidate");
        const std::string nextToken = reopened.acquireWorkingCopyLease(draftId, session, 120).token;
        const aowis::dvmdb::Bytes next =
            aowis::dvmdb::test::editEvent(UuidV7::generate(), entityId, 2, 3);
        const WorkingCopyInfo appended = reopened.saveWorkingCopyTransactions(
            draftId, session, nextToken, 1, changes, {next});
        require(appended.generation == 2 && appended.journalCount == 2 &&
                appended.journal.size() == 1 && appended.journal[0].encoded == next,
                "append did not acknowledge exactly its own events");
        const WorkingCopyInfo history = reopened.workingCopy(draftId);
        require(history.journal.size() == 2 && history.journalCount == 2 &&
                history.journal[0].encoded == event && history.journal[1].encoded == next,
                "append did not preserve transaction order");
    }
    // Damage the persisted event's bytes inside the journal file.
    const std::filesystem::path path = root.path / "working-copies" / draftId /
        draftFileName(root.path, draftId, "journal");
    std::fstream journal(path, std::ios::binary | std::ios::in | std::ios::out);
    journal.seekg(40);
    char byte = 0;
    journal.get(byte);
    journal.seekp(40);
    journal.put(static_cast<char>(byte ^ 0x01));
    journal.close();
    ProjectService reopened(root.path);
    bool rejected = false;
    try { (void)reopened.workingCopy(draftId); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "corrupt on-disk event was accepted");
}

}

// R9: leases granted by this process are enforced against a monotonic
// deadline, so a wall-clock step neither extends nor cuts them short. After
// a restart only the persisted wall-clock expiry is known and is used.
void testLeasesIgnoreWallClockSteps() {
    TemporaryRoot root;
    std::atomic<std::uint64_t> wall{1'800'000'000'000ULL};
    std::atomic<std::int64_t> steadyOffsetMs{0};
    const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    const aowis::dvmdb::LeaseClock clock{
        [&wall]() { return wall.load(); },
        [&origin, &steadyOffsetMs]() {
            return origin + std::chrono::milliseconds(steadyOffsetMs.load());
        }};
    const std::string holder = UuidV7::generate().toString();
    const std::string other = UuidV7::generate().toString();
    std::string projectId;
    std::string draftId;
    {
        ProjectService service(root.path, 4, clock);
        projectId = service.createUnbornProject("lease clock").id;
        service.configureRepositoryRole(RepositoryRole::Private);
        draftId = service.createWorkingCopy(projectId, "main", std::nullopt, holder).id;

        // Wall clock jumps an hour ahead right after the lease was granted:
        // the 30 s lease is still held, takeover is refused, saves work.
        const std::string token = service.acquireWorkingCopyLease(draftId, holder, 30).token;
        wall += 3'600'000ULL;
        expectError([&]() { (void)service.acquireWorkingCopyLease(draftId, other, 30); },
                    ProjectErrorKind::Conflict);
        require(service.saveWorkingCopyChanges(draftId, holder, token, 0, {simpleChange()})
                    .generation == 1, "a forward wall-clock step cut a live lease short");

        // Wall clock jumps two hours back: the lease still ends after its
        // 30 monotonic seconds, then another session may take over.
        wall -= 7'200'000ULL;
        steadyOffsetMs += 31'000;
        require(!service.workingCopyLeaseLive(service.workingCopy(draftId)),
                "a backward wall-clock step extended an expired lease");
        expectError([&]() {
            (void)service.saveWorkingCopyChanges(draftId, holder, token, 1, {simpleChange()});
        }, ProjectErrorKind::Conflict);
        const std::string taken = service.acquireWorkingCopyLease(draftId, other, 30).token;
        require(service.saveWorkingCopyChanges(draftId, other, taken, 1, {simpleChange()})
                    .generation == 2, "lease takeover after monotonic expiry failed");
    }
    {
        // A restarted service only knows the persisted wall-clock expiry,
        // which was written as (wall at grant + 30 s).
        ProjectService reopened(root.path, 4, clock);
        require(reopened.workingCopyLeaseLive(reopened.workingCopy(draftId)),
                "persisted wall-clock expiry was not honoured after restart");
        wall += 31'000ULL;
        require(!reopened.workingCopyLeaseLive(reopened.workingCopy(draftId)),
                "an expired persisted lease was still live after restart");
    }
}

// R11: crash remnants of the catalogue's no-replace publication are removed
// when the service opens; nothing published is touched, and a retry of an
// interrupted caller-chosen project creation succeeds instead of staying Busy.
void testCatalogueRemnantsAreRemovedOnOpen() {
    TemporaryRoot root;
    std::string draftId;
    {
        ProjectService service(root.path);
        const std::string projectId = service.createUnbornProject("remnants").id;
        service.configureRepositoryRole(RepositoryRole::Private);
        draftId = service.createWorkingCopy(projectId, "main", std::nullopt,
                                            UuidV7::generate().toString()).id;
    }
    const std::string chosen = aowis::dvmdb::ProjectId::generate().toString();
    std::filesystem::create_directories(root.path / "local-commits");
    std::filesystem::create_directories(root.path / "projects" / (".creating-" + chosen) / "repository");
    const std::filesystem::path drafts = root.path / "working-copies";
    const std::filesystem::path creating = drafts / (".creating-" + UuidV7::generate().toString());
    const std::filesystem::path deleting = drafts / (".deleting-" + UuidV7::generate().toString());
    std::filesystem::create_directory(creating);
    std::filesystem::create_directory(deleting);
    const std::vector<std::filesystem::path> remnants{
        root.path / (".repository-id-creating-" + UuidV7::generate().toString()),
        creating / "meta",
        deleting / "meta",
        drafts / draftId / (".meta-" + UuidV7::generate().toString()),
        drafts / draftId / ("candidate-" + UuidV7::generate().toString()),
        drafts / draftId / ("journal-" + UuidV7::generate().toString()),
        root.path / "local-commits" / (".creating-" + UuidV7::generate().toString()),
        root.path / "local-commits" / (".updating-" + UuidV7::generate().toString()),
        root.path / "projects" / (".creating-" + chosen) / "project.meta"};
    for (const std::filesystem::path& path : remnants) {
        std::ofstream output(path, std::ios::binary);
        output << "interrupted";
        require(output.good(), "cannot create remnant fixture");
    }
    ProjectService reopened(root.path);
    for (const std::filesystem::path& path : remnants)
        require(!std::filesystem::exists(path), "a crash remnant survived opening the catalogue");
    require(!std::filesystem::exists(creating) && !std::filesystem::exists(deleting) &&
            std::filesystem::exists(drafts / draftId / "meta"),
            "draft directory remnants survived, or the published draft lost its meta");
    require(!std::filesystem::exists(root.path / "projects" / (".creating-" + chosen)),
            "an unpublished project directory survived opening the catalogue");
    require(reopened.listWorkingCopies().size() == 1 &&
            reopened.workingCopy(draftId).id == draftId &&
            reopened.listProjects().size() == 1,
            "remnant cleanup touched published drafts or projects");
    require(reopened.createUnbornProject("retried", chosen).id == chosen,
            "retry of an interrupted caller-chosen project creation did not succeed");
}


std::uint64_t inodeOf(const std::filesystem::path& path) {
    struct ::stat status{};
    require(::stat(path.c_str(), &status) == 0, "cannot stat draft file");
    return static_cast<std::uint64_t>(status.st_ino);
}

std::string fileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), {});
}

template <typename Action>
void expectStorageError(Action action, aowis::dvmdb::StorageErrorKind kind, const char* message) {
    try { action(); }
    catch (const aowis::dvmdb::StorageError& error) {
        require(error.kind() == kind, message);
        return;
    }
    throw std::runtime_error(message);
}

// R7: a draft is a directory of a small meta record, an immutable candidate
// file and an append-only journal. Leases and listings touch only meta, a
// Save appends only its own events and rewrites the candidate only when it
// changes, and a Commit drops both data files.
void testDraftWritesAreIncremental() {
    TemporaryRoot root;
    const std::string session = UuidV7::generate().toString();
    const UuidV7 entityId = UuidV7::generate();
    ModelRecord record;
    record.kind = 5;
    record.payload = {8, 9, 10};
    const std::vector<ModelChange> changes{{entityId.bytes, record}};
    ProjectService service(root.path);
    service.configureRepositoryRole(RepositoryRole::Private);
    const std::string projectId = service.createUnbornProject("incremental drafts").id;
    const std::string draftId =
        service.createWorkingCopy(projectId, "main", std::nullopt, session).id;
    const std::filesystem::path directory = root.path / "working-copies" / draftId;
    require(std::filesystem::is_directory(directory) &&
            draftFileName(root.path, draftId, "candidate") == "-" &&
            draftFileName(root.path, draftId, "journal") == "-",
            "a new draft is not an empty draft directory");

    std::string token = service.acquireWorkingCopyLease(draftId, session, 120).token;
    const UuidV7 firstEvent = UuidV7::generate();
    (void)service.saveWorkingCopyTransactions(draftId, session, token, 0, changes,
        {aowis::dvmdb::test::editEvent(firstEvent, entityId)});
    const std::string candidate = draftFileName(root.path, draftId, "candidate");
    const std::string journal = draftFileName(root.path, draftId, "journal");
    require(candidate.starts_with("candidate-") && journal.starts_with("journal-"),
            "an event Save did not create candidate and journal files");
    const std::uint64_t candidateInode = inodeOf(directory / candidate);
    const std::uint64_t journalInode = inodeOf(directory / journal);
    const std::string firstFrames = fileBytes(directory / journal);

    // A lease, a release and a listing leave both data files alone.
    service.releaseWorkingCopyLease(draftId, session, token);
    token = service.acquireWorkingCopyLease(draftId, session, 120).token;
    require(service.listWorkingCopies().size() == 1 &&
            draftFileName(root.path, draftId, "candidate") == candidate &&
            inodeOf(directory / candidate) == candidateInode &&
            fileBytes(directory / journal) == firstFrames,
            "a lease or listing rewrote draft data files");

    // An event Save without model changes appends to the same journal file
    // and keeps the candidate file.
    const UuidV7 secondEvent = UuidV7::generate();
    const WorkingCopyInfo second = service.saveWorkingCopyTransactions(
        draftId, session, token, 1, {},
        {aowis::dvmdb::test::editEvent(secondEvent, entityId, 2, 3)});
    const std::string grown = fileBytes(directory / journal);
    require(second.journalCount == 2 && second.journal.size() == 1 &&
            second.journal[0].id == secondEvent.toString() &&
            draftFileName(root.path, draftId, "journal") == journal &&
            inodeOf(directory / journal) == journalInode &&
            grown.size() > firstFrames.size() && grown.starts_with(firstFrames) &&
            draftFileName(root.path, draftId, "candidate") == candidate,
            "an event Save rewrote the journal or an unchanged candidate");

    // A torn append (bytes past the acknowledged length) is invisible and
    // overwritten by the next Save.
    {
        std::ofstream tail(directory / journal, std::ios::binary | std::ios::app);
        tail << "torn-append-tail";
    }
    require(service.workingCopy(draftId).journal.size() == 2,
            "an unacknowledged journal tail was read as history");
    const WorkingCopyInfo third = service.saveWorkingCopyTransactions(
        draftId, session, token, 2, changes,
        {aowis::dvmdb::test::editEvent(UuidV7::generate(), entityId, 3, 4)});
    const std::string newCandidate = draftFileName(root.path, draftId, "candidate");
    require(third.journalCount == 3 && service.workingCopy(draftId).journal.size() == 3 &&
            fileBytes(directory / journal).find("torn-append-tail") == std::string::npos &&
            newCandidate != candidate && !std::filesystem::exists(directory / candidate),
            "a Save kept a torn tail, or did not replace a changed candidate file");

    // Listings read only meta: a damaged candidate still lists, but the
    // draft itself reports the corruption.
    const std::string intact = fileBytes(directory / newCandidate);
    {
        std::ofstream damaged(directory / newCandidate, std::ios::binary | std::ios::trunc);
        damaged << std::string(intact.size(), 'x');
    }
    require(service.listWorkingCopies(projectId, 10, std::nullopt).workingCopies.size() == 1,
            "a listing read draft data files");
    expectStorageError([&]() { (void)service.workingCopy(draftId); },
                       aowis::dvmdb::StorageErrorKind::Corruption,
                       "a damaged candidate file was accepted");
    {
        std::ofstream repaired(directory / newCandidate, std::ios::binary | std::ios::trunc);
        repaired << intact;
    }

    // A local Commit rebases the draft: no candidate, no journal files.
    const aowis::dvmdb::LocalCommitInfo committed = service.commitWorkingCopy(
        UuidV7::generate().toString(), draftId, session, token, 3, std::nullopt, commitMetadata());
    require(committed.state == aowis::dvmdb::LocalCommitState::Succeeded,
            "local Commit of an incremental draft failed");
    const WorkingCopyInfo rebased = service.workingCopy(draftId);
    std::size_t entries = 0;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory)) {
        (void)entry;
        ++entries;
    }
    require(rebased.journalCount == 0 && rebased.candidateChanges.empty() && entries == 1 &&
            draftFileName(root.path, draftId, "journal") == "-",
            "a rebased draft kept its candidate or journal files");

    // Discard removes the whole directory.
    service.discardWorkingCopy(draftId, session, token, rebased.generation);
    require(!std::filesystem::exists(directory), "a discarded draft directory survived");
}

// The earlier single-file draft format is rejected, never migrated, and an
// oversized candidate is a limit that changes nothing.
void testUnsupportedAndOversizedDrafts() {
    TemporaryRoot root;
    const std::string session = UuidV7::generate().toString();
    std::string projectId;
    std::string draftId;
    {
        ProjectService service(root.path);
        service.configureRepositoryRole(RepositoryRole::Private);
        projectId = service.createUnbornProject("draft limits").id;
        draftId = service.createWorkingCopy(projectId, "main", std::nullopt, session).id;
        const std::string token = service.acquireWorkingCopyLease(draftId, session, 120).token;
        std::vector<ModelChange> large;
        for (unsigned i = 0; i < 9; ++i) {
            Key key = UuidV7::generate().bytes;
            ModelRecord record;
            record.kind = 1;
            record.payload.assign(1024 * 1024, static_cast<std::uint8_t>(i));
            large.push_back(ModelChange{key, record});
        }
        expectStorageError([&]() {
            (void)service.saveWorkingCopyChanges(draftId, session, token, 0, large);
        }, aowis::dvmdb::StorageErrorKind::LimitExceeded, "an oversized candidate was not a limit");
        const WorkingCopyInfo unchanged = service.workingCopy(draftId);
        std::size_t entries = 0;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(root.path / "working-copies" / draftId)) {
            (void)entry;
            ++entries;
        }
        require(unchanged.generation == 0 && !unchanged.dirty && entries == 1 &&
                service.saveWorkingCopyChanges(draftId, session, token, 0, {simpleChange()})
                    .generation == 1,
                "a rejected oversized Save wrote to the draft or broke the service");
    }
    const std::string legacy = UuidV7::generate().toString();
    std::ofstream(root.path / "working-copies" / legacy) << "AOWIS-WORKING-COPY-3\n";
    ProjectService reopened(root.path);
    expectStorageError([&]() { (void)reopened.workingCopy(legacy); },
                       aowis::dvmdb::StorageErrorKind::Corruption,
                       "a single-file draft record was accepted");
    require(reopened.workingCopy(draftId).generation == 1,
            "an unsupported draft record broke other drafts");
}

// Readers take no lock. While a writer replaces the candidate file and
// appends to the journal, every read sees one complete Save: candidate and
// journal of the same generation.
void testLockFreeReadersSeeWholeSaves() {
    TemporaryRoot root;
    const std::string session = UuidV7::generate().toString();
    const UuidV7 entityId = UuidV7::generate();
    ProjectService service(root.path);
    service.configureRepositoryRole(RepositoryRole::Private);
    const std::string projectId = service.createUnbornProject("lock-free readers").id;
    const std::string draftId =
        service.createWorkingCopy(projectId, "main", std::nullopt, session).id;
    const std::string token = service.acquireWorkingCopyLease(draftId, session, 120).token;
    constexpr std::uint64_t saves = 60;
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> reads{0};
    std::string failure;
    std::thread reader([&]() {
        try {
            while (!done.load()) {
                const WorkingCopyInfo draft = service.workingCopy(draftId);
                // Save g stores payload {g} and the g-th event.
                if (draft.journal.size() != draft.generation || draft.journalCount != draft.generation ||
                    (draft.generation != 0 &&
                     (draft.candidateChanges.size() != 1 ||
                      draft.candidateChanges.front().record->payload !=
                          std::vector<std::uint8_t>{static_cast<std::uint8_t>(draft.generation)})))
                    throw std::runtime_error("a reader saw a mixed draft state");
                ++reads;
            }
        } catch (const std::exception& error) {
            failure = error.what();
        }
    });
    for (std::uint64_t generation = 0; generation < saves; ++generation) {
        ModelRecord record;
        record.kind = 5;
        record.payload = {static_cast<std::uint8_t>(generation + 1)};
        (void)service.saveWorkingCopyTransactions(draftId, session, token, generation,
            {ModelChange{entityId.bytes, record}},
            {aowis::dvmdb::test::editEvent(UuidV7::generate(), entityId,
                                           static_cast<std::uint8_t>(generation + 1),
                                           static_cast<std::uint8_t>(generation + 2))});
    }
    done = true;
    reader.join();
    require(failure.empty(), failure.empty() ? "" : failure.c_str());
    require(reads.load() > 0 && service.workingCopy(draftId).journal.size() == saves,
            "concurrent reads did not run or the journal is incomplete");
}

// D3: an unborn draft records the overlay at journal position 0 when its
// journal starts, so History can replay forward from it (deletions
// included) instead of reversing the journal.
std::vector<std::string> draftFiles(const std::filesystem::path& root, const std::string& draftId,
                                    const std::string& prefix) {
    std::vector<std::string> names;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(root / "working-copies" / draftId))
        if (entry.path().filename().string().starts_with(prefix))
            names.push_back(entry.path().filename().string());
    return names;
}

bool sameChanges(const std::vector<ModelChange>& left, const std::vector<ModelChange>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].key != right[index].key ||
            left[index].record.has_value() != right[index].record.has_value())
            return false;
        if (left[index].record && (left[index].record->kind != right[index].record->kind ||
                                   left[index].record->payload != right[index].record->payload ||
                                   left[index].record->references != right[index].record->references))
            return false;
    }
    return true;
}

void testUnbornJournalSeed() {
    TemporaryRoot root;
    const std::string session = UuidV7::generate().toString();
    const UuidV7 initialized = UuidV7::generate();
    const UuidV7 edited = UuidV7::generate();
    const std::function<ModelChange(const UuidV7&, std::uint8_t)> change =
        [](const UuidV7& id, std::uint8_t payload) {
            ModelRecord record;
            record.kind = 5;
            record.payload = {payload};
            return ModelChange{id.bytes, record};
        };
    std::string projectId;
    std::string draftId;
    std::vector<ModelChange> initialization;
    std::string firstSeedFile;
    {
        ProjectService service(root.path);
        service.configureRepositoryRole(RepositoryRole::Private);
        projectId = service.createUnbornProject("journal seed").id;
        draftId = service.createWorkingCopy(projectId, "main", std::nullopt, session).id;
        std::string token = service.acquireWorkingCopyLease(draftId, session, 120).token;
        // Unjournaled initialization, then the first journaled Save.
        initialization = service.saveWorkingCopyChanges(
            draftId, session, token, 0, {change(initialized, 1)}).candidateChanges;
        require(!service.workingCopy(draftId).journalSeed.has_value(),
                "a draft without a journal has a journal seed");
        (void)service.saveWorkingCopyTransactions(draftId, session, token, 1,
            {change(edited, 2)}, {aowis::dvmdb::test::editEvent(UuidV7::generate(), edited, 1, 2)});
        WorkingCopyInfo full = service.workingCopy(draftId);
        require(full.journalSeed && sameChanges(*full.journalSeed, initialization),
                "first journaled Save did not record the overlay before it as seed");
        firstSeedFile = draftFileName(root.path, draftId, "journal-seed");
        require(firstSeedFile.starts_with("seed-") && draftFiles(root.path, draftId, "seed-").size() == 1,
                "journal seed file is missing");
        // A continuing journal keeps its seed.
        (void)service.saveWorkingCopyTransactions(draftId, session, token, 2,
            {change(edited, 3)}, {aowis::dvmdb::test::editEvent(UuidV7::generate(), edited, 2, 3)});
        require(draftFileName(root.path, draftId, "journal-seed") == firstSeedFile &&
                sameChanges(*service.workingCopy(draftId).journalSeed, initialization),
                "appending to the journal changed its seed");
        // Listings and lease renewals carry no seed and keep it on disk.
        token = service.acquireWorkingCopyLease(draftId, session, 120).token;
        require(draftFileName(root.path, draftId, "journal-seed") == firstSeedFile,
                "a lease renewal dropped the journal seed");
    }
    {
        // Survives a restart; a Save from position 0 starts a new journal
        // whose seed is the overlay published at position 0.
        ProjectService service(root.path);
        service.configureRepositoryRole(RepositoryRole::Private);
        require(sameChanges(*service.workingCopy(draftId).journalSeed, initialization),
                "journal seed did not survive a restart");
        const std::string token = service.acquireWorkingCopyLease(draftId, session, 120).token;
        // A Save after an Undo that retains a prefix rewrites the journal
        // file, but the prefix still starts at the same seed.
        const WorkingCopyInfo atOne = service.publishVerifiedDraftActionNavigation(
            UuidV7::generate().toString(), draftId, session, token, 3, std::nullopt, 1,
            {change(initialized, 1), change(edited, 2)});
        const std::string journalBefore = draftFileName(root.path, draftId, "journal");
        (void)service.saveWorkingCopyTransactions(draftId, session, token, atOne.generation,
            {change(edited, 6)}, {aowis::dvmdb::test::editEvent(UuidV7::generate(), edited, 2, 6)});
        const WorkingCopyInfo branchedOnce = service.workingCopy(draftId);
        require(branchedOnce.journalCount == 2 &&
                draftFileName(root.path, draftId, "journal") != journalBefore &&
                draftFileName(root.path, draftId, "journal-seed") == firstSeedFile &&
                branchedOnce.journalSeed && sameChanges(*branchedOnce.journalSeed, initialization),
                "a Save after a partial Undo dropped the journal seed");
        const std::vector<ModelChange> atZero{change(initialized, 7)};
        const WorkingCopyInfo navigated = service.publishVerifiedDraftActionNavigation(
            UuidV7::generate().toString(), draftId, session, token, branchedOnce.generation,
            std::nullopt, 0, atZero);
        require(navigated.draftActionPosition == 0 &&
                draftFileName(root.path, draftId, "journal-seed") == firstSeedFile,
                "History navigation changed the journal seed");
        (void)service.saveWorkingCopyTransactions(draftId, session, token, navigated.generation,
            {change(edited, 4)}, {aowis::dvmdb::test::editEvent(UuidV7::generate(), edited, 3, 4)});
        const WorkingCopyInfo branched = service.workingCopy(draftId);
        require(branched.journalCount == 1 && branched.journalSeed &&
                sameChanges(*branched.journalSeed, atZero),
                "a Save from position 0 did not record the position-0 overlay as seed");
        require(draftFileName(root.path, draftId, "journal-seed") != firstSeedFile &&
                draftFiles(root.path, draftId, "seed-").size() == 1,
                "the superseded journal seed file was not removed");
    }
    // A journal begun before seeds were stored: no seed line.
    {
        const std::filesystem::path metaPath = root.path / "working-copies" / draftId / "meta";
        std::ifstream in(metaPath, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const std::size_t line = body.find("journal-seed\t");
        require(line != std::string::npos, "meta lacks the journal seed line");
        const std::string original = body;
        // Opening the store removes the then unreferenced seed file; keep a copy.
        const std::filesystem::path seedPath = root.path / "working-copies" / draftId /
            draftFileName(root.path, draftId, "journal-seed");
        const std::filesystem::path backup = root.path / "seed-backup";
        std::filesystem::copy_file(seedPath, backup);
        body.erase(line);
        {
            std::ofstream out(metaPath, std::ios::binary | std::ios::trunc);
            out << body;
        }
        {
            ProjectService service(root.path);
            const WorkingCopyInfo legacy = service.workingCopy(draftId);
            require(!legacy.journalSeed && legacy.journalCount == 1,
                    "a meta without a seed line did not read as an unrecorded seed");
        }
        require(!std::filesystem::exists(seedPath),
                "a seed file no meta references was kept");
        std::filesystem::rename(backup, seedPath);
        std::ofstream out(metaPath, std::ios::binary | std::ios::trunc);
        out << original;
    }
    // A damaged seed file is refused, a stray seed file is removed on open.
    {
        const std::filesystem::path seedPath = root.path / "working-copies" / draftId /
            draftFileName(root.path, draftId, "journal-seed");
        std::fstream seed(seedPath, std::ios::binary | std::ios::in | std::ios::out);
        seed.seekg(-1, std::ios::end);
        char byte = 0;
        seed.get(byte);
        seed.seekp(-1, std::ios::end);
        seed.put(static_cast<char>(byte ^ 0x01));
        seed.close();
        {
            ProjectService service(root.path);
            bool rejected = false;
            try { (void)service.workingCopy(draftId); }
            catch (const std::exception&) { rejected = true; }
            require(rejected, "a damaged journal seed was accepted");
        }
        std::fstream repair(seedPath, std::ios::binary | std::ios::in | std::ios::out);
        repair.seekp(-1, std::ios::end);
        repair.put(byte);
        repair.close();
        std::ofstream(root.path / "working-copies" / draftId /
                      ("seed-" + UuidV7::generate().toString())) << "stray";
    }
    {
        ProjectService service(root.path);
        service.configureRepositoryRole(RepositoryRole::Private);
        require(draftFiles(root.path, draftId, "seed-").size() == 1,
                "an unreferenced seed file survived opening the draft store");
        // Commit: the draft continues from the new HEAD without a seed, and a
        // journal on a committed base never records one.
        const WorkingCopyInfo current = service.workingCopy(draftId);
        require(current.journalSeed && !current.journal.empty(),
                "the Commit fixture has no seeded journal");
        const std::string token = service.acquireWorkingCopyLease(draftId, session, 120).token;
        const aowis::dvmdb::LocalCommitInfo initialCommit = service.commitWorkingCopy(
            UuidV7::generate().toString(), draftId, session, token,
            current.generation + 0, std::nullopt, commitMetadata());
        // The initial Commit archives its journal seed: History can start
        // its actions there although the Commit has no parent revision.
        const std::optional<RevisionId> initialRevision =
            RevisionId::parse(initialCommit.reservedRevisionId);
        require(initialRevision.has_value(), "initial Commit has no revision");
        const aowis::dvmdb::RevisionNativeActionWitness witness =
            service.verifiedRevisionNativeActionWitness(projectId, "main", *initialRevision);
        require(!witness.parentRevision && witness.journalSeed &&
                sameChanges(*witness.journalSeed, *current.journalSeed) &&
                witness.actions.size() == current.journal.size(),
                "initial Commit did not archive its journal seed");
        const std::filesystem::path record = root.path / "local-commits" / initialCommit.id;
        std::string original;
        {
            std::ifstream in(record, std::ios::binary);
            original.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const std::function<void(const std::string&)> rewrite = [&record](const std::string& body) {
            std::filesystem::permissions(record, std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write);
            std::ofstream out(record, std::ios::binary | std::ios::trunc);
            out << body;
        };
        const std::size_t seedStart = original.find("\njournal-seed\t");
        const std::size_t digestLine = original.find("\nseed-digest\t");
        require(seedStart != std::string::npos && digestLine != std::string::npos &&
                original.find("\nseed\t") != std::string::npos,
                "the local Commit record has no journal seed rows");
        // A damaged seed row (its last record byte, still decodable) fails
        // the digest.
        std::string damaged = original;
        const std::size_t rowEnd = damaged.find('\n', damaged.find("\nseed\t") + 1);
        damaged[rowEnd - 1] = damaged[rowEnd - 1] == '0' ? '2' : '0';
        rewrite(damaged);
        bool refused = false;
        try { (void)service.verifiedRevisionNativeActionWitness(projectId, "main", *initialRevision); }
        catch (const std::exception&) { refused = true; }
        require(refused, "a damaged archived journal seed was accepted");
        // A record without the seed lines (made before seeds) still reads,
        // without a seed.
        const std::size_t digestEnd = original.find('\n', digestLine + 1);
        rewrite(original.substr(0, seedStart + 1) + original.substr(digestEnd + 1));
        require(!service.verifiedRevisionNativeActionWitness(
                    projectId, "main", *initialRevision).journalSeed,
                "a local Commit record without seed lines produced a seed");
        rewrite(original);
        const WorkingCopyInfo rebased = service.workingCopy(draftId);
        require(rebased.baseRevision && !rebased.journalSeed &&
                draftFiles(root.path, draftId, "seed-").empty(),
                "Commit kept the journal seed of the unborn journal");
        (void)service.saveWorkingCopyTransactions(draftId, session, token, rebased.generation,
            {change(edited, 5)}, {aowis::dvmdb::test::editEvent(UuidV7::generate(), edited, 4, 5)});
        require(!service.workingCopy(draftId).journalSeed &&
                draftFiles(root.path, draftId, "seed-").empty(),
                "a journal on a committed base recorded a seed");
        // A Commit with a parent never archives a seed.
        const aowis::dvmdb::LocalCommitInfo second = service.commitWorkingCopy(
            UuidV7::generate().toString(), draftId, session, token,
            service.workingCopy(draftId).generation, initialRevision, commitMetadata());
        const std::optional<RevisionId> secondRevision = RevisionId::parse(second.reservedRevisionId);
        require(secondRevision.has_value(), "second Commit has no revision");
        const aowis::dvmdb::RevisionNativeActionWitness secondWitness =
            service.verifiedRevisionNativeActionWitness(projectId, "main", *secondRevision);
        require(secondWitness.parentRevision == initialRevision && !secondWitness.journalSeed,
                "a Commit with a parent archived a journal seed");
    }
    {
        // A seed line is only valid for an unborn draft with a journal.
        const std::filesystem::path metaPath = root.path / "working-copies" / draftId / "meta";
        {
            std::ofstream out(metaPath, std::ios::binary | std::ios::app);
            out << "journal-seed\t-\t0\t0\t-\n";
        }
        ProjectService service(root.path);
        bool rejected = false;
        try { (void)service.workingCopy(draftId); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "a journal seed on a committed draft was accepted");
    }
}

int main() {
    try {
        testWorkingCopyCreationAndRecovery();
        testLeasedTransactionalCandidateSaves();
        testAtomicJournalAndIdempotentReplay();
        testLeasesIgnoreWallClockSteps();
        testCatalogueRemnantsAreRemovedOnOpen();
        testDraftWritesAreIncremental();
        testUnsupportedAndOversizedDrafts();
        testLockFreeReadersSeeWholeSaves();
        testUnbornJournalSeed();
        std::cout << "VC-04 persistent working-copy bootstrap passed\n";
    } catch (const std::exception& error) {
        std::cerr << "VC-04 working-copy bootstrap failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
