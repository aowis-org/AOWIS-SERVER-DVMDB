#include "aowis/dvmdb/map_annotation_snapshot_validation.h"
#include "aowis/dvmdb/map_annotation_persistence.h"
#include "aowis/dvmdb/hydraulic_distribution_network_persistence.h"
#include "aowis/dvmdb/model_record_conversion.h"

#include <QCoreApplication>
#include <QString>

#include <cstdint>
#include <iostream>
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace db = aowis::dvmdb;
namespace ann = aowis::dvmdb::annotations;
namespace hydraulic = aowis::dvmdb::hydraulic;

using Records = std::map<db::Key, db::ModelRecord>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

QUuid uuid(std::uint32_t ordinal) {
    db::Key bytes{};
    bytes[0] = 0x44;
    bytes[6] = 0x70;
    bytes[8] = 0x80;
    for (unsigned index = 0; index < 4; ++index)
        bytes[15 - index] = static_cast<std::uint8_t>(ordinal >> (index * 8U));
    return db::record::uuidFromKey(bytes);
}

db::Key key(const QUuid& value) { return db::record::keyFromUuid(value); }

void append(Records& output, const std::vector<db::ModelChange>& changes) {
    for (const db::ModelChange& change : changes) {
        require(change.record.has_value(), "test builder emitted deletion");
        require(output.emplace(change.key, *change.record).second,
                "test builder emitted duplicate record UUID");
    }
}

void put(Records& output, const db::ModelChange& change) {
    require(change.record.has_value(), "test builder emitted deletion");
    require(output.emplace(change.key, *change.record).second,
            "test builder emitted duplicate record UUID");
}

MapAnnotationLayer distributionLayer(std::uint32_t layer, std::uint32_t owner) {
    MapAnnotationLayer result;
    result.uuid = uuid(layer);
    result.domain = MapAnnotationDomain::Distribution;
    result.owner_network_uuid = uuid(owner);
    return result;
}

MapAnnotationLayer drainageLayer(std::uint32_t layer) {
    MapAnnotationLayer result;
    result.uuid = uuid(layer);
    result.domain = MapAnnotationDomain::Drainage;
    return result;
}

MapAnnotation annotation(std::uint32_t ordinal) {
    MapAnnotation result;
    result.uuid = uuid(1000 + ordinal);
    result.text = QStringLiteral("Integrity test annotation");
    result.position_wgs84.longitude_deg = 14.0;
    result.position_wgs84.latitude_deg = 11.0;
    return result;
}

std::vector<db::ModelChange> networkChanges(std::uint32_t ordinal,
                                            std::optional<std::uint32_t> junctionOrdinal = std::nullopt) {
    if (!junctionOrdinal)
        return {hydraulic::emptyDraftHydraulicDistributionNetworkRoot(
            uuid(ordinal), QStringLiteral("Annotation test network"))};
    NetworkHydraulicDistribution network;
    network.uuid = uuid(ordinal);
    network.id = QStringLiteral("N%1").arg(static_cast<int>(ordinal));
    HydraulicNodeJunction junction;
    junction.uuid = uuid(*junctionOrdinal);
    junction.id = QStringLiteral("J%1").arg(static_cast<int>(*junctionOrdinal));
    junction.coordinate_wgs84.longitude_deg = 14.0;
    junction.coordinate_wgs84.latitude_deg = 11.0;
    network.nodes_junctions.append(junction);
    return hydraulic::fullDraftHydraulicDistributionNetworkChanges(network);
}

Records basic() {
    Records model;
    append(model, networkChanges(1));
    append(model, ann::fullDraftAnnotationLayerChanges(distributionLayer(2, 1), {annotation(1)}));
    return model;
}

std::optional<std::string> validate(const Records& records) {
    db::MergeCandidate candidate;
    candidate.get = [&records](const db::Key& id) -> std::optional<db::ModelRecord> {
        const Records::const_iterator found = records.find(id);
        return found == records.end() ? std::nullopt : std::optional<db::ModelRecord>(found->second);
    };
    candidate.forEach = [&records](
        const std::function<void(const db::Key&, const db::ModelRecord&)>& visitor) {
        for (const std::pair<const db::Key, db::ModelRecord>& entry : records)
            visitor(entry.first, entry.second);
    };
    return ann::validateAnnotationSnapshot(candidate);
}

void valid(const Records& records, const char* message) {
    const std::optional<std::string> diagnostic = validate(records);
    if (diagnostic) {
        std::cerr << *diagnostic << '\n';
        throw std::runtime_error(message);
    }
}

void invalid(const Records& records, const char* message) {
    require(validate(records).has_value(), message);
}

