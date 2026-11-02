#pragma once

#include <cstddef>
#include <functional>

// Optional Qt/hydraulic persistence adapter for a complete NetworkHydraulic aggregate. The
// authoritative entity bytes are encoded by the native-owned canonical ModelCodec;
// ordered network membership is stored as bounded, individually versioned chunks.
#include "aowis/dvmdb/model_codec_adapter.h"

#include <QList>

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace aowis::dvmdb::hydraulic {

inline constexpr std::uint16_t hydraulicNetworkKind = 100;

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
// hydraulic network. This is an ordinary draft ModelChange, not a revision.
// No external caller should manufacture kind-100 manifest bytes itself.
ModelChange emptyDraftHydraulicNetworkRoot(const QUuid& networkId, const QString& name);

// Construct a complete validated hydraulic network as native draft records.
// This does not commit anything and is only used under a fenced, empty-draft
// HTTP import transaction. Manifest/chunk bytes remain server-owned.
std::vector<ModelChange> fullDraftHydraulicNetworkChanges(const NetworkHydraulic& network);

// Read one consistent working-copy view (immutable committed base overlaid by
// the draft's complete candidate record map). Never reads a mutable HEAD.
// Validates native chunks, canonical entity payloads and typed dependencies.
std::optional<NetworkHydraulic> loadDraftHydraulicNetwork(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& networkId);


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


// Exactly the 18 top-level types persisted by saveHydraulicNetwork().
// The alternative order follows the manifest's collection order.
using HydraulicEntity = std::variant<
    HydraulicPipeMaterial, HydraulicPatternTime, HydraulicCurveTankVolume,
    HydraulicCurvePumpHead, HydraulicCurvePumpEfficiency, HydraulicCurveValveHeadloss,
    HydraulicCurveValveCharacteristic, HydraulicCurveGeneric, HydraulicNodeReservoir,
    HydraulicNodeJunction, HydraulicNodeTank, HydraulicDemandPoint, HydraulicLinkPipe,
    HydraulicLinkPump, HydraulicLinkValve, HydraulicControlSimple,
    HydraulicControlRule, HydraulicMapLabel>;

// Ordinal matches the corresponding alternative in HydraulicEntity, and the
// canonical NetworkHydraulic collection order. This is an API enum, not a
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
    RuleControl,
    MapLabel
};

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
// `metadata` describes the merge revision, if one is created.
MergeResult mergeHydraulicBranches(Repository& repository, const ProjectId& project,
                                       std::string_view target, std::string_view source,
                                       const HydraulicMergeSelections& selections,
                                       const CommitMetadata& metadata);

// Canonical project-scoped UUIDv7 hydraulic operations. All reads pin an
// immutable revision; saves/edits compare expected HEAD during atomic commit.
// Whitelisted scalar, physical, catalogue, topology, dependent-reference and
// structural membership edits use revision-pinned indexed validation. Other
// edits fall back to complete canonical network validation and replacement.
// Every revision records the caller's `metadata` (author and message).
RevisionId saveHydraulicNetwork(Repository& repository, const ProjectId& project,
                                  std::string_view branch, const NetworkHydraulic& network,
                                  const RevisionId& expectedHead,
                                  const CommitMetadata& metadata);
RevisionId editHydraulicNetwork(Repository& repository, const ProjectId& project,
                                  std::string_view branch, const QUuid& networkUuid,
                                  const std::vector<HydraulicEntityChange>& changes,
                                  const RevisionId& expectedHead,
                                  const CommitMetadata& metadata,
                                  HydraulicEditDiagnostics* diagnostics = nullptr);
std::optional<NetworkHydraulic> loadHydraulicNetworkAtRevision(
    Repository& repository, const ProjectId& project, const RevisionId& revisionId,
    const QUuid& networkUuid);
std::optional<QList<QUuid>> listHydraulicEntityIdsAtRevisionLimited(
    Repository& repository, const ProjectId& project, const RevisionId& revisionId,
    const QUuid& networkUuid, HydraulicEntityType type, std::size_t maxCount);
std::optional<HydraulicEntity> loadHydraulicEntityAtRevision(
    Repository& repository, const ProjectId& project, const RevisionId& revisionId,
    const QUuid& networkUuid, const QUuid& entityUuid);

} // namespace aowis::dvmdb::hydraulic
