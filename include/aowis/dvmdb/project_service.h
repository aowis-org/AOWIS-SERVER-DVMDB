#pragma once

#include "aowis/dvmdb/native_store.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <optional>
#include <vector>

namespace aowis::dvmdb {

// Transport-independent catalogue and repository lifecycle API. One process
// owns a project root at a time; operations on this instance are thread-safe.
//
// Locking: the catalogue mutex guards only the project map and is never held
// while a repository is opened or used. Each project opens lazily under its
// own mutex. Scoped operations (withRepository, createBranch) of one project
// run one at a time under that project's operation mutex; projects never wait
// for each other. Single reads (head, history, ...) take no operation lock;
// Repository serializes its own calls internally.
struct ProjectInfo {
    std::string id;
    std::string name;
};

// Busy: the project already has its maximum number of scoped operations
// waiting or running; the caller should retry later (HTTP 503, Retry-After).
enum class ProjectErrorKind { InvalidInput, NotFound, Conflict, Unavailable, Busy };
class ProjectError : public std::runtime_error {
public:
    ProjectError(ProjectErrorKind kind, std::string message);
    ProjectErrorKind kind() const noexcept;
private:
    ProjectErrorKind kind_;
};

// VC-03 metadata. "Private" and "Shared" describe deployment intent;
// neither is authentication or an authorization capability.
enum class RepositoryRole { Unconfigured, Private, Shared };

struct RemoteRepository {
    std::string repositoryId;      // Actual remote root UUIDv7, not a GUI profile name.
    std::string endpoint;          // Connection hint, NOT an authenticated identity.
};

struct RemoteTrackingRef {
    std::string remoteRepositoryId;
    std::string projectId;         // Logical project UUIDv7 remains stable on clone.
    std::string branchId;          // Logical branch UUIDv7 remains stable on clone.
    std::optional<RevisionId> observedHead; // Null means observed unborn HEAD.
};

// VC-04: persisted, uncommitted working-copy identity. A working copy is
// private repository state, never a committed revision or mutable branch HEAD.
// Saves persist an overlay of typed model changes without advancing HEAD.
// The UUIDv7 fencing token is rotated on every lease acquisition or takeover.
struct WorkingCopyEvent {
    std::string id;  // UUIDv7 of a MODEL edit transaction.
    Bytes encoded;    // Exact ModelEditTransactionCodec binary frame.
};

struct WorkingCopyInfo {
    std::string id;
    std::string projectId;
    std::string branchId;
    std::optional<RevisionId> baseRevision;
    std::string sessionId;
    std::uint64_t generation = 0;
    bool dirty = false;
    std::vector<ModelChange> candidateChanges; // Not filled by listings.
    // Saved MODEL events. Filled only where documented: workingCopy() with
    // includeJournal, and the events of the Save that saveWorkingCopyTransactions
    // just persisted (or replayed). journalCount is always the full count.
    std::vector<WorkingCopyEvent> journal;
    std::uint64_t journalCount = 0;
    std::optional<std::string> lastSaveFingerprint; // Last journalled batch for idempotency.
    // HIST-03b: exact snapshot selected in this private draft. These fields
    // are part of the same atomic meta publication as candidateChanges.
    // An ordinary subsequent edit/Commit clears them: this is not yet the
    // permanent cross-Commit action cursor or an Undo/Redo implementation.
    std::optional<std::string> historyCheckpointRevisionId;
    std::optional<std::string> lastHistoryNavigationOperationId;
    std::optional<std::uint64_t> lastHistoryNavigationSourceGeneration;
    std::optional<std::string> leaseSessionId;
    std::optional<std::string> leaseToken;
    std::uint64_t leaseExpiresAtMs = 0;
};

// Clocks for draft leases. A lease's expiry is persisted as wall-clock
// epoch milliseconds (it survives a restart and is shown to clients). While
// the granting process runs, the lease is enforced against a monotonic
// deadline instead, so a wall-clock step neither extends nor cuts it short.
// Defaults are the system and steady clocks; tests may inject their own.
struct LeaseClock {
    std::function<std::uint64_t()> wallMs;
    std::function<std::chrono::steady_clock::time_point()> steady;
};

// One page of drafts in draft-UUID order. `next` is the last returned draft
// UUID when more drafts follow; pass it as `after` to continue.
struct WorkingCopyPage {
    std::vector<WorkingCopyInfo> workingCopies;
    std::optional<std::string> next;
};

// VC-05 local Commit operation. The reserved revision UUID is durable before
// HEAD publication. Pending means the caller must query/retry with the SAME ID;
// it does not mean that the commit definitely failed.
enum class LocalCommitState { Pending, Succeeded, Rejected };
struct LocalCommitInfo {
    std::string id;
    std::string repositoryId;
    std::string projectId;
    std::string branchId;
    std::string draftId;
    std::optional<RevisionId> expectedHead;
    std::string reservedRevisionId;
    std::uint64_t expectedGeneration = 0;
    std::string payloadDigest;
    LocalCommitState state = LocalCommitState::Pending;
    std::string rejection;
};

// HIST-02: immutable action frames archived by the local Commit operation.
// The source draft journal is still cleared when the Commit succeeds. A
// successful Commit may have no actions if it used the older changes-only
// Save path; callers must show a checkpoint, not fabricate edit steps.
struct LocalCommitActionHistory {
    LocalCommitInfo commit;
    std::uint64_t committedAtMs = 0;
    std::vector<WorkingCopyEvent> actions;
};

// Read-only first-parent branch timeline. Entries are ordered newest to
// oldest; a revision without an action archive is a checkpoint-only entry.
// Action UUIDs are complete for each archived Commit, not guessed from a
// decoded model diff. The current draft remains a separate, paged section.
struct BranchActionHistoryEntry {
    RevisionGraphRevision revision;
    std::optional<std::string> commitOperationId;
    std::vector<std::string> actionIds;
};

// Immutable original MODEL frames for one first-parent Commit checkpoint.
// No archive means an explicit checkpoint-only response (operationId absent).
// Frames are original bytes, not inverse actions or reconstructed guesses.
struct RevisionActionPage {
    std::string revisionId;
    std::optional<std::string> commitOperationId;
    std::size_t total = 0;
    std::vector<WorkingCopyEvent> actions;
    std::optional<std::size_t> nextOffset;
};

struct BranchActionHistoryPage {
    std::vector<BranchActionHistoryEntry> commits;
    std::optional<RevisionId> next;
    std::optional<std::string> draftId;
    std::uint64_t draftGeneration = 0;
    std::uint64_t draftEventCount = 0;
    std::vector<std::string> draftActionIds;
    // Original MODEL frames for the current page only, for HTTP display
    // descriptions. Does not grow with the full draft or Commit ancestry.
    std::vector<WorkingCopyEvent> draftActionFrames;
    std::optional<std::size_t> nextDraftOffset;
};

// HIST-03 foundation: immutable, read-only *plan* for selecting a committed
// checkpoint as the private draft's desired model content. candidateChanges
// is a COMPLETE replacement overlay against expectedHead, not a Save delta.
// No writer can apply this result without rechecking both the generation and
// local HEAD and atomically replacing the overlay together with cursor state.
struct HistoryCheckpointPlan {
    std::string draftId;
    std::string projectId;
    std::string branchId;
    std::string targetRevisionId;
    std::optional<RevisionId> expectedHead;
    std::uint64_t expectedGeneration = 0;
    std::vector<ModelChange> candidateChanges;
};

struct WorkingCopyLease {
    std::string token;                // UUIDv7, changes on every acquisition
    std::uint64_t expiresAtMs = 0;   // UTC epoch milliseconds
    std::uint64_t generation = 0;    // caller must pin for Save
};

class ProjectService final {
    struct Project;
public:
    // A reserved place in one project's operation queue. Reserving fails with
    // Busy when the queue is full; the place is released on destruction.
    // Lets a transport refuse work before it accepts a large request body.
    class Operation final {
    public:
        Operation(Operation&& other) noexcept;
        Operation& operator=(Operation&& other) = delete;
        Operation(const Operation&) = delete;
        Operation& operator=(const Operation&) = delete;
        ~Operation();
    private:
        friend class ProjectService;
        explicit Operation(std::shared_ptr<Project> project) noexcept;
        std::shared_ptr<Project> project_;
    };

