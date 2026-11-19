#include "aowis/dvmdb/map_annotation_action_replay.h"

#include "aowis/dvmdb/map_annotation_persistence.h"
#include "aowis/dvmdb/hydraulic_distribution_network_persistence.h"
#include "aowis/dvmdb/map_annotation_snapshot_validation.h"
#include "aowis/dvmdb/model_record_conversion.h"

#include <aowis/model/edit/model_edit_transaction.h>
#include <aowis/model/edit/hydraulic_distribution_root_edit.h>

#include <QByteArray>
#include <QSet>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace aowis::dvmdb::annotations {
namespace {

using Records = std::map<Key, ModelRecord>;
constexpr std::size_t maximumArchiveActions = 4096;
constexpr std::size_t maximumArchiveBytes = 8U * 1024U * 1024U;
constexpr std::size_t maximumSnapshotRecords = 200000;
constexpr std::size_t maximumSnapshotBytes = 256U * 1024U * 1024U;

bool equalRecord(const ModelRecord& first, const ModelRecord& second) {
    return first.kind == second.kind && first.payload == second.payload &&
           first.references == second.references;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

MergeCandidate view(const Records& records) {
    MergeCandidate candidate;
    candidate.get = [&records](const Key& key) -> std::optional<ModelRecord> {
        const Records::const_iterator found = records.find(key);
        return found != records.end() ? std::optional<ModelRecord>(found->second)
                                      : std::nullopt;
    };
    candidate.forEach = [&records](
            const std::function<void(const Key&, const ModelRecord&)>& visitor) {
        for (const std::pair<const Key, ModelRecord>& entry : records)
            visitor(entry.first, entry.second);
    };
    return candidate;
}

Records copyRecords(const MergeCandidate& candidate) {
    Records result;
    std::size_t bytes = 0;
    candidate.forEach([&result, &bytes](const Key& key, const ModelRecord& record) {
        require(result.size() < maximumSnapshotRecords, "annotation replay model record limit exceeded");
        require(record.payload.size() <= maximumSnapshotBytes - bytes,
                "annotation replay model byte limit exceeded");
        bytes += record.payload.size();
        require(result.emplace(key, record).second, "duplicate native model key in replay candidate");
    });
    return result;
}

void validate(const Records& records) {
    const std::optional<std::string> problem = validateAnnotationSnapshot(view(records));
    if (problem) throw std::runtime_error("invalid annotation replay snapshot: " + *problem);
}

std::vector<ModelChange> changesBetween(const Records& parent, const Records& child) {
    std::vector<ModelChange> changes;
    for (const std::pair<const Key, ModelRecord>& entry : child) {
        const Records::const_iterator found = parent.find(entry.first);
        if (found == parent.end() || !equalRecord(found->second, entry.second))
            changes.push_back(ModelChange{entry.first, entry.second});
    }
    for (const std::pair<const Key, ModelRecord>& entry : parent)
        if (!child.contains(entry.first))
            changes.push_back(ModelChange{entry.first, std::nullopt});
    return changes;
}

void requireSameChanges(const std::vector<ModelChange>& expected,
                        const std::vector<ModelChange>& supplied) {
    std::map<Key, std::optional<ModelRecord>> expectedByKey;
    std::map<Key, std::optional<ModelRecord>> suppliedByKey;
    for (const ModelChange& change : expected)
        require(expectedByKey.emplace(change.key, change.record).second,
                "duplicate expected annotation replay delta key");
    for (const ModelChange& change : supplied)
        require(suppliedByKey.emplace(change.key, change.record).second,
                "duplicate native witness delta key");
    require(expectedByKey.size() == suppliedByKey.size(),
            "archived annotation native delta lacks exact record coverage");
    for (const std::pair<const Key, std::optional<ModelRecord>>& entry : expectedByKey) {
        const std::map<Key, std::optional<ModelRecord>>::const_iterator found =
            suppliedByKey.find(entry.first);
        require(found != suppliedByKey.end() && entry.second.has_value() == found->second.has_value(),
                "native witness has an unexpected annotation delta key");
        if (entry.second)
            require(equalRecord(*entry.second, *found->second),
                    "native witness annotation delta bytes differ from immutable revision");
    }
}

// Immutable endpoints constrain ownership of surviving/previous members.
// The canonical edit frame itself now authenticates ownerLayerId, so a member
// absent at both endpoints can still be replayed without guessing.
std::map<Key, Key> knownOwners(const Records& parent, const Records& committed) {
    std::map<Key, Key> owners;
    const std::function<void(const Records&)> collect = [&owners](const Records& records) {
        const MergeCandidate candidate = view(records);
        for (const std::pair<const Key, ModelRecord>& entry : records) {
            if (entry.second.kind != annotationLayerKind) continue;
            const QUuid layerId = record::uuidFromKey(entry.first);
            const std::optional<AnnotationLayerSnapshot> layer =
                loadAnnotationLayer(candidate.get, layerId);
            require(layer.has_value(), "missing annotation layer during history attribution");
            for (const MapAnnotation& annotation : layer->annotations) {
                const Key member = record::keyFromUuid(annotation.uuid);
                const std::map<Key, Key>::const_iterator owner = owners.find(member);
                require(owner == owners.end() || owner->second == entry.first,
                        "annotation ownership changed between historical endpoints");
                owners[member] = entry.first;
            }
        }
    };
    collect(parent);
    collect(committed);
    return owners;
}

// The immutable local Commit sidecar authenticates this exact ordered chain.
// Replay must check it before applying actions: endpoint equality cannot prove
// the order of two causally independent edits.
Hash archiveActionChain(const std::vector<WorkingCopyEvent>& actions) {
    Hash chained{};
    for (const WorkingCopyEvent& action : actions) {
        Bytes input(chained.begin(), chained.end());
        input.insert(input.end(), action.encoded.begin(), action.encoded.end());
        chained = sha256(input);
    }
    return chained;
}

// A hydraulic member edit does not yet carry network provenance in MODEL.
// Immutable native endpoint membership may prove its owning network, and a
// single-root revision has an unambiguous owner for transient creations.
// Never guess an owner when multiple network roots are present.
struct HydraulicOwner final {
    Key network;
    std::uint16_t collection = 0;
    std::uint16_t kind = 0;
};

struct HydraulicOwnership final {
    std::set<Key> roots;
    std::map<Key, HydraulicOwner> members;
};

HydraulicOwnership knownHydraulicOwners(const Records& parent, const Records& committed) {
    HydraulicOwnership result;
    std::set<Key> parentRoots;
    std::set<Key> committedRoots;
    for (const std::pair<const Key, ModelRecord>& entry : parent)
        if (entry.second.kind == hydraulic::hydraulicDistributionNetworkKind)
            parentRoots.insert(entry.first);
    for (const std::pair<const Key, ModelRecord>& entry : committed)
        if (entry.second.kind == hydraulic::hydraulicDistributionNetworkKind)
            committedRoots.insert(entry.first);
    require(parentRoots == committedRoots,
            "mixed history cannot replay hydraulic network root creation/deletion");
    result.roots = parentRoots;
    const std::function<void(const Records&)> collect = [&result](const Records& snapshot) {
        const MergeCandidate candidate = view(snapshot);
        for (const Key& root : result.roots) {
            const std::optional<NetworkHydraulicDistribution> network =
                hydraulic::loadDraftHydraulicDistributionNetwork(
                    candidate.get, record::uuidFromKey(root));
            require(network.has_value(), "unreadable hydraulic network in history checkpoint");
            std::uint16_t collection = 0;
#define REGISTER_HISTORY_MEMBER(name, type) \
            for (const type& item : network->name) { \
                const Key key = record::keyFromUuid(item.uuid); \
                const HydraulicOwner owner{root, collection, hydraulic::hydraulicMemberKindForCollection(collection)}; \
                const std::map<Key, HydraulicOwner>::const_iterator existing = result.members.find(key); \
                require(existing == result.members.end() || \
                        (existing->second.network == owner.network && \
                         existing->second.collection == owner.collection && \
                         existing->second.kind == owner.kind), \
                        "hydraulic member ownership changed across archived endpoints"); \
                result.members[key] = owner; \
            } \
            ++collection;
            REGISTER_HISTORY_MEMBER(pipe_materials, HydraulicPipeMaterial)
            REGISTER_HISTORY_MEMBER(patterns_time, HydraulicPatternTime)
            REGISTER_HISTORY_MEMBER(curves_tank_volume, HydraulicCurveTankVolume)
            REGISTER_HISTORY_MEMBER(curves_pump_head, HydraulicCurvePumpHead)
            REGISTER_HISTORY_MEMBER(curves_pump_efficiency, HydraulicCurvePumpEfficiency)
            REGISTER_HISTORY_MEMBER(curves_valve_headloss, HydraulicCurveValveHeadloss)
            REGISTER_HISTORY_MEMBER(curves_valve_characteristic, HydraulicCurveValveCharacteristic)
            REGISTER_HISTORY_MEMBER(curves_generic, HydraulicCurveGeneric)
            REGISTER_HISTORY_MEMBER(nodes_reservoirs, HydraulicNodeReservoir)
            REGISTER_HISTORY_MEMBER(nodes_junctions, HydraulicNodeJunction)
            REGISTER_HISTORY_MEMBER(nodes_tanks, HydraulicNodeTank)
            REGISTER_HISTORY_MEMBER(demand_points, HydraulicDemandPoint)
            REGISTER_HISTORY_MEMBER(links_pipes, HydraulicLinkPipe)
            REGISTER_HISTORY_MEMBER(links_pumps, HydraulicLinkPump)
            REGISTER_HISTORY_MEMBER(links_valves, HydraulicLinkValve)
            REGISTER_HISTORY_MEMBER(controls_simple, HydraulicControlSimple)
            REGISTER_HISTORY_MEMBER(controls_rules, HydraulicControlRule)
#undef REGISTER_HISTORY_MEMBER
        }
    };
    collect(parent);
    collect(committed);
    return result;
}

void applyHydraulicScalar(Records& current, const aowis::model::ModelEntityEdit& edit) {
    require(edit.before && edit.after,
            "mixed hydraulic creation/deletion must use native membership materializer");
    const hydraulic::CanonicalHydraulicMemberImage previous =
        hydraulic::canonicalHydraulicMemberImage(edit, *edit.before);
    const hydraulic::CanonicalHydraulicMemberImage next =
        hydraulic::canonicalHydraulicMemberImage(edit, *edit.after);
    require(previous.collection == next.collection &&
            previous.record.kind == next.record.kind,
            "hydraulic entity cannot change collection in-place");
    const Records::iterator stored = current.find(record::keyFromUuid(edit.entityId));
    require(stored != current.end() && equalRecord(stored->second, previous.record),
            "stale or noncanonical mixed hydraulic before-image/reference index");
    stored->second = next.record;
}

// Use the same indexed chunk/manifest materializer as an ordinary HTTP Save.
// The original MODEL event's edit order provides insertion order. If an HTTP
// request originally used a different membership order, immutable child-byte
// equality will reject the archive rather than guessing an ordering.
void applyHydraulicMembership(Records& current,
        const std::vector<aowis::model::ModelEntityEdit>& edits,
        HydraulicOwnership& ownership) {
    struct Group final {
        std::vector<ModelChange> changes;
        std::vector<hydraulic::DraftMembershipIntent> intents;
    };
    std::map<Key, Group> byNetwork;
    for (const aowis::model::ModelEntityEdit& edit : edits) {
        require(edit.before.has_value() != edit.after.has_value(),
                "invalid hydraulic membership action images");
        const QByteArray& image = edit.after ? *edit.after : *edit.before;
        const hydraulic::CanonicalHydraulicMemberImage decoded =
            hydraulic::canonicalHydraulicMemberImage(edit, image);
        const Key member = record::keyFromUuid(edit.entityId);
        const Records::const_iterator stored = current.find(member);
        if (edit.before)
            require(stored != current.end() && equalRecord(stored->second, decoded.record),
                    "stale hydraulic membership removal before-image");
        else
            require(stored == current.end(),
                    "hydraulic membership creation collides with existing record");
        const std::map<Key, HydraulicOwner>::const_iterator owner =
            ownership.members.find(member);
        HydraulicOwner selected;
        if (owner != ownership.members.end()) {
            selected = owner->second;
        } else {
            require(ownership.roots.size() == 1,
                    "transient hydraulic member lacks authenticated network provenance");
            selected = HydraulicOwner{*ownership.roots.begin(), decoded.collection,
                                       decoded.record.kind};
            ownership.members.emplace(member, selected);
        }
        require(selected.collection == decoded.collection &&
                selected.kind == decoded.record.kind,
                "hydraulic archive member kind/collection disagrees with ownership");
        Group& group = byNetwork[selected.network];
        group.changes.push_back(ModelChange{member,
            edit.after ? std::optional<ModelRecord>(decoded.record) : std::nullopt});
        group.intents.push_back(hydraulic::DraftMembershipIntent{
            selected.network, member, decoded.record.kind, decoded.collection,
            edit.after.has_value()});
    }
    for (std::pair<const Key, Group>& part : byNetwork) {
        const MergeCandidate pinned = view(current);
        hydraulic::materializeDraftMembership(pinned.get, part.second.intents,
                                             part.second.changes);
        for (const ModelChange& change : part.second.changes) {
            if (change.record) current[change.key] = *change.record;
            else current.erase(change.key);
        }
    }
}

void applyHydraulicRoot(Records& current, const aowis::model::ModelEntityEdit& edit) {
    require(!edit.entityId.isNull() && !edit.ownerLayerId,
            "mixed hydraulic root has invalid identity or annotation ownership");
    const MergeCandidate pinned = view(current);
    const ModelChange change = hydraulic::materializeDraftRootEdit(pinned.get, edit);
    require(change.record.has_value(), "hydraulic root metadata edit removed its manifest");
    current[change.key] = *change.record;
}

void applyChanges(Records& records, const std::vector<ModelChange>& changes) {
    for (const ModelChange& change : changes) {
        if (change.record) records[change.key] = *change.record;
        else records.erase(change.key);
    }
}

} // namespace

// Shared canonical single-action materializer, used both for forward replay
// and for reversing an unborn journal back to its unjournaled initialization
// seed. Each action is materialized with the same per-domain checks as a Save
// (before-images, membership, reference index). The complete snapshot
// validator is NOT run per action (A2: that made History O(actions x
// project)); callers validate every state they return or publish, and the
// endpoints are proven against the immutable, validated revisions.
static void applyVerifiedActionEdits(Records& current,
        const QVector<aowis::model::ModelEntityEdit>& edits, bool combined,
        std::map<Key, Key>& owners, HydraulicOwnership& hydraulicOwners,
        std::set<Key>& changedLayers, bool& annotationSeen, bool& hydraulicSeen,
        bool reversing = false) {
    std::set<Key> actionEntities;
    std::map<Key, std::vector<aowis::model::ModelEntityEdit>> byLayer;
    std::vector<aowis::model::ModelEntityEdit> hydraulicEdits;
    for (const aowis::model::ModelEntityEdit& edit : edits) {
        const Key entityId = record::keyFromUuid(edit.entityId);
        require(!edit.entityId.isNull() && actionEntities.insert(entityId).second,
                "repeated or null UUID within archived transaction");
        if (edit.entityType != QStringLiteral("map.annotation")) {
            require(combined, "mixed/non-annotation archive needs multi-domain replay");
            hydraulicSeen = true;
            hydraulicEdits.push_back(edit);
            continue;
        }
        annotationSeen = true;
        require(edit.ownerLayerId && !edit.ownerLayerId->isNull(),
                "archived annotation edit lacks authenticated layer provenance");
        const Key claimedLayer = record::keyFromUuid(*edit.ownerLayerId);
        const std::map<Key, Key>::const_iterator owner = owners.find(entityId);
        require(owner == owners.end() || owner->second == claimedLayer,
                "annotation ownership changed within archived history");
        owners.emplace(entityId, claimedLayer);
        byLayer[claimedLayer].push_back(edit);
    }
    // When reversing an action, remove/update annotations BEFORE removing
    // their hydraulic anchors. The forward direction creates anchors first.
    const std::function<void()> materializeAnnotations = [&]() {
        for (const std::pair<const Key, std::vector<aowis::model::ModelEntityEdit>>& part : byLayer) {
            const QUuid layerId = record::uuidFromKey(part.first);
            const MergeCandidate pinned = view(current);
            const std::vector<ModelChange> delta =
                materializeDraftAnnotationEdits(pinned.get, layerId, part.second);
            applyChanges(current, delta);
            changedLayers.insert(part.first);
        }
    };
    if (reversing) materializeAnnotations();
    // Apply member updates, then native-owned membership changes,
    // then root metadata. Validate the complete atomic action only
    // after all domain edits are materialized.
    std::vector<aowis::model::ModelEntityEdit> membershipEdits;
    for (const aowis::model::ModelEntityEdit& edit : hydraulicEdits) {
        if (edit.entityType == QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType))
            continue;
        require(!edit.entityId.isNull() && !edit.ownerLayerId,
                "invalid hydraulic archive identity or annotation owner");
        if (edit.before && edit.after) applyHydraulicScalar(current, edit);
        else membershipEdits.push_back(edit);
    }
    applyHydraulicMembership(current, membershipEdits, hydraulicOwners);
    for (const aowis::model::ModelEntityEdit& edit : hydraulicEdits)
        if (edit.entityType == QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType))
            applyHydraulicRoot(current, edit);
    if (!reversing) materializeAnnotations();
}

