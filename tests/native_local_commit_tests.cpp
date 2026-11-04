#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/object_id.h"
#include "aowis/dvmdb/storage_error.h"
#include "model_edit_event_fixture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <thread>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <unistd.h>

using aowis::dvmdb::test::commitMetadata;

namespace {
using aowis::dvmdb::Key;
using aowis::dvmdb::LocalCommitInfo;
using aowis::dvmdb::LocalCommitState;
using aowis::dvmdb::ModelChange;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::ProjectError;
using aowis::dvmdb::ProjectErrorKind;
using aowis::dvmdb::ProjectService;
using aowis::dvmdb::RepositoryRole;
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::UuidV7;
using aowis::dvmdb::WorkingCopyInfo;
using aowis::dvmdb::WorkingCopyLease;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void expectConflict(Function function) {
    try { function(); }
    catch (const ProjectError& exception) {
        require(exception.kind() == ProjectErrorKind::Conflict, "unexpected project error kind");
        return;
    }
    throw std::runtime_error("expected stale or reused local Commit to conflict");
}

struct TempRoot {
    TempRoot() {
        std::string pattern = (std::filesystem::temp_directory_path() /
            "aowis-local-commit-XXXXXXXX").string();
        std::vector<char> chars(pattern.begin(), pattern.end());
        chars.push_back('\0');
        const char* created = ::mkdtemp(chars.data());
        require(created != nullptr, "cannot create local commit test root");
        this->path = created;
    }
    ~TempRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(this->path, ignored);
    }
    std::filesystem::path path;
};

ModelChange changeWithValue(std::uint8_t value) {
    Key key{};
    key[15] = 7;
    ModelRecord record;
    record.kind = 1;
    record.payload = {value};
    return ModelChange{key, record};
}

// HIST-02: a successful Commit must not erase its underlying MODEL actions,
// even though the active draft is rebased and its short journal disappears.
// All frames are part of the durable intent BEFORE publishing a revision.
// HIST-03a: a historical checkpoint must reconstruct a COMPLETE candidate
// overlay relative to the *current* HEAD. This is a read-only plan: it must
// not publish a revision, erase draft events, or move the logical cursor.
void testHistoryCheckpointPlan() {
    TempRoot root;
    const std::string session = UuidV7::generate().toString();
    std::string projectId;
    std::string draftId;
    std::string firstRevision;
    std::string secondRevision;
    const ModelChange initial = changeWithValue(10);
    ModelChange companion = changeWithValue(77);
    companion.key[15] = 8;
    const ModelChange modified = changeWithValue(20);
    ModelChange introduced = changeWithValue(99);
    introduced.key[15] = 9;
    ModelChange removed;
    removed.key = companion.key;
    {
        ProjectService service(root.path);
        service.configureRepositoryRole(RepositoryRole::Private);
        projectId = service.createUnbornProject("history checkpoint plans").id;
        const aowis::dvmdb::RevisionId first = service.commitModelAtExpectedHead(
            projectId, "main", {initial, companion}, commitMetadata(), std::nullopt);
        firstRevision = first.toString();
        const aowis::dvmdb::RevisionId second = service.commitModelAtExpectedHead(
            projectId, "main", {modified, removed, introduced}, commitMetadata(), first);
        secondRevision = second.toString();
        draftId = service.createWorkingCopy(projectId, "main", second, session).id;
        const std::string token = service.acquireWorkingCopyLease(draftId, session, 120).token;
        const aowis::dvmdb::HistoryCheckpointPlan older = service.planHistoryCheckpoint(
            draftId, first, 0);
        require(older.draftId == draftId && older.projectId == projectId &&
                older.branchId == service.branchId(projectId, "main")->toString() &&
                older.expectedHead == second && older.targetRevisionId == firstRevision &&
                older.expectedGeneration == 0 && older.candidateChanges.size() == 3 &&
                older.candidateChanges[0].record && older.candidateChanges[1].record &&
                !older.candidateChanges[2].record &&
                older.candidateChanges[0].record->payload == initial.record->payload &&
                older.candidateChanges[1].record->payload == companion.record->payload &&
                older.candidateChanges[2].key == introduced.key,
                "HIST-03a older checkpoint lost replacement, resurrection or tombstone");
        const aowis::dvmdb::HistoryCheckpointPlan current = service.planHistoryCheckpoint(
            draftId, second, 0);
        require(current.candidateChanges.empty(),
                "HIST-03a current HEAD should require an empty replacement overlay");
        ModelChange independent = changeWithValue(88);
        independent.key[15] = 10;
        require(service.saveWorkingCopyChanges(draftId, session, token, 0,
                    {independent}).generation == 1, "test edit was not saved");
        expectConflict([&] { (void)service.planHistoryCheckpoint(draftId, first, 0); });
        const aowis::dvmdb::HistoryCheckpointPlan afterLocalEdit =
            service.planHistoryCheckpoint(draftId, first, 1);
        require(afterLocalEdit.candidateChanges.size() == 3 &&
                afterLocalEdit.candidateChanges[0].key == initial.key &&
                afterLocalEdit.candidateChanges[1].key == companion.key &&
                !afterLocalEdit.candidateChanges[2].record,
                "HIST-03a replacement plan incorrectly retained unrelated local edits");
        require(service.workingCopy(draftId).generation == 1 &&
                service.workingCopy(draftId).candidateChanges.size() == 1 &&
                service.optionalHead(projectId, "main") == second,
                "HIST-03a planning mutated the draft or immutable HEAD");
        const std::string otherProject = service.createUnbornProject("unrelated history").id;
        const aowis::dvmdb::RevisionId unrelated = service.commitModelAtExpectedHead(
            otherProject, "main", {changeWithValue(33)}, commitMetadata(), std::nullopt);
        bool unrelatedRejected = false;
        try { (void)service.planHistoryCheckpoint(draftId, unrelated, 1); }
        catch (const ProjectError& error) {
            unrelatedRejected = error.kind() == ProjectErrorKind::InvalidInput;
        }
        require(unrelatedRejected, "HIST-03a accepted a revision from another branch/project");
    }
    {
        ProjectService reopened(root.path);
        const aowis::dvmdb::HistoryCheckpointPlan recovered =
            reopened.planHistoryCheckpoint(draftId, *RevisionId::parse(firstRevision), 1);
        require(recovered.targetRevisionId == firstRevision &&
                recovered.expectedHead == RevisionId::parse(secondRevision) &&
                recovered.candidateChanges.size() == 3 &&
                reopened.workingCopy(draftId).candidateChanges.size() == 1,
                "HIST-03a checkpoint preview changed or mutated after repository restart");
        // Publishing another immutable HEAD invalidates a plan against this
        // stale draft, even if its generation has not changed.
        const aowis::dvmdb::RevisionId moved = reopened.commitModelAtExpectedHead(
            projectId, "main", {changeWithValue(44)}, commitMetadata(),
            *RevisionId::parse(secondRevision));
        require(reopened.optionalHead(projectId, "main") == moved,
                "HIST-03a stale HEAD fixture did not publish");
        expectConflict([&] {
            (void)reopened.planHistoryCheckpoint(draftId,
                *RevisionId::parse(firstRevision), 1);
        });
    }
}