    // maxPendingOperations: scoped operations per project that may wait or
    // run at the same time (>= 1). Keep it below the server's worker count,
    // so one busy project can never occupy every worker.
    explicit ProjectService(std::filesystem::path root, std::size_t maxPendingOperations = 4,
                            LeaseClock leaseClock = {});
    ~ProjectService();
    ProjectService(const ProjectService&) = delete;
    ProjectService& operator=(const ProjectService&) = delete;

    // Revision IDs are UUIDv7; SHA-256 only authenticates stored records.
    // Creates `main` with an empty first revision described by
    // `rootMetadata`. New clients use createUnbornProject (roadmap VC-01).
    ProjectInfo createProject(std::string_view name, const CommitMetadata& rootMetadata);
    // Explicit opt-in until all clients support nullable initial HEADs.
    // With a caller-chosen projectId (UUIDv7) the creation is idempotent: a
    // repeated request returns the existing project of that ID and name, so
    // a client may safely retry after a lost response; the same ID with a
    // different name is a Conflict.
    ProjectInfo createUnbornProject(std::string_view name,
                                      std::optional<std::string_view> projectId = std::nullopt);
    // Provision an empty catalogue project with an existing logical UUIDv7.
    // Only a full verified branch bundle may populate it. Safe to retry.
    ProjectInfo provisionCloneTarget(std::string_view projectId, std::string_view name);
    // Rename catalogue metadata without changing the project UUID or its repository.
    // expectedName provides a stale-editor check for concurrent clients.
    ProjectInfo renameProject(std::string_view projectId, std::string_view expectedName,
                              std::string_view name);
    // Import the exact identity of a root unborn branch into an empty private
    // clone target. Does not make an initial revision. Safe to retry.
    void provisionUnbornBranch(std::string_view projectId, std::string_view name,
                                 std::string_view branchId);
    // Project-level creation: no source branch or revision. UUIDv7 ID optional
    // for native callers, required for retry-safe HTTP clients.
    BranchId createUnbornBranch(std::string_view projectId, std::string_view name,
                                  std::optional<std::string_view> branchId = std::nullopt,
                                  const BranchMetadata& metadata = {});
    RevisionGraphRevision revision(std::string_view projectId, const RevisionId& id);
    void updateBranchProperties(std::string_view projectId, std::string_view branchId,
                                std::string_view expectedName, std::string_view name,
                                const BranchMetadata& metadata);
    void updateRevisionDisplayMessage(std::string_view projectId, std::string_view revisionId,
                                      std::string_view expected, std::string_view message);
    std::optional<std::string> revisionDisplayMessage(std::string_view projectId,
                                                       std::string_view revisionId);
    void renameBranch(std::string_view projectId, std::string_view previous,
                      std::string_view renamed, const RevisionId& expectedHead);
    RevisionId createBranch(std::string_view projectId, std::string_view name,
                              std::optional<std::string_view> source = std::nullopt);
    BranchCreationResult createBranchWithIdentity(
        std::string_view projectId, std::string_view name,
        std::optional<std::string_view> source = std::nullopt);
    RevisionId head(std::string_view projectId, std::string_view branch);
    std::optional<RevisionId> optionalHead(std::string_view projectId, std::string_view branch);
    std::vector<RevisionGraphRevision> history(
        std::string_view projectId, std::string_view branch, std::size_t maxCount = 100,
        std::optional<RevisionId> after = std::nullopt);
    RevisionGraphPage revisionGraph(
        std::string_view projectId, std::size_t maxCount = 100,
        std::optional<RevisionGraphCursor> cursor = std::nullopt);
    RevisionId createBranchAtRevision(std::string_view projectId, std::string_view name,
                                        std::string_view sourceBranch, const RevisionId& revision);
    BranchCreationResult createBranchAtRevisionWithIdentity(
        std::string_view projectId, std::string_view name,
        std::string_view sourceBranch, const RevisionId& revision);
    // Branches by UUIDv7, independent of renamable names (DG-01). See
    // Repository::branchById() / createBranchFromId(): the creation is
    // idempotent for the caller-chosen `createdBranchId`.
    std::optional<RevisionGraphHead> branchById(std::string_view projectId,
                                                    std::string_view branchId);
    BranchCreationResult createBranchFromId(
        std::string_view projectId, std::string_view name, std::string_view sourceBranchId,
        std::optional<RevisionId> revision, std::string_view createdBranchId);
    // Every operation that creates a revision records `metadata` with it.
    MergeResult mergeBranches(std::string_view projectId, std::string_view target,
                                  std::string_view source, const CommitMetadata& metadata,
                                  std::optional<RevisionId> expectedTarget = std::nullopt,
                                  std::optional<RevisionId> expectedSource = std::nullopt);
    RevisionId commitModel(std::string_view projectId, std::string_view branch,
                             std::vector<ModelChange> changes, const CommitMetadata& metadata,
                             std::optional<RevisionId> expectedHead = std::nullopt);
    // Strict expected-local-HEAD CAS for VC-05 on a private repository.
    // Unlike commitModel(), nullopt means the branch MUST still be unborn.
    // Does not consume a draft
    // or provide operation-ID replay yet; only use under a commit coordinator.
    RevisionId commitModelAtExpectedHead(std::string_view projectId,
        std::string_view branch, std::vector<ModelChange> changes,
        const CommitMetadata& metadata, std::optional<RevisionId> expectedLocalHead);
    std::optional<ModelRecord> getModelAtRevision(std::string_view projectId,
                                                   const RevisionId& revision, const Key& key);
    // Persistent identity of this DVMDB catalogue / repository root (UUIDv7).
    // Not a user identity and not an authorization credential.
    std::string repositoryId() const;
    // Metadata only: these calls NEVER mutate a local or remote branch HEAD.
    // Missing tracking entry differs from an observed null (unborn) HEAD.
    RepositoryRole repositoryRole() const;
    void configureRepositoryRole(RepositoryRole role);
    std::vector<RemoteRepository> remoteRepositories() const;
    // Re-registering the same ID/endpoint is idempotent. Conflicting endpoint
    // or a remote equal to this repository UUID is rejected.
    void registerRemoteRepository(std::string_view remoteRepositoryId,
                                    std::string_view endpoint);
    std::vector<RemoteTrackingRef> remoteTrackingRefs() const;
    // expectedObservedHead: nullopt => no tracking record exists;
    // engaged with null inner value => record exists with unborn HEAD;
    // engaged with revision => record exists with that exact HEAD.
    // The caller must obtain the observation from a verified fetch; recording
    // it does not verify an endpoint or grant access to that endpoint.
    void recordRemoteTrackingRef(
        const RemoteTrackingRef& observation,
        std::optional<std::optional<RevisionId>> expectedObservedHead);
    // Begin a durable, empty draft at an exact currently observed local HEAD.
    // An unborn branch supplies null base. Only a private repository can
    // create drafts. Does not commit or change any branch HEAD.
    WorkingCopyInfo createWorkingCopy(
        std::string_view projectId, std::string_view branch,
        std::optional<RevisionId> baseRevision, std::string_view sessionId);
    // The same, addressing the branch by UUIDv7. With a caller-chosen draftId
    // the creation is idempotent: a repeated request returns the existing
    // draft if project, branch, base and session match; otherwise Conflict.
    WorkingCopyInfo createWorkingCopyForBranch(
        std::string_view projectId, std::string_view branchId,
        std::optional<RevisionId> baseRevision, std::string_view sessionId,
        std::optional<std::string_view> draftId = std::nullopt);
    // The draft with its candidate overlay and, with includeJournal, its
    // verified event journal (otherwise only journalCount).
    WorkingCopyInfo workingCopy(std::string_view draftId, bool includeJournal = true) const;
    // Every draft of the root (offline tools and tests). Like the bounded
    // listing, reads only each draft's small meta record: no candidate
    // changes, no journal.
    std::vector<WorkingCopyInfo> listWorkingCopies() const;
    // Bounded listing: at most `limit` (1..1000) drafts after the draft UUID
    // `after`, optionally only those of one project. Reads only the meta
    // records it returns plus, with a project filter, those it skips.
    WorkingCopyPage listWorkingCopies(std::optional<std::string_view> projectId,
                                      std::size_t limit,
                                      std::optional<std::string_view> after) const;
    // Whether the draft's current lease is still held (see LeaseClock).
    bool workingCopyLeaseLive(const WorkingCopyInfo& draft) const;
    // Removes a draft for good. Fenced like a Save: the live lease holder,
    // its token and the current generation. Local Commit records keep their
    // own snapshot, so discarding never affects a committed or pending
    // operation. A repeated request after success answers NotFound.
    void discardWorkingCopy(std::string_view draftId, std::string_view sessionId,
                            std::string_view fencingToken, std::uint64_t expectedGeneration);
    // Only one nonexpired writer may hold a draft lease. The same session
    // may reacquire, invalidating its previous token. Expired leases may be
    // explicitly taken over by a different session; no candidate is lost.
    WorkingCopyLease acquireWorkingCopyLease(
        std::string_view draftId, std::string_view sessionId, std::uint32_t seconds);
    // All changes are applied atomically to the draft's candidate overlay.
    // The new overlay is fsynced before this returns; Save never touches
    // committed HEADs or immutable revisions.
    WorkingCopyInfo saveWorkingCopyChanges(
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken, std::uint64_t expectedGeneration,
        const std::vector<ModelChange>& changes);
    // Atomically persist MODEL transaction frames and the corresponding candidate
    // model changes. Original event UUIDs are retained across retries. A replay
    // of the immediately preceding identical batch is idempotent. The result's
    // journal holds the events of this (or the replayed) Save only; the new
    // events are appended to the stored journal, which is never rewritten.
    WorkingCopyInfo saveWorkingCopyTransactions(
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken, std::uint64_t expectedGeneration,
        const std::vector<ModelChange>& changes,
        const std::vector<Bytes>& encodedTransactions,
        // Optional exact logical request fingerprint for server-materialized
        // membership writes. Remains stable across dropped responses even
        // though native chunk records are derived from the previous draft.
        std::optional<std::string> logicalRequestFingerprint = std::nullopt);
    // Commit a saved working-copy overlay to one private local branch. The
    // request is bound to its UUIDv7 operation ID, draft generation, writer
    // fencing token and exact expected local HEAD (null means unborn).
    // Replays of an accepted request return the same immutable revision UUID.
    // `metadata` (author and message) is part of the request; the commit time
    // is reserved with the operation, so a retry writes the same revision.
    LocalCommitInfo commitWorkingCopy(
        std::string_view operationId, std::string_view draftId,
        std::string_view sessionId, std::string_view fencingToken,
        std::uint64_t expectedGeneration,
        std::optional<RevisionId> expectedLocalHead,
        const CommitMetadata& metadata);
    // Durable status query. A pending record is reconciled against committed
    // ancestry before returning, including after a process restart.
    LocalCommitInfo localCommitStatus(std::string_view operationId);
    // Read exactly the accepted MODEL frames captured in the durable Commit
    // intent. Requires a succeeded operation and never changes HEAD or draft.
    // Future timeline APIs will paginate by branch/revision/action; this read
    // is indexed by the stable Commit operation UUIDv7 only.
    LocalCommitActionHistory localCommitActionHistory(std::string_view operationId);
    // HIST-02 read-only timeline. Pagination is by first-parent revision UUID;
    // active-draft actions use a separate offset and remain bounded by the
    // draft's existing journal limits. A continuation must pin the generation
    // returned by page one; concurrent Save/Commit returns Conflict. Returned
    // rows never move HEAD or perform navigation. Indexed lookup is by UUID.
    BranchActionHistoryPage branchActionHistory(
        std::string_view projectId, std::string_view branch, std::size_t limit = 5,
        std::optional<RevisionId> after = std::nullopt,
        std::optional<std::string_view> draftId = std::nullopt,
        std::size_t draftOffset = 0,
        std::optional<std::uint64_t> expectedDraftGeneration = std::nullopt);
    // Page full MODEL frames for an immutable first-parent revision. No
    // mutations or cursor operations; checkpoint-only revisions return empty.
    RevisionActionPage revisionActionPage(
        std::string_view projectId, std::string_view branch,
        const RevisionId& revision, std::size_t offset = 0,
        std::size_t limit = 100);
    // Read-only checkpoint reconstruction for upcoming cross-Commit Undo/Redo.
    // The result is a replacement overlay relative to the draft's actual
    // immutable HEAD, never a sequence of guessed edits. Only an ancestor on
    // the selected branch's first-parent chain is a valid target. The method
    // neither changes the draft nor creates a history cursor or journal entry.
    HistoryCheckpointPlan planHistoryCheckpoint(
        std::string_view draftId, const RevisionId& target,
        std::uint64_t expectedGeneration);
    // HIST-03b: execute a checkpoint selection as ONE atomic replacement of
    // candidate and durable cursor fields, without touching immutable HEAD.
    // The writer pins its lease, generation and expected committed HEAD.
    // Retry an uncertain result with exactly the SAME navigation operation ID
    // and parameters. For safety this first slice refuses an active MODEL
    // event journal rather than silently dropping or misattributing actions.
    // It does not yet implement stepwise Undo/Redo or action-record archival.
    WorkingCopyInfo navigateHistoryCheckpoint(
        std::string_view operationId, std::string_view draftId,
        std::string_view sessionId, std::string_view fencingToken,
        std::uint64_t expectedGeneration, const RevisionId& expectedHead,
        const RevisionId& target);
    void releaseWorkingCopyLease(
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken);
    std::vector<ProjectInfo> listProjects() const;
    ProjectInfo project(std::string_view id) const;
    std::vector<std::string> listBranches(std::string_view projectId);
    std::optional<BranchId> branchId(std::string_view projectId, std::string_view branch);
    // Execute a scoped operation with this project's repository and its
    // operation mutex held (other projects are not blocked). Callbacks must
    // not retain the Repository or call ProjectService. This is the
    // transport-independent adapter boundary for model operations.
    // Throws ProjectError(Busy) when the project's queue is full.
    void withRepository(std::string_view projectId,
                        const std::function<void(Repository&)>& action);
    Operation reserveOperation(std::string_view projectId);
    void withRepository(Operation& operation, const std::function<void(Repository&)>& action);
    // Scoped operations of this project currently waiting or running.
    std::size_t pendingOperations(std::string_view projectId) const;
    std::size_t maxPendingOperations() const noexcept;
    // Nameless scratch file in the project's repository (for streaming
    // uploads). The catalogue lock is held only while creating it.
    ScratchFile createScratchFile(std::string_view projectId);

