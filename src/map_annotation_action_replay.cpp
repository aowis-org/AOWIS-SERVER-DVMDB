#include "aowis/dvmdb/map_annotation_action_replay.h"

#include "aowis/dvmdb/map_annotation_persistence.h"
#include "aowis/dvmdb/hydraulic_distribution_network_persistence.h"
#include "aowis/dvmdb/map_annotation_snapshot_validation.h"
#include "aowis/dvmdb/model_record_conversion.h"
#include "aowis/dvmdb/engineering_delta_scope.h"
#include "aowis/dvmdb/storage_error.h"

#include <aowis/model/edit/model_edit_transaction.h>
#include <aowis/model/edit/hydraulic_distribution_root_edit.h>

#include <QByteArray>
#include <QSet>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace aowis::dvmdb::annotations {
namespace {

// History replay works on changes (A2): the replayed state is a small native
// overlay on top of a pinned, already validated revision, and every lookup is
// a point read or a reverse-reference query. No revision is copied whole.
using Records = std::map<Key, ModelRecord>;
using View = EngineeringCandidateView;
constexpr std::size_t maximumArchiveActions = 4096;
constexpr std::size_t maximumArchiveBytes = 8U * 1024U * 1024U;
// Drafts hold at most this many native changes; History overlays and the
// revision differences it reads are bounded the same way.
constexpr std::size_t maximumHistoryChanges = 20000;

bool equalRecord(const ModelRecord& first, const ModelRecord& second) {
    return first.kind == second.kind && first.payload == second.payload &&
           first.references == second.references;
}

bool sameRecord(const std::optional<ModelRecord>& first, const std::optional<ModelRecord>& second) {
    return first.has_value() == second.has_value() && (!first || equalRecord(*first, *second));
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

// Complete in-memory candidate with an exact reverse index. Used only for
// caller-supplied complete candidates and for unborn drafts, which have no
// stored revision and are bounded by the draft change limit.
View recordsView(const std::shared_ptr<const Records>& records) {
    const std::shared_ptr<std::map<Key, std::vector<Key>>> index =
        std::make_shared<std::map<Key, std::vector<Key>>>();
    for (const std::pair<const Key, ModelRecord>& entry : *records)
        for (const Key& reference : entry.second.references)
            (*index)[reference].push_back(entry.first); // keys visited in order
    View result;
    result.get = [records](const Key& key) -> std::optional<ModelRecord> {
        const Records::const_iterator found = records->find(key);
        return found != records->end() ? std::optional<ModelRecord>(found->second)
                                       : std::nullopt;
    };
    result.referrers = [index](const Key& key) -> std::vector<Key> {
        const std::map<Key, std::vector<Key>>::const_iterator found = index->find(key);
        return found != index->end() ? found->second : std::vector<Key>{};
    };
    return result;
}

View pinnedView(const PinnedModelReader& reader) {
    View result;
    result.get = [reader](const Key& key) { return reader.get(key); };
    result.referrers = [reader](const Key& key) { return reader.referrers(key); };
    return result;
}

View layeredView(const View& base, const std::vector<ModelChange>& changes) {
    return overlayEngineeringCandidateView(base.get, base.referrers, {changes});
}

// A base candidate plus the native changes made on top of it. Copies share
// the base and copy only the (small) overlay.
class State final {
public:
    explicit State(View base) : base_(std::make_shared<const View>(std::move(base))) {}

    std::optional<ModelRecord> get(const Key& key) const {
        const std::map<Key, std::optional<ModelRecord>>::const_iterator found =
            this->overlay_.find(key);
        if (found != this->overlay_.end()) return found->second;
        return this->base_->get(key);
    }
    // Valid while this State is alive and not moved.
    std::function<std::optional<ModelRecord>(const Key&)> reader() const {
        return [this](const Key& key) { return this->get(key); };
    }
    void put(const Key& key, const ModelRecord& record) { this->overlay_[key] = record; }
    void apply(const std::vector<ModelChange>& changes) {
        for (const ModelChange& change : changes) this->overlay_[change.key] = change.record;
    }
    // Changes from the base in key order, without entries equal to the base.
    std::vector<ModelChange> changes() const {
        std::vector<ModelChange> result;
        for (const std::pair<const Key, std::optional<ModelRecord>>& entry : this->overlay_)
            if (!sameRecord(entry.second, this->base_->get(entry.first)))
                result.push_back(ModelChange{entry.first, entry.second});
        return result;
    }
    std::set<Key> touchedKeys() const {
        std::set<Key> keys;
        for (const std::pair<const Key, std::optional<ModelRecord>>& entry : this->overlay_)
            keys.insert(entry.first);
        return keys;
    }
    // Complete candidate view (point reads and exact referrers).
    View view() const { return layeredView(*this->base_, this->changes()); }
    const View& base() const { return *this->base_; }

private:
    std::shared_ptr<const View> base_;
    std::map<Key, std::optional<ModelRecord>> overlay_;
};

// Records of `to` that differ from `from`; both States must share one base.
std::vector<ModelChange> difference(const State& from, const State& to) {
    std::set<Key> keys = from.touchedKeys();
    const std::set<Key> more = to.touchedKeys();
    keys.insert(more.begin(), more.end());
    std::vector<ModelChange> result;
    for (const Key& key : keys) {
        const std::optional<ModelRecord> after = to.get(key);
        if (!sameRecord(from.get(key), after)) result.push_back(ModelChange{key, after});
    }
    return result;
}

Records copyRecords(const MergeCandidate& candidate) {
    Records result;
    candidate.forEach([&result](const Key& key, const ModelRecord& record) {
        require(result.emplace(key, record).second, "duplicate native model key in replay candidate");
    });
    return result;
}

std::vector<Key> networkRootsOf(const Records& records) {
    std::vector<Key> roots;
    for (const std::pair<const Key, ModelRecord>& entry : records)
        if (entry.second.kind == engineeringNetworkKind) roots.push_back(entry.first);
    return roots;
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

void applyChanges(Records& records, const std::vector<ModelChange>& changes) {
    for (const ModelChange& change : changes) {
        if (change.record) records[change.key] = *change.record;
        else records.erase(change.key);
    }
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

std::uint16_t collectionForKind(std::uint16_t kind) {
    for (std::size_t collection = 0;; ++collection) {
        std::uint16_t candidate = 0;
        try {
            candidate = hydraulic::hydraulicMemberKindForCollection(collection);
        } catch (const StorageError&) {
            break;
        }
        if (candidate == kind) return static_cast<std::uint16_t>(collection);
    }
    throw std::runtime_error("unknown hydraulic member kind in History endpoint");
}

// The layer owning `member` in `view` (member -> chunk -> layer), if any.
std::optional<Key> annotationOwnerIn(const View& view, const Key& member) {
    std::optional<Key> owner;
    for (const Key& chunk : engineeringReferrersOfKind(view, member, annotationChunkKind)) {
        for (const Key& layer : engineeringReferrersOfKind(view, chunk, annotationLayerKind)) {
            require(!owner || *owner == layer,
                    "annotation belongs to several layers in a History endpoint");
            owner = layer;
        }
    }
    return owner;
}

// The network owning hydraulic member `member` in `view` (member -> chunk ->
// root), if the member exists there.
std::optional<HydraulicOwner> hydraulicOwnerIn(const View& view, const Key& member) {
    const std::optional<ModelRecord> record = view.get(member);
    if (!record || !isHydraulicMemberKind(record->kind)) return std::nullopt;
    std::optional<HydraulicOwner> owner;
    for (const Key& chunk : engineeringReferrersOfKind(view, member, engineeringNetworkChunkKind)) {
        for (const Key& root : engineeringReferrersOfKind(view, chunk, engineeringNetworkKind)) {
            require(!owner || owner->network == root,
                    "historical entity belongs to multiple networks");
            owner = HydraulicOwner{root, collectionForKind(record->kind), record->kind};
        }
    }
    require(owner.has_value(), "hydraulic member without network membership in a History endpoint");
    return owner;
}

// Ownership of the archive's entities, looked up on demand in the two
// immutable endpoints through the reverse-reference index.
struct Attribution final {
    View parent;
    View committed;
    std::vector<Key> roots; // network roots, the same in both endpoints
    std::map<Key, Key> annotationOwners;
    std::map<Key, HydraulicOwner> hydraulicOwners;
};

// The canonical edit frame authenticates ownerLayerId; endpoint membership
// additionally proves that a surviving member did not change its layer.
void claimAnnotationOwner(Attribution& attribution, const Key& entity, const Key& claimed) {
    const std::map<Key, Key>::const_iterator known = attribution.annotationOwners.find(entity);
    if (known != attribution.annotationOwners.end()) {
        require(known->second == claimed, "annotation ownership changed within archived history");
        return;
    }
    const std::optional<Key> before = annotationOwnerIn(attribution.parent, entity);
    const std::optional<Key> after = annotationOwnerIn(attribution.committed, entity);
    require(!before || !after || *before == *after,
            "annotation ownership changed between historical endpoints");
    const std::optional<Key> endpoint = before ? before : after;
    require(!endpoint || *endpoint == claimed, "annotation ownership changed within archived history");
    attribution.annotationOwners.emplace(entity, claimed);
}

HydraulicOwner hydraulicOwnerOf(Attribution& attribution, const Key& member,
                                std::uint16_t collection, std::uint16_t kind) {
    const std::map<Key, HydraulicOwner>::const_iterator known =
        attribution.hydraulicOwners.find(member);
    if (known != attribution.hydraulicOwners.end()) return known->second;
    const std::optional<HydraulicOwner> before = hydraulicOwnerIn(attribution.parent, member);
    const std::optional<HydraulicOwner> after = hydraulicOwnerIn(attribution.committed, member);
    require(!before || !after ||
            (before->network == after->network && before->collection == after->collection &&
             before->kind == after->kind),
            "hydraulic member ownership changed across archived endpoints");
    HydraulicOwner selected;
    if (before || after) {
        selected = before ? *before : *after;
    } else {
        require(attribution.roots.size() == 1,
                "transient hydraulic member lacks authenticated network provenance");
        selected = HydraulicOwner{attribution.roots.front(), collection, kind};
    }
    attribution.hydraulicOwners.emplace(member, selected);
    return selected;
}

void applyHydraulicScalar(State& current, const aowis::model::ModelEntityEdit& edit) {
    require(edit.before && edit.after,
            "mixed hydraulic creation/deletion must use native membership materializer");
    const hydraulic::CanonicalHydraulicMemberImage previous =
        hydraulic::canonicalHydraulicMemberImage(edit, *edit.before);
    const hydraulic::CanonicalHydraulicMemberImage next =
        hydraulic::canonicalHydraulicMemberImage(edit, *edit.after);
    require(previous.collection == next.collection &&
            previous.record.kind == next.record.kind,
            "hydraulic entity cannot change collection in-place");
    const Key member = record::keyFromUuid(edit.entityId);
    const std::optional<ModelRecord> stored = current.get(member);
    require(stored && equalRecord(*stored, previous.record),
            "stale or noncanonical mixed hydraulic before-image/reference index");
    current.put(member, next.record);
}

// Use the same indexed chunk/manifest materializer as an ordinary HTTP Save.
// The original MODEL event's edit order provides insertion order. If an HTTP
// request originally used a different membership order, immutable child-byte
// equality will reject the archive rather than guessing an ordering.
void applyHydraulicMembership(State& current,
        const std::vector<aowis::model::ModelEntityEdit>& edits,
        Attribution& attribution) {
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
        const std::optional<ModelRecord> stored = current.get(member);
        if (edit.before)
            require(stored && equalRecord(*stored, decoded.record),
                    "stale hydraulic membership removal before-image");
        else
            require(!stored, "hydraulic membership creation collides with existing record");
        const HydraulicOwner selected =
            hydraulicOwnerOf(attribution, member, decoded.collection, decoded.record.kind);
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
        hydraulic::materializeDraftMembership(current.reader(), part.second.intents,
                                             part.second.changes);
        current.apply(part.second.changes);
    }
}

void applyHydraulicRoot(State& current, const aowis::model::ModelEntityEdit& edit) {
    require(!edit.entityId.isNull() && !edit.ownerLayerId,
            "mixed hydraulic root has invalid identity or annotation ownership");
    const ModelChange change = hydraulic::materializeDraftRootEdit(current.reader(), edit);
    require(change.record.has_value(), "hydraulic root metadata edit removed its manifest");
    current.put(change.key, *change.record);
}

// What replay needs about the immutable starting point.
struct ReplayInput final {
    // Complete candidate the actions start from. It already passed the
    // complete validator (a published revision or a draft saved through the
    // Save validators); replay does not re-check it.
    View parent;
    // Exact native changes from `parent` to the proven end state.
    std::vector<ModelChange> nativeDelta;
    // Network roots of `parent` (combined replay refuses root creation).
    std::vector<Key> roots;
    // Complete validation of parent + changes, needed only when the Save
    // delta validator asks for it (root creation/deletion, generic records).
    // Empty: History refuses such a boundary instead of scanning the project.
    std::function<std::optional<std::string>(const std::vector<ModelChange>&)> completeValidation;
};

} // namespace

// Shared canonical single-action materializer, used both for forward replay
// and for reversing an unborn journal back to its unjournaled initialization
// seed. Each action is materialized with the same per-domain checks as a Save
// (before-images, membership, reference index). No complete snapshot
// validation runs per action (A2); see replayActionBoundary.
static void applyVerifiedActionEdits(State& current,
        const QVector<aowis::model::ModelEntityEdit>& edits, bool combined,
        Attribution& attribution, std::set<Key>& changedLayers,
        bool& annotationSeen, bool& hydraulicSeen, bool reversing = false) {
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
        claimAnnotationOwner(attribution, entityId, claimedLayer);
        byLayer[claimedLayer].push_back(edit);
    }
    // When reversing an action, remove/update annotations BEFORE removing
    // their hydraulic anchors. The forward direction creates anchors first.
    const std::function<void()> materializeAnnotations = [&]() {
        for (const std::pair<const Key, std::vector<aowis::model::ModelEntityEdit>>& part : byLayer) {
            const QUuid layerId = record::uuidFromKey(part.first);
            const std::vector<ModelChange> delta =
                materializeDraftAnnotationEdits(current.reader(), layerId, part.second);
            current.apply(delta);
            changedLayers.insert(part.first);
        }
    };
    if (reversing) materializeAnnotations();
    // Apply member updates, then native-owned membership changes,
    // then root metadata.
    std::vector<aowis::model::ModelEntityEdit> membershipEdits;
    for (const aowis::model::ModelEntityEdit& edit : hydraulicEdits) {
        if (edit.entityType == QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType))
            continue;
        require(!edit.entityId.isNull() && !edit.ownerLayerId,
                "invalid hydraulic archive identity or annotation owner");
        if (edit.before && edit.after) applyHydraulicScalar(current, edit);
        else membershipEdits.push_back(edit);
    }
    applyHydraulicMembership(current, membershipEdits, attribution);
    for (const aowis::model::ModelEntityEdit& edit : hydraulicEdits)
        if (edit.entityType == QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType))
            applyHydraulicRoot(current, edit);
    if (!reversing) materializeAnnotations();
}