void testValidScopes() {
    valid({}, "empty project must be permitted");
    valid(basic(), "a distribution root and its layer must be permitted");
    Records multi = basic();
    append(multi, networkChanges(3));
    append(multi, ann::fullDraftAnnotationLayerChanges(distributionLayer(4, 3), {}));
    append(multi, ann::fullDraftAnnotationLayerChanges(drainageLayer(5), {annotation(5)}));
    valid(multi, "two network-owned layers and one drainage layer must be permitted");
    // Explicit support for an annotation-only, pre-network Drainage project.
    Records drainageOnly;
    append(drainageOnly, ann::fullDraftAnnotationLayerChanges(drainageLayer(5), {}));
    valid(drainageOnly, "pre-network drainage annotations must not need a hydraulic root");
}

void testLayerCardinalityAndOwnership() {
    Records missing;
    append(missing, networkChanges(1));
    invalid(missing, "distribution root without annotation layer must be rejected");
    Records duplicate = basic();
    append(duplicate, ann::fullDraftAnnotationLayerChanges(distributionLayer(6, 1), {}));
    invalid(duplicate, "two layers on one network must be rejected");
    Records missingOwner;
    append(missingOwner, ann::fullDraftAnnotationLayerChanges(distributionLayer(2, 1), {}));
    invalid(missingOwner, "annotation layer with absent network owner must be rejected");
    Records doubleDrainage = basic();
    append(doubleDrainage, ann::fullDraftAnnotationLayerChanges(drainageLayer(5), {}));
    append(doubleDrainage, ann::fullDraftAnnotationLayerChanges(drainageLayer(6), {}));
    invalid(doubleDrainage, "multiple project-domain drainage layers must be rejected");
}

void testOrphanRecords() {
    const Records initial = basic();
    const std::vector<db::ModelChange> surplus =
        ann::fullDraftAnnotationLayerChanges(distributionLayer(9, 1), {annotation(99)});
    for (const std::uint16_t kind : {ann::annotationMemberKind, ann::annotationChunkKind}) {
        Records candidate = initial;
        for (const db::ModelChange& change : surplus)
            if (change.record && change.record->kind == kind) put(candidate, change);
        invalid(candidate, "unowned annotation member/chunk must be rejected");
    }
    Records orphanHydraulic = initial;
    db::ModelRecord rogue;
    rogue.kind = 101;
    rogue.payload = {1, 2, 3};
    require(orphanHydraulic.emplace(key(uuid(501)), rogue).second, "bad test key");
    invalid(orphanHydraulic, "unowned hydraulic membership chunk must be rejected");
    Records wrongKind = initial;
    wrongKind.at(key(annotation(1).uuid)).kind = 217;
    invalid(wrongKind, "annotation membership with wrong record kind must be rejected");
    Records unknown = initial;
    db::ModelRecord unsupported;
    unsupported.kind = 250;
    require(unknown.emplace(key(uuid(600)), unsupported).second, "bad test key");
    invalid(unknown, "unrecognized record kinds must fail closed");
}

void testCrossLayerReferences() {
    Records shared = basic();
    // Reusing an annotation UUID in a second layer must not silently transfer
    // it. Duplicate native keys are not allowed, so simulate the second
    // layer's chunk referring to a member owned by the first layer.
    append(shared, networkChanges(3));
    const std::vector<db::ModelChange> second =
        ann::fullDraftAnnotationLayerChanges(distributionLayer(4, 3), {annotation(1)});
    for (const db::ModelChange& entry : second)
        if (entry.record && entry.record->kind != ann::annotationMemberKind)
            put(shared, entry);
    invalid(shared, "shared annotation member between layers must be rejected");
}

void testTypedAnchors() {
    Records good;
    append(good, networkChanges(1, 200));
    MapAnnotation first = annotation(1);
    MapAnnotationAnchor anchor;
    anchor.entity_uuid = uuid(200);
    anchor.entity_type = QStringLiteral("HydraulicNodeJunction");
    first.anchor = anchor;
    append(good, ann::fullDraftAnnotationLayerChanges(distributionLayer(2, 1), {first}));
    valid(good, "annotation anchored to its owning network junction must be accepted");

    Records wrongType;
    append(wrongType, networkChanges(1, 200));
    first.anchor->entity_type = QStringLiteral("HydraulicNodeTank");
    append(wrongType, ann::fullDraftAnnotationLayerChanges(distributionLayer(2, 1), {first}));
    invalid(wrongType, "anchor with wrong canonical MODEL entity type must be rejected");

    Records foreign;
    append(foreign, networkChanges(1, 200));
    append(foreign, networkChanges(3, 201));
    first.anchor->entity_type = QStringLiteral("HydraulicNodeJunction");
    first.anchor->entity_uuid = uuid(201);
    append(foreign, ann::fullDraftAnnotationLayerChanges(distributionLayer(2, 1), {first}));
    append(foreign, ann::fullDraftAnnotationLayerChanges(distributionLayer(4, 3), {}));
    invalid(foreign, "cross-network anchor must be rejected even if target exists");

    Records noTarget;
    append(noTarget, networkChanges(1, 200));
    first.anchor->entity_uuid = uuid(202);
    append(noTarget, ann::fullDraftAnnotationLayerChanges(distributionLayer(2, 1), {first}));
    invalid(noTarget, "missing anchor target must be rejected");

    // The layer loader currently enforces this as well; the candidate-wide
    // checker must not turn it into an implicitly attached Drainage note.
    Records drainage;
    append(drainage, networkChanges(1, 200));
    append(drainage, ann::fullDraftAnnotationLayerChanges(distributionLayer(2, 1), {}));
    first.anchor->entity_uuid = uuid(200);
    append(drainage, ann::fullDraftAnnotationLayerChanges(drainageLayer(5), {first}));
    invalid(drainage, "pre-network drainage anchor must be rejected");
}

