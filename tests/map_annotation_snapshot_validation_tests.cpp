#include "aowis/dvmdb/map_annotation_snapshot_validation.h"
#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/storage_error.h"
#include "aowis/dvmdb/map_annotation_persistence.h"
#include "aowis/dvmdb/hydraulic_distribution_network_persistence.h"
#include "aowis/dvmdb/model_record_conversion.h"

#include <aowis/model/serialization/model_codec.h>

#include <QCoreApplication>
#include <QString>

#include <cstdint>
#include <iostream>
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <random>
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

aowis::model::ModelEntityEdit annotationEdit(const std::optional<MapAnnotation>& before,
                                             const std::optional<MapAnnotation>& after) {
    aowis::model::ModelEntityEdit edit;
    edit.entityId = before ? before->uuid : after->uuid;
    edit.entityType = QStringLiteral("map.annotation");
    edit.ownerLayerId = uuid(2);
    if (before) edit.before = aowis::model::ModelCodec<MapAnnotation>::encode(*before);
    if (after) edit.after = aowis::model::ModelCodec<MapAnnotation>::encode(*after);
    return edit;
}

// ---- Indexed hydraulic Save check: must agree with the complete loader ----

struct HydraulicScene final {
    NetworkHydraulicDistribution a; // uuid 300, layer 301
    NetworkHydraulicDistribution b; // uuid 400, layer 401
};

HydraulicScene hydraulicScene() {
    HydraulicScene scene;
    scene.a.uuid = uuid(300);
    scene.a.id = QStringLiteral("A");
    HydraulicPatternTime pattern;
    pattern.uuid = uuid(310);
    pattern.id = QStringLiteral("P");
    pattern.multipliers = {1.0, 0.5};
    HydraulicPatternTime settingsPattern = pattern;
    settingsPattern.uuid = uuid(312);
    settingsPattern.id = QStringLiteral("PS");
    scene.a.patterns_time = {pattern, settingsPattern};
    scene.a.options_hydraulic.default_demand_pattern_uuid = uuid(312);
    HydraulicPipeMaterial material;
    material.uuid = uuid(311);
    material.id = QStringLiteral("M");
    scene.a.pipe_materials = {material};
    for (std::uint32_t ordinal = 320; ordinal < 324; ++ordinal) {
        HydraulicNodeJunction junction;
        junction.uuid = uuid(ordinal);
        junction.id = QStringLiteral("J%1").arg(static_cast<int>(ordinal));
        junction.coordinate_wgs84.longitude_deg = 14.0;
        junction.coordinate_wgs84.latitude_deg = 11.0;
        scene.a.nodes_junctions.append(junction);
    }
    const std::function<HydraulicLinkPipe(std::uint32_t, std::uint32_t, std::uint32_t)> pipe =
        [](std::uint32_t ordinal, std::uint32_t from, std::uint32_t to) {
            HydraulicLinkPipe value;
            value.uuid = uuid(ordinal);
            value.id = QStringLiteral("L%1").arg(static_cast<int>(ordinal));
            value.node_uuid_from = uuid(from);
            value.node_uuid_to = uuid(to);
            value.material_uuid = uuid(311);
            return value;
        };
    scene.a.links_pipes = {pipe(330, 320, 321), pipe(331, 321, 322)};
    HydraulicDemandPoint point;
    point.uuid = uuid(340);
    point.id = QStringLiteral("D");
    point.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
    point.attachment.pipe_uuid = uuid(330);
    point.attachment.pipe_position = 0.5;
    point.attachment.pipe_allocation_mode = HydraulicDemandPointPipeAllocationMode::AssignedJunction;
    point.attachment.pipe_assigned_junction_uuid = uuid(320);
    scene.a.demand_points = {point};
    scene.b.uuid = uuid(400);
    scene.b.id = QStringLiteral("B");
    HydraulicNodeJunction other;
    other.uuid = uuid(420);
    other.id = QStringLiteral("JB");
    scene.b.nodes_junctions = {other};
    return scene;
}