// An unborn draft has no immutable parent. Reverse ONLY the actions that led
// to its saved cursor, retaining its unjournaled network/layer initialization.
// Deletions cannot be inverted without the original membership insertion
// index, so they fail closed rather than manufacturing a different seed.
static Records reconstructUnbornJournalSeed(const Records& saved,
        const std::vector<WorkingCopyEvent>& journal,
        std::size_t currentPosition, bool combined) {
    Records seed = saved;
    std::map<Key, Key> owners = knownOwners(seed, seed);
    HydraulicOwnership hydraulicOwners;
    if (combined) hydraulicOwners = knownHydraulicOwners(seed, seed);
    QSet<QUuid> transactionIds;
    for (const WorkingCopyEvent& frame : journal) {
        QString error;
        const std::optional<aowis::model::ModelEditTransaction> transaction =
            aowis::model::ModelEditTransactionCodec::decode(
                record::qByteArrayFromBytes(frame.encoded), &error);
        require(transaction && !transaction->edits.empty() &&
                transaction->transactionId.toString(QUuid::WithoutBraces).toStdString() == frame.id &&
                !transactionIds.contains(transaction->transactionId),
                "unborn annotation journal has an invalid or duplicate transaction");
        transactionIds.insert(transaction->transactionId);
    }
    for (std::size_t index = currentPosition; index > 0; --index) {
        QString error;
        std::optional<aowis::model::ModelEditTransaction> inverse =
            aowis::model::ModelEditTransactionCodec::decode(
                record::qByteArrayFromBytes(journal[index - 1].encoded), &error);
        require(inverse.has_value(), "cannot decode unborn annotation journal action");
        for (aowis::model::ModelEntityEdit& edit : inverse->edits) {
            require(!edit.before || edit.after,
                    "unborn annotation deletion needs the original membership order");
            std::swap(edit.before, edit.after);
        }
        std::reverse(inverse->edits.begin(), inverse->edits.end());
        bool seenAnnotations = false;
        bool seenHydraulics = false;
        std::set<Key> changedLayers;
        applyVerifiedActionEdits(seed, inverse->edits, combined, owners,
                                 hydraulicOwners, changedLayers,
                                 seenAnnotations, seenHydraulics, true);
    }
    return seed;
}

