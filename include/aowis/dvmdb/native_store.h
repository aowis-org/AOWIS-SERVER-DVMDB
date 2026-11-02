#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aowis/dvmdb/storage_error.h"
#include "aowis/dvmdb/object_id.h"

namespace aowis::dvmdb {

struct RevisionRecord;

using Bytes = std::vector<std::uint8_t>;
using Hash = std::array<std::uint8_t, 32>;
using Key = std::array<std::uint8_t, 16>;

// Process-wide logical-extent cap for native internal replication/verification
// scratch files. Zero disables the limit (offline library default). Configure
// before starting operations; changing while scratch is active is rejected.
// Does not include HTTP transfer spools, pack growth or compaction copies;
// not a physical-space guarantee or an interprocess filesystem quota.
void setInternalScratchLimit(std::uint64_t maximumBytes);
std::uint64_t internalScratchReservedBytes();

std::string toHex(const Hash& value);
Hash fromHex(std::string_view value);
Hash sha256(const Bytes& value);

struct Change {
    Key key{};
    std::optional<Bytes> value;  // nullopt deletes the key
};

// Typed model envelope: payload bytes come from AOWIS ModelCodec, never from
// the native store. References describe required live UUID dependencies.
struct ModelRecord {
    std::uint16_t kind = 0;
    std::vector<Key> references;
    Bytes payload;
};

struct ModelChange {
    Key key{};
    std::optional<ModelRecord> record;
};

// Who made a revision and why; every revision records it, together with the
// time the repository published it. There is no authentication: the author
// is whoever the caller says it is.
struct CommitMetadata {
    UuidV7 authorId;         // stable UUIDv7 of the author
    std::string authorName;  // 1..200 bytes of UTF-8 without control characters
    std::string message;     // 1..65536 bytes of UTF-8; tab and newline are the only controls
};
inline constexpr std::size_t maxAuthorNameBytes = 200;
inline constexpr std::size_t maxCommitMessageBytes = 64 * 1024;
// StorageError(InvalidInput) unless `metadata` follows the rules above.
void validateCommitMetadata(const CommitMetadata& metadata);

// A revision UUIDv7 and commit time chosen before the revision is written
// (local Commit), so a retry publishes byte-identical records.
struct ReservedRevision {
    RevisionId id;
    std::uint64_t committedAtMs = 0;
};

Bytes encodeModelRecord(const ModelRecord& record);
ModelRecord decodeModelRecord(const Bytes& bytes);

struct Difference {
    Key key{};
    std::optional<Hash> before;
    std::optional<Hash> after;
};

// A merge never overwrites conflicting edits. Conflicts leave both refs intact.
enum class MergeStatus { UpToDate, FastForward, Merged, Conflicts };

struct MergeConflict {
    Key key{};
    std::optional<Hash> base;
    std::optional<Hash> target;
    std::optional<Hash> source;
    std::string reason;
};

// Optional typed-record reconciliation. Callbacks execute while the repository
// lock is held: they must be pure, must not call Repository methods, and must
// not retain a candidate lookup beyond the callback.
struct MergeResolution {
    std::optional<ModelRecord> record; // nullopt = unresolved
    std::vector<std::string> conflictingFields;
};
struct MergeCandidate {
    // Proposed merged snapshot, including pending additions/deletions.
    std::function<std::optional<ModelRecord>(const Key&)> get;
    // Visits records in UUID order without retaining all decoded payloads.
    std::function<void(const std::function<void(const Key&, const ModelRecord&)>&)> forEach;
};
// An explicit whole-record selection is allowed only for a genuine same-UUID
// divergence (including delete-versus-modify). Model-specific callers must
// still validate the complete selected snapshot before publication.
enum class MergeChoiceSide { Target, Source };
struct MergeOptions {
    // This callback is only invoked for concurrently changed, different UUID
    // records on a model-indexed branch, before field-level reconciliation.
    // Returning nullopt preserves the normal conflict/field resolver behavior.
    std::function<std::optional<MergeChoiceSide>(const MergeConflict&)> chooseConflict;
    std::function<MergeResolution(const Key&, const ModelRecord&, const ModelRecord&,
                                  const ModelRecord&)> resolve;
    // Explicit resolver for two independently added records with the SAME UUID
    // (no ancestor record). Needed by deterministic membership-chunk IDs.
    // Callers must verify they represent the same logical internal object.
    std::function<MergeResolution(const Key&, const ModelRecord&,
                                  const ModelRecord&)> resolveConcurrentAddition;
    // Verify that every user-supplied choice was actually consumed by this
    // merge. Called before any pack writes, including fast-forward publication.
    // Must be pure with respect to the Repository; cannot call its methods.
    std::function<std::optional<std::string>()> checkSelections;
    // Optional model-aware reconciliation of coordinated changes. Operates on a
    // private copy-on-write candidate BEFORE reference and semantic preflight.
    // `set` changes candidate records only; nothing is written until every
    // validator succeeds. Readers provide immutable branch-tip records.
    std::function<std::optional<std::string>(
        const MergeCandidate&,
        const std::function<void(const Key&, std::optional<ModelRecord>)>& set,
        const std::function<std::optional<ModelRecord>(const Key&)>& target,
        const std::function<std::optional<ModelRecord>(const Key&)>& source)> reconcile;
    // Return a nonempty diagnostic on semantic failure. Invoked on divergent
    // candidate merges AND fast-forwards (against the immutable source tip)
    // before any object bytes or branch refs are written. Up-to-date merges
    // do not revalidate an already incorporated snapshot. The source tip must
    // be model-indexed when validate is supplied for a fast-forward. Callbacks
    // cannot call Repository methods while its lock is held.
    std::function<std::optional<std::string>(const MergeCandidate&)> validate;
};

// Offline, physical-format-independent replication. Bundles carry the full
// immutable ancestry and both trees for a single branch tip. Imported branch
// refs are created or fast-forwarded only; divergent history is never replaced.
enum class BundleImportStatus { Created, FastForward, UpToDate };
// Optional per-transfer ceilings. Zero disables that particular ceiling for
// the existing offline API; HTTP always supplies finite limits.
struct BundleLimits {
    std::uint64_t maxBytes = 0;
    std::uint64_t maxObjects = 0;
    std::uint64_t maxObjectBytes = 0;
};

// Optional import-stage timings, measured inside the native importer. All
// durations are wall-clock milliseconds and can overlap neither each other
// nor the separate post-import repository audit. Populated on success only.
struct BundleImportProfile {
    double scanMs = 0;               // Header, per-object SHA-256, and disk index.
    double graphTraversalMs = 0;     // Typed revision/visited trie verification.
    double modelReferenceMs = 0;     // Semantic model and reverse-index checks.
    double reachabilityMs = 0;       // Unreachable object and ancestry checks.
    double branchCheckMs = 0;        // Fast-forward and branch status checks.
    double objectWriteMs = 0;        // Copy missing objects into local storage.
    double publicationMs = 0;       // Sync and atomic branch ref publication.
    double totalMs = 0;
    std::uint64_t bundleObjectReads = 0;
    std::uint64_t bundleObjectBytes = 0;
    std::uint64_t localObjectReads = 0;
    std::uint64_t localObjectBytes = 0;
};

// UUIDv7 revision replication. Branch identity and fork provenance are
// preserved; full and incremental bundles carry verified UUIDv7 revisions.
struct RevisionBundleImport {
    BundleImportStatus status = BundleImportStatus::UpToDate;
    RevisionId head;
    std::uint64_t objectsImported = 0;
};

// Results from physical verification and validation of every revision and
// logical index reachable from each published branch head.
struct VerificationReport {
    std::uint64_t physicalObjects = 0;
    std::uint64_t reachableObjects = 0;
    std::uint64_t branches = 0;
    std::uint64_t revisions = 0;
    std::uint64_t treeNodes = 0;
    std::uint64_t modelSnapshots = 0;
};

struct CompactionStats {
    std::uint64_t objectsBefore = 0;
    std::uint64_t objectsAfter = 0;
    std::uint64_t packBytesBefore = 0;
    std::uint64_t packBytesAfter = 0;
    std::uint64_t indexBytesBefore = 0;
    std::uint64_t indexBytesAfter = 0;
    // Leftover pack generations and pointer temporaries from interrupted
    // earlier compactions, removed before this compaction started.
    std::uint64_t staleFilesRemoved = 0;
};

// UUIDv7 equivalent of the full all-parent graph. IDs are domain identities;
// recordHash and expectedRecordHash authenticate the immutable object bytes.
// Cursor pins a complete branch HEAD snapshot, independent of moving refs.
struct RevisionGraphParent {
    RevisionId id;
    Hash expectedRecordHash{};
};

struct RevisionGraphRevision {
    RevisionId id;
    Hash recordHash{};
    Hash root{};
    Hash referencesRoot{};
    bool modelIndexed = false;
    std::vector<RevisionGraphParent> parents;
    CommitMetadata metadata;
    std::uint64_t committedAtMs = 0;  // Unix epoch ms (UTC)
    std::optional<std::string> displayMessage; // Mutable display annotation; canonical metadata and hash stay immutable.
};

struct BranchMetadata {
    std::string description;
    std::vector<std::string> tags;
    bool operator==(const BranchMetadata&) const = default;
};

struct RevisionGraphHead {
    BranchId branchId;
    std::string name;
    std::optional<RevisionId> revision;
    Hash recordHash{};
    std::optional<BranchId> originBranchId; // Immutable creation provenance.
    std::optional<RevisionId> forkRevisionId; // Exact immutable fork point, not a revision parent.
    BranchMetadata metadata;
};

struct RevisionGraphCursor {
    std::vector<RevisionGraphHead> heads;
    std::size_t skip = 0;
};

struct RevisionGraphPage {
    std::vector<RevisionGraphHead> heads;
    std::vector<RevisionGraphRevision> revisions;
    std::optional<RevisionGraphCursor> next;
};

// The identity and immutable provenance returned by one atomic branch creation.
// These fields are creation metadata, never revision-parent edges.
struct BranchCreationResult {
    BranchId branchId;
    RevisionId revisionId;
    std::optional<BranchId> originBranchId;
    std::optional<RevisionId> forkRevisionId;
};

// UUIDv7 merge result: the target and source branch names retain their own
// identities; only a successful merge can advance the target HEAD.
struct MergeResult {
    MergeStatus status = MergeStatus::UpToDate;
    std::optional<RevisionId> base;
    std::optional<RevisionId> targetBefore;
    std::optional<RevisionId> sourceHead;
    std::optional<RevisionId> resultHead;
    std::vector<MergeConflict> conflicts;
};

// Anonymous file in a repository's scratch directory (<repository>/tmp: the
// pack's filesystem, never TMPDIR). It has no name and disappears on close.
class ScratchFile final {
public:
    ScratchFile() = default;
    explicit ScratchFile(int fd) noexcept;
    ~ScratchFile();
    ScratchFile(ScratchFile&& other) noexcept;
    ScratchFile& operator=(ScratchFile&& other) noexcept;
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
    int fd() const noexcept;
    // Hands the descriptor to a new owner (for example QFile/AutoCloseHandle).
    int release() noexcept;
private:
    int fd_ = -1;
};

// Failures are reported as StorageError (a std::runtime_error) carrying a
// StorageErrorKind; callers must classify by kind, never by message text.
// Linux/POSIX prototype. All operations on one instance are thread-safe;
// processes using the same repository serialize operations with flock(2).
// A successful commit syncs its append-only pack before publishing its ref.
// Storage is one append-only, content-addressed object pack per repository.
class Repository final {
public:
    explicit Repository(std::filesystem::path directory);
    ~Repository();
    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

