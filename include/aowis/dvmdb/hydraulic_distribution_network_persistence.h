#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

// Optional Qt/hydraulic persistence adapter for a complete NetworkHydraulicDistribution aggregate. The
// authoritative entity bytes are encoded by the native-owned canonical ModelCodec;
// ordered network membership is stored as bounded, individually versioned chunks.
#include "aowis/dvmdb/engineering_delta_scope.h"
#include "aowis/dvmdb/model_codec_adapter.h"
#include "aowis/dvmdb/project_service.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QUuid>
#include <aowis/model/edit/model_edit_transaction.h>

#include <optional>
#include <string_view>
#include <variant>
#include <utility>
#include <vector>

namespace aowis::dvmdb::hydraulic {

inline constexpr std::uint16_t hydraulicDistributionNetworkKind = 100;

// VC-04 draft transaction membership operation. The GUI transmits logical
// collection identity; only this DVMDB adapter knows chunk and manifest bytes.
// The owning network must already exist in the pinned base or candidate.
struct DraftMembershipIntent final {
    Key network;
    Key member;
    std::uint16_t kind = 0;
    std::uint16_t collection = 0;
    bool inserted = false;
};

// Construct the server-owned empty membership manifest for a newly created
// hydraulic distribution network. This is an ordinary draft ModelChange, not a revision.
// No external caller should manufacture kind-100 manifest bytes itself.
ModelChange emptyDraftHydraulicDistributionNetworkRoot(const QUuid& networkId, const QString& name);

// Construct a complete validated hydraulic distribution network as native draft records.
// This does not commit anything and is only used under a fenced, empty-draft
// HTTP import transaction. Manifest/chunk bytes remain server-owned.
std::vector<ModelChange> fullDraftHydraulicDistributionNetworkChanges(const NetworkHydraulicDistribution& network);

// Read one consistent working-copy view (immutable committed base overlaid by
// the draft's complete candidate record map). Never reads a mutable HEAD.
// Validates native chunks, canonical entity payloads and typed dependencies.
// Save-time check of one network touched by a draft delta, WITHOUT loading
// the whole network. `before` must be a candidate that passed the complete
// network check; `after` is before + `delta`. Proves what the complete
// loader proves for the touched part:
// - root manifest and every new, changed or re-listed membership chunk
//   (network, collection, member kinds);
// - every changed or newly entered member: canonical payload, physical
//   values, every reference resolved to a member of the right type in THIS
//   network, stored reference index equal to the canonical dependencies;
// - demand points attached to a changed pipe (endpoint rule);
// - no member that left the network is referenced by the network settings.
// Returns false when the complete loader is needed instead: root created or
// deleted, network settings changed, or a member moved to another network.
// Invalid data throws StorageError (InvalidInput or Corruption).
bool checkDraftHydraulicNetworkDelta(const EngineeringCandidateView& before,
                                     const EngineeringCandidateView& after,
                                     const QUuid& networkId,
                                     const std::vector<ModelChange>& delta);
// Building blocks for change-based History replies (no member is decoded):
// - the network settings image (aowis::model::hydraulicDistributionRootImage)
//   of a stored root record;
// - the ordered member UUIDs of one collection, read from the root manifest
//   and its membership chunks only;
// - collection index of a member record kind, collection count and the
//   MODEL type name of a collection (e.g. "HydraulicLinkPipe");
// - network and collection of a membership chunk record.
QByteArray hydraulicRootImageOfRecord(const ModelRecord& root);
QList<QUuid> hydraulicCollectionMembers(
    const std::function<std::optional<ModelRecord>(const Key&)>& read,
    const QUuid& networkId, std::size_t collection);
std::optional<std::size_t> hydraulicCollectionForKind(std::uint16_t kind);
std::size_t hydraulicCollectionCount();
QString hydraulicCollectionTypeName(std::size_t collection);
std::optional<std::pair<QUuid, std::size_t>> hydraulicChunkCollection(const ModelRecord& chunk);

// True when a network root record was created, deleted, or its network
// settings (everything except membership chunk lists) changed.
bool hydraulicRootSettingsChanged(const std::optional<ModelRecord>& before,
                                  const std::optional<ModelRecord>& after);

std::optional<NetworkHydraulicDistribution> loadDraftHydraulicDistributionNetwork(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& networkId);


// Convert one reversible, metadata-only MODEL root edit to a server-owned
// kind-100 manifest change. Checks exact before-image, canonical after-image,
// unchanged network identity, full hydraulic dependencies and chunk ownership.
// No change is published if validation fails; callers save it atomically with
// the originating journal event under the draft generation/lease fence.
ModelChange materializeDraftRootEdit(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const aowis::model::ModelEntityEdit& edit);

// Add native root/chunk ModelChanges for a bounded batch of validated logical
// membership intents. `readCurrent` returns the draft BEFORE the incoming
// batch (candidate overlay first, immutable base second); `changes` initially
// contains the caller's ordinary entity updates. This function never writes.
// The resulting changes are published together with the events by the
// existing generation-checked, fsynced ProjectService draft replacement.
void materializeDraftMembership(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const std::vector<DraftMembershipIntent>& intents,
    std::vector<ModelChange>& changes);


// Hydraulic Undo/Redo is replayed on changes by the shared History engine
// (map_annotation_action_replay.h), with the membership and root
// materializers above; this module has no separate History replay.

// Exactly the 17 top-level types persisted by saveHydraulicDistributionNetwork().
// The alternative order follows the manifest's collection order.
using HydraulicEntity = std::variant<
    HydraulicPipeMaterial, HydraulicPatternTime, HydraulicCurveTankVolume,
    HydraulicCurvePumpHead, HydraulicCurvePumpEfficiency, HydraulicCurveValveHeadloss,
    HydraulicCurveValveCharacteristic, HydraulicCurveGeneric, HydraulicNodeReservoir,
    HydraulicNodeJunction, HydraulicNodeTank, HydraulicDemandPoint, HydraulicLinkPipe,
    HydraulicLinkPump, HydraulicLinkValve, HydraulicControlSimple,
    HydraulicControlRule>;

// Ordinal matches the corresponding alternative in HydraulicEntity, and the
// canonical NetworkHydraulicDistribution collection order. This is an API enum, not a
// serialized/on-disk kind identifier.
enum class HydraulicEntityType : std::size_t {
    PipeMaterial = 0,
    TimePattern,
    TankVolumeCurve,
    PumpHeadCurve,
    PumpEfficiencyCurve,
    ValveHeadlossCurve,
    ValveCharacteristicCurve,
    GenericCurve,
    Reservoir,
    Junction,
    Tank,
    DemandPoint,
    Pipe,
    Pump,
    Valve,
    SimpleControl,
    RuleControl
};

// Authoritative native record-kind mapping for the 17 manifest collections.
// History replay must not duplicate these numeric kinds.
std::uint16_t hydraulicMemberKindForCollection(std::size_t collection);

// Decode one canonical MODEL hydraulic member image into its exact native
// payload/reference representation and manifest collection ordinal. History
// replay uses this same encoder as the native membership writer; it never
// invents root/chunk records or trusts the client's reference index.
struct CanonicalHydraulicMemberImage final {
    std::uint16_t collection = 0;
    ModelRecord record;
};
CanonicalHydraulicMemberImage canonicalHydraulicMemberImage(
    const aowis::model::ModelEntityEdit& edit, const QByteArray& image);

struct HydraulicEntityChange {
    QUuid uuid;
    // nullopt removes the UUID from its owning collection; a value replaces
    // the existing same-kind member or appends a new member to its collection.
    std::optional<HydraulicEntity> value;
};

// Diagnostics for choosing between bounded indexed validation and the
// conservative complete-network validator. Populated on success only.
struct HydraulicEditDiagnostics {
    bool indexedScalarFastPath = false;
    bool indexedTopologyFastPath = false;
    bool indexedMembershipFastPath = false;
    bool indexedCatalogueFastPath = false;
    bool indexedPhysicalFastPath = false;
    bool indexedDependentUpdateFastPath = false;
    std::size_t validatedEntities = 0; // changed payloads decoded on fast path
};

// A user may select a side for a genuinely conflicting canonical model field.
// `field` is an exact name from the merge conflict diagnostic, including
// nested metadata fields (e.g. "metadata.description"). Nonconflicting or
// unknown names are rejected, not silently ignored.
enum class HydraulicMergeSide { Target, Source };
struct HydraulicFieldChoice {
    QUuid entityUuid;
    QString field;
    HydraulicMergeSide side = HydraulicMergeSide::Target;
};
struct HydraulicObjectChoice {
    QUuid entityUuid;
    // Selects one complete existing/deleted record from a genuinely divergent
    // UUID conflict. A deleted side means deletion; a kept side may fail the
    // whole-network validation if membership changes are incompatible.
    HydraulicMergeSide side = HydraulicMergeSide::Target;
};

// Canonical project-scoped UUIDv7 hydraulic merge. Both immutable branch-tip
// UUIDs pin every interactive selection; full-network validation is mandatory.
struct HydraulicMergeSelections {
    RevisionId expectedTargetHead;
    RevisionId expectedSourceHead;
    std::vector<HydraulicFieldChoice> fields;
    std::vector<HydraulicObjectChoice> objects;
};
// Optional resolver for same-UUID records of another engineering domain
// (map annotations). It receives the caller's field choices for that UUID.
// A value means "handled": the result is used and those field choices count
// as consumed, exactly like hydraulic field choices. nullopt hands the record
// to the hydraulic resolver. It runs under the repository merge lock and must
// be pure; an invalid selection is reported by throwing InvalidInput.
using DomainMergeResolver = std::function<std::optional<MergeResolution>(
    const Key& key, const ModelRecord& base, const ModelRecord& target,
    const ModelRecord& source, const QMap<QString, bool>& fieldChoices)>;

// `metadata` describes the merge revision, if one is created.
MergeResult mergeHydraulicBranches(Repository& repository, const ProjectId& project,
                                       std::string_view target, std::string_view source,
                                       const HydraulicMergeSelections& selections,
                                       const CommitMetadata& metadata,
                                       const std::function<void(MergeOptions&)>& configure = {},
                                       const DomainMergeResolver& domainResolver = {});

// Canonical project-scoped UUIDv7 hydraulic operations. All reads pin an
// immutable revision; saves/edits compare expected HEAD during atomic commit.
// Whitelisted scalar, physical, catalogue, topology, dependent-reference and
// structural membership edits use revision-pinned indexed validation. Other
// edits fall back to complete canonical network validation and replacement.
// Every revision records the caller's `metadata` (author and message).
RevisionId saveHydraulicDistributionNetwork(Repository& repository, const ProjectId& project,
                                  std::string_view branch, const NetworkHydraulicDistribution& network,
                                  const RevisionId& expectedHead,
                                  const CommitMetadata& metadata);
RevisionId editHydraulicDistributionNetwork(Repository& repository, const ProjectId& project,
                                  std::string_view branch, const QUuid& networkUuid,
                                  const std::vector<HydraulicEntityChange>& changes,
                                  const RevisionId& expectedHead,
                                  const CommitMetadata& metadata,
                                  HydraulicEditDiagnostics* diagnostics = nullptr);
std::optional<NetworkHydraulicDistribution> loadHydraulicDistributionNetworkAtRevision(
    Repository& repository, const ProjectId& project, const RevisionId& revisionId,
    const QUuid& networkUuid);
std::optional<QList<QUuid>> listHydraulicEntityIdsAtRevisionLimited(
    Repository& repository, const ProjectId& project, const RevisionId& revisionId,
    const QUuid& networkUuid, HydraulicEntityType type, std::size_t maxCount);
std::optional<HydraulicEntity> loadHydraulicEntityAtRevision(
    Repository& repository, const ProjectId& project, const RevisionId& revisionId,
    const QUuid& networkUuid, const QUuid& entityUuid);

} // namespace aowis::dvmdb::hydraulic