// HIST-03b: selecting an immutable checkpoint atomically replaces the private
// overlay and the persistent cursor. It never changes committed HEAD, and
// replay of an uncertain request cannot advance the generation a second time.
void testAtomicHistoryCheckpointNavigation() {
    TempRoot root;
    const std::string session = UuidV7::generate().toString();
    const std::string operation = UuidV7::generate().toString();
    const std::string backOperation = UuidV7::generate().toString();
    std::string projectId;
    std::string draftId;
    std::string firstId;
    std::string latestId;
    std::string originalLease;
    ModelChange initial = changeWithValue(10);
    ModelChange replaced = changeWithValue(20);
    ModelChange added = changeWithValue(50);
    added.key[15] = 22;
    ModelChange unrelated = changeWithValue(99);
    unrelated.key[15] = 23;
    {
        ProjectService service(root.path);
        service.configureRepositoryRole(RepositoryRole::Private);
        projectId = service.createUnbornProject("HIST-03b atomic checkpoints").id;
        const RevisionId first = service.commitModelAtExpectedHead(
            projectId, "main", {initial}, commitMetadata(), std::nullopt);
        const RevisionId latest = service.commitModelAtExpectedHead(
            projectId, "main", {replaced, added}, commitMetadata(), first);
        firstId = first.toString();
        latestId = latest.toString();
        draftId = service.createWorkingCopy(projectId, "main", latest, session).id;
        originalLease = service.acquireWorkingCopyLease(draftId, session, 120).token;
        require(service.saveWorkingCopyChanges(draftId, session, originalLease, 0,
                    {unrelated}).generation == 1, "HIST-03b fixture Save failed");
        const WorkingCopyInfo selected = service.navigateHistoryCheckpoint(
            operation, draftId, session, originalLease, 1, latest, first);
        require(selected.generation == 2 && selected.dirty &&
                selected.candidateChanges.size() == 2 &&
                selected.historyCheckpointRevisionId == firstId &&
                selected.lastHistoryNavigationOperationId == operation &&
                selected.lastHistoryNavigationSourceGeneration == 1 &&
                selected.journalCount == 0 &&
                service.optionalHead(projectId, "main") == latest,
                "HIST-03b checkpoint selection was not a complete atomic overlay replacement");
        bool retainedUnrelated = false;
        for (const ModelChange& item : selected.candidateChanges)
            if (item.key == unrelated.key) retainedUnrelated = true;
        require(!retainedUnrelated,
                "HIST-03b navigation merged an unrelated draft edit into the target checkpoint");
        const WorkingCopyInfo replayed = service.navigateHistoryCheckpoint(
            operation, draftId, session, originalLease, 1, latest, first);
        require(replayed.generation == 2 && replayed.candidateChanges.size() == 2,
                "HIST-03b same-identity retry executed navigation twice");
        expectConflict([&] { (void)service.navigateHistoryCheckpoint(
            operation, draftId, session, originalLease, 1, latest, latest); });
        expectConflict([&] { (void)service.navigateHistoryCheckpoint(
            backOperation, draftId, session, originalLease, 1, latest, latest); });
        expectConflict([&] { (void)service.navigateHistoryCheckpoint(
            UuidV7::generate().toString(), draftId, session, originalLease, 2,
            first, latest); });
        const WorkingCopyInfo restored = service.navigateHistoryCheckpoint(
            backOperation, draftId, session, originalLease, 2, latest, latest);
        require(restored.generation == 3 && !restored.dirty &&
                restored.candidateChanges.empty() &&
                restored.historyCheckpointRevisionId == latestId &&
                service.optionalHead(projectId, "main") == latest,
                "HIST-03b forward checkpoint did not restore current HEAD content");
    }
    {
        ProjectService reopened(root.path);
        const WorkingCopyInfo persisted = reopened.workingCopy(draftId);
        require(persisted.generation == 3 &&
                persisted.historyCheckpointRevisionId == latestId &&
                persisted.lastHistoryNavigationOperationId == backOperation &&
                persisted.lastHistoryNavigationSourceGeneration == 2,
                "HIST-03b navigation cursor was not durable across restart");
        const RevisionId latest = *RevisionId::parse(latestId);
        const RevisionId first = *RevisionId::parse(firstId);
        const WorkingCopyInfo retried = reopened.navigateHistoryCheckpoint(
            backOperation, draftId, session, originalLease, 2, latest, latest);
        require(retried.generation == 3,
                "HIST-03b lost-acknowledgement replay after restart advanced generation");
        const std::string rotatedToken = reopened.acquireWorkingCopyLease(
            draftId, session, 120).token;
        expectConflict([&] { (void)reopened.navigateHistoryCheckpoint(
            UuidV7::generate().toString(), draftId, session, originalLease,
            3, latest, first); });
        const WorkingCopyInfo older = reopened.navigateHistoryCheckpoint(
            UuidV7::generate().toString(), draftId, session, rotatedToken,
            3, latest, first);
        require(older.generation == 4 && older.historyCheckpointRevisionId == firstId,
                "HIST-03b navigation failed after lease rotation");
        require(reopened.saveWorkingCopyChanges(draftId, session, rotatedToken,
                    4, {unrelated}).generation == 5,
                "HIST-03b post-navigation normal Save failed");
        const WorkingCopyInfo edited = reopened.workingCopy(draftId);
        require(!edited.historyCheckpointRevisionId &&
                !edited.lastHistoryNavigationOperationId &&
                !edited.lastHistoryNavigationSourceGeneration,
                "HIST-03b normal edit left a stale exact-checkpoint cursor");
        expectConflict([&] { (void)reopened.navigateHistoryCheckpoint(
            backOperation, draftId, session, rotatedToken, 2, latest, latest); });
        const std::string journalDraft = reopened.createWorkingCopy(
            projectId, "main", latest, UuidV7::generate().toString()).id;
        const WorkingCopyInfo journalInfo = reopened.workingCopy(journalDraft);
        const std::string writer = journalInfo.sessionId;
        const std::string token = reopened.acquireWorkingCopyLease(journalDraft, writer, 120).token;
        const ModelChange changed = changeWithValue(74);
        const aowis::dvmdb::Bytes event = aowis::dvmdb::test::editEvent(
            UuidV7::generate(), changed.key, {20}, {74});
        require(reopened.saveWorkingCopyTransactions(journalDraft, writer, token, 0,
                    {changed}, {event}).journalCount == 1,
                "HIST-03b active-journal fixture Save failed");
        expectConflict([&] { (void)reopened.navigateHistoryCheckpoint(
            UuidV7::generate().toString(), journalDraft, writer, token, 1,
            latest, first); });
        require(reopened.workingCopy(journalDraft).journal.size() == 1 &&
                reopened.workingCopy(journalDraft).generation == 1,
                "HIST-03b checkpoint navigation lost or reinterpreted active MODEL events");
        const RevisionId moved = reopened.commitModelAtExpectedHead(
            projectId, "main", {changeWithValue(33)}, commitMetadata(), latest);
        expectConflict([&] { (void)reopened.navigateHistoryCheckpoint(
            UuidV7::generate().toString(), draftId, session, rotatedToken,
            5, latest, first); });
        require(reopened.optionalHead(projectId, "main") == moved,
                "HIST-03b rejected stale HEAD navigation mutated an immutable revision");
    }
}

// HIST-03c3a: compare the durable local Commit's candidate with the *whole*
// immutable native revision delta. MODEL events are carried alongside that
// independently verified witness, but do not become a proof of logical
// action coverage until the MODEL-aware verifier has checked them all.
void testVerifiedRevisionNativeWitness() {
    TempRoot root;
    const std::string session = UuidV7::generate().toString();
    std::string projectId;
    std::string firstId;
    std::string secondId;
    const ModelChange one = changeWithValue(11);
    ModelChange two = changeWithValue(22);
    two.key[15] = 8;
    ModelChange three = changeWithValue(33);
    three.key[15] = 9;
    const aowis::dvmdb::Bytes eventOne = aowis::dvmdb::test::editEvent(
        UuidV7::generate(), one.key, {}, {11});
    const aowis::dvmdb::Bytes eventTwo = aowis::dvmdb::test::editEvent(
        UuidV7::generate(), two.key, {}, {22});
    {
        ProjectService service(root.path);
        projectId = service.createUnbornProject("HIST-03c3 native revision witness").id;
        service.configureRepositoryRole(RepositoryRole::Private);
        const std::string draft =
            service.createWorkingCopy(projectId, "main", std::nullopt, session).id;
        const std::string token = service.acquireWorkingCopyLease(draft, session, 120).token;
        require(service.saveWorkingCopyTransactions(draft, session, token, 0,
                    {one}, {eventOne}).generation == 1 &&
                service.saveWorkingCopyTransactions(draft, session, token, 1,
                    {two}, {eventTwo}).generation == 2,
                "HIST-03c3 first candidate Save failed");
        const LocalCommitInfo committed = service.commitWorkingCopy(
            UuidV7::generate().toString(), draft, session, token, 2,
            std::nullopt, commitMetadata());
        require(committed.state == LocalCommitState::Succeeded,
                "HIST-03c3 first local Commit failed");
        firstId = committed.reservedRevisionId;
        const aowis::dvmdb::RevisionNativeActionWitness initial =
            service.verifiedRevisionNativeActionWitness(
                projectId, "main", *RevisionId::parse(firstId));
        require(initial.revisionId == firstId &&
                initial.commitOperationId == committed.id &&
                !initial.parentRevision && initial.actions.size() == 2 &&
                initial.actions[0].encoded == eventOne &&
                initial.actions[1].encoded == eventTwo &&
                initial.actualNativeChanges.size() == 2 &&
                initial.actualNativeChanges[0].key == one.key &&
                initial.actualNativeChanges[1].key == two.key &&
                initial.actualNativeChanges[0].record &&
                initial.actualNativeChanges[0].record->payload == one.record->payload &&
                initial.actualNativeChanges[1].record &&
                initial.actualNativeChanges[1].record->payload == two.record->payload,
                "HIST-03c3 initial immutable delta differs from archived candidate");
        const std::string newer = service.createWorkingCopy(
            projectId, "main", *RevisionId::parse(firstId), session).id;
        const std::string newerToken =
            service.acquireWorkingCopyLease(newer, session, 120).token;
        ModelChange modified = changeWithValue(44);
        const aowis::dvmdb::Bytes update = aowis::dvmdb::test::editEvent(
            UuidV7::generate(), modified.key, {11}, {44});
        ModelChange deleted;
        deleted.key = two.key;
        // Native-only Save validates frame integrity, but not MODEL entity
        // images. Deliberately include two unjournaled native changes: this
        // must remain visible to the higher-level action coverage validator.
        require(service.saveWorkingCopyTransactions(newer, session, newerToken, 0,
                    {modified, deleted, three}, {update}).generation == 1,
                "HIST-03c3 mixed native candidate Save failed");
        const LocalCommitInfo changed = service.commitWorkingCopy(
            UuidV7::generate().toString(), newer, session, newerToken, 1,
            *RevisionId::parse(firstId), commitMetadata());
        require(changed.state == LocalCommitState::Succeeded,
                "HIST-03c3 second local Commit failed");
        secondId = changed.reservedRevisionId;
        const aowis::dvmdb::RevisionNativeActionWitness delta =
            service.verifiedRevisionNativeActionWitness(
                projectId, "main", *RevisionId::parse(secondId));
        require(delta.parentRevision == RevisionId::parse(firstId) &&
                delta.actions.size() == 1 && delta.actions[0].encoded == update &&
                delta.actualNativeChanges.size() == 3 &&
                delta.actualNativeChanges[0].key == one.key &&
                delta.actualNativeChanges[0].record &&
                delta.actualNativeChanges[0].record->payload == modified.record->payload &&
                delta.actualNativeChanges[1].key == two.key &&
                !delta.actualNativeChanges[1].record &&
                delta.actualNativeChanges[2].key == three.key &&
                delta.actualNativeChanges[2].record &&
                delta.actualNativeChanges[2].record->payload == three.record->payload,
                "HIST-03c3 missed native deletion or changes-only addition");
        // The witness deliberately does NOT mark the second archive complete:
        // one MODEL action cannot explain all three changed native keys.
        (void)service.createBranchAtRevision(
            projectId, "witness-feature", "main", *RevisionId::parse(firstId));
        bool branchRejected = false;
        try { (void)service.verifiedRevisionNativeActionWitness(
            projectId, "witness-feature", *RevisionId::parse(secondId)); }
        catch (const ProjectError& error) {
            branchRejected = error.kind() == ProjectErrorKind::InvalidInput;
        }
        require(branchRejected, "HIST-03c3 leaked a revision outside first-parent ancestry");
    }
    {
        ProjectService reopened(root.path);
        const aowis::dvmdb::RevisionNativeActionWitness recovered =
            reopened.verifiedRevisionNativeActionWitness(
                projectId, "main", *RevisionId::parse(secondId));
        require(recovered.parentRevision == RevisionId::parse(firstId) &&
                recovered.actualNativeChanges.size() == 3 &&
                recovered.actions.size() == 1,
                "HIST-03c3 native delta witness did not survive restart");
        const RevisionId generic = reopened.commitModelAtExpectedHead(
            projectId, "main", {changeWithValue(55)}, commitMetadata(),
            *RevisionId::parse(secondId));
        bool checkpointRejected = false;
        try { (void)reopened.verifiedRevisionNativeActionWitness(
            projectId, "main", generic); }
        catch (const ProjectError& error) {
            checkpointRejected = error.kind() == ProjectErrorKind::Conflict;
        }
        require(checkpointRejected,
                "HIST-03c3 fabricated archived actions for generic Commit checkpoint");
    }
}