    // The owning project UUIDv7, pinned by the first published revision or
    // imported bundle; nullopt for a repository that has none yet. Every
    // project-qualified call below must name this project.
    std::optional<ProjectId> projectId();

    // Branches. A repository's first branch is created unborn (no revision,
    // null HEAD); later branches start at an existing committed revision
    // (`from` names the source branch; without it the call is invalid).
    RevisionId createBranch(const ProjectId& project, std::string_view name,
                              std::optional<std::string_view> from = std::nullopt);
    // Return the new branch UUID and fork metadata from the same locked write.
    BranchCreationResult createBranchWithIdentity(
        const ProjectId& project, std::string_view name,
        std::optional<std::string_view> from = std::nullopt);
    // Add an independent, unborn branch to this project's repository. The
    // caller-supplied UUID makes POST retries idempotent; no revision is made.
    BranchId createUnbornBranch(const ProjectId& project, std::string_view name,
                                 std::optional<BranchId> identity = std::nullopt,
                                 const BranchMetadata& metadata = {});
    // The first branch with an empty first revision. Only for projects that
    // are not created unborn (see docs/roadmap.md, VC-01).
    RevisionId createRootBranch(const ProjectId& project, std::string_view name,
                                  const CommitMetadata& metadata);
    // Clone an unborn branch with its original UUIDv7 into a branchless project.
    // A repeated identical request is safe after a lost acknowledgement.
    // No initial revision, manifest or synthetic fork is created.
    void provisionUnbornBranch(const ProjectId& project, std::string_view name,
                                 const BranchId& identity);
    RevisionId createBranchAtRevision(const ProjectId& project, std::string_view name,
                                        std::string_view sourceBranch, const RevisionId& revision);
    BranchCreationResult createBranchAtRevisionWithIdentity(
        const ProjectId& project, std::string_view name,
        std::string_view sourceBranch, const RevisionId& revision);
    // Create `name` from the branch identified by `source`: at its HEAD, or
    // at the exact `revision` reachable from it. `created` is the caller's
    // new branch UUIDv7. Repeating the same request returns the original
    // creation result instead of failing, so a retried request is safe;
    // `created` naming any different branch is a Conflict.
    BranchCreationResult createBranchFromId(const ProjectId& project, std::string_view name,
                                                const BranchId& source,
                                                std::optional<RevisionId> revision,
                                                const BranchId& created);
    // One atomically published UUIDv7 record: rename preserves ID and HEAD.
    void renameBranch(const ProjectId& project, std::string_view previous,
                        std::string_view renamed, const RevisionId& expectedHead);
    // Update branch name and metadata in the same atomic UUID-keyed ref write;
    // works for unborn branches. Caller pins the old name for CAS.
    void updateBranchProperties(const ProjectId& project, const BranchId& branch,
                                std::string_view expectedName, std::string_view name,
                                const BranchMetadata& metadata);
    // Commit message edits are separate mutable annotations. A revision record
    // (and its SHA-256/parents/content) is never rewritten.
    void updateRevisionDisplayMessage(const ProjectId& project, const RevisionId& revision,
                                      std::string_view expected, std::string_view message);
    std::optional<std::string> revisionDisplayMessage(const ProjectId& project,
                                                       const RevisionId& revision);