// An unborn draft has no immutable parent. Reverse ONLY the actions that led
// to its saved cursor, retaining its unjournaled network/layer initialization.
// Deletions cannot be inverted without the original membership insertion
// index, so they fail closed rather than manufacturing a different seed.
// Unborn drafts have no stored revision: everything is in the draft overlay,
// which the draft change limit bounds, so this works on complete records.
static Records reconstructUnbornJournalSeed(const Records& saved,
        const std::vector<WorkingCopyEvent>& journal,
        std::size_t currentPosition, bool combined) {
    const std::shared_ptr<const Records> savedRecords = std::make_shared<const Records>(saved);
    const View savedView = recordsView(savedRecords);
    State seed(savedView);
    Attribution attribution{savedView, savedView, networkRootsOf(saved), {}, {}};
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
        applyVerifiedActionEdits(seed, inverse->edits, combined, attribution,
                                 changedLayers, seenAnnotations, seenHydraulics, true);
    }
    Records result = saved;
    applyChanges(result, seed.changes());
    return result;
}

// Replays `actions` from `input.parent` and proves that the proven position
// (the archive tip, or the persisted cursor of an active draft) reproduces
// `input.nativeDelta` exactly: member, chunk, root and every other native
// record. Only the selected boundary is returned; it is checked with the
// Save delta validator against the (validated) parent unless it is position
// 0 or the proven position, which equal validated states.
static std::optional<VerifiedAnnotationActionBoundary> replayActionBoundary(
        const ReplayInput& input,
        const std::vector<WorkingCopyEvent>& actions,
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
        std::set<Key> deltaKeys;
        for (const ModelChange& change : input.nativeDelta)
            require(deltaKeys.insert(change.key).second, "duplicate native witness delta key");
        State committed(input.parent);
        committed.apply(input.nativeDelta);
        const std::vector<ModelChange> expected = committed.changes();
        if (combined) {
            for (const ModelChange& change : expected) {
                const std::optional<ModelRecord> before = input.parent.get(change.key);
                const bool wasRoot = before && before->kind == engineeringNetworkKind;
                const bool isRoot = change.record && change.record->kind == engineeringNetworkKind;
                require(wasRoot == isRoot,
                        "mixed history cannot replay hydraulic network root creation/deletion");
            }
        }
        Attribution attribution{input.parent, layeredView(input.parent, expected),
                                input.roots, {}, {}};

        State current(input.parent);
        State selected = current;
        State persisted = current;
        std::set<Key> changedLayers;
        bool annotationSeen = false;
        bool hydraulicSeen = false;
        QSet<QUuid> transactionsSeen;
        for (std::size_t index = 0; index < actions.size(); ++index) {
            const WorkingCopyEvent& archived = actions[index];
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
            applyVerifiedActionEdits(current, transaction->edits, combined, attribution,
                                     changedLayers, annotationSeen, hydraulicSeen);
            if (index + 1 == actionPosition) selected = current;
            if (persistedBoundary && index + 1 == *persistedBoundary) persisted = current;
        }
        // Multi-domain mode covers hydraulic-only, annotation-only and mixed
        // archives alike; annotation-only mode refuses any hydraulic edit.
        (void)annotationSeen;
        (void)hydraulicSeen;
        // Membership manifests, member bytes and unrelated native records
        // must ALL match the proven end state, not just changed objects.
        const std::vector<ModelChange> reproduced =
            (persistedBoundary ? persisted : current).changes();
        require(reproduced.size() == expected.size(),
                "History MODEL archive does not cover the committed native snapshot (unjournaled native change)");
        for (std::size_t index = 0; index < expected.size(); ++index)
            require(reproduced[index].key == expected[index].key &&
                    sameRecord(reproduced[index].record, expected[index].record),
                    "History MODEL archive differs from the committed native records (unjournaled native change)");
        const std::vector<ModelChange> selectedChanges = selected.changes();
        const std::size_t provenPosition = persistedBoundary ? *persistedBoundary : actions.size();
        if (actionPosition != 0 && actionPosition != provenPosition) {
            const View after = layeredView(input.parent, selectedChanges);
            const std::function<std::optional<std::string>()> complete =
                [&input, &selectedChanges]() -> std::optional<std::string> {
                    if (!input.completeValidation)
                        return std::string("History boundary needs a complete project check, "
                                           "which History does not run");
                    return input.completeValidation(selectedChanges);
                };
            const std::optional<std::string> problem =
                validateAnnotationSnapshotDelta(input.parent, after, selectedChanges, complete);
            if (problem)
                throw std::runtime_error("invalid annotation replay snapshot: " + *problem);
        }
        VerifiedAnnotationActionBoundary result;
        result.actionCount = actions.size();
        result.changesFromParent = selectedChanges;
        for (const Key& key : changedLayers)
            result.changedLayerIds.push_back(record::uuidFromKey(key));
        return result;
    } catch (const std::exception& failure) {
        rethrowIfNotVerificationFailure(); // A3: environment errors keep their status
        return fail(QString::fromUtf8(failure.what()));
    }
}