void testCommittedActionHistory() {
    TempRoot root;
    const std::string session = UuidV7::generate().toString();
    const std::string operationId = UuidV7::generate().toString();
    std::string projectId;
    std::string draftId;
    std::string revisionId;
    const ModelChange initial = changeWithValue(41);
    const ModelChange updated = changeWithValue(42);
    const aowis::dvmdb::Bytes firstFrame = aowis::dvmdb::test::editEvent(
        UuidV7::generate(), initial.key, {40}, {41});
    const aowis::dvmdb::Bytes secondFrame = aowis::dvmdb::test::editEvent(
        UuidV7::generate(), initial.key, {41}, {42});
    {
        ProjectService service(root.path);
        projectId = service.createUnbornProject("HIST-02 immutable actions").id;
        service.configureRepositoryRole(RepositoryRole::Private);
        draftId = service.createWorkingCopy(projectId, "main", std::nullopt, session).id;
        const std::string token = service.acquireWorkingCopyLease(draftId, session, 120).token;
        require(service.saveWorkingCopyTransactions(draftId, session, token, 0,
                    {initial}, {firstFrame}).generation == 1,
                "first HIST-02 action was not saved");
        require(service.saveWorkingCopyTransactions(draftId, session, token, 1,
                    {updated}, {secondFrame}).generation == 2,
                "second HIST-02 action was not saved");
        const aowis::dvmdb::BranchActionHistoryPage pending =
            service.branchActionHistory(projectId, "main", 2, std::nullopt, draftId);
        require(pending.commits.empty() && pending.draftId == draftId &&
                pending.draftGeneration == 2 && pending.draftEventCount == 2 &&
                pending.draftActionIds.size() == 2 &&
                pending.draftActionIds[0] == service.workingCopy(draftId).journal[0].id &&
                pending.draftActionIds[1] == service.workingCopy(draftId).journal[1].id &&
                !pending.next && !pending.nextDraftOffset,
                "HIST-02 unborn branch timeline lost pending draft actions");
        const LocalCommitInfo commit = service.commitWorkingCopy(operationId, draftId,
            session, token, 2, std::nullopt, commitMetadata());
        require(commit.state == LocalCommitState::Succeeded,
                "HIST-02 Commit did not succeed");
        revisionId = commit.reservedRevisionId;
        const aowis::dvmdb::LocalCommitActionHistory archive =
            service.localCommitActionHistory(operationId);
        require(archive.commit.id == operationId &&
                archive.commit.reservedRevisionId == revisionId &&
                archive.actions.size() == 2 &&
                archive.actions[0].encoded == firstFrame &&
                archive.actions[1].encoded == secondFrame &&
                archive.committedAtMs != 0 &&
                service.workingCopy(draftId).journal.empty() &&
                service.workingCopy(draftId).journalCount == 0,
                "Commit lost, reordered, or mutated durable actions during draft rebase");
        const aowis::dvmdb::BranchActionHistoryPage timeline =
            service.branchActionHistory(projectId, "main", 2);
        require(timeline.commits.size() == 1 &&
                timeline.commits[0].commitOperationId == operationId &&
                timeline.commits[0].revision.id.toString() == revisionId &&
                timeline.commits[0].actionIds.size() == 2 &&
                timeline.commits[0].actionIds[0] == archive.actions[0].id &&
                timeline.commits[0].actionIds[1] == archive.actions[1].id &&
                !timeline.next && !timeline.draftId,
                "HIST-02 branch timeline did not expose committed action archive");
        const std::filesystem::path index =
            root.path / "local-commit-revisions" / revisionId;
        require(std::filesystem::is_regular_file(index),
                "HIST-02 durable revision index was not published before Commit");
        const aowis::dvmdb::RevisionActionPage firstPage =
            service.revisionActionPage(projectId, "main", *RevisionId::parse(revisionId), 0, 1);
        const aowis::dvmdb::RevisionActionPage secondPage =
            service.revisionActionPage(projectId, "main", *RevisionId::parse(revisionId), 1, 1);
        require(firstPage.revisionId == revisionId && firstPage.commitOperationId == operationId &&
                firstPage.total == 2 && firstPage.actions.size() == 1 &&
                firstPage.actions[0].encoded == firstFrame && firstPage.nextOffset == 1 &&
                secondPage.total == 2 && secondPage.actions.size() == 1 &&
                secondPage.actions[0].encoded == secondFrame && !secondPage.nextOffset,
                "HIST-02 archived MODEL frame pagination lost order or exact bytes");
        // HIST-03c1: a position is an action boundary, not a model
        // snapshot. The planner returns original frames newest-first for
        // future MODEL-aware inversion, never an unvalidated overlay.
        const aowis::dvmdb::HistoryReverseActionWindow lastAction =
            service.readHistoryReverseActionWindow(draftId,
                *RevisionId::parse(revisionId), 2, 3, 1);
        const aowis::dvmdb::HistoryReverseActionWindow firstAction =
            service.readHistoryReverseActionWindow(draftId,
                *RevisionId::parse(revisionId), 1, 3, 1);
        const aowis::dvmdb::HistoryReverseActionWindow firstBoundary =
            service.readHistoryReverseActionWindow(draftId,
                *RevisionId::parse(revisionId), 0, 3, 1);
        require(lastAction.commitOperationId == operationId &&
                lastAction.expectedHead == RevisionId::parse(revisionId) &&
                lastAction.actionCount == 2 && lastAction.fromPosition == 2 &&
                lastAction.originalFramesNewestFirst.size() == 1 &&
                lastAction.originalFramesNewestFirst[0].encoded == secondFrame &&
                lastAction.nextPosition == 1 &&
                firstAction.originalFramesNewestFirst.size() == 1 &&
                firstAction.originalFramesNewestFirst[0].encoded == firstFrame &&
                !firstAction.nextPosition &&
                firstBoundary.originalFramesNewestFirst.empty() &&
                !firstBoundary.nextPosition,
                "HIST-03c1 reversed action boundaries skipped or mutated MODEL frames");
        expectConflict([&]() { (void)service.readHistoryReverseActionWindow(
            draftId, *RevisionId::parse(revisionId), 2, 2); });
        bool invalidActionBoundary = false;
        try { (void)service.readHistoryReverseActionWindow(
            draftId, *RevisionId::parse(revisionId), 3, 3); }
        catch (const ProjectError& error) {
            invalidActionBoundary = error.kind() == ProjectErrorKind::InvalidInput;
        }
        require(invalidActionBoundary,
                "HIST-03c1 accepted an action boundary outside the Commit archive");
        bool refusedPage = false;
        try {
            (void)service.revisionActionPage(projectId, "main",
                *RevisionId::parse(revisionId), 3, 1);
        } catch (const ProjectError& error) {
            refusedPage = error.kind() == ProjectErrorKind::InvalidInput;
        }
        require(refusedPage, "archived MODEL frame retrieval accepted an invalid offset");
        const LocalCommitInfo replay = service.commitWorkingCopy(operationId, draftId,
            session, token, 2, std::nullopt, commitMetadata());
        require(replay.state == LocalCommitState::Succeeded &&
                service.localCommitActionHistory(operationId).actions.size() == 2,
                "Commit replay duplicated or erased the action archive");
    }
    // Model an interrupted outcome publication: the revision is already in
    // HEAD, but the durable intent says pending. Status recovery may rewrite
    // the outcome; it must never rewrite or discard the archived actions.
    {
        const std::filesystem::path ledger = root.path / "local-commits" / operationId;
        std::ifstream input(ledger, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(input)), {});
        const std::string oldState = "state\tsucceeded\n";
        const std::size_t state = body.find(oldState);
        require(state != std::string::npos, "missing HIST-02 succeeded fixture state");
        body.replace(state, oldState.size(), "state\tpending\n");
        std::ofstream output(ledger, std::ios::binary | std::ios::trunc);
        output.write(body.data(), static_cast<std::streamsize>(body.size()));
        require(output.good(), "could not stage interrupted HIST-02 outcome");
    }
    {
        ProjectService reopened(root.path);
        const aowis::dvmdb::LocalCommitActionHistory history =
            reopened.localCommitActionHistory(operationId);
        require(history.actions.size() == 2 &&
                history.actions[0].encoded == firstFrame &&
                history.actions[1].encoded == secondFrame &&
                history.commit.reservedRevisionId == revisionId,
                "process restart did not preserve exact archived MODEL frames");
        const aowis::dvmdb::RevisionActionPage recoveredPage =
            reopened.revisionActionPage(projectId, "main", *RevisionId::parse(revisionId), 0, 100);
        require(recoveredPage.actions.size() == 2 &&
                recoveredPage.actions[0].encoded == firstFrame &&
                recoveredPage.actions[1].encoded == secondFrame && !recoveredPage.nextOffset,
                "HIST-02 revision index lost original frames after restart and reconciliation");
        const RevisionId committed = *RevisionId::parse(revisionId);
        const aowis::dvmdb::HistoryReverseActionWindow restartPlan =
            reopened.readHistoryReverseActionWindow(draftId, committed, 2, 3, 2);
        require(restartPlan.originalFramesNewestFirst.size() == 2 &&
                restartPlan.originalFramesNewestFirst[0].encoded == secondFrame &&
                restartPlan.originalFramesNewestFirst[1].encoded == firstFrame &&
                !restartPlan.nextPosition,
                "HIST-03c1 action plan did not survive repository restart");
        const std::string nextDraft = reopened.createWorkingCopy(
            projectId, "main", committed, session).id;
        const std::string nextToken =
            reopened.acquireWorkingCopyLease(nextDraft, session, 120).token;
        require(reopened.saveWorkingCopyChanges(nextDraft, session, nextToken, 0,
                    {changeWithValue(43)}).generation == 1,
                "history checkpoint-only candidate save failed");
        const std::string nextOperation = UuidV7::generate().toString();
        const LocalCommitInfo next = reopened.commitWorkingCopy(nextOperation,
            nextDraft, session, nextToken, 1, committed, commitMetadata());
        require(next.state == LocalCommitState::Succeeded &&
                reopened.localCommitActionHistory(nextOperation).actions.empty() &&
                reopened.localCommitActionHistory(operationId).actions.size() == 2,
                "checkpoint-only Commit fabricated actions or modified older archive");
        const aowis::dvmdb::HistoryReverseActionWindow emptyCheckpoint =
            reopened.readHistoryReverseActionWindow(nextDraft,
                *RevisionId::parse(next.reservedRevisionId), 0, 2);
        require(emptyCheckpoint.actionCount == 0 &&
                emptyCheckpoint.originalFramesNewestFirst.empty() &&
                emptyCheckpoint.commitOperationId == nextOperation,
                "HIST-03c1 fabricated actions inside a checkpoint-only revision");
        expectConflict([&]() { (void)reopened.readHistoryReverseActionWindow(
            draftId, committed, 2, 3); }); // Old draft no longer tracks HEAD.
        const aowis::dvmdb::RevisionActionPage checkpoint =
            reopened.revisionActionPage(projectId, "main",
                *RevisionId::parse(next.reservedRevisionId));
        require(checkpoint.revisionId == next.reservedRevisionId &&
                checkpoint.actions.empty() && checkpoint.total == 0 &&
                checkpoint.commitOperationId == nextOperation,
                "HIST-02 checkpoint without events fabricated archived MODEL frames");
        const aowis::dvmdb::BranchActionHistoryPage latest =
            reopened.branchActionHistory(projectId, "main", 1);
        require(latest.commits.size() == 1 &&
                latest.commits[0].commitOperationId == nextOperation &&
                latest.commits[0].actionIds.empty() && latest.next &&
                latest.commits[0].revision.id.toString() == next.reservedRevisionId,
                "HIST-02 page one should be a checkpoint-only newest Commit");
        const aowis::dvmdb::BranchActionHistoryPage older =
            reopened.branchActionHistory(projectId, "main", 1, latest.next);
        require(older.commits.size() == 1 &&
                older.commits[0].commitOperationId == operationId &&
                older.commits[0].actionIds.size() == 2 && !older.next,
                "HIST-02 exclusive revision cursor skipped the earlier actions");
        const std::string feature = "hist-feature";
        (void)reopened.createBranchAtRevision(projectId, feature, "main", *RevisionId::parse(revisionId));
        const aowis::dvmdb::BranchActionHistoryPage inherited =
            reopened.branchActionHistory(projectId, feature, 2);
        require(inherited.commits.size() == 1 &&
                inherited.commits[0].actionIds.size() == 2 &&
                inherited.commits[0].revision.id.toString() == revisionId,
                "HIST-02 feature branch did not inherit first-parent action history");
        bool badCursor = false;
        try {
            (void)reopened.branchActionHistory(projectId, feature, 2, *RevisionId::parse(next.reservedRevisionId));
        } catch (const ProjectError& error) {
            badCursor = error.kind() == ProjectErrorKind::InvalidInput;
        }
        require(badCursor, "HIST-02 cursor accepted a revision outside the branch lineage");
        bool unrelatedFrame = false;
        try {
            (void)reopened.revisionActionPage(projectId, feature,
                *RevisionId::parse(next.reservedRevisionId));
        } catch (const ProjectError& error) {
            unrelatedFrame = error.kind() == ProjectErrorKind::InvalidInput;
        }
        require(unrelatedFrame, "HIST-02 exposed frames from a revision outside the branch");
        bool badDraftOffset = false;
        try {
            (void)reopened.branchActionHistory(projectId, "main", 2, std::nullopt, nextDraft, 1);
        } catch (const ProjectError& error) {
            badDraftOffset = error.kind() == ProjectErrorKind::InvalidInput;
        }
        require(badDraftOffset, "HIST-02 accepted invalid draft event offset");
    }
    // The sidecar is authenticated and may not redirect a revision to a
    // different operation after restart.
    const std::filesystem::path sidecar = root.path / "local-commit-revisions" / revisionId;
    std::string originalSidecar;
    {
        std::ifstream input(sidecar, std::ios::binary);
        originalSidecar.assign(std::istreambuf_iterator<char>(input), {});
        require(originalSidecar.starts_with("AOWIS-LOCAL-REVISION-INDEX-1\n"),
                "HIST-02 revision sidecar fixture is missing");
        std::string tampered = originalSidecar;
        const std::size_t checksum = tampered.rfind('\n', tampered.size() - 2);
        require(checksum != std::string::npos && checksum + 1 < tampered.size() - 1,
                "HIST-02 revision sidecar checksum missing");
        tampered[checksum + 1] = tampered[checksum + 1] == 'a' ? 'b' : 'a';
        std::ofstream output(sidecar, std::ios::binary | std::ios::trunc);
        output.write(tampered.data(), static_cast<std::streamsize>(tampered.size()));
        require(output.good(), "could not corrupt revision sidecar fixture");
    }
    {
        ProjectService corruptedIndex(root.path);
        bool rejectedIndex = false;
        try { (void)corruptedIndex.branchActionHistory(projectId, "main", 2); }
        catch (const aowis::dvmdb::StorageError& error) {
            rejectedIndex = error.kind() == aowis::dvmdb::StorageErrorKind::Corruption;
        }
        require(rejectedIndex, "corrupted revision index silently lost action history");
    }
    {
        std::ofstream output(sidecar, std::ios::binary | std::ios::trunc);
        output.write(originalSidecar.data(), static_cast<std::streamsize>(originalSidecar.size()));
        require(output.good(), "could not restore revision sidecar fixture");
    }
    // Unrelated ledger entries must not change indexed lookup capacity.
    // This exceeds the previous 10,000-operation bounded scan limit.
    for (int count = 0; count < 10010; ++count) {
        const std::filesystem::path unrelated =
            root.path / "local-commits" / UuidV7::generate().toString();
        std::ofstream output(unrelated, std::ios::binary);
        output << "unrelated unindexed operation";
        require(output.good(), "cannot create unrelated Commit fixture");
    }
    {
        ProjectService largeLedger(root.path);
        const aowis::dvmdb::BranchActionHistoryPage page =
            largeLedger.branchActionHistory(projectId, "main", 2);
        require(page.commits.size() == 2 && page.commits.back().actionIds.size() == 2,
                "HIST-02 revision index still scans unrelated Commit ledger records");
    }
    // Alter the archival bytes without updating their chain digest: unlike
    // an unavailable old action, corrupted durable history is never silently
    // presented as an empty list.
    const std::filesystem::path ledger = root.path / "local-commits" / operationId;
    {
        std::ifstream input(ledger, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(input)), {});
        const std::size_t line = body.find("action\t");
        require(line != std::string::npos, "HIST-02 missing durable action row");
        const std::size_t payload = body.find('\t', line + 7);
        require(payload != std::string::npos && payload + 1 < body.size(),
                "HIST-02 malformed fixture action row");
        body[payload + 1] = body[payload + 1] == 'a' ? 'b' : 'a';
        std::ofstream output(ledger, std::ios::binary | std::ios::trunc);
        output.write(body.data(), static_cast<std::streamsize>(body.size()));
        require(output.good(), "could not corrupt action archive fixture");
    }
    ProjectService corrupted(root.path);
    bool rejected = false;
    try { (void)corrupted.branchActionHistory(projectId, "main", 2); }
    catch (const aowis::dvmdb::StorageError& error) {
        rejected = error.kind() == aowis::dvmdb::StorageErrorKind::Corruption;
    }
    require(rejected, "corrupted action archive was exposed in paginated branch history");
    rejected = false;
    try { (void)corrupted.localCommitActionHistory(operationId); }
    catch (const aowis::dvmdb::StorageError& error) {
        rejected = error.kind() == aowis::dvmdb::StorageErrorKind::Corruption;
    }
    require(rejected, "tampered Commit action was not rejected as corruption");
}