    // A single validated branch-file lookup, independent of how many other
    // branches are present. A symlink or a non-regular ref is never a branch.
    bool hasBranch(std::string_view branch);
    // Sorted branch names, independent of authorization. Filter at the service boundary.
    std::vector<std::string> listBranches();
    // Every published branch has a durable UUIDv7 name-independent ID.
    std::optional<BranchId> branchId(std::string_view name);
    // The branch with this UUIDv7 regardless of its current (renamable) name;
    // nullopt when no branch has it. An unborn branch has no revision.
    std::optional<RevisionGraphHead> branchById(const ProjectId& project, const BranchId& branch);

    // HEADs and history. head requires a committed HEAD; optionalHead
    // returns nullopt for an unborn branch.
    RevisionId head(const ProjectId& project, std::string_view branch);
    std::optional<RevisionId> optionalHead(const ProjectId& project, std::string_view branch);
    // First-parent history, newest-first; after is an exclusive revision
    // UUID cursor and must belong to the pinned branch's first-parent chain.
    std::vector<RevisionGraphRevision> history(
        const ProjectId& project, std::string_view branch, std::size_t maxCount = 100,
        std::optional<RevisionId> after = std::nullopt);
    // Stateless all-parent pagination across all branch HEADs. The cursor
    // freezes branch ID/name/HEAD/digest; continuation never reads moving refs.
    // Missing snapshot revisions fail, never truncate history.
    RevisionGraphPage revisionGraph(
        const ProjectId& project, std::size_t maxCount = 100,
        std::optional<RevisionGraphCursor> cursor = std::nullopt);
    // True when `candidate` is `tip` or one of its ancestors (either merge
    // parent). An unknown candidate is false without walking any history.
    bool revisionReachableFrom(const ProjectId& project, const RevisionId& tip,
                                 const RevisionId& candidate);
    // Canonical immutable revision record bytes, or nullopt for an unknown UUID.
    std::optional<Bytes> readRevision(const ProjectId& project, const RevisionId& id);