// Complete caller-supplied candidates: both are validated completely and the
// supplied native delta must equal their full difference.
static std::optional<VerifiedAnnotationActionBoundary> replayCompleteCandidates(
        const MergeCandidate& parent, const MergeCandidate& committed,
        const std::vector<WorkingCopyEvent>& actions,
        const std::vector<ModelChange>& actualNativeChanges,
        const Hash& authenticatedActionChain,
        std::size_t actionPosition, bool combined, QString* error) {
    if (error) error->clear();
    try {
        const std::shared_ptr<const Records> before =
            std::make_shared<const Records>(copyRecords(parent));
        const Records after = copyRecords(committed);
        validate(*before);
        validate(after);
        requireSameChanges(changesBetween(*before, after), actualNativeChanges);
        ReplayInput input;
        input.parent = recordsView(before);
        input.nativeDelta = actualNativeChanges;
        input.roots = networkRootsOf(*before);
        input.completeValidation = [before](const std::vector<ModelChange>& changes) {
            Records candidate = *before;
            applyChanges(candidate, changes);
            return validateAnnotationSnapshot(view(candidate));
        };
        return replayActionBoundary(input, actions, authenticatedActionChain,
                                    actionPosition, combined, error);
    } catch (const std::exception& failure) {
        rethrowIfNotVerificationFailure();
        if (error) *error = QString::fromUtf8(failure.what());
        return std::nullopt;
    }
}