void testBranchDraftHistoryPaging() {
    TempRoot root;
    const std::string session = UuidV7::generate().toString();
    std::string projectId;
    std::string draftId;
    std::vector<std::string> expected;
    {
        ProjectService service(root.path);
        projectId = service.createUnbornProject("HIST-02 draft action pages").id;
        service.configureRepositoryRole(RepositoryRole::Private);
        draftId = service.createWorkingCopy(projectId, "main", std::nullopt, session).id;
        const std::string token = service.acquireWorkingCopyLease(draftId, session, 120).token;
        for (std::uint64_t index = 0; index < 125; ++index) {
            const ModelChange change = changeWithValue(static_cast<std::uint8_t>(index + 1));
            const aowis::dvmdb::Bytes frame = aowis::dvmdb::test::editEvent(
                UuidV7::generate(), change.key,
                {static_cast<std::uint8_t>(index)},
                {static_cast<std::uint8_t>(index + 1)});
            const WorkingCopyInfo saved = service.saveWorkingCopyTransactions(
                draftId, session, token, index, {change}, {frame});
            require(saved.generation == index + 1 && saved.journal.size() == 1,
                    "HIST-02 repeated draft Save lost the journal event");
            expected.push_back(saved.journal.front().id);
        }
        const aowis::dvmdb::BranchActionHistoryPage first =
            service.branchActionHistory(projectId, "main", 1, std::nullopt, draftId);
        const aowis::dvmdb::BranchActionHistoryPage second =
            service.branchActionHistory(projectId, "main", 1, std::nullopt, draftId, 100, 125);
        require(first.commits.empty() && !first.next && first.draftGeneration == 125 &&
                first.draftEventCount == 125 && first.draftActionIds.size() == 100 &&
                first.draftActionFrames.size() == 100 && first.nextDraftOffset == 100 &&
                second.draftActionIds.size() == 25 && second.draftActionFrames.size() == 25 &&
                !second.nextDraftOffset &&
                std::equal(expected.begin(), expected.begin() + 100,
                           first.draftActionIds.begin()) &&
                std::equal(expected.begin() + 100, expected.end(),
                           second.draftActionIds.begin()),
                "HIST-02 draft pagination skipped or duplicated MODEL actions");
        bool missingGenerationRejected = false;
        try {
            (void)service.branchActionHistory(projectId, "main", 1,
                                             std::nullopt, draftId, 100);
        } catch (const ProjectError& error) {
            missingGenerationRejected = error.kind() == ProjectErrorKind::InvalidInput;
        }
        require(missingGenerationRejected,
                "HIST-02 offset continuation without generation was accepted");
        const ModelChange subsequent = changeWithValue(127);
        const aowis::dvmdb::Bytes newFrame = aowis::dvmdb::test::editEvent(
            UuidV7::generate(), subsequent.key, {126}, {127});
        const WorkingCopyInfo updated = service.saveWorkingCopyTransactions(
            draftId, session, token, 125, {subsequent}, {newFrame});
        require(updated.generation == 126 && updated.journal.size() == 1,
                "HIST-02 concurrent writer did not advance draft generation");
        expected.push_back(updated.journal.front().id);
        expectConflict([&]() {
            (void)service.branchActionHistory(projectId, "main", 1,
                                             std::nullopt, draftId, 100, 125);
        });
        const aowis::dvmdb::BranchActionHistoryPage updatedSecond =
            service.branchActionHistory(projectId, "main", 1,
                                        std::nullopt, draftId, 100, 126);
        require(updatedSecond.draftGeneration == 126 &&
                updatedSecond.draftActionIds.size() == 26 &&
                updatedSecond.draftActionFrames.size() == 26 &&
                !updatedSecond.nextDraftOffset &&
                updatedSecond.draftActionIds.back() == expected.back(),
                "HIST-02 generation-bound restart did not recover complete journal");
        (void)service.createUnbornBranch(projectId, "other");
        expectConflict([&]() {
            (void)service.branchActionHistory(projectId, "other", 1, std::nullopt, draftId);
        });
        bool invalidOffset = false;
        try {
            (void)service.branchActionHistory(projectId, "main", 1, std::nullopt, draftId, 126);
        } catch (const ProjectError& error) {
            invalidOffset = error.kind() == ProjectErrorKind::InvalidInput;
        }
        require(invalidOffset, "HIST-02 draft offset past the journal was accepted");
    }
    {
        ProjectService reopened(root.path);
        const aowis::dvmdb::BranchActionHistoryPage second =
            reopened.branchActionHistory(projectId, "main", 1, std::nullopt, draftId, 100, 126);
        require(second.draftGeneration == 126 && second.draftActionIds.size() == 26 &&
                std::equal(expected.begin() + 100, expected.end(),
                           second.draftActionIds.begin()),
                "HIST-02 paginated draft action history changed after restart");
        const std::string token =
            reopened.acquireWorkingCopyLease(draftId, session, 120).token;
        const std::string operation = UuidV7::generate().toString();
        const LocalCommitInfo committed = reopened.commitWorkingCopy(operation,
            draftId, session, token, 126, std::nullopt, commitMetadata());
        require(committed.state == LocalCommitState::Succeeded,
                "HIST-02 126-event draft could not be committed");
        expectConflict([&]() {
            (void)reopened.branchActionHistory(projectId, "main", 1,
                                               std::nullopt, draftId, 100, 126);
        });
        const RevisionId revision = *RevisionId::parse(committed.reservedRevisionId);
        const aowis::dvmdb::RevisionActionPage first =
            reopened.revisionActionPage(projectId, "main", revision, 0, 100);
        const aowis::dvmdb::RevisionActionPage last =
            reopened.revisionActionPage(projectId, "main", revision, 100, 100);
        require(first.total == 126 && first.actions.size() == 100 &&
                first.nextOffset == 100 && last.total == 126 &&
                last.actions.size() == 26 && !last.nextOffset,
                "HIST-02 committed frame pages did not preserve 100+25 boundary");
        for (std::size_t index = 0; index < expected.size(); ++index) {
            const aowis::dvmdb::WorkingCopyEvent& action =
                index < 100 ? first.actions[index] : last.actions[index - 100];
            require(action.id == expected[index] && !action.encoded.empty(),
                    "HIST-02 committed frame pagination changed event identity or bytes");
        }
        // 126 archived actions: walk backward over a 100-action boundary.
        const aowis::dvmdb::HistoryReverseActionWindow reversed =
            reopened.readHistoryReverseActionWindow(draftId, revision, 126, 127, 100);
        const aowis::dvmdb::HistoryReverseActionWindow remainder =
            reopened.readHistoryReverseActionWindow(draftId, revision, 26, 127, 100);
        require(reversed.actionCount == 126 && reversed.fromPosition == 126 &&
                reversed.originalFramesNewestFirst.size() == 100 &&
                reversed.nextPosition == 26 &&
                reversed.originalFramesNewestFirst.front().id == expected[125] &&
                reversed.originalFramesNewestFirst.back().id == expected[26] &&
                remainder.originalFramesNewestFirst.size() == 26 &&
                remainder.originalFramesNewestFirst.front().id == expected[25] &&
                remainder.originalFramesNewestFirst.back().id == expected[0] &&
                !remainder.nextPosition,
                "HIST-03c1 reversed action paging did not cross 100-frame boundary");
    }
}