    // Commits. A failed preflight never advances HEAD or publishes a revision.
    // Raw revisions hold opaque values; model revisions hold ModelRecords plus
    // a persistent reverse-reference index and cannot be mixed on one branch.
    // Every revision records `metadata` and the time it was published.
    RevisionId commit(const ProjectId& project, std::string_view branch,
                        const std::vector<Change>& changes, const CommitMetadata& metadata,
                        std::optional<RevisionId> expectedHead = std::nullopt);
    // Unpinned writes may omit expectedHead. Local Commit passes
    // requireExactHead=true so an explicit null pins an unborn branch instead
    // of silently accepting a concurrently published first commit, and a
    // reserved revision UUID and commit time, so a retry writes the
    // identical record.
    RevisionId commitModel(const ProjectId& project, std::string_view branch,
                             std::vector<ModelChange> changes, const CommitMetadata& metadata,
                             std::optional<RevisionId> expectedHead = std::nullopt,
                             bool requireExactHead = false,
                             std::optional<ReservedRevision> reserved = std::nullopt);
    // Low-level revision publication over already stored trees (tests/tools).
    RevisionId writeRevision(const ProjectId& project, const Hash& root,
                               const Hash& referencesRoot, bool modelIndexed,
                               const CommitMetadata& metadata,
                               const std::vector<RevisionId>& parents = {});