std::optional<VerifiedAnnotationActionBoundary> replayVerifiedAnnotationActionBoundary(
        const MergeCandidate& parent, const MergeCandidate& committed,
        const std::vector<WorkingCopyEvent>& actions,
        const std::vector<ModelChange>& actualNativeChanges,
        const Hash& authenticatedActionChain,
        std::size_t actionPosition, QString* error) {
    return replayCompleteCandidates(parent, committed, actions, actualNativeChanges,
                                    authenticatedActionChain, actionPosition, false, error);
}

std::optional<VerifiedAnnotationActionBoundary> replayVerifiedCombinedActionBoundary(
        const MergeCandidate& parent, const MergeCandidate& committed,
        const std::vector<WorkingCopyEvent>& actions,
        const std::vector<ModelChange>& actualNativeChanges,
        const Hash& authenticatedActionChain,
        std::size_t actionPosition, QString* error) {
    return replayCompleteCandidates(parent, committed, actions, actualNativeChanges,
                                    authenticatedActionChain, actionPosition, true, error);
}

// Replay of a REAL persisted local Commit. All state comes from pinned
// immutable revisions and the authenticated Commit witness. An archive that
// started at an older History position (a fork) starts from that verified
// source boundary instead of the first parent; the source is verified the
// same way, and it must lie on the first-parent ancestry of this revision.
static std::optional<VerifiedIndexedAnnotationActionBoundary> verifyIndexedActionBoundary(
        ProjectService& service, std::string_view projectId, std::string_view branch,
        const RevisionId& revision, std::size_t actionPosition, bool combined, QString* error,
        std::size_t forkDepth = 0) {
    // A fork's source is an older Commit whose Redo suffix was abandoned by
    // the fork, so it is no longer completely on the visible History path.
    // It is still authenticated (witness, action chain, first-parent
    // ancestry checked by the caller) and replayed completely.
    const bool forkSource = forkDepth > 0;
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
            return fail(QStringLiteral("Initial Commit has no immutable parent checkpoint"));
        if (witness.actions.empty() || actionPosition > witness.actions.size())
            return fail(QStringLiteral("Archived action position is invalid"));
        // First-parent reachability alone is insufficient after History forks:
        // a discarded Redo suffix may remain in the immutable Commit archive.
        // Only a fully visible action path may be presented as verified.
        if (!forkSource) {
            const RevisionActionPage active =
                service.revisionActionPage(projectId, branch, revision, 0, 1);
            if (!active.commitOperationId ||
                *active.commitOperationId != witness.commitOperationId ||
                active.total != witness.actions.size())
                return fail(QStringLiteral("Archive is not completely on the active History path"));
        }

        ReplayInput input;
        input.roots = witness.hydraulicDistributionRootKeys;
        RevisionId originRevision = *witness.parentRevision;
        std::vector<ModelChange> originChanges;
        const bool forked = witness.historyForkRevision || witness.historyForkActionPosition ||
                            witness.historyForkArchiveCommitOperationId;
        if (!forked) {
            // The witness is the store's exact revision difference; the
            // parent is read point by point.
            input.parent = pinnedView(service.pinModelRevision(projectId, *witness.parentRevision));
            input.nativeDelta = witness.actualNativeChanges;
        } else {
            if (!witness.historyForkRevision || !witness.historyForkActionPosition ||
                !witness.historyForkArchiveCommitOperationId)
                return fail(QStringLiteral("Archived History fork provenance is incomplete"));
            if (forkDepth >= 64)
                return fail(QStringLiteral("Archived History forks are nested too deeply"));
            // The fork source must be a first-parent ancestor: otherwise an
            // invalid sidecar could create cycles in recursive replay.
            std::optional<RevisionId> ancestor = witness.parentRevision;
            bool onAncestry = false;
            for (std::size_t depth = 0; ancestor && depth < 65536; ++depth) {
                if (*ancestor == *witness.historyForkRevision) {
                    onAncestry = true;
                    break;
                }
                const RevisionGraphRevision prior = service.revision(projectId, *ancestor);
                if (prior.parents.size() > 1) break;
                ancestor = prior.parents.empty()
                    ? std::nullopt : std::optional<RevisionId>(prior.parents.front().id);
            }
            if (!onAncestry)
                return fail(QStringLiteral("Archived History fork does not follow immutable ancestry"));
            QString sourceError;
            const std::optional<VerifiedIndexedAnnotationActionBoundary> source =
                verifyIndexedActionBoundary(service, projectId, branch, *witness.historyForkRevision,
                    static_cast<std::size_t>(*witness.historyForkActionPosition), true,
                    &sourceError, forkDepth + 1);
            if (!source || source->commitOperationId != *witness.historyForkArchiveCommitOperationId ||
                !source->originRevision)
                return fail(QStringLiteral("Archived History fork source is not verified: ") + sourceError);
            // Everything over the source's starting revision: the archive
            // starts at source origin + source boundary, and ends at this
            // revision (origin revision + the store's revision difference).
            originRevision = *source->originRevision;
            State origin(pinnedView(service.pinModelRevision(projectId, originRevision)));
            origin.apply(source->originChanges);
            origin.apply(source->boundary.changesFromParent);
            State committed(origin.base());
            committed.apply(service.modelChangesBetweenRevisions(
                projectId, originRevision, revision, maximumHistoryChanges));
            originChanges = origin.changes();
            input.parent = origin.view();
            input.nativeDelta = difference(origin, committed);
        }
        const std::optional<VerifiedAnnotationActionBoundary> boundary =
            replayActionBoundary(input, witness.actions, witness.authenticatedActionChain,
                                 actionPosition, combined, error);
        if (!boundary) return std::nullopt;
        VerifiedIndexedAnnotationActionBoundary result;
        result.revisionId = witness.revisionId;
        result.commitOperationId = witness.commitOperationId;
        result.boundary = *boundary;
        result.originRevision = originRevision;
        result.originChanges = std::move(originChanges);
        return result;
    } catch (const std::exception& exception) {
        rethrowIfNotVerificationFailure(); // A3: environment errors keep their status
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

struct ArchivedActionPlan final {
    std::string commitOperationId;
    std::vector<ModelChange> changes; // relative to HEAD
};

// Read-only plan of an archived action boundary as a HEAD-relative overlay.
static std::optional<ArchivedActionPlan> planArchivedAction(
        ProjectService& service, std::string_view draftId, const RevisionId& expectedHead,
        const RevisionId& revision, std::size_t actionPosition,
        std::uint64_t expectedGeneration, bool combined, QString* error) {
    if (error) error->clear();
    const std::function<std::optional<ArchivedActionPlan>(const QString&)> failPlan =
        [error](const QString& message) -> std::optional<ArchivedActionPlan> {
            if (error) *error = message;
            return std::nullopt;
        };
    try {
        const WorkingCopyInfo draft = service.workingCopy(draftId, false);
        if (draft.generation != expectedGeneration ||
            !draft.baseRevision || *draft.baseRevision != expectedHead)
            return failPlan(QStringLiteral("History draft generation or HEAD is stale"));
        const bool suspendedJournal = draft.journalCount > 0 &&
            draft.draftActionPosition == 0;
        if ((draft.journalCount != 0 && !suspendedJournal) ||
            (draft.dirty && !draft.historyCheckpointRevisionId) ||
            (suspendedJournal && !draft.historyCheckpointRevisionId &&
             !draft.candidateChanges.empty()))
            return failPlan(QStringLiteral("History requires a clean suspended draft or checkpoint"));
        const std::optional<RevisionGraphHead> branch =
            service.branchById(draft.projectId, draft.branchId);
        if (!branch || branch->revision != draft.baseRevision)
            return failPlan(QStringLiteral("History branch HEAD changed"));
        QString replayError;
        const std::optional<VerifiedIndexedAnnotationActionBoundary> selected =
            verifyIndexedActionBoundary(service, draft.projectId,
                branch->name, revision, actionPosition, combined, &replayError);
        if (!selected)
            return failPlan(QStringLiteral("History archive is not verified: ") + replayError);
        const RevisionNativeActionWitness witness =
            service.verifiedRevisionNativeActionWitness(draft.projectId, branch->name, revision);
        if (!witness.parentRevision ||
            witness.commitOperationId != selected->commitOperationId ||
            witness.actions.size() != selected->boundary.actionCount)
            return failPlan(QStringLiteral("History witness changed during planning"));
        if (!selected->originRevision)
            return failPlan(QStringLiteral("History archive has no verified starting state"));
        const HistoryCheckpointPlan checkpoint = service.planHistoryCheckpoint(
            draftId, *selected->originRevision, expectedGeneration);
        if (checkpoint.expectedHead != draft.baseRevision ||
            checkpoint.projectId != draft.projectId ||
            checkpoint.branchId != draft.branchId)
            return failPlan(QStringLiteral("History checkpoint became stale"));
        // The checkpoint plan is the store's exact difference from HEAD to the
        // archive's starting revision, built by the service (never by a
        // client). HEAD + checkpoint is that revision; the verified origin
        // changes (a fork's source boundary) and the boundary delta on top give
        // exactly the replayed boundary state.
        // Entries equal to HEAD are dropped, so returning to the archived tip
        // produces a clean draft when the revision is the current HEAD.
        std::set<Key> checkpointKeys;
        for (const ModelChange& change : checkpoint.candidateChanges)
            require(checkpointKeys.insert(change.key).second,
                    "duplicate annotation checkpoint change key");
        State candidate(pinnedView(service.pinModelRevision(draft.projectId, expectedHead)));
        candidate.apply(checkpoint.candidateChanges);
        candidate.apply(selected->originChanges);
        candidate.apply(selected->boundary.changesFromParent);
        const std::vector<ModelChange> changes = candidate.changes();
        if (changes.size() > maximumHistoryChanges)
            return failPlan(QStringLiteral("History candidate exceeds draft record limit"));
        // A concurrent writer can invalidate planning, never make it
        // executable. The native publisher checks these pins again under
        // draft/repository locks, including the current lease and archive ID.
        const WorkingCopyInfo rechecked = service.workingCopy(draftId, false);
        const std::optional<RevisionGraphHead> currentBranch =
            service.branchById(draft.projectId, draft.branchId);
        if (rechecked.generation != expectedGeneration ||
            rechecked.baseRevision != draft.baseRevision ||
            !currentBranch || currentBranch->revision != draft.baseRevision)
            return failPlan(QStringLiteral("History draft changed during planning"));
        return ArchivedActionPlan{selected->commitOperationId, changes};
    } catch (const std::exception& exception) {
        rethrowIfNotVerificationFailure(); // A3: environment errors keep their status
        return failPlan(QString::fromUtf8(exception.what()));
    }
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
        const std::optional<ArchivedActionPlan> plan = planArchivedAction(service, draftId,
            expectedHead, revision, actionPosition, expectedGeneration, combined, error);
        if (!plan) return std::nullopt;
        return service.publishVerifiedHistoryActionNavigation(
            operationId, draftId, sessionId, fencingToken, expectedGeneration,
            expectedHead, revision, plan->commitOperationId, actionPosition, plan->changes);
    } catch (const std::exception& exception) {
        rethrowIfNotVerificationFailure(); // A3: environment errors keep their status
        return fail(QString::fromUtf8(exception.what()));
    }
}