static std::optional<VerifiedAnnotationActionBoundary> replayActionBoundary(
        const MergeCandidate& parent, const MergeCandidate& committed,
        const std::vector<WorkingCopyEvent>& actions,
        const std::vector<ModelChange>& actualNativeChanges,
        const Hash& authenticatedActionChain,
        std::size_t actionPosition, bool combined, QString* error,
        std::optional<std::size_t> persistedBoundary = std::nullopt) {
    if (error) error->clear();
    const std::function<std::optional<VerifiedAnnotationActionBoundary>(const QString&)> fail =
        [error](const QString& reason) -> std::optional<VerifiedAnnotationActionBoundary> {
            if (error) *error = reason;
            return std::nullopt;
        };
    try {
        require(actions.size() <= maximumArchiveActions && actionPosition <= actions.size() &&
                (!persistedBoundary || *persistedBoundary <= actions.size()),
                "annotation archive action boundary is out of range");
        // Check the authenticated action order before accepting any boundary.
        // Transaction identities are not sequence numbers, even for UUIDv7.
        std::size_t chainedBytes = 0;
        for (const WorkingCopyEvent& action : actions) {
            require(!action.encoded.empty() &&
                    action.encoded.size() <= maximumArchiveBytes - chainedBytes,
                    "annotation archive exceeds verified byte budget");
            chainedBytes += action.encoded.size();
        }
        require(archiveActionChain(actions) == authenticatedActionChain,
                "annotation archive action order differs from authenticated Commit chain");
        const Records before = copyRecords(parent);
        const Records after = copyRecords(committed);
        validate(before);
        validate(after);
        std::map<Key, Key> owners = knownOwners(before, after);
        HydraulicOwnership hydraulicOwners;
        if (combined) hydraulicOwners = knownHydraulicOwners(before, after);
        const std::vector<ModelChange> nativeDelta = changesBetween(before, after);
        requireSameChanges(nativeDelta, actualNativeChanges);

        Records current = before;
        Records selected = before;
        Records persisted = before;
        std::set<Key> changedLayers;
        bool annotationSeen = false;
        bool hydraulicSeen = false;
        QSet<QUuid> transactionsSeen;
        std::size_t totalBytes = 0;
        for (std::size_t index = 0; index < actions.size(); ++index) {
            const WorkingCopyEvent& archived = actions[index];
            require(!archived.encoded.empty() &&
                    archived.encoded.size() <= maximumArchiveBytes - totalBytes,
                    "annotation archive exceeds verified byte budget");
            totalBytes += archived.encoded.size();
            QString decodeError;
            const std::optional<aowis::model::ModelEditTransaction> transaction =
                aowis::model::ModelEditTransactionCodec::decode(
                    record::qByteArrayFromBytes(archived.encoded), &decodeError);
            require(transaction.has_value() &&
                    !transactionsSeen.contains(transaction->transactionId) &&
                    transaction->transactionId.toString(QUuid::WithoutBraces).toStdString() == archived.id,
                    "invalid or duplicate archived annotation transaction identity");
            transactionsSeen.insert(transaction->transactionId);
            require(!transaction->edits.empty(), "empty archived annotation transaction");
            applyVerifiedActionEdits(current, transaction->edits, combined, owners,
                                     hydraulicOwners, changedLayers, annotationSeen,
                                     hydraulicSeen);
            if (index + 1 == actionPosition) selected = current;
            if (persistedBoundary && index + 1 == *persistedBoundary) persisted = current;
        }
        if (combined)
            require(annotationSeen && hydraulicSeen,
                    "combined history archive must contain both annotation and hydraulic edits");
        // Membership manifests, member bytes and unrelated native records
        // must ALL match the real immutable child, not just changed objects.
        const Records& verifiedPosition = persistedBoundary ? persisted : current;
        require(verifiedPosition.size() == after.size(),
                "annotation MODEL archive does not cover the committed native snapshot");
        for (const std::pair<const Key, ModelRecord>& entry : after) {
            const Records::const_iterator reproduced = verifiedPosition.find(entry.first);
            require(reproduced != verifiedPosition.end() && equalRecord(reproduced->second, entry.second),
                    "annotation MODEL archive differs from committed native records");
        }
        // The selected boundary is the only replayed state handed to a
        // caller. Position 0 is `before`; the proven position (journal tip,
        // or the persisted cursor of an active draft) equals `after`. Both
        // were validated above; every other boundary is validated here.
        const std::size_t provenPosition = persistedBoundary ? *persistedBoundary : actions.size();
        if (actionPosition != 0 && actionPosition != provenPosition)
            validate(selected);
        VerifiedAnnotationActionBoundary result;
        result.actionCount = actions.size();
        result.changesFromParent = changesBetween(before, selected);
        for (const Key& key : changedLayers)
            result.changedLayerIds.push_back(record::uuidFromKey(key));
        return result;
    } catch (const std::exception& failure) {
        return fail(QString::fromUtf8(failure.what()));
    }
}