    // Reads at an immutable revision, never at a moving branch ref. The raw
    // forms return the stored value bytes: the opaque value of a raw
    // revision, the encoded ModelRecord of a model revision.
    std::optional<Bytes> getAtRevision(const ProjectId& project, const RevisionId& revisionId,
                                         const Key& key);
    // Every key/value of a revision, in key order.
    std::vector<std::pair<Key, Bytes>> scanAtRevision(const ProjectId& project,
                                                        const RevisionId& revisionId);
    std::optional<ModelRecord> getModelAtRevision(const ProjectId& project,
                                                   const RevisionId& revisionId, const Key& key);
    // Sorted IDs of entities referring to key; O(log N + inbound references).
    std::vector<Key> referencingModelAtRevision(const ProjectId& project,
                                                   const RevisionId& revisionId, const Key& key);
    // Work is proportional to the changed trie paths; more than
    // maxDifferences results raise StorageError(LimitExceeded) before the full
    // difference is materialized.
    std::vector<Difference> diffRevisions(const ProjectId& project,
            const RevisionId& fromRevision, const RevisionId& toRevision,
            std::size_t maxDifferences);

    // Three-way merge. Domain callbacks are pure and execute under the
    // repository lock; both HEAD pins are checked atomically with publication.
    // A conflicting merge leaves both heads and the pack unchanged.
    // `metadata` describes the merge revision, if one is created.
    MergeResult mergeBranches(const ProjectId& project, std::string_view target,
                                  std::string_view source, const CommitMetadata& metadata,
                                  std::optional<RevisionId> expectedTarget = std::nullopt,
                                  std::optional<RevisionId> expectedSource = std::nullopt,
                                  const MergeOptions& options = {});

