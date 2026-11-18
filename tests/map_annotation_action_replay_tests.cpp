#include "aowis/dvmdb/map_annotation_action_replay.h"
#include "aowis/dvmdb/map_annotation_persistence.h"
#include "aowis/dvmdb/hydraulic_distribution_network_persistence.h"
#include "aowis/dvmdb/model_codec.h"
#include "aowis/dvmdb/model_record_conversion.h"

#include <aowis/model/edit/model_edit_transaction.h>

#include <QCoreApplication>
#include <QList>

#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace db = aowis::dvmdb;
namespace ann = aowis::dvmdb::annotations;
namespace hydraulic = aowis::dvmdb::hydraulic;
namespace model = aowis::model;
using Records = std::map<db::Key, db::ModelRecord>;

void ensure(bool condition, const char* error) {
    if (!condition) throw std::runtime_error(error);
}

QUuid uuid(std::uint32_t ordinal) {
    db::Key value{};
    value[0] = 0x45;
    value[6] = 0x70;
    value[8] = 0x80;
    for (unsigned index = 0; index < 4; ++index)
        value[15 - index] = static_cast<std::uint8_t>(ordinal >> (index * 8U));
    return db::record::uuidFromKey(value);
}

db::MergeCandidate view(const Records& records) {
    db::MergeCandidate candidate;
    candidate.get = [&records](const db::Key& key) -> std::optional<db::ModelRecord> {
        const Records::const_iterator found = records.find(key);
        return found == records.end() ? std::nullopt : std::optional<db::ModelRecord>(found->second);
    };
    candidate.forEach = [&records](
            const std::function<void(const db::Key&, const db::ModelRecord&)>& consumer) {
        for (const std::pair<const db::Key, db::ModelRecord>& entry : records)
            consumer(entry.first, entry.second);
    };
    return candidate;
}

void applyChanges(Records& records, const std::vector<db::ModelChange>& changes) {
    for (const db::ModelChange& change : changes) {
        if (change.record) records[change.key] = *change.record;
        else records.erase(change.key);
    }
}

bool equalRecord(const db::ModelRecord& first, const db::ModelRecord& second) {
    return first.kind == second.kind && first.payload == second.payload &&
           first.references == second.references;
}

std::vector<db::ModelChange> difference(const Records& parent, const Records& committed) {
    std::vector<db::ModelChange> result;
    for (const std::pair<const db::Key, db::ModelRecord>& entry : committed) {
        const Records::const_iterator old = parent.find(entry.first);
        if (old == parent.end() || !equalRecord(old->second, entry.second))
            result.push_back({entry.first, entry.second});
    }
    for (const std::pair<const db::Key, db::ModelRecord>& entry : parent)
        if (!committed.contains(entry.first)) result.push_back({entry.first, std::nullopt});
    return result;
}

MapAnnotation annotation(std::uint32_t ordinal) {
    MapAnnotation value;
    value.uuid = uuid(1000 + ordinal);
    value.text = QStringLiteral("Annotation %1").arg(static_cast<int>(ordinal));
    value.position_wgs84.latitude_deg = 14.1;
    value.position_wgs84.longitude_deg = 32.2;
    return value;
}

MapAnnotationLayer layer(std::uint32_t layerOrdinal = 2, std::uint32_t networkOrdinal = 1) {
    MapAnnotationLayer result;
    result.uuid = uuid(layerOrdinal);
    result.domain = MapAnnotationDomain::Distribution;
    result.owner_network_uuid = uuid(networkOrdinal);
    return result;
}

Records makeParent(bool withMember = true) {
    Records records;
    applyChanges(records, {hydraulic::emptyDraftHydraulicDistributionNetworkRoot(
        uuid(1), QStringLiteral("Distribution"))});
    applyChanges(records, ann::fullDraftAnnotationLayerChanges(
        layer(), withMember ? QList<MapAnnotation>{annotation(1)} : QList<MapAnnotation>{}));
    return records;
}

model::ModelEntityEdit create(const MapAnnotation& value) {
    model::ModelEntityEdit edit;
    edit.entityType = QStringLiteral("map.annotation");
    edit.entityId = value.uuid;
    edit.after = db::ModelCodec<MapAnnotation>::encode(value);
    return edit;
}

model::ModelEntityEdit update(const MapAnnotation& previous, const MapAnnotation& current) {
    model::ModelEntityEdit edit = create(current);
    edit.before = db::ModelCodec<MapAnnotation>::encode(previous);
    return edit;
}

model::ModelEntityEdit remove(const MapAnnotation& previous) {
    model::ModelEntityEdit edit = create(previous);
    edit.before = edit.after;
    edit.after.reset();
    return edit;
}