std::optional<VerifiedAnnotationActionBoundary> replayVerifiedAnnotationActionBoundary(
        const MergeCandidate& parent, const MergeCandidate& committed,
        const std::vector<WorkingCopyEvent>& actions,
        const std::vector<ModelChange>& actualNativeChanges,
        const Hash& authenticatedActionChain,
        std::size_t actionPosition, QString* error) {
    return replayActionBoundary(parent, committed, actions, actualNativeChanges,
                                authenticatedActionChain, actionPosition, false, error);
}

std::optional<VerifiedAnnotationActionBoundary> replayVerifiedCombinedActionBoundary(
        const MergeCandidate& parent, const MergeCandidate& committed,
        const std::vector<WorkingCopyEvent>& actions,
        const std::vector<ModelChange>& actualNativeChanges,
        const Hash& authenticatedActionChain,
        std::size_t actionPosition, QString* error) {
    return replayActionBoundary(parent, committed, actions, actualNativeChanges,
                                authenticatedActionChain, actionPosition, true, error);
}

// First integration of standalone annotation proof with a REAL persisted
// local Commit. All state comes from pinned immutable revision IDs; archived
// forks are rejected until the mixed-domain ancestry planner can reconstruct
// their logical (non-first-parent) starting coordinate.
static std::optional<VerifiedIndexedAnnotationActionBoundary> verifyIndexedActionBoundary(
        ProjectService& service, std::string_view projectId, std::string_view branch,
        const RevisionId& revision, std::size_t actionPosition, bool combined, QString* error) {
    if (error) error->clear();
    const std::function<std::optional<VerifiedIndexedAnnotationActionBoundary>(const QString&)> fail =
        [error](const QString& reason) -> std::optional<VerifiedIndexedAnnotationActionBoundary> {
            if (error) *error = reason;
            return std::nullopt;
        };
    try {
        const RevisionNativeActionWitness witness =
            service.verifiedRevisionNativeActionWitness(projectId, branch, revision);
        if (!witness.parentRevision)
            return fail(QStringLiteral("Initial annotation Commit has no immutable parent checkpoint"));
        if (witness.historyForkRevision || witness.historyForkActionPosition ||
            witness.historyForkArchiveCommitOperationId)
            return fail(QStringLiteral("Forked annotation archive needs combined History replay"));
        if (witness.actions.empty() || actionPosition > witness.actions.size())
            return fail(QStringLiteral("Archived annotation action position is invalid"));
        // First-parent reachability alone is insufficient after History forks:
        // a discarded Redo suffix may remain in the immutable Commit archive.
        // Only a fully visible action path may be presented as verified.
        const RevisionActionPage active =
            service.revisionActionPage(projectId, branch, revision, 0, 1);
        if (!active.commitOperationId ||
            *active.commitOperationId != witness.commitOperationId ||
            active.total != witness.actions.size())
            return fail(QStringLiteral("Annotation archive is not completely on the active History path"));

        const std::vector<std::pair<Key, ModelRecord>> parentRecords =
            service.modelRecordsAtRevision(projectId, *witness.parentRevision);
        const std::vector<std::pair<Key, ModelRecord>> committedRecords =
            service.modelRecordsAtRevision(projectId, revision);
        Records before;
        Records after;
        for (const std::pair<Key, ModelRecord>& entry : parentRecords)
            require(before.emplace(entry.first, entry.second).second,
                    "duplicate MODEL record in immutable annotation parent");
        for (const std::pair<Key, ModelRecord>& entry : committedRecords)
            require(after.emplace(entry.first, entry.second).second,
                    "duplicate MODEL record in immutable annotation child");
        const std::optional<VerifiedAnnotationActionBoundary> boundary =
            replayActionBoundary(
                view(before), view(after), witness.actions, witness.actualNativeChanges,
                witness.authenticatedActionChain, actionPosition, combined, error);
        if (!boundary) return std::nullopt;
        return VerifiedIndexedAnnotationActionBoundary{
            witness.revisionId, witness.commitOperationId, *boundary};
    } catch (const std::exception& exception) {
        return fail(QString::fromUtf8(exception.what()));
    }
}