    // Offline replication. No filesystem path is ever derived from a
    // user-controlled branch name. Descriptor export requires an empty
    // caller-owned regular file and leaves it empty on failure. The incremental export
    // carries the revisions not reachable from the recipient's base and, per
    // revision, only the trie objects that differ from one of its parents.
    void exportRevisionBundle(const ProjectId& project, std::string_view branch,
                                int fd, BundleLimits limits = {});
    void exportIncrementalRevisionBundle(const ProjectId& project, std::string_view branch,
                                           const RevisionId& recipientBase, int fd,
                                           BundleLimits limits = {});
    // Named-file forms: the bundle appears at `bundlePath` only when complete
    // and fsynced (no-replace link, then a directory fsync). A crash or
    // failure never leaves a partial bundle there; an existing file is never
    // replaced (Conflict).
    void exportRevisionBundle(const ProjectId& project, std::string_view branch,
                                const std::filesystem::path& bundlePath, BundleLimits limits = {});
    void exportIncrementalRevisionBundle(const ProjectId& project, std::string_view branch,
                                           const RevisionId& recipientBase,
                                           const std::filesystem::path& bundlePath,
                                           BundleLimits limits = {});
    // Import creates a new local branch or advances it only by fast-forward;
    // it never overwrites divergent local work. All bundle objects are
    // verified before any object is written or a ref is published. Objects
    // an incremental bundle references but does not carry must exist
    // locally; they are checked only where they attach to bundled objects.
    RevisionBundleImport importRevisionBundle(const ProjectId& expectedProject, int fd,
                                                  std::string_view localBranch,
                                                  BundleLimits limits = {},
                                                  BundleImportProfile* profile = nullptr);

    // Scratch space on the repository's filesystem (see ScratchFile).
    ScratchFile createScratchFile();
    const std::filesystem::path& scratchDirectory() const noexcept;

    std::uint64_t packSize();
    std::uint64_t objectCount();
    // Full SHA-256 verification of every physical object. Normal reopening
    // trusts the checksummed sidecar index and verifies object bytes on read.
    void verifyPack();
    // Read-only full verification: physical objects, published revision history,
    // canonical raw/model/reference trees and exact model reverse references.
    // Checks every reachable ancestor, including second merge parents.
    VerificationReport verifyRepository();
    // Offline maintenance: retain all branch heads AND their full parent histories.
    // Refuses to run while another Repository instance has this directory open.
    // Publishes an immutable replacement pack using an fsynced atomic pointer.
    CompactionStats compact();
    // Bytes of an unpublished, torn pack tail discarded by this handle while
    // opening or refreshing the repository. A copy of every discarded region
    // is kept under <repository>/recovered-pack-tails/ for inspection.
    std::uint64_t recoveredPackTailBytes();
private:
    struct Location {
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };
    struct TreeNode {
        bool leaf = true;
        Key key{};
        Hash value{};
        std::uint8_t depth = 0;
        Key prefix{}; // representative UUID; nibbles before depth are shared
        std::array<Hash, 16> children{};
    };

    std::filesystem::path directory_;
    std::filesystem::path scratch_; // <directory_>/tmp
    int lockFd_ = -1;
    int lifetimeFd_ = -1;
    std::string packToken_;
    int packFd_ = -1;
    int indexFd_ = -1;
    std::mutex mutex_;
    struct HashHasher {
        std::size_t operator()(const Hash& hash) const noexcept {
            std::uint64_t value = 14695981039346656037ULL;
            for (std::uint8_t byte : hash) value = (value ^ byte) * 1099511628211ULL;
            return static_cast<std::size_t>(value);
        }
    };
    // Compact volatile accelerator over the checksummed append-only sidecar.
    // Each slot is [32-bit hash fingerprint | 32-bit 1-based entry ordinal].
    // Full SHA-256 equality and pack locations are read from objects.idx on
    // fingerprint matches. No alternative physical storage format is created.
    std::vector<std::uint64_t> indexSlots_;
    std::uint64_t indexCount_ = 0;
    struct CachedIndexEntry {
        Hash hash{};
        Location location{};
    };
    // Bounded direct-mapped hot-object cache; lookups remain correct without it.
    mutable std::vector<CachedIndexEntry> indexCache_;
    std::uint32_t indexFingerprint(const Hash& hash) const noexcept;
    void reserveIndexEntries(std::uint64_t entries);
    void indexEntry(const Hash& hash, std::uint64_t position);
    std::optional<Location> indexLocation(const Hash& hash) const;
    Hash indexedHash(std::uint32_t ordinal) const;
    void readIndexedEntry(std::uint32_t ordinal, std::uint8_t* destination) const;
    void clearIndex() noexcept;
    bool containsIndex(const Hash& hash) const;
    std::uint64_t scanOffset_ = 0;
    std::uint64_t indexReadOffset_ = 0;
    bool needsReopen_ = false; // fatal failure after publishing a compaction pointer
    std::uint64_t recoveredPackTailBytes_ = 0;
    Bytes pendingIndex_;