// ---- A1: incremental (delta) validation must agree with the complete one ----

std::vector<db::ModelChange> networkFor(std::uint32_t ordinal,
                                        const std::vector<std::uint32_t>& junctions,
                                        double longitude) {
    if (junctions.empty())
        return {hydraulic::emptyDraftHydraulicDistributionNetworkRoot(
            uuid(ordinal), QStringLiteral("Annotation test network"))};
    NetworkHydraulicDistribution network;
    network.uuid = uuid(ordinal);
    network.id = QStringLiteral("N%1").arg(static_cast<int>(ordinal));
    for (const std::uint32_t item : junctions) {
        HydraulicNodeJunction junction;
        junction.uuid = uuid(item);
        junction.id = QStringLiteral("J%1").arg(static_cast<int>(item));
        junction.coordinate_wgs84.longitude_deg = longitude;
        junction.coordinate_wgs84.latitude_deg = 11.0;
        network.nodes_junctions.append(junction);
    }
    return hydraulic::fullDraftHydraulicDistributionNetworkChanges(network);
}

MapAnnotation anchored(std::uint32_t ordinal, std::uint32_t target,
                       const QString& type = QStringLiteral("HydraulicNodeJunction")) {
    MapAnnotation result = annotation(ordinal);
    MapAnnotationAnchor anchor;
    anchor.entity_uuid = uuid(target);
    anchor.entity_type = type;
    result.anchor = anchor;
    return result;
}

struct Scene {
    std::vector<std::uint32_t> junctions1{200, 202};
    double longitude1 = 14.0;
    QList<MapAnnotation> notes1{anchored(1, 200), annotation(2)};
    bool network3 = true;
    QList<MapAnnotation> notes3;
    bool drainageOwnsLayer4 = false;
    QList<MapAnnotation> drainageNotes{annotation(5)};
};

Records build(const Scene& scene) {
    Records model;
    append(model, networkFor(1, scene.junctions1, scene.longitude1));
    append(model, ann::fullDraftAnnotationLayerChanges(distributionLayer(2, 1), scene.notes1));
    if (scene.network3) {
        append(model, networkFor(3, {201}, 14.0));
        append(model, ann::fullDraftAnnotationLayerChanges(
            scene.drainageOwnsLayer4 ? drainageLayer(4) : distributionLayer(4, 3), scene.notes3));
    }
    append(model, ann::fullDraftAnnotationLayerChanges(drainageLayer(5), scene.drainageNotes));
    return model;
}

std::vector<db::ModelChange> difference(const Records& before, const Records& after) {
    std::vector<db::ModelChange> result;
    for (const std::pair<const db::Key, db::ModelRecord>& entry : after) {
        const Records::const_iterator old = before.find(entry.first);
        if (old == before.end() || old->second.kind != entry.second.kind ||
            old->second.references != entry.second.references ||
            old->second.payload != entry.second.payload)
            result.push_back(db::ModelChange{entry.first, entry.second});
    }
    for (const std::pair<const db::Key, db::ModelRecord>& entry : before)
        if (!after.contains(entry.first)) result.push_back(db::ModelChange{entry.first, std::nullopt});
    return result;
}

db::EngineeringCandidateView engineeringView(const Records& records) {
    db::EngineeringCandidateView result;
    result.get = [&records](const db::Key& id) -> std::optional<db::ModelRecord> {
        const Records::const_iterator found = records.find(id);
        return found == records.end() ? std::nullopt : std::optional<db::ModelRecord>(found->second);
    };
    result.referrers = [&records](const db::Key& id) -> std::vector<db::Key> {
        std::vector<db::Key> referrers;
        for (const std::pair<const db::Key, db::ModelRecord>& entry : records)
            if (std::find(entry.second.references.begin(), entry.second.references.end(), id) !=
                entry.second.references.end())
                referrers.push_back(entry.first);
        return referrers;
    };
    return result;
}