void testLocalCommitRecovery() {
    TempRoot root;
    const std::string session = UuidV7::generate().toString();
    const std::string secondSession = UuidV7::generate().toString();
    const std::string operationId = UuidV7::generate().toString();
    const std::string staleOperationId = UuidV7::generate().toString();
    std::string projectId;
    std::string branchId;
    std::string firstDraft;
    std::string firstToken;
    std::string staleDraft;
    std::string staleToken;
    std::string firstRevision;
    {
        ProjectService service(root.path);
        const aowis::dvmdb::ProjectInfo project =
            service.createUnbornProject("private local commits");
        projectId = project.id;
        branchId = service.branchId(projectId, "main")->toString();
        service.configureRepositoryRole(RepositoryRole::Private);
        const WorkingCopyInfo first = service.createWorkingCopyForBranch(
            projectId, branchId, std::nullopt, session);
        const WorkingCopyInfo second = service.createWorkingCopyForBranch(
            projectId, branchId, std::nullopt, secondSession);
        firstDraft = first.id;
        staleDraft = second.id;
        firstToken = service.acquireWorkingCopyLease(firstDraft, session, 120).token;
        staleToken = service.acquireWorkingCopyLease(staleDraft, secondSession, 120).token;
        require(service.saveWorkingCopyChanges(firstDraft, session, firstToken, 0,
                        {changeWithValue(15)}).generation == 1,
                "first candidate was not durably saved");
        require(service.saveWorkingCopyChanges(staleDraft, secondSession, staleToken, 0,
                        {changeWithValue(22)}).generation == 1,
                "concurrent candidate was not durably saved");
        require(service.revisionGraph(projectId).revisions.empty(),
                "draft autosaves unexpectedly committed a revision");
        const LocalCommitInfo committed = service.commitWorkingCopy(
            operationId, firstDraft, session, firstToken, 1, std::nullopt, commitMetadata());
        require(committed.state == LocalCommitState::Succeeded &&
                committed.expectedHead == std::nullopt &&
                committed.repositoryId == service.repositoryId() &&
                RevisionId::parse(committed.reservedRevisionId).has_value() &&
                committed.payloadDigest.size() == 64,
                "first Commit did not return durable UUIDv7 outcome");
        firstRevision = committed.reservedRevisionId;
        const std::optional<RevisionId> saved = service.optionalHead(projectId, "main");
        require(saved && saved->toString() == firstRevision,
                "first Commit did not advance only the local HEAD");
        const std::optional<ModelRecord> loaded = service.getModelAtRevision(
            projectId, *saved, changeWithValue(15).key);
        require(loaded && loaded->payload == std::vector<std::uint8_t>{15},
                "first immutable revision did not contain saved draft candidate");
        const aowis::dvmdb::RevisionGraphRevision revision = service.revision(projectId, *saved);
        require(revision.parents.empty(), "first Commit must have zero parents");
        const LocalCommitInfo replay = service.commitWorkingCopy(
            operationId, firstDraft, session, firstToken, 1, std::nullopt, commitMetadata());
        require(replay.state == LocalCommitState::Succeeded &&
                replay.reservedRevisionId == firstRevision &&
                service.revisionGraph(projectId).revisions.size() == 1,
                "operation replay duplicated the first Commit");
        expectConflict([&]() {
            (void)service.commitWorkingCopy(operationId, staleDraft, secondSession,
                                               staleToken, 1, std::nullopt, commitMetadata());
        });
        const LocalCommitInfo stale = service.commitWorkingCopy(
            staleOperationId, staleDraft, secondSession, staleToken, 1, std::nullopt, commitMetadata());
        require(stale.state == LocalCommitState::Rejected &&
                stale.rejection == "conflict" &&
                service.localCommitStatus(staleOperationId).state == LocalCommitState::Rejected,
                "stale HEAD outcome was not durably rejected");
        require(service.workingCopy(staleDraft).dirty &&
                service.workingCopy(staleDraft).generation == 1,
                "stale first-commit conflict destroyed the competing draft");
    }
    {
        // Simulate interruption after committed HEAD publication but before
        // recording the successful operation status. Reconciliation must not
        // replay the commit or allocate another revision UUID.
        const std::filesystem::path path = root.path / "local-commits" / operationId;
        std::ifstream input(path, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
        const std::string succeeded = "state\tsucceeded\n";
        const std::size_t offset = body.find(succeeded);
        require(offset != std::string::npos, "missing durable operation outcome fixture");
        body.replace(offset, succeeded.size(), "state\tpending\n");
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << body;
        require(output.good(), "failed to stage interrupted status fixture");
    }
    {
        ProjectService reopened(root.path);
        const LocalCommitInfo recovered = reopened.localCommitStatus(operationId);
        require(recovered.state == LocalCommitState::Succeeded &&
                recovered.reservedRevisionId == firstRevision,
                "restart failed to recover committed operation from branch ancestry");
        const LocalCommitInfo retry = reopened.commitWorkingCopy(
            operationId, firstDraft, session, firstToken, 1, std::nullopt, commitMetadata());
        require(retry.state == LocalCommitState::Succeeded &&
                reopened.revisionGraph(projectId).revisions.size() == 1,
                "recovery retry created a duplicate revision");

        const RevisionId parent = *RevisionId::parse(firstRevision);
        const std::string draftId = reopened.createWorkingCopyForBranch(
            projectId, branchId, parent, session).id;
        const WorkingCopyLease lease = reopened.acquireWorkingCopyLease(draftId, session, 120);
        const ModelChange changed = changeWithValue(27);
        reopened.saveWorkingCopyChanges(draftId, session, lease.token, 0, {changed});
        const std::string secondOperationId = UuidV7::generate().toString();
        const LocalCommitInfo result = reopened.commitWorkingCopy(
            secondOperationId, draftId, session, lease.token, 1, parent, commitMetadata());
        require(result.state == LocalCommitState::Succeeded &&
                result.reservedRevisionId != firstRevision,
                "subsequent Commit did not allocate a new revision UUID");
        const RevisionId child = *RevisionId::parse(result.reservedRevisionId);
        const aowis::dvmdb::RevisionGraphRevision revision = reopened.revision(projectId, child);
        require(revision.parents.size() == 1 && revision.parents[0].id == parent,
                "second local Commit did not preserve immutable parent");
        require(reopened.getModelAtRevision(projectId, parent, changed.key)->payload ==
                    std::vector<std::uint8_t>{15} &&
                reopened.getModelAtRevision(projectId, child, changed.key)->payload ==
                    std::vector<std::uint8_t>{27},
                "local Commit modified historical model content");
    }
}


void testConcurrentFirstCommitIsSerialized() {
    TempRoot root;
    ProjectService service(root.path);
    const std::string projectId = service.createUnbornProject("concurrent local Commit").id;
    service.configureRepositoryRole(RepositoryRole::Private);
    const std::string firstSession = UuidV7::generate().toString();
    const std::string secondSession = UuidV7::generate().toString();
    const std::string firstDraft = service.createWorkingCopy(
        projectId, "main", std::nullopt, firstSession).id;
    const std::string secondDraft = service.createWorkingCopy(
        projectId, "main", std::nullopt, secondSession).id;
    const std::string firstToken = service.acquireWorkingCopyLease(
        firstDraft, firstSession, 120).token;
    const std::string secondToken = service.acquireWorkingCopyLease(
        secondDraft, secondSession, 120).token;
    service.saveWorkingCopyChanges(firstDraft, firstSession, firstToken, 0,
                                     {changeWithValue(31)});
    service.saveWorkingCopyChanges(secondDraft, secondSession, secondToken, 0,
                                     {changeWithValue(32)});
    const std::string firstOperation = UuidV7::generate().toString();
    const std::string secondOperation = UuidV7::generate().toString();
    LocalCommitInfo first;
    LocalCommitInfo second;
    std::exception_ptr firstError;
    std::exception_ptr secondError;
    std::thread left([&]() {
        try { first = service.commitWorkingCopy(firstOperation, firstDraft,
                                                   firstSession, firstToken, 1, std::nullopt, commitMetadata()); }
        catch (...) { firstError = std::current_exception(); }
    });
    std::thread right([&]() {
        try { second = service.commitWorkingCopy(secondOperation, secondDraft,
                                                    secondSession, secondToken, 1, std::nullopt, commitMetadata()); }
        catch (...) { secondError = std::current_exception(); }
    });
    left.join();
    right.join();
    if (firstError) std::rethrow_exception(firstError);
    if (secondError) std::rethrow_exception(secondError);
    require((first.state == LocalCommitState::Succeeded &&
             second.state == LocalCommitState::Rejected) ||
            (second.state == LocalCommitState::Succeeded &&
             first.state == LocalCommitState::Rejected),
            "concurrent initial Commit allowed two successful branches");
    const bool firstWon = first.state == LocalCommitState::Succeeded;
    const WorkingCopyInfo winner = service.workingCopy(firstWon ? firstDraft : secondDraft);
    const WorkingCopyInfo loser = service.workingCopy(firstWon ? secondDraft : firstDraft);
    const std::string committed = firstWon ? first.reservedRevisionId : second.reservedRevisionId;
    require(service.revisionGraph(projectId).revisions.size() == 1 &&
            !winner.dirty && winner.generation == 2 &&
            winner.baseRevision == RevisionId::parse(committed) &&
            loser.dirty && loser.generation == 1 && !loser.baseRevision,
            "concurrent first Commit wrote duplicate history, lost a draft or did not "
            "rebase only the committed draft");
}

void testRejectedOperationIsDurable() {
    TempRoot root;
    const std::string session = UuidV7::generate().toString();
    const std::string operationId = UuidV7::generate().toString();
    std::string draftId;
    std::string token;
    {
        ProjectService service(root.path);
        const std::string projectId = service.createUnbornProject("rejected Commit").id;
        service.configureRepositoryRole(RepositoryRole::Private);
        draftId = service.createWorkingCopy(projectId, "main", std::nullopt, session).id;
        token = service.acquireWorkingCopyLease(draftId, session, 120).token;
        ModelChange invalid = changeWithValue(5);
        Key absent{};
        absent[15] = 9;
        invalid.record->references.push_back(absent);
        service.saveWorkingCopyChanges(draftId, session, token, 0, {invalid});
        const LocalCommitInfo rejected = service.commitWorkingCopy(
            operationId, draftId, session, token, 1, std::nullopt, commitMetadata());
        require(rejected.state == LocalCommitState::Rejected &&
                rejected.rejection == "invalid-input" &&
                !service.optionalHead(projectId, "main"),
                "invalid model commit was not rejected without changing HEAD");
        const LocalCommitInfo retry = service.commitWorkingCopy(
            operationId, draftId, session, token, 1, std::nullopt, commitMetadata());
        require(retry.state == LocalCommitState::Rejected &&
                retry.reservedRevisionId == rejected.reservedRevisionId,
                "rejected operation was retried instead of returning outcome");
    }
    {
        ProjectService reopened(root.path);
        require(reopened.localCommitStatus(operationId).state == LocalCommitState::Rejected,
                "rejected operation status disappeared after restart");
        require(reopened.workingCopy(draftId).dirty,
                "rejected Commit unexpectedly deleted saved draft");
    }
}
// R4: a session that took over an expired lease is the writer: it can save
// AND commit, while the creating session's stale token can do neither.
void testLeaseTakeoverCanCommit() {
    TempRoot root;
    ProjectService service(root.path);
    const std::string projectId = service.createUnbornProject("lease takeover").id;
    service.configureRepositoryRole(RepositoryRole::Private);
    const std::string creator = UuidV7::generate().toString();
    const std::string successor = UuidV7::generate().toString();
    const std::string draftId = service.createWorkingCopy(
        projectId, "main", std::nullopt, creator).id;
    const std::string staleToken = service.acquireWorkingCopyLease(draftId, creator, 1).token;
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const std::string token = service.acquireWorkingCopyLease(draftId, successor, 120).token;
    expectConflict([&]() {
        (void)service.saveWorkingCopyChanges(draftId, creator, staleToken, 0, {changeWithValue(3)});
    });
    require(service.saveWorkingCopyChanges(draftId, successor, token, 0,
                                           {changeWithValue(4)}).generation == 1,
            "lease successor could not save");
    expectConflict([&]() {
        (void)service.commitWorkingCopy(UuidV7::generate().toString(), draftId, creator,
                                        staleToken, 1, std::nullopt, commitMetadata());
    });
    const LocalCommitInfo committed = service.commitWorkingCopy(
        UuidV7::generate().toString(), draftId, successor, token, 1, std::nullopt, commitMetadata());
    require(committed.state == LocalCommitState::Succeeded &&
            service.optionalHead(projectId, "main") ==
                RevisionId::parse(committed.reservedRevisionId),
            "lease successor could save but not Commit the recovered draft");
}

// Commits a one-record candidate from a fresh draft at the current HEAD.
LocalCommitInfo commitFreshDraft(ProjectService& service, const std::string& projectId,
                                 const std::string& operationId, const ModelChange& change) {
    const std::string session = UuidV7::generate().toString();
    const std::optional<RevisionId> head = service.optionalHead(projectId, "main");
    const std::string draftId = service.createWorkingCopy(projectId, "main", head, session).id;
    const std::string token = service.acquireWorkingCopyLease(draftId, session, 120).token;
    service.saveWorkingCopyChanges(draftId, session, token, 0, {change});
    return service.commitWorkingCopy(operationId, draftId, session, token, 1, head, commitMetadata());
}

// R5: a Commit that failed with an I/O error before HEAD publication stays
// pending only while HEAD is still at its base. Once another Commit moves
// HEAD, the outcome is decided: status and retries converge to Rejected.
void testFailedCommitIsRejectedOnceHeadMoves() {
    TempRoot root;
    const std::string session = UuidV7::generate().toString();
    const std::string operationId = UuidV7::generate().toString();
    std::string projectId;
    std::string draftId;
    std::string token;
    std::optional<RevisionId> base;
    {
        ProjectService service(root.path);
        projectId = service.createUnbornProject("decidable pending Commit").id;
        service.configureRepositoryRole(RepositoryRole::Private);
        // A large first revision makes the pack far bigger than any draft
        // or ledger record, so a file-size limit only hits the pack append.
        ModelChange large = changeWithValue(1);
        large.key[15] = 1;
        large.record->payload.assign(256 * 1024, 0x5a);
        require(commitFreshDraft(service, projectId, UuidV7::generate().toString(), large).state ==
                    LocalCommitState::Succeeded, "large base Commit failed");
        base = service.optionalHead(projectId, "main");
        draftId = service.createWorkingCopy(projectId, "main", base, session).id;
        token = service.acquireWorkingCopyLease(draftId, session, 120).token;
        ModelChange small = changeWithValue(2);
        small.record->payload.assign(64 * 1024, 0x33); // unique, not deduplicated
        service.saveWorkingCopyChanges(draftId, session, token, 0, {small});

        const std::filesystem::path pack =
            root.path / "projects" / projectId / "repository" / "objects.pack";
        struct rlimit previous{};
        require(::getrlimit(RLIMIT_FSIZE, &previous) == 0, "getrlimit failed");
        void (*const oldHandler)(int) = std::signal(SIGXFSZ, SIG_IGN);
        struct rlimit limited = previous;
        limited.rlim_cur = static_cast<rlim_t>(std::filesystem::file_size(pack) + 1024);
        require(::setrlimit(RLIMIT_FSIZE, &limited) == 0, "setrlimit failed");
        bool failed = false;
        try {
            (void)service.commitWorkingCopy(operationId, draftId, session, token, 1, base, commitMetadata());
        } catch (const std::exception&) {
            failed = true;
        }
        require(::setrlimit(RLIMIT_FSIZE, &previous) == 0, "restoring RLIMIT_FSIZE failed");
        std::signal(SIGXFSZ, oldHandler);
        require(failed, "file-size limit did not make the pack write fail");
        require(service.optionalHead(projectId, "main") == base,
                "failed Commit moved HEAD");
        require(service.localCommitStatus(operationId).state == LocalCommitState::Pending,
                "an undecided Commit (HEAD still at its base) must stay pending");

        // Another Commit moves HEAD; the failed operation is now decidable.
        const LocalCommitInfo other = commitFreshDraft(
            service, projectId, UuidV7::generate().toString(), changeWithValue(9));
        require(other.state == LocalCommitState::Succeeded, "competing Commit failed");
        const LocalCommitInfo status = service.localCommitStatus(operationId);
        require(status.state == LocalCommitState::Rejected && status.rejection == "conflict",
                "status of an undecidable-no-longer Commit did not converge to Rejected");
        const LocalCommitInfo retry = service.commitWorkingCopy(
            operationId, draftId, session, token, 1, base, commitMetadata());
        require(retry.state == LocalCommitState::Rejected &&
                retry.reservedRevisionId == status.reservedRevisionId,
                "retry of a decided Commit did not return its rejected outcome");
    }
    ProjectService reopened(root.path);
    require(reopened.localCommitStatus(operationId).state == LocalCommitState::Rejected,
            "rejected outcome was not durable");
    require(reopened.workingCopy(draftId).dirty, "rejected Commit lost the saved draft");
}

// Holds the repository's file lock from a second descriptor, so every
// repository operation of that project blocks inside the engine.
class RepositoryLockHold {
public:
    explicit RepositoryLockHold(const std::filesystem::path& repository)
        : fd_(::open((repository / "repo.lock").c_str(), O_RDWR | O_CLOEXEC)) {
        require(this->fd_ >= 0 && ::flock(this->fd_, LOCK_EX) == 0, "cannot hold repository lock");
    }
    ~RepositoryLockHold() { this->release(); }
    RepositoryLockHold(const RepositoryLockHold&) = delete;
    RepositoryLockHold& operator=(const RepositoryLockHold&) = delete;
    void release() {
        if (this->fd_ >= 0) {
            (void)::flock(this->fd_, LOCK_UN);
            (void)::close(this->fd_);
            this->fd_ = -1;
        }
    }
private:
    int fd_ = -1;
};

// Runs `action` on a thread and reports whether it finished within `limit`.
bool finishesWithin(const std::function<void()>& action, std::chrono::milliseconds limit,
                    std::future<void>& pending) {
    pending = std::async(std::launch::async, action);
    return pending.wait_for(limit) == std::future_status::ready;
}

// R3: a local Commit (and a status lookup) blocked inside the repository of
// project A must not stall the catalogue, other projects, other drafts of
// A, or draft reads. Only the committing draft itself waits.
void testCommitDoesNotHoldCatalogue() {
    TempRoot root;
    ProjectService service(root.path);
    const std::string a = service.createUnbornProject("blocked project").id;
    const std::string b = service.createUnbornProject("independent project").id;
    service.configureRepositoryRole(RepositoryRole::Private);
    require(commitFreshDraft(service, a, UuidV7::generate().toString(), changeWithValue(1)).state ==
                LocalCommitState::Succeeded, "fixture Commit failed");
    const std::optional<RevisionId> head = service.optionalHead(a, "main");
    const std::string session = UuidV7::generate().toString();
    const std::string committing = service.createWorkingCopy(a, "main", head, session).id;
    const std::string token = service.acquireWorkingCopyLease(committing, session, 120).token;
    service.saveWorkingCopyChanges(committing, session, token, 0, {changeWithValue(2)});
    const std::string sibling = service.createWorkingCopy(a, "main", head, session).id;
    const std::string siblingToken = service.acquireWorkingCopyLease(sibling, session, 120).token;
    const std::string other = service.createWorkingCopy(b, "main", std::nullopt, session).id;
    const std::string otherToken = service.acquireWorkingCopyLease(other, session, 120).token;
    const std::string operationId = UuidV7::generate().toString();

    RepositoryLockHold hold(root.path / "projects" / a / "repository");
    std::future<void> commit = std::async(std::launch::async, [&]() {
        require(service.commitWorkingCopy(operationId, committing, session, token, 1, head, commitMetadata()).state ==
                    LocalCommitState::Succeeded, "blocked Commit did not succeed after release");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    require(commit.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
            "repository lock fixture did not block the Commit");
    std::future<void> independent;
    const bool responsive = finishesWithin([&]() {
        (void)service.project(b);
        (void)service.listProjects();
        (void)service.listWorkingCopies();
        (void)service.workingCopy(committing);
        require(service.saveWorkingCopyChanges(sibling, session, siblingToken, 0,
                                               {changeWithValue(3)}).generation == 1,
                "sibling draft save failed");
        require(service.saveWorkingCopyChanges(other, session, otherToken, 0,
                                               {changeWithValue(4)}).generation == 1,
                "other project's draft save failed");
    }, std::chrono::seconds(5), independent);
    std::future<void> sameDraft;
    const bool sameDraftWaited = !finishesWithin([&]() {
        (void)service.acquireWorkingCopyLease(committing, session, 120);
    }, std::chrono::milliseconds(200), sameDraft);
    hold.release();
    commit.get();
    independent.get();
    sameDraft.get();
    require(responsive, "a Commit blocked in one repository stalled the catalogue or other drafts");
    require(sameDraftWaited, "the committing draft was not protected against concurrent writers");

    // The status lookup takes the same path; it must not block either.
    RepositoryLockHold statusHold(root.path / "projects" / a / "repository");
    std::future<void> status = std::async(std::launch::async, [&]() {
        require(service.localCommitStatus(operationId).state == LocalCommitState::Succeeded,
                "status after release is wrong");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::future<void> catalogue;
    const bool available = finishesWithin([&]() {
        (void)service.project(b);
        (void)service.listWorkingCopies();
    }, std::chrono::seconds(5), catalogue);
    statusHold.release();
    status.get();
    catalogue.get();
    require(available, "a blocked status lookup stalled the catalogue");
}
// R6: a successful Commit rebases its draft onto the new HEAD, so the same
// draft (and lease) keeps working: saves continue, a second Commit from it
// succeeds with the committed revision as parent, and the journal is empty.
void testCommitRebasesDraft() {
    TempRoot root;
    ProjectService service(root.path);
    const std::string projectId = service.createUnbornProject("rebased draft").id;
    service.configureRepositoryRole(RepositoryRole::Private);
    const std::string session = UuidV7::generate().toString();
    const std::string draftId = service.createWorkingCopy(projectId, "main", std::nullopt, session).id;
    const std::string token = service.acquireWorkingCopyLease(draftId, session, 120).token;
    const UuidV7 entity = UuidV7::generate();
    const ModelChange first = changeWithValue(41);
    service.saveWorkingCopyTransactions(draftId, session, token, 0, {first},
        {aowis::dvmdb::test::editEvent(UuidV7::generate(), entity)});
    const LocalCommitInfo one = service.commitWorkingCopy(
        UuidV7::generate().toString(), draftId, session, token, 1, std::nullopt, commitMetadata());
    require(one.state == LocalCommitState::Succeeded, "first Commit failed");
    const RevisionId firstRevision = *RevisionId::parse(one.reservedRevisionId);
    const WorkingCopyInfo rebased = service.workingCopy(draftId);
    require(rebased.baseRevision == firstRevision && rebased.generation == 2 &&
            !rebased.dirty && rebased.candidateChanges.empty() && rebased.journal.empty() &&
            rebased.leaseToken == token,
            "successful Commit did not rebase its draft onto the new HEAD");
    // Replaying the same Commit request must not rebase a second time.
    require(service.commitWorkingCopy(one.id, draftId, session, token, 1, std::nullopt, commitMetadata()).state ==
                LocalCommitState::Succeeded &&
            service.workingCopy(draftId).generation == 2,
            "replayed Commit changed the rebased draft");
    const ModelChange second = changeWithValue(42);
    require(service.saveWorkingCopyChanges(draftId, session, token, 2, {second}).generation == 3,
            "save after Commit failed on the rebased draft");
    const LocalCommitInfo two = service.commitWorkingCopy(
        UuidV7::generate().toString(), draftId, session, token, 3, firstRevision, commitMetadata());
    require(two.state == LocalCommitState::Succeeded, "second Commit from the same draft failed");
    const aowis::dvmdb::RevisionGraphRevision child =
        service.revision(projectId, *RevisionId::parse(two.reservedRevisionId));
    require(child.parents.size() == 1 && child.parents[0].id == firstRevision &&
            service.workingCopy(draftId).baseRevision == RevisionId::parse(two.reservedRevisionId),
            "second Commit did not continue from the first");
}

// R6: a full event journal is a limit (413), not a client error, and a
// Commit empties it so event saves continue.
void testFullJournalIsALimitAndCommitClearsIt() {
    TempRoot root;
    ProjectService service(root.path);
    const std::string projectId = service.createUnbornProject("journal limit").id;
    service.configureRepositoryRole(RepositoryRole::Private);
    const std::string session = UuidV7::generate().toString();
    const std::string draftId = service.createWorkingCopy(projectId, "main", std::nullopt, session).id;
    const std::string token = service.acquireWorkingCopyLease(draftId, session, 120).token;
    const UuidV7 entity = UuidV7::generate();
    const std::function<std::vector<aowis::dvmdb::Bytes>(std::size_t)> batch = [&](std::size_t count) {
        std::vector<aowis::dvmdb::Bytes> events;
        for (std::size_t i = 0; i < count; ++i)
            events.push_back(aowis::dvmdb::test::editEvent(UuidV7::generate(), entity));
        return events;
    };
    std::uint64_t generation = 0;
    for (unsigned save = 0; save < 32; ++save)
        generation = service.saveWorkingCopyTransactions(draftId, session, token, generation,
                                                         {changeWithValue(1)}, batch(128)).generation;
    require(service.workingCopy(draftId).journal.size() == 4096, "journal fixture not full");
    bool limited = false;
    try {
        (void)service.saveWorkingCopyTransactions(draftId, session, token, generation,
                                                  {changeWithValue(2)}, batch(1));
    } catch (const aowis::dvmdb::StorageError& error) {
        limited = error.kind() == aowis::dvmdb::StorageErrorKind::LimitExceeded;
    }
    require(limited && service.workingCopy(draftId).generation == generation,
            "a full journal was not reported as LimitExceeded without changing the draft");
    const LocalCommitInfo committed = service.commitWorkingCopy(
        UuidV7::generate().toString(), draftId, session, token, generation, std::nullopt, commitMetadata());
    require(committed.state == LocalCommitState::Succeeded, "Commit of a full-journal draft failed");
    require(service.saveWorkingCopyTransactions(draftId, session, token, generation + 1,
                                                {changeWithValue(3)}, batch(1)).journal.size() == 1,
            "event saves did not continue after Commit emptied the journal");
}

// R6: discarding is fenced like a Save, removes the draft durably, and
// leaves committed history and local Commit records untouched.
void testDiscardWorkingCopy() {
    TempRoot root;
    const std::string session = UuidV7::generate().toString();
    std::string projectId;
    std::string keptDraft;
    std::string operationId = UuidV7::generate().toString();
    {
        ProjectService service(root.path);
        projectId = service.createUnbornProject("discard").id;
        service.configureRepositoryRole(RepositoryRole::Private);
        const std::string draftId = service.createWorkingCopy(
            projectId, "main", std::nullopt, session).id;
        keptDraft = service.createWorkingCopy(projectId, "main", std::nullopt, session).id;
        const std::string token = service.acquireWorkingCopyLease(draftId, session, 120).token;
        service.saveWorkingCopyChanges(draftId, session, token, 0, {changeWithValue(7)});
        require(service.commitWorkingCopy(operationId, draftId, session, token, 1,
                                          std::nullopt, commitMetadata()).state == LocalCommitState::Succeeded,
                "fixture Commit failed");
        expectConflict([&]() {
            service.discardWorkingCopy(draftId, session, token, 1); // stale generation
        });
        expectConflict([&]() {
            service.discardWorkingCopy(draftId, session, UuidV7::generate().toString(), 2);
        });
        service.discardWorkingCopy(draftId, session, token, 2);
        bool missing = false;
        try { (void)service.workingCopy(draftId); }
        catch (const ProjectError& error) { missing = error.kind() == ProjectErrorKind::NotFound; }
        require(missing, "discarded draft is still readable");
        bool repeated = false;
        try { service.discardWorkingCopy(draftId, session, token, 2); }
        catch (const ProjectError& error) { repeated = error.kind() == ProjectErrorKind::NotFound; }
        require(repeated, "repeated discard did not answer NotFound");
        require(service.localCommitStatus(operationId).state == LocalCommitState::Succeeded &&
                service.optionalHead(projectId, "main").has_value(),
                "discard affected committed history or its operation record");
    }
    ProjectService reopened(root.path);
    const std::vector<WorkingCopyInfo> remaining = reopened.listWorkingCopies();
    require(remaining.size() == 1 && remaining.front().id == keptDraft,
            "discard was not durable or removed another draft");
}

// R6: draft listing is paged in draft-UUID order and can be filtered by
// project, so it no longer fails for many drafts.
void testDraftListPaging() {
    TempRoot root;
    ProjectService service(root.path);
    const std::string a = service.createUnbornProject("paging a").id;
    const std::string b = service.createUnbornProject("paging b").id;
    service.configureRepositoryRole(RepositoryRole::Private);
    const std::string session = UuidV7::generate().toString();
    std::vector<std::string> all;
    std::vector<std::string> ofA;
    for (unsigned i = 0; i < 7; ++i) {
        const std::string project = i % 3 == 0 ? b : a;
        const std::string id = service.createWorkingCopy(project, "main", std::nullopt, session).id;
        all.push_back(id);
        if (project == a) ofA.push_back(id);
    }
    std::sort(all.begin(), all.end());
    std::sort(ofA.begin(), ofA.end());
    std::vector<std::string> seen;
    std::optional<std::string> cursor;
    for (unsigned page = 0; page < 10; ++page) {
        const aowis::dvmdb::WorkingCopyPage result = service.listWorkingCopies(
            std::nullopt, 3, cursor ? std::optional<std::string_view>(*cursor) : std::nullopt);
        require(result.workingCopies.size() <= 3, "draft page exceeded its limit");
        for (const WorkingCopyInfo& draft : result.workingCopies) seen.push_back(draft.id);
        if (!result.next) break;
        require(*result.next == result.workingCopies.back().id, "draft cursor is not the last ID");
        cursor = result.next;
    }
    require(seen == all, "paged draft listing skipped, repeated or misordered drafts");
    const aowis::dvmdb::WorkingCopyPage filtered = service.listWorkingCopies(a, 1000, std::nullopt);
    std::vector<std::string> filteredIds;
    for (const WorkingCopyInfo& draft : filtered.workingCopies) filteredIds.push_back(draft.id);
    require(filteredIds == ofA && !filtered.next, "project-filtered draft listing is wrong");
    bool rejected = false;
    try { (void)service.listWorkingCopies(std::nullopt, 0, std::nullopt); }
    catch (const ProjectError& error) { rejected = error.kind() == ProjectErrorKind::InvalidInput; }
    require(rejected, "draft page size 0 accepted");
}
}

// A local Commit records the request's author and message. The commit time is
// reserved with the operation: a retry after a failed publication writes a
// byte-identical revision, and the same operation ID with another author or
// message is a different request.
void testCommitRecordsAuthorMessageAndReservedTime() {
    TempRoot root;
    std::atomic<std::uint64_t> wall{1'800'000'000'000ULL};
    const aowis::dvmdb::LeaseClock clock{[&wall]() { return wall.load(); }, {}};
    const std::string session = UuidV7::generate().toString();
    const std::string operationId = UuidV7::generate().toString();
    const aowis::dvmdb::CommitMetadata metadata{
        *UuidV7::parse("018f84c0-0000-7000-8000-00000000c001"), "Carla",
        "Close valve V12\n\nfor maintenance"};
    ProjectService service(root.path, 4, clock);
    const std::string projectId = service.createUnbornProject("authored commits").id;
    service.configureRepositoryRole(RepositoryRole::Private);
    ModelChange large = changeWithValue(1);
    large.key[15] = 1;
    large.record->payload.assign(256 * 1024, 0x5a);
    require(commitFreshDraft(service, projectId, UuidV7::generate().toString(), large).state ==
                LocalCommitState::Succeeded, "base Commit failed");
    const std::optional<RevisionId> base = service.optionalHead(projectId, "main");
    const std::string draftId = service.createWorkingCopy(projectId, "main", base, session).id;
    const std::string token = service.acquireWorkingCopyLease(draftId, session, 120).token;
    ModelChange small = changeWithValue(2);
    small.record->payload.assign(64 * 1024, 0x33);
    service.saveWorkingCopyChanges(draftId, session, token, 0, {small});

    // Invalid metadata is refused before a ledger record exists.
    aowis::dvmdb::CommitMetadata invalid = metadata;
    invalid.message.clear();
    bool refused = false;
    try { (void)service.commitWorkingCopy(operationId, draftId, session, token, 1, base, invalid); }
    catch (const ProjectError& error) { refused = error.kind() == ProjectErrorKind::InvalidInput; }
    require(refused && !std::filesystem::exists(root.path / "local-commits" / operationId),
            "a Commit without a message was not refused before it was recorded");

    // The first attempt fails while appending to the pack and stays pending.
    const std::uint64_t reservedAt = wall.load();
    const std::filesystem::path pack =
        root.path / "projects" / projectId / "repository" / "objects.pack";
    struct rlimit previous{};
    require(::getrlimit(RLIMIT_FSIZE, &previous) == 0, "getrlimit failed");
    void (*const oldHandler)(int) = std::signal(SIGXFSZ, SIG_IGN);
    struct rlimit limited = previous;
    limited.rlim_cur = static_cast<rlim_t>(std::filesystem::file_size(pack) + 1024);
    require(::setrlimit(RLIMIT_FSIZE, &limited) == 0, "setrlimit failed");
    bool failed = false;
    try { (void)service.commitWorkingCopy(operationId, draftId, session, token, 1, base, metadata); }
    catch (const std::exception&) { failed = true; }
    require(::setrlimit(RLIMIT_FSIZE, &previous) == 0, "restoring RLIMIT_FSIZE failed");
    std::signal(SIGXFSZ, oldHandler);
    require(failed && service.localCommitStatus(operationId).state == LocalCommitState::Pending,
            "file-size limit did not leave the Commit pending");
    {
        std::ifstream input(root.path / "local-commits" / operationId, std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(input)), {});
        require(body.starts_with("AOWIS-LOCAL-COMMIT-3\n") &&
                body.find("committed-at\t" + std::to_string(reservedAt) + "\n") != std::string::npos &&
                body.find("author-id\t" + metadata.authorId.toString() + "\n") != std::string::npos,
                "the pending ledger record lacks the reserved time or the author");
    }

    // Another author or message under the same operation ID is another request.
    aowis::dvmdb::CommitMetadata other = metadata;
    other.message = "Something else";
    expectConflict([&] {
        (void)service.commitWorkingCopy(operationId, draftId, session, token, 1, base, other);
    });
    other = metadata;
    other.authorName = "Mallory";
    expectConflict([&] {
        (void)service.commitWorkingCopy(operationId, draftId, session, token, 1, base, other);
    });

    // The retry, later, publishes the revision with the reserved time.
    wall += 60'000;
    const LocalCommitInfo committed =
        service.commitWorkingCopy(operationId, draftId, session, token, 1, base, metadata);
    require(committed.state == LocalCommitState::Succeeded, "retry of the pending Commit failed");
    const aowis::dvmdb::RevisionGraphRevision revision =
        service.revision(projectId, *RevisionId::parse(committed.reservedRevisionId));
    require(revision.metadata.authorId == metadata.authorId &&
            revision.metadata.authorName == metadata.authorName &&
            revision.metadata.message == metadata.message &&
            revision.committedAtMs == reservedAt,
            "the committed revision lost its author or message, or took the retry's time");
    const std::vector<aowis::dvmdb::RevisionGraphRevision> history =
        service.history(projectId, "main", 10);
    require(!history.empty() && history.front().metadata.message == metadata.message,
            "history does not show the commit message");
    const LocalCommitInfo replay =
        service.commitWorkingCopy(operationId, draftId, session, token, 1, base, metadata);
    require(replay.state == LocalCommitState::Succeeded &&
            replay.reservedRevisionId == committed.reservedRevisionId,
            "replay did not return the recorded outcome");
}

int main() {
    try {
        testHistoryCheckpointPlan();
        testAtomicHistoryCheckpointNavigation();
        testCommittedActionHistory();
        testVerifiedRevisionNativeWitness();
        testBranchDraftHistoryPaging();
        testLocalCommitRecovery();
        testConcurrentFirstCommitIsSerialized();
        testRejectedOperationIsDurable();
        testLeaseTakeoverCanCommit();
        testFailedCommitIsRejectedOnceHeadMoves();
        testCommitDoesNotHoldCatalogue();
        testCommitRebasesDraft();
        testFullJournalIsALimitAndCommitClearsIt();
        testDiscardWorkingCopy();
        testDraftListPaging();
        testCommitRecordsAuthorMessageAndReservedTime();
        std::cout << "VC-05 durable local Commit tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "VC-05 durable local Commit tests failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