std::optional<VerifiedIndexedAnnotationActionBoundary> verifyIndexedAnnotationActionBoundary(
        ProjectService& service, std::string_view projectId, std::string_view branch,
        const RevisionId& revision, std::size_t actionPosition, QString* error) {
    return verifyIndexedActionBoundary(service, projectId, branch, revision,
                                       actionPosition, false, error);
}

std::optional<VerifiedIndexedAnnotationActionBoundary> verifyIndexedCombinedActionBoundary(
        ProjectService& service, std::string_view projectId, std::string_view branch,
        const RevisionId& revision, std::size_t actionPosition, QString* error) {
    return verifyIndexedActionBoundary(service, projectId, branch, revision,
                                       actionPosition, true, error);
}

static std::optional<WorkingCopyInfo> navigateVerifiedAction(
        ProjectService& service, std::string_view operationId,
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken, const RevisionId& expectedHead,
        const RevisionId& revision, std::size_t actionPosition,
        std::uint64_t expectedGeneration, bool combined, QString* error) {
    if (error) error->clear();
    const std::function<std::optional<WorkingCopyInfo>(const QString&)> fail =
        [error](const QString& message) -> std::optional<WorkingCopyInfo> {
            if (error) *error = message;
            return std::nullopt;
        };
    try {
        const WorkingCopyInfo draft = service.workingCopy(draftId, false);
        // Reconcile a lost response BEFORE re-planning: success already changed
        // the generation and the archived selection. Native publication proves
        // exact operation identity and source generation on the retry.
        if (draft.lastHistoryNavigationOperationId == operationId)
            return service.publishVerifiedHistoryActionNavigation(
                operationId, draftId, sessionId, fencingToken,
                expectedGeneration, expectedHead, revision,
                draft.historyArchiveCommitOperationId.value_or(""), actionPosition, {});
        if (draft.generation != expectedGeneration ||
            !draft.baseRevision || *draft.baseRevision != expectedHead)
            return fail(QStringLiteral("Annotation History draft generation or HEAD is stale"));
        const bool suspendedJournal = draft.journalCount > 0 &&
            draft.draftActionPosition == 0;
        if ((draft.journalCount != 0 && !suspendedJournal) ||
            (draft.dirty && !draft.historyCheckpointRevisionId) ||
            (suspendedJournal && !draft.historyCheckpointRevisionId &&
             !draft.candidateChanges.empty()))
            return fail(QStringLiteral("Annotation History requires a clean suspended draft or checkpoint"));
        const std::optional<RevisionGraphHead> branch =
            service.branchById(draft.projectId, draft.branchId);
        if (!branch || branch->revision != draft.baseRevision)
            return fail(QStringLiteral("Annotation History branch HEAD changed"));
        QString replayError;
        const std::optional<VerifiedIndexedAnnotationActionBoundary> selected =
            verifyIndexedActionBoundary(service, draft.projectId,
                branch->name, revision, actionPosition, combined, &replayError);
        if (!selected)
            return fail(QStringLiteral("Annotation History archive is not verified: ") + replayError);
        const RevisionNativeActionWitness witness =
            service.verifiedRevisionNativeActionWitness(draft.projectId, branch->name, revision);
        if (!witness.parentRevision ||
            witness.commitOperationId != selected->commitOperationId ||
            witness.actions.size() != selected->boundary.actionCount)
            return fail(QStringLiteral("Annotation History witness changed during planning"));
        const HistoryCheckpointPlan checkpoint = service.planHistoryCheckpoint(
            draftId, *witness.parentRevision, expectedGeneration);
        if (checkpoint.expectedHead != draft.baseRevision ||
            checkpoint.projectId != draft.projectId ||
            checkpoint.branchId != draft.branchId)
            return fail(QStringLiteral("Annotation History checkpoint became stale"));
        std::map<Key, std::optional<ModelRecord>> overlay;
        for (const ModelChange& change : checkpoint.candidateChanges)
            require(overlay.emplace(change.key, change.record).second,
                    "duplicate annotation checkpoint change key");
        // The checkpoint replaces HEAD with the immutable parent. Applying the
        // selected boundary delta to that parent constructs the exact desired
        // full model state, not a merge with a stale working-copy candidate.
        for (const ModelChange& change : selected->boundary.changesFromParent)
            overlay[change.key] = change.record;
        if (overlay.size() > 20000)
            return fail(QStringLiteral("Annotation History candidate exceeds draft record limit"));
        Records original;
        for (const std::pair<Key, ModelRecord>& entry :
             service.modelRecordsAtRevision(draft.projectId, expectedHead))
            require(original.emplace(entry.first, entry.second).second,
                    "duplicate annotation HEAD record");
        // Build the candidate from the pinned HEAD; compare it with the
        // independently replayed parent boundary before returning any writes.
        Records candidate = original;
        for (const std::pair<const Key, std::optional<ModelRecord>>& change : overlay) {
            if (change.second) candidate[change.first] = *change.second;
            else candidate.erase(change.first);
        }
        Records replayed;
        for (const std::pair<Key, ModelRecord>& entry :
             service.modelRecordsAtRevision(draft.projectId, *witness.parentRevision))
            require(replayed.emplace(entry.first, entry.second).second,
                    "duplicate annotation parent record");
        applyChanges(replayed, selected->boundary.changesFromParent);
        require(replayed.size() == candidate.size(),
                "annotation History checkpoint and replay differ in record count");
        for (const std::pair<const Key, ModelRecord>& entry : replayed) {
            const Records::const_iterator found = candidate.find(entry.first);
            require(found != candidate.end() && equalRecord(found->second, entry.second),
                    "annotation History checkpoint differs from verified archived state");
        }
        // `candidate` equals the replayed boundary record for record, and
        // replayActionBoundary validated that boundary (or proved it equal to
        // a validated immutable revision): no second complete validation.
        // Normalize redundant writes so returning to the archived tip produces
        // a clean draft when the revision is the current HEAD.
        for (std::map<Key, std::optional<ModelRecord>>::iterator item = overlay.begin();
             item != overlay.end();) {
            const Records::const_iterator prior = original.find(item->first);
            const bool equal = (!item->second && prior == original.end()) ||
                (item->second && prior != original.end() &&
                 equalRecord(*item->second, prior->second));
            if (equal) item = overlay.erase(item);
            else ++item;
        }
        // A concurrent writer can invalidate planning, never make it
        // executable. The native publisher checks these pins again under
        // draft/repository locks, including the current lease and archive ID.
        const WorkingCopyInfo rechecked = service.workingCopy(draftId, false);
        const std::optional<RevisionGraphHead> currentBranch =
            service.branchById(draft.projectId, draft.branchId);
        if (rechecked.generation != expectedGeneration ||
            rechecked.baseRevision != draft.baseRevision ||
            !currentBranch || currentBranch->revision != draft.baseRevision)
            return fail(QStringLiteral("Annotation History draft changed during planning"));
        std::vector<ModelChange> changes;
        changes.reserve(overlay.size());
        for (const std::pair<const Key, std::optional<ModelRecord>>& entry : overlay)
            changes.push_back(ModelChange{entry.first, entry.second});
        return service.publishVerifiedHistoryActionNavigation(
            operationId, draftId, sessionId, fencingToken, expectedGeneration,
            expectedHead, revision, selected->commitOperationId, actionPosition, changes);
    } catch (const std::exception& exception) {
        return fail(QString::fromUtf8(exception.what()));
    }
}