    // Canonical lowercase UUIDv7.
    static bool validProjectId(std::string_view id);
    static bool validBranchName(std::string_view name);
private:
    struct Project {
        explicit Project(ProjectInfo value);
        ProjectInfo info;
        std::mutex openMutex;                    // guards repository
        std::shared_ptr<Repository> repository;  // opened on first use
        std::mutex operationMutex;               // one scoped operation at a time
        std::atomic<std::size_t> pending{0};     // reserved Operations
    };
    // Looks the project up under the catalogue mutex only.
    std::shared_ptr<Project> findProject(std::string_view id) const;
    std::shared_ptr<Repository> openRepository(Project& project) const;
    void ensureHealthy() const;
    // Short catalogue-mutex sections for working-copy operations. Draft and
    // local-commit file I/O never runs under the catalogue mutex.
    void checkHealthy() const;
    void requirePrivateRepository(const char* message) const;
    void markUncertain();
    // Serializes all writers of one draft record (lease, saves, release,
    // the draft side of a local Commit). Readers need no lock: records are
    // replaced atomically. Lock order: project operation, draft, catalogue.
    std::shared_ptr<std::mutex> draftLock(std::string_view draftId) const;
    // requestedProjectId: caller-chosen UUIDv7, idempotent by (ID, name).
    // With unborn == false it provisions a clone target (no branch at all).
    ProjectInfo createProjectImpl(std::string_view name, bool unborn = false,
                                  std::optional<std::string> requestedProjectId = std::nullopt,
                                  const CommitMetadata* rootMetadata = nullptr);