    void prepareScratchDirectory();
    void scanPackTail();
    void discardTornPackTail(std::uint64_t position, std::uint64_t size);
    void preserveDiscardedPackBytes(std::uint64_t position, std::uint64_t size);
    std::uint64_t removeStalePackGenerations();
    void rebuildIndex();

    void verifyPackLocked();
    void refreshIndex();
    Bytes load(const Hash& hash);
    Hash store(const Bytes& bytes);
    Hash store(Bytes&& bytes);
    RevisionId writeRevisionLocked(const ProjectId& project, const Hash& root,
                                     const Hash& referencesRoot, bool modelIndexed,
                                     const std::vector<RevisionId>& parents,
                                     const CommitMetadata& metadata,
                                     std::optional<ReservedRevision> reserved = std::nullopt);
    void exportRevisionBundleLocked(const ProjectId& project, std::string_view branch,
                                      std::optional<RevisionId> base, int fd,
                                      BundleLimits limits);
    struct RevisionRecord branchRevisionLocked(const ProjectId& project,
                                                   std::string_view branch);
    std::optional<BranchId> branchIdLocked(std::string_view name);
    BranchCreationResult createBranchLocked(const ProjectId& project, std::string_view name,
                                                std::optional<std::string_view> from,
                                                const BranchId& createdId,
                                                const CommitMetadata* rootMetadata = nullptr);
    BranchCreationResult createBranchAtRevisionLocked(const ProjectId& project,
                                                          std::string_view name,
                                                          std::string_view sourceBranch,
                                                          const RevisionId& revision,
                                                          const BranchId& createdId);
    TreeNode readNode(const Hash& hash);
    Hash storeNode(const TreeNode& node);
    using TreeUpdate = std::pair<Key, std::optional<Hash>>;
    Hash buildTree(const std::vector<TreeUpdate>& updates, std::size_t begin,
                   std::size_t end, unsigned minDepth);
    Hash applyTree(const Hash& root, const std::vector<TreeUpdate>& updates,
                   std::size_t begin, std::size_t end, unsigned minDepth);
    std::optional<Hash> lookup(const Hash& root, const Key& key);
    void listHashes(const Hash& root, std::vector<std::pair<Key, Hash>>& output);
    void diffHashes(const Hash& from, const Hash& to, std::vector<Difference>& output,
                    std::size_t limit = std::numeric_limits<std::size_t>::max());
    void diffWholeSubtree(const Hash& root, bool added, std::vector<Difference>& output,
                          std::size_t limit);
    // Incremental export: objects reachable from `tip` but not held by a
    // recipient of `base`, collected per new revision by a trie delta.
    // `reachedBase` reports whether `base` is an ancestor of (or equal to) `tip`.
    std::set<Hash> collectDelta(const Hash& tip, const Hash& base, std::uint64_t maxObjects,
                                  bool& reachedBase);
    void collectTreeDelta(const Hash& before, const Hash& after, std::set<Hash>& output,
                          std::uint64_t maxObjects);
    struct PreparedModelCommit {
        std::vector<Change> encoded;
        Hash referencesRoot{};
    };
    PreparedModelCommit prepareModelCommitLocked(const Hash& root, const Hash& referencesRoot,
                                                  std::vector<ModelChange> changes);
    Hash applyEncodedChangesLocked(const Hash& original, const std::vector<Change>& changes);
    std::vector<Key> readReferrers(const Hash& root, const Key& target);
    void syncPack();
};

}  // namespace aowis::dvmdb