Records hydraulicRecords(const HydraulicScene& scene) {
    Records records;
    append(records, hydraulic::fullDraftHydraulicDistributionNetworkChanges(scene.a));
    append(records, ann::fullDraftAnnotationLayerChanges(distributionLayer(301, 300), {}));
    append(records, hydraulic::fullDraftHydraulicDistributionNetworkChanges(scene.b));
    append(records, ann::fullDraftAnnotationLayerChanges(distributionLayer(401, 400), {}));
    return records;
}

template<typename T>
aowis::model::ModelEntityEdit hydraulicEdit(const char* type, const T& value) {
    aowis::model::ModelEntityEdit edit;
    edit.entityId = value.uuid;
    edit.entityType = QString::fromLatin1(type);
    edit.after = aowis::model::ModelCodec<T>::encode(value);
    return edit;
}

// Member record exactly as a Save stores it (payload + canonical references).
template<typename T>
db::ModelRecord memberRecord(const char* type, const T& value) {
    const aowis::model::ModelEntityEdit edit = hydraulicEdit(type, value);
    return hydraulic::canonicalHydraulicMemberImage(edit, *edit.after).record;
}

// Insertion or deletion through the Save membership materializer.
Records membership(const Records& before, std::uint32_t network, const db::Key& member,
                   std::optional<db::ModelRecord> inserted, std::uint16_t kind,
                   std::uint16_t collection) {
    std::vector<db::ModelChange> changes{db::ModelChange{member, inserted}};
    const std::function<std::optional<db::ModelRecord>(const db::Key&)> read =
        [&before](const db::Key& id) -> std::optional<db::ModelRecord> {
        const Records::const_iterator found = before.find(id);
        return found == before.end() ? std::nullopt : std::optional<db::ModelRecord>(found->second);
    };
    hydraulic::materializeDraftMembership(read,
        {hydraulic::DraftMembershipIntent{key(uuid(network)), member, kind, collection,
                                          inserted.has_value()}}, changes);
    Records after = before;
    for (const db::ModelChange& change : changes) {
        if (change.record) after[change.key] = *change.record;
        else after.erase(change.key);
    }
    return after;
}

// Both validators must give the same verdict; returns it.
bool hydraulicAgree(const Records& before, const Records& after, const char* message) {
    const bool completeValid = !validate(after).has_value();
    const std::optional<std::string> incremental = ann::validateAnnotationSnapshotDelta(
        engineeringView(before), engineeringView(after), difference(before, after),
        [&after]() { return validate(after); });
    if (incremental.has_value() == completeValid) {
        std::cerr << message << ": complete " << (completeValid ? "accepts" : "rejects")
                  << ", incremental " << incremental.value_or("accepts") << '\n';
        throw std::runtime_error(std::string(message) + ": incremental hydraulic check disagrees");
    }
    return completeValid;
}