std::optional<WorkingCopyInfo> navigateVerifiedAnnotationAction(
        ProjectService& service, std::string_view operationId,
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken, const RevisionId& expectedHead,
        const RevisionId& revision, std::size_t actionPosition,
        std::uint64_t expectedGeneration, QString* error) {
    return navigateVerifiedAction(service, operationId, draftId, sessionId, fencingToken,
                                  expectedHead, revision, actionPosition,
                                  expectedGeneration, false, error);
}

std::optional<WorkingCopyInfo> navigateVerifiedCombinedAction(
        ProjectService& service, std::string_view operationId,
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken, const RevisionId& expectedHead,
        const RevisionId& revision, std::size_t actionPosition,
        std::uint64_t expectedGeneration, QString* error) {
    return navigateVerifiedAction(service, operationId, draftId, sessionId, fencingToken,
                                  expectedHead, revision, actionPosition,
                                  expectedGeneration, true, error);
}


// A retained working-copy journal is not an immutable Commit archive. Its
// fsync-pinned MODEL frames and the saved candidate nevertheless form a
// complete replay witness at the *persisted cursor*, which may be behind the
// journal tip after Undo. Check that cursor before offering another boundary.
// The native publisher supplies the actual lease, HEAD, generation and
// operation-ID fences; no native deltas are accepted from HTTP clients.
static std::optional<std::vector<ModelChange>> planVerifiedActiveAnnotationDraftAction(
        ProjectService& service, std::string_view draftId,
        std::optional<RevisionId> expectedHead, std::size_t actionPosition,
        std::uint64_t expectedGeneration, QString* error) {
    if (error) error->clear();
    try {
        const WorkingCopyInfo draft = service.workingCopy(draftId, true);
        require(draft.journal.size() == draft.journalCount &&
                draft.journalCount <= maximumArchiveActions &&
                actionPosition <= draft.journalCount,
                "active annotation journal is incomplete or boundary is invalid");
        bool hasAnnotation = false;
        bool hasHydraulic = false;
        for (const WorkingCopyEvent& event : draft.journal) {
            QString decodeError;
            const std::optional<aowis::model::ModelEditTransaction> decoded =
                aowis::model::ModelEditTransactionCodec::decode(
                    record::qByteArrayFromBytes(event.encoded), &decodeError);
            require(decoded && !decoded->edits.empty() &&
                    decoded->transactionId.toString(QUuid::WithoutBraces).toStdString() == event.id,
                    "active annotation journal contains invalid MODEL transaction");
            for (const aowis::model::ModelEntityEdit& edit : decoded->edits) {
                if (edit.entityType == QStringLiteral("map.annotation")) hasAnnotation = true;
                else hasHydraulic = true;
            }
        }
        require(hasAnnotation, "active journal has no annotation edits");
        require(draft.generation == expectedGeneration &&
                draft.baseRevision == expectedHead,
                "active annotation journal generation or committed base changed");
        require(!draft.historyCheckpointRevisionId && !draft.historyActionPosition &&
                !draft.historyArchiveCommitOperationId,
                "active annotation journal is still at an archived checkpoint");
        const std::optional<RevisionGraphHead> branch =
            service.branchById(draft.projectId, draft.branchId);
        require(branch && branch->revision == draft.baseRevision,
                "active annotation journal does not follow its branch HEAD");
        // Native overlays are always relative to physical HEAD. The logical
        // journal origin can instead be a VERIFIED archived action boundary
        // when the first new edit abandoned the old immutable Redo suffix.
        Records head;
        if (draft.baseRevision) {
            for (const std::pair<Key, ModelRecord>& entry :
                 service.modelRecordsAtRevision(draft.projectId, *draft.baseRevision))
                require(head.emplace(entry.first, entry.second).second,
                        "duplicate record in active History HEAD");
        }
        Records parent = head;
        const bool forked = draft.historyForkRevisionId.has_value() ||
            draft.historyForkActionPosition.has_value() ||
            draft.historyForkArchiveCommitOperationId.has_value();
        if (forked) {
            require(draft.baseRevision.has_value(),
                    "unborn annotation journal cannot fork immutable History");
            require(draft.historyForkRevisionId && draft.historyForkActionPosition &&
                    draft.historyForkArchiveCommitOperationId,
                    "active annotation History fork provenance is incomplete");
            const std::optional<RevisionId> sourceRevision =
                RevisionId::parse(*draft.historyForkRevisionId);
            require(sourceRevision.has_value(),
                    "active annotation History fork revision is invalid");
            QString sourceError;
            // The indexed verifier checks the authenticated action chain,
            // exact native delta, ancestry visibility and the entire source
            // revision. The combined form is needed when an archived source
            // action also changed hydraulic membership or root metadata.
            std::optional<VerifiedIndexedAnnotationActionBoundary> source =
                verifyIndexedAnnotationActionBoundary(service, draft.projectId,
                    branch->name, *sourceRevision,
                    static_cast<std::size_t>(*draft.historyForkActionPosition), &sourceError);
            if (!source) {
                source = verifyIndexedCombinedActionBoundary(service, draft.projectId,
                    branch->name, *sourceRevision,
                    static_cast<std::size_t>(*draft.historyForkActionPosition), &sourceError);
            }
            require(source && source->commitOperationId ==
                    *draft.historyForkArchiveCommitOperationId,
                    "active annotation History fork source is not an authenticated action boundary");
            const RevisionNativeActionWitness witness =
                service.verifiedRevisionNativeActionWitness(
                    draft.projectId, branch->name, *sourceRevision);
            require(witness.parentRevision.has_value() &&
                    witness.commitOperationId == source->commitOperationId &&
                    source->boundary.actionCount == witness.actions.size(),
                    "active annotation History fork source witness changed");
            parent.clear();
            for (const std::pair<Key, ModelRecord>& entry :
                 service.modelRecordsAtRevision(draft.projectId, *witness.parentRevision))
                require(parent.emplace(entry.first, entry.second).second,
                        "duplicate record in active History fork parent");
            applyChanges(parent, source->boundary.changesFromParent);
            // Validated as the replay's `before` snapshot below.
        }
        Records saved = head;
        std::set<Key> changedKeys;
        for (const ModelChange& change : draft.candidateChanges) {
            require(changedKeys.insert(change.key).second,
                    "duplicate native key in active annotation draft");
            if (change.record) saved[change.key] = *change.record;
            else saved.erase(change.key);
        }
        const std::size_t currentPosition = static_cast<std::size_t>(
            draft.draftActionPosition.value_or(draft.journalCount));
        require(currentPosition <= draft.journalCount,
                "invalid retained active annotation History position");
        if (!draft.baseRevision) {
            require(!draft.historyForkRevisionId && !draft.historyForkActionPosition &&
                    !draft.historyForkArchiveCommitOperationId,
                    "unborn annotation journal cannot have an archived fork");
            parent = reconstructUnbornJournalSeed(saved, draft.journal,
                                                   currentPosition, hasHydraulic);
        }
        const std::vector<ModelChange> persistedDelta = changesBetween(parent, saved);
        QString replayError;
        const std::optional<VerifiedAnnotationActionBoundary> selected =
            replayActionBoundary(view(parent), view(saved), draft.journal,
                persistedDelta, archiveActionChain(draft.journal),
                actionPosition, hasHydraulic, &replayError, currentPosition);
        if (!selected)
            throw std::runtime_error(
                (QStringLiteral("Active annotation History failed MODEL verification: ") +
                 replayError).toStdString());
        // `desired` is exactly the replay's selected boundary, which
        // replayActionBoundary validated or proved equal to a validated end.
        Records desired = parent;
        applyChanges(desired, selected->changesFromParent);
        // Publish an overlay relative to physical HEAD, not the logical
        // archived fork parent or the current Undo boundary. Otherwise
        // undoing the first fork action would resurrect abandoned Redo.
        const std::vector<ModelChange> changes = changesBetween(head, desired);
        require(changes.size() <= 20000,
                "active annotation History candidate exceeds draft change limit");
        const WorkingCopyInfo fresh = service.workingCopy(draftId, false);
        const std::optional<RevisionGraphHead> currentBranch =
            service.branchById(draft.projectId, draft.branchId);
        require(fresh.generation == expectedGeneration &&
                fresh.baseRevision == draft.baseRevision &&
                fresh.draftActionPosition == draft.draftActionPosition &&
                fresh.journalCount == draft.journalCount &&
                fresh.historyForkRevisionId == draft.historyForkRevisionId &&
                fresh.historyForkActionPosition == draft.historyForkActionPosition &&
                fresh.historyForkArchiveCommitOperationId == draft.historyForkArchiveCommitOperationId &&
                currentBranch && currentBranch->revision == draft.baseRevision,
                "active annotation draft changed during History verification");
        return changes;
    } catch (const std::exception& exception) {
        if (error) *error = QString::fromUtf8(exception.what());
        return std::nullopt;
    }
}