db::WorkingCopyEvent event(std::uint32_t ordinal,
                           const std::vector<model::ModelEntityEdit>& edits) {
    model::ModelEditTransaction transaction;
    transaction.transactionId = uuid(4000 + ordinal);
    transaction.operation = QStringLiteral("map.annotation.edit");
    transaction.description = QStringLiteral("Test action");
    for (const model::ModelEntityEdit& edit : edits)
        transaction.edits.append(edit);
    const QByteArray encoded = model::ModelEditTransactionCodec::encode(transaction);
    ensure(!encoded.isEmpty(), "could not encode MODEL annotation transaction");
    return {transaction.transactionId.toString(QUuid::WithoutBraces).toStdString(),
            db::record::bytesFromQByteArray(encoded)};
}

void applyEvent(Records& records, const QUuid& layerId,
                const std::vector<model::ModelEntityEdit>& edits) {
    applyChanges(records, ann::materializeDraftAnnotationEdits(view(records).get, layerId, edits));
}

// Simulated trusted local-Commit sidecar chain. The production witness comes
// from ProjectService after it validates the immutable action-chain field.
db::Hash trustedActionChain(const std::vector<db::WorkingCopyEvent>& actions) {
    db::Hash chained{};
    for (const db::WorkingCopyEvent& action : actions) {
        db::Bytes frame(chained.begin(), chained.end());
        frame.insert(frame.end(), action.encoded.begin(), action.encoded.end());
        chained = db::sha256(frame);
    }
    return chained;
}

std::optional<ann::VerifiedAnnotationActionBoundary> replay(
        const Records& parent, const Records& committed,
        const std::vector<db::WorkingCopyEvent>& actions,
        std::size_t boundary, QString* error = nullptr,
        std::optional<db::Hash> authenticatedChain = std::nullopt) {
    const db::Hash witness = authenticatedChain.value_or(trustedActionChain(actions));
    return ann::replayVerifiedAnnotationActionBoundary(
        view(parent), view(committed), actions, difference(parent, committed),
        witness, boundary, error);
}

void mustReject(const Records& parent, const Records& committed,
                const std::vector<db::WorkingCopyEvent>& actions,
                const char* explanation,
                std::optional<db::Hash> authenticatedChain = std::nullopt) {
    QString diagnostic;
    const std::optional<ann::VerifiedAnnotationActionBoundary> proof =
        replay(parent, committed, actions, actions.size(), &diagnostic,
               authenticatedChain);
    if (proof || diagnostic.isEmpty()) throw std::runtime_error(explanation);
}

void testReplayAndBoundary() {
    const Records parent = makeParent();
    Records child = parent;
    MapAnnotation changed = annotation(1);
    changed.text = QStringLiteral("Moved annotation description");
    applyEvent(child, layer().uuid, {update(annotation(1), changed)});
    applyEvent(child, layer().uuid, {create(annotation(2))});
    const std::vector<db::WorkingCopyEvent> actions = {
        event(1, {update(annotation(1), changed)}), event(2, {create(annotation(2))})};

    const std::optional<ann::VerifiedAnnotationActionBoundary> before =
        replay(parent, child, actions, 0);
    ensure(before && before->changesFromParent.empty() && before->actionCount == 2,
           "zero boundary must preserve exact immutable parent state");
    const std::optional<ann::VerifiedAnnotationActionBoundary> middle =
        replay(parent, child, actions, 1);
    ensure(middle && middle->changesFromParent.size() == 1 &&
           middle->changesFromParent.front().key == db::record::keyFromUuid(changed.uuid),
           "middle action boundary changed annotation membership unexpectedly");
    const std::optional<ann::VerifiedAnnotationActionBoundary> final =
        replay(parent, child, actions, 2);
    ensure(final && final->changedLayerIds.size() == 1 &&
           final->changedLayerIds.front() == layer().uuid &&
           final->changesFromParent.size() == difference(parent, child).size(),
           "final action boundary does not cover native annotation membership");

    mustReject(parent, child, {actions.front()},
               "missing archived member creation was accepted");
    // These two actions commute and still produce the exact final snapshot.
    // Only the authenticated Commit chain can detect their swapped order.
    mustReject(parent, child, {actions.back(), actions.front()},
               "reordered archived event sequence was accepted",
               trustedActionChain(actions));
    db::Hash forgedChain = trustedActionChain(actions);
    forgedChain.front() ^= 0x01;
    mustReject(parent, child, actions,
               "incorrect authenticated Commit action chain was accepted",
               forgedChain);
    mustReject(parent, child, {actions.front(), actions.front()},
               "duplicate transaction identity was accepted");
    QString diagnostic;
    ensure(!replay(parent, child, actions, 3, &diagnostic) && !diagnostic.isEmpty(),
           "out-of-range archived action boundary was accepted");
}

void testDeletionAndUnjournaledChange() {
    const Records parent = makeParent();
    Records removed = parent;
    applyEvent(removed, layer().uuid, {remove(annotation(1))});
    const std::vector<db::WorkingCopyEvent> deletion = {
        event(20, {remove(annotation(1))})};
    const std::optional<ann::VerifiedAnnotationActionBoundary> verified =
        replay(parent, removed, deletion, 1);
    ensure(verified && verified->changesFromParent.size() >= 2,
           "verified deletion must cover member tombstone and membership changes");

    Records edited = parent;
    MapAnnotation changed = annotation(1);
    changed.text = QStringLiteral("Unjournaled annotation edit");
    applyEvent(edited, layer().uuid, {update(annotation(1), changed)});
    mustReject(parent, edited, {},
               "valid native annotation update without MODEL journal was accepted");
}