void testHydraulicDeltaMatchesComplete() {
    const HydraulicScene scene = hydraulicScene();
    const Records before = hydraulicRecords(scene);
    valid(before, "hydraulic fixture must be valid");
    const HydraulicLinkPipe& p1 = scene.a.links_pipes[0];
    const HydraulicLinkPipe& p2 = scene.a.links_pipes[1];
    const std::function<Records(const db::Key&, const db::ModelRecord&)> replaced =
        [&before](const db::Key& id, const db::ModelRecord& record) {
            Records after = before;
            after[id] = record;
            return after;
        };
    const std::function<void(bool, bool, const char*)> expect =
        [](bool actual, bool wanted, const char* message) {
            require(actual == wanted, (std::string(message) + ": unexpected verdict").c_str());
        };
    // Scalar edits.
    HydraulicNodeJunction moved = scene.a.nodes_junctions[0];
    moved.coordinate_wgs84.longitude_deg = 15.0;
    moved.elevation_m = 42.0;
    expect(hydraulicAgree(before, replaced(key(moved.uuid), memberRecord("HydraulicNodeJunction", moved)),
                          "junction scalar edit"), true, "junction scalar edit");
    HydraulicLinkPipe negative = p1;
    negative.diameter_mm = -5.0;
    expect(hydraulicAgree(before, replaced(key(p1.uuid), memberRecord("HydraulicLinkPipe", negative)),
                          "negative pipe diameter"), false, "negative pipe diameter");
    // Rewires.
    HydraulicLinkPipe rewired = p2;
    rewired.node_uuid_to = uuid(323);
    expect(hydraulicAgree(before, replaced(key(p2.uuid), memberRecord("HydraulicLinkPipe", rewired)),
                          "valid pipe rewire"), true, "valid pipe rewire");
    HydraulicLinkPipe foreign = p2;
    foreign.node_uuid_to = uuid(420);
    expect(hydraulicAgree(before, replaced(key(p2.uuid), memberRecord("HydraulicLinkPipe", foreign)),
                          "rewire into another network"), false, "rewire into another network");
    HydraulicLinkPipe wrongType = p2;
    wrongType.node_uuid_to = uuid(311);
    expect(hydraulicAgree(before, replaced(key(p2.uuid), memberRecord("HydraulicLinkPipe", wrongType)),
                          "rewire to a material"), false, "rewire to a material");
    HydraulicLinkPipe missing = p2;
    missing.node_uuid_to = uuid(999);
    expect(hydraulicAgree(before, replaced(key(p2.uuid), memberRecord("HydraulicLinkPipe", missing)),
                          "rewire to a missing node"), false, "rewire to a missing node");
    // Moving P1 off the demand point's assigned junction breaks the demand point.
    HydraulicLinkPipe detached = p1;
    detached.node_uuid_from = uuid(323);
    expect(hydraulicAgree(before, replaced(key(p1.uuid), memberRecord("HydraulicLinkPipe", detached)),
                          "pipe rewire breaks demand point"), false, "pipe rewire breaks demand point");
    // Stored references and payload must be canonical.
    db::ModelRecord stale = memberRecord("HydraulicLinkPipe", rewired);
    stale.references = before.at(key(p2.uuid)).references;
    expect(hydraulicAgree(before, replaced(key(p2.uuid), stale), "stale reference index"),
           false, "stale reference index");
    db::ModelRecord padded = before.at(key(p1.uuid));
    padded.payload.push_back(0);
    expect(hydraulicAgree(before, replaced(key(p1.uuid), padded), "noncanonical payload"),
           false, "noncanonical payload");
    // Membership through the Save materializer.
    HydraulicLinkPipe added;
    added.uuid = uuid(332);
    added.id = QStringLiteral("L332");
    added.node_uuid_from = uuid(322);
    added.node_uuid_to = uuid(323);
    const std::uint16_t pipeCollection = 12;
    const std::uint16_t junctionCollection = 9;
    const std::uint16_t patternCollection = 1;
    expect(hydraulicAgree(before, membership(before, 300, key(added.uuid),
                              memberRecord("HydraulicLinkPipe", added), hydraulic::pipeKind, pipeCollection),
                          "pipe insertion"), true, "pipe insertion");
    HydraulicLinkPipe bridging = added;
    bridging.node_uuid_to = uuid(420);
    expect(hydraulicAgree(before, membership(before, 300, key(bridging.uuid),
                              memberRecord("HydraulicLinkPipe", bridging), hydraulic::pipeKind, pipeCollection),
                          "pipe insertion into another network"), false,
           "pipe insertion into another network");
    expect(hydraulicAgree(before, membership(before, 300, key(uuid(323)), std::nullopt,
                              hydraulic::junctionKind, junctionCollection),
                          "unreferenced junction deletion"), true, "unreferenced junction deletion");
    expect(hydraulicAgree(before, membership(before, 300, key(uuid(322)), std::nullopt,
                              hydraulic::junctionKind, junctionCollection),
                          "deletion of a pipe end node"), false, "deletion of a pipe end node");
    expect(hydraulicAgree(before, membership(before, 300, key(uuid(312)), std::nullopt,
                              hydraulic::hydraulicMemberKindForCollection(patternCollection),
                              patternCollection),
                          "deletion of a pattern the settings use"), false,
           "deletion of a pattern the settings use");
    expect(hydraulicAgree(before, membership(before, 300, key(uuid(310)), std::nullopt,
                              hydraulic::hydraulicMemberKindForCollection(patternCollection),
                              patternCollection),
                          "deletion of an unused pattern"), true, "deletion of an unused pattern");
    // Network settings changes use the complete network loader.
    HydraulicScene settings = scene;
    settings.a.options_hydraulic.default_demand_pattern_uuid = uuid(310);
    expect(hydraulicAgree(before, hydraulicRecords(settings), "settings change"), true,
           "settings change");
    settings.a.options_hydraulic.default_demand_pattern_uuid = QUuid();
    expect(hydraulicAgree(before, hydraulicRecords(settings), "settings cleared"), true,
           "settings cleared");
}