// Runs both validators. The complete one is the reference result; the
// incremental one must agree and must use the complete fallback only when
// `expectComplete` says so.
void agree(const Records& before, const Records& after, bool expectValid,
           bool expectComplete, const char* message) {
    valid(before, "delta fixture base must be valid");
    const bool completeValid = !validate(after).has_value();
    require(completeValid == expectValid, (std::string(message) + ": complete validator").c_str());
    bool fallback = false;
    const std::optional<std::string> incremental = ann::validateAnnotationSnapshotDelta(
        engineeringView(before), engineeringView(after), difference(before, after),
        [&after, &fallback]() {
            fallback = true;
            return validate(after);
        });
    if (incremental.has_value() == expectValid) {
        std::cerr << message << ": " << incremental.value_or("accepted") << '\n';
        throw std::runtime_error(std::string(message) + ": incremental validator disagrees");
    }
    require(fallback == expectComplete,
            (std::string(message) + ": unexpected complete-validation fallback").c_str());
}

void testDeltaMatchesCompleteValidation() {
    const Scene base;
    const Records before = build(base);

    Scene text = base;
    text.notes1[1].text = QStringLiteral("Edited text");
    agree(before, build(text), true, false, "annotation text edit");

    Scene attach = base;
    attach.notes1[1] = anchored(2, 202);
    agree(before, build(attach), true, false, "attach to own network junction");

    Scene foreign = base;
    foreign.notes1[1] = anchored(2, 201);
    agree(before, build(foreign), false, false, "attach to another network's junction");

    Scene wrongType = base;
    wrongType.notes1[1] = anchored(2, 200, QStringLiteral("HydraulicNodeTank"));
    agree(before, build(wrongType), false, false, "anchor with the wrong entity type");

    Scene added = base;
    added.notes1.append(annotation(3));
    agree(before, build(added), true, false, "annotation insertion");

    Scene removed = base;
    removed.notes1.removeFirst();
    agree(before, build(removed), true, false, "annotation deletion");

    Scene targetGone = base;
    targetGone.junctions1 = {202};
    agree(before, build(targetGone), false, false, "anchor target deleted without detach");

    Scene detached = targetGone;
    detached.notes1[0].anchor.reset();
    agree(before, build(detached), true, false, "anchor target deleted with detach");

    Scene moved = base;
    moved.longitude1 = 15.0;
    agree(before, build(moved), true, false, "anchor target moved");

    Scene drainageAnchor = base;
    drainageAnchor.drainageNotes[0] = anchored(5, 200);
    agree(before, build(drainageAnchor), false, false, "anchored pre-network drainage note");

    Scene networkRemoved = base;
    networkRemoved.network3 = false;
    agree(before, build(networkRemoved), true, true, "network and layer removal");

    Scene domainChange = base;
    domainChange.drainageOwnsLayer4 = true;
    // The changed owner reference leaves network 3 without a layer: the
    // incremental graph check rejects it before any complete fallback.
    agree(before, build(domainChange), false, false, "layer domain change");

    // Raw record tampering that no Save materializer produces.
    Records noncanonical = before;
    noncanonical.at(key(annotation(2).uuid)).payload.push_back(0);
    agree(before, noncanonical, false, false, "noncanonical annotation payload");

    Records badNode = before;
    badNode.at(key(uuid(202))).payload = {1, 2, 3};
    agree(before, badNode, false, false, "undecodable hydraulic member");

    Records kindChange = before;
    kindChange.at(key(annotation(2).uuid)).kind = 217;
    agree(before, kindChange, false, false, "annotation record kind change");

    Records chunkSteals = before;
    for (std::pair<const db::Key, db::ModelRecord>& entry : chunkSteals) {
        if (entry.second.kind != ann::annotationChunkKind) continue;
        std::vector<db::Key>& references = entry.second.references;
        if (std::find(references.begin(), references.end(), key(annotation(2).uuid)) ==
            references.end()) continue;
        references.push_back(key(uuid(202)));
        std::sort(references.begin(), references.end());
    }
    agree(before, chunkSteals, false, false, "annotation chunk listing a hydraulic member");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        testValidScopes();
        testLayerCardinalityAndOwnership();
        testOrphanRecords();
        testCrossLayerReferences();
        testTypedAnchors();
        testDeltaMatchesCompleteValidation();
        std::cout << "DVMDB B2 annotation candidate integrity tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DVMDB B2 annotation candidate integrity: " << error.what() << '\n';
        return 1;
    }
}