void testDeleteAndReinsert() {
    const Records parent = makeParent();
    Records child = parent;
    applyEvent(child, layer().uuid, {remove(annotation(1))});
    applyEvent(child, layer().uuid, {create(annotation(1))});
    const std::vector<db::WorkingCopyEvent> actions = {
        event(3, {remove(annotation(1))}), event(4, {create(annotation(1))})};
    const std::optional<ann::VerifiedAnnotationActionBoundary> middle =
        replay(parent, child, actions, 1);
    ensure(middle && !middle->changesFromParent.empty(),
           "deletion action did not produce a historical boundary");
    const std::optional<ann::VerifiedAnnotationActionBoundary> final =
        replay(parent, child, actions, 2);
    ensure(final && final->changesFromParent.empty(),
           "delete/reinsert changed immutable native membership unexpectedly");
}

void testFailClosedProvenance() {
    const Records parent = makeParent(false);
    Records child = parent;
    // An intermediate-only annotation cannot be attributed to a specific
    // layer from immutable endpoints. Do not guess, even for one network.
    const std::vector<db::WorkingCopyEvent> transient = {
        event(5, {create(annotation(3))}), event(6, {remove(annotation(3))})};
    mustReject(parent, child, transient,
               "created-then-deleted annotation without owner provenance was accepted");

    const std::vector<db::WorkingCopyEvent> stale = {
        event(7, {update(annotation(1), annotation(2))})};
    mustReject(parent, child, stale, "stale or unowned annotation update was accepted");

    // Foreign kind changes cannot hide behind a legitimate annotation event.
    Records invalidChild = child;
    db::ModelRecord invalidRoot = invalidChild.at(db::record::keyFromUuid(uuid(1)));
    invalidRoot.payload.push_back(0);
    invalidChild[db::record::keyFromUuid(uuid(1))] = invalidRoot;
    mustReject(parent, invalidChild, {}, "native hydraulic change without MODEL action was accepted");

    db::WorkingCopyEvent corrupt = event(8, {create(annotation(4))});
    corrupt.id = uuid(4999).toString(QUuid::WithoutBraces).toStdString();
    mustReject(parent, child, {corrupt}, "mismatched archived transaction ID was accepted");

    model::ModelEntityEdit foreign = create(annotation(4));
    foreign.entityType = QStringLiteral("HydraulicNodeJunction");
    mustReject(parent, child, {event(9, {foreign})},
               "mixed or foreign entity type was silently skipped");
}

void testNativeWitnessMismatch() {
    const Records parent = makeParent();
    Records child = parent;
    MapAnnotation changed = annotation(1);
    changed.text = QStringLiteral("Updated");
    applyEvent(child, layer().uuid, {update(annotation(1), changed)});
    const std::vector<db::WorkingCopyEvent> actions = {
        event(10, {update(annotation(1), changed)})};
    std::vector<db::ModelChange> forged = difference(parent, child);
    forged.front().record = std::nullopt;
    QString diagnostic;
    ensure(!ann::replayVerifiedAnnotationActionBoundary(
               view(parent), view(child), actions, forged, trustedActionChain(actions),
               1, &diagnostic) &&
           !diagnostic.isEmpty(),
           "forged exact-native-delta witness was accepted");
}

void testMultipleLayers() {
    Records parent = makeParent();
    applyChanges(parent, {hydraulic::emptyDraftHydraulicDistributionNetworkRoot(
        uuid(10), QStringLiteral("Second network"))});
    MapAnnotation second = annotation(10);
    applyChanges(parent, ann::fullDraftAnnotationLayerChanges(
        layer(20, 10), {second}));
    Records child = parent;
    MapAnnotation firstChanged = annotation(1);
    firstChanged.text = QStringLiteral("First network changed");
    MapAnnotation secondChanged = second;
    secondChanged.text = QStringLiteral("Second network changed");
    applyEvent(child, layer().uuid, {update(annotation(1), firstChanged)});
    applyEvent(child, layer(20, 10).uuid, {update(second, secondChanged)});
    const std::vector<db::WorkingCopyEvent> actions = {
        event(11, {update(annotation(1), firstChanged), update(second, secondChanged)})};
    const std::optional<ann::VerifiedAnnotationActionBoundary> verified =
        replay(parent, child, actions, 1);
    ensure(verified && verified->changedLayerIds.size() == 2 &&
           verified->changesFromParent.size() == 2,
           "two-network annotation MODEL event was not partitioned by layer");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        testReplayAndBoundary();
        testDeleteAndReinsert();
        testDeletionAndUnjournaledChange();
        testFailClosedProvenance();
        testNativeWitnessMismatch();
        testMultipleLayers();
        std::cout << "B4.3 annotation-only archive replay proofs passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "B4.3 annotation archive replay test failure: " << error.what() << '\n';
        return 1;
    }
}