bool canNavigateVerifiedActiveAnnotationDraftAction(
        ProjectService& service, std::string_view draftId,
        std::optional<RevisionId> expectedHead, std::size_t actionPosition,
        std::uint64_t expectedGeneration, QString* error) {
    return planVerifiedActiveAnnotationDraftAction(service, draftId, expectedHead,
               actionPosition, expectedGeneration, error).has_value();
}

std::optional<WorkingCopyInfo> navigateVerifiedActiveDraftAction(
        ProjectService& service, std::string_view operationId,
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken, std::optional<RevisionId> expectedHead,
        std::size_t actionPosition, std::uint64_t expectedGeneration,
        QString* error) {
    if (error) error->clear();
    const std::function<std::optional<WorkingCopyInfo>(const QString&)> fail =
        [error](const QString& reason) -> std::optional<WorkingCopyInfo> {
            if (error) *error = reason;
            return std::nullopt;
        };
    try {
        const WorkingCopyInfo draft = service.workingCopy(draftId, true);
        // Retry the exact native operation before planning against the new
        // generation. The publisher itself checks argument identity.
        if (draft.lastHistoryNavigationOperationId == operationId)
            return service.publishVerifiedDraftActionNavigation(
                operationId, draftId, sessionId, fencingToken,
                expectedGeneration, expectedHead, actionPosition, {});
        bool containsAnnotation = false;
        for (const WorkingCopyEvent& frame : draft.journal) {
            QString decodeError;
            const std::optional<aowis::model::ModelEditTransaction> transaction =
                aowis::model::ModelEditTransactionCodec::decode(
                    record::qByteArrayFromBytes(frame.encoded), &decodeError);
            require(transaction && !transaction->edits.empty(),
                    "invalid active MODEL History transaction");
            for (const aowis::model::ModelEntityEdit& edit : transaction->edits)
                if (edit.entityType == QStringLiteral("map.annotation"))
                    containsAnnotation = true;
        }
        if (!containsAnnotation)
            return hydraulic::navigateVerifiedHydraulicDraftAction(
                service, operationId, draftId, sessionId, fencingToken,
                expectedHead, actionPosition, expectedGeneration, error);
        const std::optional<std::vector<ModelChange>> changes =
            planVerifiedActiveAnnotationDraftAction(service, draftId, expectedHead,
                actionPosition, expectedGeneration, error);
        if (!changes) return std::nullopt;
        return service.publishVerifiedDraftActionNavigation(
            operationId, draftId, sessionId, fencingToken, expectedGeneration,
            expectedHead, actionPosition, *changes);
    } catch (const std::exception& exception) {
        return fail(QString::fromUtf8(exception.what()));
    }
}

} // namespace aowis::dvmdb::annotations