    std::filesystem::path root_;
    std::string repository_id_;
    RepositoryRole repository_role_ = RepositoryRole::Unconfigured;
    std::map<std::string, RemoteRepository> remotes_;
    using TrackingKey = std::tuple<std::string, std::string, std::string>;
    std::map<TrackingKey, RemoteTrackingRef> tracking_;
    void persistTrackingMetadata(); // called with catalogue mutex held
    std::filesystem::path projects_;
    int lockFd_ = -1;
    const std::size_t maxPending_;
    mutable std::mutex mutex_;                   // catalogue: entries_, uncertain_
    std::uint64_t wallClockMs() const;
    void rememberLease(const WorkingCopyInfo& draft, std::uint32_t seconds);
    void forgetLease(std::string_view draftId);
    LeaseClock leaseClock_;
    mutable std::mutex leaseDeadlinesMutex_;     // guards leaseDeadlines_ only
    // Draft UUID -> (lease token, monotonic deadline) of leases granted here.
    std::map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>> leaseDeadlines_;
    mutable std::mutex draftLocksMutex_;         // guards draftLocks_ only
    mutable std::map<std::string, std::weak_ptr<std::mutex>> draftLocks_;
    bool uncertain_ = false;
    std::map<std::string, std::shared_ptr<Project>> entries_;
};

} // namespace aowis::dvmdb