std::optional<std::vector<ModelChange>> planVerifiedArchivedAction(
        ProjectService& service, std::string_view draftId, const RevisionId& expectedHead,
        const RevisionId& revision, std::size_t actionPosition,
        std::uint64_t expectedGeneration, QString* error) {
    const std::optional<ArchivedActionPlan> plan = planArchivedAction(service, draftId,
        expectedHead, revision, actionPosition, expectedGeneration, true, error);
    if (!plan) return std::nullopt;
    return plan->changes;
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
                if (edit.entityType != QStringLiteral("map.annotation")) hasHydraulic = true;
            }
        }
        require(draft.generation == expectedGeneration &&
                draft.baseRevision == expectedHead,
                "active annotation journal generation or committed base changed");
        // While an archived position is shown, the journal is suspended at
        // position 0 and the saved draft holds the archived state, not a
        // journal state. Returning into the journal then starts from HEAD.
        const bool atArchivedCheckpoint = draft.historyCheckpointRevisionId.has_value();
        require(!atArchivedCheckpoint ||
                (draft.historyActionPosition && draft.historyArchiveCommitOperationId &&
                 draft.draftActionPosition == 0 && draft.baseRevision),
                "active journal is at an incomplete archived checkpoint");
        const std::optional<RevisionGraphHead> branch =
            service.branchById(draft.projectId, draft.branchId);
        require(branch && branch->revision == draft.baseRevision,
                "active annotation journal does not follow its branch HEAD");
        // Native overlays are always relative to physical HEAD. The logical
        // journal origin can instead be a VERIFIED archived action boundary
        // when the first new edit abandoned the old immutable Redo suffix.
        std::set<Key> candidateKeys;
        for (const ModelChange& change : draft.candidateChanges)
            require(candidateKeys.insert(change.key).second,
                    "duplicate native key in active annotation draft");
        const std::size_t currentPosition = static_cast<std::size_t>(
            draft.draftActionPosition.value_or(draft.journalCount));
        require(currentPosition <= draft.journalCount,
                "invalid retained active annotation History position");
        const bool forked = draft.historyForkRevisionId.has_value() ||
            draft.historyForkActionPosition.has_value() ||
            draft.historyForkArchiveCommitOperationId.has_value();
        ReplayInput input;
        // Turns the selected boundary into the overlay relative to HEAD.
        std::function<std::vector<ModelChange>(const std::vector<ModelChange>&)> relativeToHead;
        if (!draft.baseRevision) {
            require(!forked, "unborn annotation journal cannot have an archived fork");
            // No stored revision: the draft overlay is the whole model, and it
            // is bounded by the draft change limit.
            Records saved;
            for (const ModelChange& change : draft.candidateChanges)
                if (change.record) saved.emplace(change.key, *change.record);
            const std::shared_ptr<const Records> seed = std::make_shared<const Records>(
                reconstructUnbornJournalSeed(saved, draft.journal, currentPosition, hasHydraulic));
            // The reconstructed seed is not a stored, validated state.
            validate(*seed);
            validate(saved);
            input.parent = recordsView(seed);
            input.nativeDelta = changesBetween(*seed, saved);
            input.roots = networkRootsOf(*seed);
            input.completeValidation = [seed](const std::vector<ModelChange>& changes) {
                Records candidate = *seed;
                applyChanges(candidate, changes);
                return validateAnnotationSnapshot(view(candidate));
            };
            relativeToHead = [seed](const std::vector<ModelChange>& boundary) {
                Records desired = *seed;
                applyChanges(desired, boundary);
                return changesBetween(Records{}, desired);
            };
        } else if (!forked) {
            // The journal starts at HEAD, and the saved draft is HEAD + its
            // overlay (validated at each Save).
            const View head = pinnedView(service.pinModelRevision(draft.projectId, *draft.baseRevision));
            State saved(head);
            if (!atArchivedCheckpoint) saved.apply(draft.candidateChanges);
            input.parent = head;
            input.nativeDelta = saved.changes();
            input.roots = service.hydraulicDistributionRootKeysAtRevision(
                draft.projectId, *draft.baseRevision);
            relativeToHead = [](const std::vector<ModelChange>& boundary) { return boundary; };
        } else {
            require(draft.historyForkRevisionId && draft.historyForkActionPosition &&
                    draft.historyForkArchiveCommitOperationId,
                    "active annotation History fork provenance is incomplete");
            const std::optional<RevisionId> sourceRevision =
                RevisionId::parse(*draft.historyForkRevisionId);
            require(sourceRevision.has_value(),
                    "active annotation History fork revision is invalid");
            QString sourceError;
            // The indexed verifier checks the authenticated action chain,
            // exact native delta and ancestry visibility of the source, in
            // the multi-domain mode (annotation, hydraulic or both).
            const std::optional<VerifiedIndexedAnnotationActionBoundary> source =
                verifyIndexedCombinedActionBoundary(service, draft.projectId,
                    branch->name, *sourceRevision,
                    static_cast<std::size_t>(*draft.historyForkActionPosition), &sourceError);
            require(source && source->commitOperationId ==
                    *draft.historyForkArchiveCommitOperationId && source->originRevision,
                    "active History fork source is not an authenticated action boundary");
            // Everything is expressed over the source's starting revision:
            // the journal origin is that revision + the verified source
            // origin and boundary, HEAD is that revision + the store's
            // revision difference, and the saved draft is HEAD + its overlay.
            const RevisionId& sourceStart = *source->originRevision;
            const View forkParent =
                pinnedView(service.pinModelRevision(draft.projectId, sourceStart));
            const std::vector<ModelChange> headFromForkParent =
                service.modelChangesBetweenRevisions(draft.projectId, sourceStart,
                                                     *draft.baseRevision, maximumHistoryChanges);
            State origin(forkParent);
            origin.apply(source->originChanges);
            origin.apply(source->boundary.changesFromParent);
            State head(forkParent);
            head.apply(headFromForkParent);
            State saved = head;
            saved.apply(draft.candidateChanges);
            input.parent = origin.view();
            // At an archived checkpoint the suspended journal is at position
            // 0, i.e. at its origin.
            input.nativeDelta = atArchivedCheckpoint ? std::vector<ModelChange>{}
                                                     : difference(origin, saved);
            // The source replay refused network root creation, so the origin
            // has the parent's roots.
            input.roots = service.hydraulicDistributionRootKeysAtRevision(
                draft.projectId, *draft.baseRevision);
            relativeToHead = [origin, head](const std::vector<ModelChange>& boundary) {
                State desired = origin;
                desired.apply(boundary);
                return difference(head, desired);
            };
        }
        QString replayError;
        const std::optional<VerifiedAnnotationActionBoundary> selected =
            replayActionBoundary(input, draft.journal, archiveActionChain(draft.journal),
                actionPosition, hasHydraulic, &replayError, currentPosition);
        if (!selected)
            throw std::runtime_error(
                (QStringLiteral("Active annotation History failed MODEL verification: ") +
                 replayError).toStdString());
        // Publish an overlay relative to physical HEAD, not the logical
        // archived fork parent or the current Undo boundary. Otherwise
        // undoing the first fork action would resurrect abandoned Redo.
        const std::vector<ModelChange> changes = relativeToHead(selected->changesFromParent);
        require(changes.size() <= maximumHistoryChanges,
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
        rethrowIfNotVerificationFailure(); // A3: environment errors keep their status
        if (error) *error = QString::fromUtf8(exception.what());
        return std::nullopt;
    }
}

std::optional<std::vector<ModelChange>> planVerifiedActiveDraftAction(
        ProjectService& service, std::string_view draftId,
        std::optional<RevisionId> expectedHead, std::size_t actionPosition,
        std::uint64_t expectedGeneration, QString* error) {
    return planVerifiedActiveAnnotationDraftAction(service, draftId, expectedHead,
               actionPosition, expectedGeneration, error);
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
        // Annotation, hydraulic and mixed journals all replay on changes
        // through the same planner (the Save materializers and validators).
        const std::optional<std::vector<ModelChange>> changes =
            planVerifiedActiveAnnotationDraftAction(service, draftId, expectedHead,
                actionPosition, expectedGeneration, error);
        if (!changes) return std::nullopt;
        return service.publishVerifiedDraftActionNavigation(
            operationId, draftId, sessionId, fencingToken, expectedGeneration,
            expectedHead, actionPosition, *changes);
    } catch (const std::exception& exception) {
        rethrowIfNotVerificationFailure(); // A3: environment errors keep their status
        return fail(QString::fromUtf8(exception.what()));
    }
}

} // namespace aowis::dvmdb::annotations