// Random record-level edits of the fixture: every verdict of the incremental
// check must equal the complete loader's.
void testRandomHydraulicDeltaAgreement() {
    const HydraulicScene scene = hydraulicScene();
    const Records base = hydraulicRecords(scene);
    std::mt19937 random(0x4A11U);
    const std::vector<std::uint32_t> pool{320, 321, 322, 323, 420, 311, 310, 312, 330, 999};
    const std::function<QUuid()> anyTarget = [&]() { return uuid(pool[random() % pool.size()]); };
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    for (int round = 0; round < 3000; ++round) {
        Records after = base;
        const unsigned operation = static_cast<unsigned>(random() % 7U);
        if (operation <= 1) {
            HydraulicLinkPipe pipe = scene.a.links_pipes[random() % 2U];
            if (random() % 2U) pipe.node_uuid_from = anyTarget();
            if (random() % 2U) pipe.node_uuid_to = anyTarget();
            if (random() % 3U == 0) pipe.material_uuid = random() % 2U ? QUuid() : anyTarget();
            if (random() % 4U == 0) pipe.diameter_mm = random() % 2U ? -1.0 : 250.0;
            db::ModelRecord record = memberRecord("HydraulicLinkPipe", pipe);
            if (random() % 6U == 0) record.references = base.at(key(pipe.uuid)).references;
            after[key(pipe.uuid)] = record;
        } else if (operation == 2) {
            HydraulicDemandPoint point = scene.a.demand_points[0];
            if (random() % 2U) point.attachment.pipe_uuid = anyTarget();
            if (random() % 2U) point.attachment.pipe_assigned_junction_uuid = anyTarget();
            if (random() % 4U == 0) point.attachment.pipe_position = random() % 2U ? 1.5 : 0.25;
            after[key(point.uuid)] = memberRecord("HydraulicDemandPoint", point);
        } else if (operation == 3) {
            HydraulicNodeJunction junction = scene.a.nodes_junctions[random() % 4U];
            junction.elevation_m = static_cast<double>(random() % 100U);
            if (random() % 3U == 0) {
                HydraulicDemand demand;
                demand.pattern_uuid = anyTarget();
                junction.demands = {demand};
            }
            after[key(junction.uuid)] = memberRecord("HydraulicNodeJunction", junction);
        } else if (operation == 4) {
            HydraulicLinkPipe added;
            added.uuid = uuid(5000 + static_cast<std::uint32_t>(round));
            added.id = QStringLiteral("N%1").arg(round);
            added.node_uuid_from = anyTarget();
            added.node_uuid_to = anyTarget();
            after = membership(base, 300, key(added.uuid), memberRecord("HydraulicLinkPipe", added),
                               hydraulic::pipeKind, 12);
        } else if (operation == 5) {
            const std::uint32_t victim = std::vector<std::uint32_t>{320, 321, 322, 323}[random() % 4U];
            after = membership(base, 300, key(uuid(victim)), std::nullopt, hydraulic::junctionKind, 9);
        } else {
            const std::uint32_t victim = random() % 2U ? 310 : 312;
            after = membership(base, 300, key(uuid(victim)), std::nullopt,
                               hydraulic::hydraulicMemberKindForCollection(1), 1);
        }
        if (hydraulicAgree(base, after, "random hydraulic delta")) ++accepted;
        else ++rejected;
    }
    require(accepted > 200 && rejected > 200,
            "random hydraulic deltas must exercise both valid and invalid edits");
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

    // A5: membership edits produced by the real materializer (chunk-local
    // layout) are checked through the root and the changed chunks only.
    const std::function<Records(const std::vector<aowis::model::ModelEntityEdit>&)> edited =
        [&before](const std::vector<aowis::model::ModelEntityEdit>& edits) {
            Records after = before;
            const std::function<std::optional<db::ModelRecord>(const db::Key&)> read =
                [&before](const db::Key& id) -> std::optional<db::ModelRecord> {
                const Records::const_iterator found = before.find(id);
                return found == before.end() ? std::nullopt
                                             : std::optional<db::ModelRecord>(found->second);
            };
            for (const db::ModelChange& change :
                 ann::materializeDraftAnnotationEdits(read, uuid(2), edits)) {
                if (change.record) after[change.key] = *change.record;
                else after.erase(change.key);
            }
            return after;
        };
    agree(before, edited({annotationEdit(base.notes1[0], std::nullopt)}), true, false,
          "materialized deletion of an anchored annotation");
    agree(before, edited({annotationEdit(std::nullopt, anchored(3, 202))}), true, false,
          "materialized insertion of an anchored annotation");
    agree(before, edited({annotationEdit(std::nullopt, anchored(3, 201))}), false, false,
          "materialized insertion anchored to another network");
    agree(before, edited({annotationEdit(base.notes1[1], std::nullopt),
                          annotationEdit(std::nullopt, annotation(4)),
                          annotationEdit(std::nullopt, anchored(7, 200))}), true, false,
          "materialized mixed membership batch");

    // A7: version-8 UUIDs are reserved for server-derived containers.
    Scene reserved = base;
    MapAnnotation derivedLooking = annotation(6);
    QByteArray rfc = derivedLooking.uuid.toRfc4122();
    rfc[6] = static_cast<char>((static_cast<unsigned char>(rfc[6]) & 0x0fU) | 0x80U);
    derivedLooking.uuid = QUuid::fromRfc4122(rfc);
    reserved.notes1.append(derivedLooking);
    agree(before, build(reserved), false, false, "annotation with a version-8 UUID");

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

// A3: a candidate that cannot be READ is not an invalid candidate. I/O,
// ENOSPC and busy failures must reach the caller's status mapping, while
// decode/verification failures stay a diagnostic.
template <typename Failure>
bool completeValidationThrows(const Records& records, const Failure& failure) {
    db::MergeCandidate candidate;
    candidate.get = [&failure](const db::Key&) -> std::optional<db::ModelRecord> { throw failure; };
    candidate.forEach = [&records](const std::function<void(const db::Key&, const db::ModelRecord&)>& visit) {
        for (const std::pair<const db::Key, db::ModelRecord>& entry : records)
            visit(entry.first, entry.second);
    };
    try {
        (void)ann::validateAnnotationSnapshot(candidate);
        return false;
    } catch (const Failure&) {
        return true;
    }
}

void testUnreadableCandidateIsNotInvalid() {
    const Records model = build(Scene{});
    require(completeValidationThrows(model, db::StorageError(db::StorageErrorKind::Io, "read failed")),
            "complete validator turned an I/O error into an invalid candidate");
    require(completeValidationThrows(model, db::StorageError(db::StorageErrorKind::ResourceExhausted, "full")),
            "complete validator turned ENOSPC into an invalid candidate");
    require(completeValidationThrows(model, db::ProjectError(db::ProjectErrorKind::Busy, "busy")),
            "complete validator turned a busy project into an invalid candidate");
    require(!completeValidationThrows(model, db::StorageError(db::StorageErrorKind::Corruption, "bad")),
            "complete validator must report corrupt candidate data as invalid");

    // The incremental validator follows the same policy.
    Scene text;
    text.notes1[1].text = QStringLiteral("Edited while the disk fails");
    const Records after = build(text);
    db::EngineeringCandidateView failing = engineeringView(after);
    failing.get = [](const db::Key&) -> std::optional<db::ModelRecord> {
        throw db::StorageError(db::StorageErrorKind::Io, "read failed");
    };
    bool io = false;
    try {
        (void)ann::validateAnnotationSnapshotDelta(engineeringView(model), failing,
            difference(model, after), []() { return std::optional<std::string>(); });
    } catch (const db::StorageError& error) {
        io = error.kind() == db::StorageErrorKind::Io;
    }
    require(io, "incremental validator turned an I/O error into an invalid candidate");
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
        testHydraulicDeltaMatchesComplete();
        testRandomHydraulicDeltaAgreement();
        testUnreadableCandidateIsNotInvalid();
        std::cout << "DVMDB B2 annotation candidate integrity tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DVMDB B2 annotation candidate integrity: " << error.what() << '\n';
        return 1;
    }
}
