#include "aowis/dvmdb/map_annotation_snapshot_validation.h"
#include "aowis/dvmdb/map_annotation_persistence.h"
#include "aowis/dvmdb/model_record_conversion.h"
#include "aowis/dvmdb/storage_error.h"
#include "project_repository.h"

#include <QCoreApplication>
#include <QString>
#include <aowis/model/serialization/model_codec.h>

#include <cstdint>
#include <map>
#include <functional>
#include <memory>
#include <set>
#include <filesystem>
#include <optional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
namespace db = aowis::dvmdb;
namespace ann = aowis::dvmdb::annotations;
namespace hydraulic = aowis::dvmdb::hydraulic;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

QUuid uuid(std::uint32_t ordinal) {
    db::Key value{};
    value[0] = 0x44;
    value[6] = 0x70;
    value[8] = 0x80;
    for (unsigned index = 0; index < 4; ++index)
        value[15 - index] = static_cast<std::uint8_t>(ordinal >> (index * 8U));
    return db::record::uuidFromKey(value);
}

std::filesystem::path tempDirectory() {
    const std::string pattern = (std::filesystem::temp_directory_path() /
        "aowis-annotation-merge-XXXXXX").string();
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back('\0');
    char* directory = ::mkdtemp(bytes.data());
    if (!directory) throw std::runtime_error("mkdtemp failed");
    return directory;
}

MapAnnotationLayer layer() {
    MapAnnotationLayer value;
    value.uuid = uuid(2);
    value.domain = MapAnnotationDomain::Distribution;
    value.owner_network_uuid = uuid(1);
    return value;
}

MapAnnotation note() {
    MapAnnotation value;
    value.uuid = uuid(3);
    value.text = QStringLiteral("Original note");
    value.position_wgs84.longitude_deg = 12.0;
    value.position_wgs84.latitude_deg = 43.0;
    return value;
}

std::vector<db::ModelChange> initial() {
    std::vector<db::ModelChange> changes = {
        hydraulic::emptyDraftHydraulicDistributionNetworkRoot(
            uuid(1), QStringLiteral("Distribution"))};
    const std::vector<db::ModelChange> annotations =
        ann::fullDraftAnnotationLayerChanges(layer(), {note()});
    changes.insert(changes.end(), annotations.begin(), annotations.end());
    return changes;
}

std::vector<db::ModelChange> edit(const MapAnnotation& value) {
    db::ModelRecord record;
    record.kind = ann::annotationMemberKind;
    record.payload = db::record::bytesFromQByteArray(
        aowis::model::ModelCodec<MapAnnotation>::encode(value));
    require(!record.payload.empty(), "invalid edited annotation fixture");
    // The native reference set is exactly the anchor target, if any.
    if (value.anchor) record.references.push_back(db::record::keyFromUuid(value.anchor->entity_uuid));
    return {{db::record::keyFromUuid(value.uuid), record}};
}

// These tests use the same B2 full-candidate semantic validator as the
// production annotation Commit path. An annotation-bearing native Commit
// without a validator must be rejected, even in test fixtures.
db::RevisionId commitValidated(db::test::ProjectRepository& store,
                               std::string_view branch,
                               std::vector<db::ModelChange> changes) {
    return store.native().commitModel(
        store.project(), branch, std::move(changes),
        db::test::commitMetadata("validated annotation fixture"),
        std::nullopt, false, std::nullopt, ann::validateAnnotationSnapshot);
}

db::MergeResult merge(db::test::ProjectRepository& store,
                      const db::RevisionId& mainHead, const db::RevisionId& featureHead,
                      const std::vector<hydraulic::HydraulicObjectChoice>& objects = {},
                      const std::vector<hydraulic::HydraulicFieldChoice>& fields = {}) {
    const hydraulic::HydraulicMergeSelections selections{mainHead, featureHead, fields, objects};
    return ann::mergeDistributionAnnotationBranches(
        store.native(), store.project(), "main", "feature", selections,
        db::test::commitMetadata("annotation merge"));
}

MapAnnotation additionalNote(std::uint32_t ordinal) {
    MapAnnotation item = note();
    item.uuid = uuid(ordinal);
    item.text = QStringLiteral("Annotation %1").arg(static_cast<qulonglong>(ordinal));
    return item;
}

std::vector<db::ModelChange> annotationDelta(const QList<MapAnnotation>& before,
                                              const QList<MapAnnotation>& after) {
    std::map<db::Key, db::ModelRecord> oldRecords;
    std::map<db::Key, db::ModelRecord> newRecords;
    for (const db::ModelChange& item : ann::fullDraftAnnotationLayerChanges(layer(), before))
        oldRecords.emplace(item.key, *item.record);
    for (const db::ModelChange& item : ann::fullDraftAnnotationLayerChanges(layer(), after))
        newRecords.emplace(item.key, *item.record);
    std::vector<db::ModelChange> delta;
    for (const std::pair<const db::Key, db::ModelRecord>& item : newRecords) {
        const std::map<db::Key, db::ModelRecord>::const_iterator previous =
            oldRecords.find(item.first);
        if (previous == oldRecords.end() || previous->second.kind != item.second.kind ||
            previous->second.payload != item.second.payload ||
            previous->second.references != item.second.references)
            delta.push_back(db::ModelChange{item.first, item.second});
    }
    for (const std::pair<const db::Key, db::ModelRecord>& item : oldRecords)
        if (!newRecords.contains(item.first))
            delta.push_back(db::ModelChange{item.first, std::nullopt});
    return delta;
}

void testConcurrentInsertAndCanonicalChunkSplit() {
    const std::filesystem::path root = tempDirectory();
    try {
        db::test::ProjectRepository store(root);
        store.createBranch("main");
        QList<MapAnnotation> original;
        for (std::uint32_t ordinal = 0; ordinal < 255; ++ordinal)
            original.append(additionalNote(ordinal + 100));
        std::vector<db::ModelChange> first = {
            hydraulic::emptyDraftHydraulicDistributionNetworkRoot(
                uuid(1), QStringLiteral("Distribution"))};
        const std::vector<db::ModelChange> records =
            ann::fullDraftAnnotationLayerChanges(layer(), original);
        first.insert(first.end(), records.begin(), records.end());
        const db::RevisionId base = commitValidated(store, "main", first);
        store.createBranch("feature", "main");
        QList<MapAnnotation> left = original;
        left.append(additionalNote(900));
        QList<MapAnnotation> right = original;
        right.append(additionalNote(901));
        const db::RevisionId target = commitValidated(store, "main", annotationDelta(original, left));
        const db::RevisionId source = commitValidated(store, "feature", annotationDelta(original, right));
        const db::MergeResult result = merge(store, target, source);
        require(result.status == db::MergeStatus::Merged,
                "concurrent appends across a full chunk boundary must merge");
        const std::optional<ann::AnnotationLayerSnapshot> merged =
            ann::loadAnnotationLayerAtRevision(store.native(), store.project(),
                                               store.head("main"), layer().uuid);
        require(merged && merged->annotations.size() == 257 &&
                merged->annotations.at(255).uuid == uuid(900) &&
                merged->annotations.at(256).uuid == uuid(901),
                "concurrent appends lost order or chunk splitting");
        const std::optional<ann::AnnotationLayerSnapshot> historical =
            ann::loadAnnotationLayerAtRevision(store.native(), store.project(),
                                               base, layer().uuid);
        require(historical && historical->annotations.size() == 255,
                "concurrent inserts modified historical membership");
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

void testConcurrentFirstChunkCreation() {
    const std::filesystem::path root = tempDirectory();
    try {
        db::test::ProjectRepository store(root);
        store.createBranch("main");
        std::vector<db::ModelChange> first = {
            hydraulic::emptyDraftHydraulicDistributionNetworkRoot(
                uuid(1), QStringLiteral("Distribution"))};
        const std::vector<db::ModelChange> records =
            ann::fullDraftAnnotationLayerChanges(layer(), {});
        first.insert(first.end(), records.begin(), records.end());
        (void)commitValidated(store, "main", first);
        store.createBranch("feature", "main");
        const db::RevisionId target = commitValidated(store, "main",
            annotationDelta({}, {additionalNote(1000)}));
        const db::RevisionId source = commitValidated(store, "feature",
            annotationDelta({}, {additionalNote(1001)}));
        const db::MergeResult result = merge(store, target, source);
        require(result.status == db::MergeStatus::Merged,
                "simultaneously created first membership chunks must merge");
        const std::optional<ann::AnnotationLayerSnapshot> merged =
            ann::loadAnnotationLayerAtRevision(store.native(), store.project(),
                                               store.head("main"), layer().uuid);
        require(merged && merged->annotations.size() == 2 &&
                merged->annotations.first().uuid == uuid(1000) &&
                merged->annotations.last().uuid == uuid(1001),
                "concurrent first-chunk members are missing or misordered");
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

void testDeleteVersusModifyRequiresObjectChoice() {
    const std::filesystem::path root = tempDirectory();
    try {
        db::test::ProjectRepository store(root);
        store.createBranch("main");
        (void)commitValidated(store, "main", initial());
        store.createBranch("feature", "main");
        const db::RevisionId target = commitValidated(store, "main",
            annotationDelta({note()}, {}));
        MapAnnotation revised = note();
        revised.text = QStringLiteral("Modified after fork");
        const db::RevisionId source = commitValidated(store, "feature", edit(revised));
        const db::MergeResult refused = merge(store, target, source);
        require(refused.status == db::MergeStatus::Conflicts &&
                store.head("main") == target,
                "delete-vs-edit must fail without an explicit annotation choice");
        const std::vector<hydraulic::HydraulicObjectChoice> chooseSource = {
            {note().uuid, hydraulic::HydraulicMergeSide::Source}};
        const db::MergeResult restored = merge(store, target, source, chooseSource);
        require(restored.status == db::MergeStatus::Merged,
                "explicit source-side annotation selection should restore the member");
        const std::optional<ann::AnnotationLayerSnapshot> merged =
            ann::loadAnnotationLayerAtRevision(store.native(), store.project(),
                                               store.head("main"), layer().uuid);
        require(merged && merged->annotations.size() == 1 &&
                merged->annotations.first().text == revised.text,
                "explicit annotation source selection was lost during membership rebuild");
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

void testIndependentDeletionAndAddition() {
    const std::filesystem::path root = tempDirectory();
    try {
        db::test::ProjectRepository store(root);
        store.createBranch("main");
        (void)commitValidated(store, "main", initial());
        store.createBranch("feature", "main");
        const db::RevisionId target = commitValidated(store, "main",
            annotationDelta({note()}, {}));
        const db::RevisionId source = commitValidated(store, "feature",
            annotationDelta({note()}, {note(), additionalNote(2200)}));
        const db::MergeResult outcome = merge(store, target, source);
        require(outcome.status == db::MergeStatus::Merged,
                "deleting the last baseline member while another branch adds one must merge");
        const std::optional<ann::AnnotationLayerSnapshot> merged =
            ann::loadAnnotationLayerAtRevision(store.native(), store.project(),
                                               store.head("main"), layer().uuid);
        require(merged && merged->annotations.size() == 1 &&
                merged->annotations.first().uuid == uuid(2200),
                "independent delete/add lost the surviving annotation or left an orphan");
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

void testConcurrentReorderIsRejected() {
    const std::filesystem::path root = tempDirectory();
    try {
        db::test::ProjectRepository store(root);
        store.createBranch("main");
        const QList<MapAnnotation> original{note(), additionalNote(2000)};
        std::vector<db::ModelChange> first = {
            hydraulic::emptyDraftHydraulicDistributionNetworkRoot(
                uuid(1), QStringLiteral("Distribution"))};
        const std::vector<db::ModelChange> records =
            ann::fullDraftAnnotationLayerChanges(layer(), original);
        first.insert(first.end(), records.begin(), records.end());
        (void)commitValidated(store, "main", first);
        store.createBranch("feature", "main");
        const db::RevisionId target = commitValidated(store, "main",
            annotationDelta(original, {additionalNote(2000), note()}));
        const db::RevisionId source = commitValidated(store, "feature",
            annotationDelta(original, {note(), additionalNote(2000), additionalNote(2001)}));
        const db::MergeResult result = merge(store, target, source);
        require(result.status == db::MergeStatus::Conflicts &&
                store.head("main") == target,
                "concurrent reorder/append must require explicit membership resolution");
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

void testIndependentFieldsAndHistoricalRevision() {
    const std::filesystem::path root = tempDirectory();
    try {
        db::test::ProjectRepository store(root);
        store.createBranch("main");
        const db::RevisionId first = commitValidated(store, "main", initial());
        store.createBranch("feature", "main");
        MapAnnotation left = note();
        left.text = QStringLiteral("Updated text");
        const db::RevisionId mainHead = commitValidated(store, "main", edit(left));
        MapAnnotation right = note();
        right.style.visible = false;
        const db::RevisionId featureHead = commitValidated(store, "feature", edit(right));
        const db::MergeResult outcome = merge(store, mainHead, featureHead);
        require(outcome.status == db::MergeStatus::Merged,
                "independent annotation fields should merge");
        const std::optional<ann::AnnotationLayerSnapshot> merged =
            ann::loadAnnotationLayerAtRevision(store.native(), store.project(),
                                               store.head("main"), layer().uuid);
        require(merged && merged->annotations.size() == 1 &&
                merged->annotations.first().text == left.text &&
                !merged->annotations.first().style.visible,
                "annotation field merge lost an independent change");
        const std::optional<ann::AnnotationLayerSnapshot> original =
            ann::loadAnnotationLayerAtRevision(store.native(), store.project(),
                                               first, layer().uuid);
        require(original && original->annotations.first().text == note().text &&
                original->annotations.first().style.visible,
                "merge modified immutable parent revision");
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

void testSameFieldConflict() {
    const std::filesystem::path root = tempDirectory();
    try {
        db::test::ProjectRepository store(root);
        store.createBranch("main");
        (void)commitValidated(store, "main", initial());
        store.createBranch("feature", "main");
        MapAnnotation left = note();
        left.text = QStringLiteral("A");
        const db::RevisionId mainHead = commitValidated(store, "main", edit(left));
        MapAnnotation right = note();
        right.text = QStringLiteral("B");
        const db::RevisionId featureHead = commitValidated(store, "feature", edit(right));
        const db::MergeResult outcome = merge(store, mainHead, featureHead);
        require(outcome.status == db::MergeStatus::Conflicts &&
                store.head("main") == mainHead,
                "same-field conflict must not advance target HEAD");
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

// ---- A4/A10: field choices and placement as one merge unit ----

// A Distribution network with two junctions as anchor targets.
std::vector<db::ModelChange> initialWithJunctions() {
    NetworkHydraulicDistribution network;
    network.uuid = uuid(1);
    network.id = QStringLiteral("N1");
    for (const std::uint32_t ordinal : {10U, 11U}) {
        HydraulicNodeJunction junction;
        junction.uuid = uuid(ordinal);
        junction.id = QStringLiteral("J%1").arg(static_cast<qulonglong>(ordinal));
        junction.coordinate_wgs84.longitude_deg = 12.0;
        junction.coordinate_wgs84.latitude_deg = 43.0;
        network.nodes_junctions.append(junction);
    }
    std::vector<db::ModelChange> changes =
        hydraulic::fullDraftHydraulicDistributionNetworkChanges(network);
    const std::vector<db::ModelChange> annotations =
        ann::fullDraftAnnotationLayerChanges(layer(), {note()});
    changes.insert(changes.end(), annotations.begin(), annotations.end());
    return changes;
}

MapAnnotation attached(MapAnnotation value, std::uint32_t target) {
    MapAnnotationAnchor anchor;
    anchor.entity_uuid = uuid(target);
    anchor.entity_type = QStringLiteral("HydraulicNodeJunction");
    anchor.offset_east_m = 5.0;
    anchor.offset_north_m = -2.0;
    value.anchor = anchor;
    return value;
}

hydraulic::HydraulicFieldChoice choose(const QString& field, bool source) {
    return hydraulic::HydraulicFieldChoice{note().uuid, field,
        source ? hydraulic::HydraulicMergeSide::Source : hydraulic::HydraulicMergeSide::Target};
}

bool reasonNames(const db::MergeResult& outcome, const char* field) {
    for (const db::MergeConflict& conflict : outcome.conflicts)
        if (conflict.key == db::record::keyFromUuid(note().uuid) &&
            conflict.reason.find(field) != std::string::npos)
            return true;
    return false;
}

bool rejectedSelection(db::test::ProjectRepository& store, const db::RevisionId& mainHead,
                       const db::RevisionId& featureHead,
                       const std::vector<hydraulic::HydraulicFieldChoice>& fields) {
    try {
        (void)merge(store, mainHead, featureHead, {}, fields);
    } catch (const db::StorageError& error) {
        return error.kind() == db::StorageErrorKind::InvalidInput && store.head("main") == mainHead;
    }
    return false;
}

MapAnnotation mergedNote(db::test::ProjectRepository& store) {
    const std::optional<ann::AnnotationLayerSnapshot> merged =
        ann::loadAnnotationLayerAtRevision(store.native(), store.project(),
                                           store.head("main"), layer().uuid);
    require(merged && merged->annotations.size() == 1, "merged annotation layer is incomplete");
    return merged->annotations.first();
}

template <typename Body>
void inRepository(Body&& body) {
    const std::filesystem::path root = tempDirectory();
    try {
        db::test::ProjectRepository store(root);
        body(store);
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

void testAnnotationFieldChoices() {
    for (const bool sourceWins : {false, true}) {
        inRepository([sourceWins](db::test::ProjectRepository& store) {
            store.createBranch("main");
            (void)commitValidated(store, "main", initial());
            store.createBranch("feature", "main");
            MapAnnotation left = note();
            left.text = QStringLiteral("A");
            const db::RevisionId mainHead = commitValidated(store, "main", edit(left));
            MapAnnotation right = note();
            right.text = QStringLiteral("B");
            const db::RevisionId featureHead = commitValidated(store, "feature", edit(right));
            const db::MergeResult conflict = merge(store, mainHead, featureHead);
            require(conflict.status == db::MergeStatus::Conflicts && reasonNames(conflict, "text"),
                    "annotation text conflict must name the field");
            require(rejectedSelection(store, mainHead, featureHead,
                        {choose(QStringLiteral("style.visible"), true)}),
                    "a choice for a non-conflicting annotation field must be rejected");
            const db::MergeResult resolved = merge(store, mainHead, featureHead, {},
                {choose(QStringLiteral("text"), sourceWins)});
            require(resolved.status == db::MergeStatus::Merged,
                    "an annotation field choice must resolve the text conflict");
            require(mergedNote(store).text == (sourceWins ? right.text : left.text),
                    "the annotation field choice selected the wrong side");
        });
    }
}

void testAnchorCombinesWithText() {
    inRepository([](db::test::ProjectRepository& store) {
        store.createBranch("main");
        (void)commitValidated(store, "main", initialWithJunctions());
        store.createBranch("feature", "main");
        const MapAnnotation left = attached(note(), 10);
        const db::RevisionId mainHead = commitValidated(store, "main", edit(left));
        MapAnnotation right = note();
        right.text = QStringLiteral("Edited on feature");
        const db::RevisionId featureHead = commitValidated(store, "feature", edit(right));
        const db::MergeResult outcome = merge(store, mainHead, featureHead);
        require(outcome.status == db::MergeStatus::Merged,
                "attaching on one branch must combine with a text edit on the other");
        const MapAnnotation merged = mergedNote(store);
        require(merged.text == right.text && merged.anchor &&
                merged.anchor->entity_uuid == uuid(10) &&
                merged.anchor->offset_east_m == 5.0 &&
                merged.position_wgs84.longitude_deg == note().position_wgs84.longitude_deg,
                "combined annotation lost the attach or the text edit");
    });
    // Mirror: text on the target branch, detach on the source branch.
    inRepository([](db::test::ProjectRepository& store) {
        store.createBranch("main");
        std::vector<db::ModelChange> changes = initialWithJunctions();
        (void)commitValidated(store, "main", changes);
        (void)commitValidated(store, "main", edit(attached(note(), 11)));
        store.createBranch("feature", "main");
        MapAnnotation left = attached(note(), 11);
        left.text = QStringLiteral("Edited on main");
        const db::RevisionId mainHead = commitValidated(store, "main", edit(left));
        MapAnnotation right = note();
        right.position_wgs84.longitude_deg = 12.5;
        const db::RevisionId featureHead = commitValidated(store, "feature", edit(right));
        const db::MergeResult outcome = merge(store, mainHead, featureHead);
        require(outcome.status == db::MergeStatus::Merged,
                "detaching on one branch must combine with a text edit on the other");
        const MapAnnotation merged = mergedNote(store);
        require(merged.text == left.text && !merged.anchor &&
                merged.position_wgs84.longitude_deg == 12.5,
                "combined annotation lost the detach placement or the text edit");
    });
}

void testPlacementIsOneUnit() {
    for (const bool sourceWins : {false, true}) {
        inRepository([sourceWins](db::test::ProjectRepository& store) {
            store.createBranch("main");
            (void)commitValidated(store, "main", initialWithJunctions());
            store.createBranch("feature", "main");
            // Main attaches (keeps the last explicit position), feature drags
            // the still-free note. Combining both would silently drop the drag.
            const MapAnnotation left = attached(note(), 10);
            const db::RevisionId mainHead = commitValidated(store, "main", edit(left));
            MapAnnotation right = note();
            right.position_wgs84.longitude_deg = 13.0;
            right.text = QStringLiteral("Moved and edited");
            const db::RevisionId featureHead = commitValidated(store, "feature", edit(right));
            const db::MergeResult conflict = merge(store, mainHead, featureHead);
            require(conflict.status == db::MergeStatus::Conflicts &&
                    reasonNames(conflict, "placement") && !reasonNames(conflict, "text") &&
                    store.head("main") == mainHead,
                    "attach versus free drag must conflict as one placement field");
            require(rejectedSelection(store, mainHead, featureHead,
                        {choose(QStringLiteral("anchor"), true)}),
                    "anchor alone must not be selectable separately from placement");
            const db::MergeResult resolved = merge(store, mainHead, featureHead, {},
                {choose(QStringLiteral("placement"), sourceWins)});
            require(resolved.status == db::MergeStatus::Merged,
                    "a placement choice must resolve the placement conflict");
            const MapAnnotation merged = mergedNote(store);
            require(merged.text == right.text, "placement choice lost the independent text edit");
            if (sourceWins)
                require(!merged.anchor && merged.position_wgs84.longitude_deg == 13.0,
                        "source placement must keep the free drag");
            else
                require(merged.anchor && merged.anchor->entity_uuid == uuid(10) &&
                        merged.position_wgs84.longitude_deg == note().position_wgs84.longitude_deg,
                        "target placement must keep the attach and its explicit position");
        });
    }
    // A placement choice without a placement conflict is inapplicable.
    inRepository([](db::test::ProjectRepository& store) {
        store.createBranch("main");
        (void)commitValidated(store, "main", initialWithJunctions());
        store.createBranch("feature", "main");
        MapAnnotation left = note();
        left.text = QStringLiteral("A");
        const db::RevisionId mainHead = commitValidated(store, "main", edit(left));
        MapAnnotation right = note();
        right.text = QStringLiteral("B");
        const db::RevisionId featureHead = commitValidated(store, "feature", edit(right));
        require(rejectedSelection(store, mainHead, featureHead,
                    {choose(QStringLiteral("text"), true), choose(QStringLiteral("placement"), true)}),
                "a placement choice without a placement conflict must be rejected");
    });
}

// The merged anchor is re-checked by the complete candidate validator: an
// attach to a node the other branch deleted must not publish.
void testMergedAnchorTargetIsValidated() {
    inRepository([](db::test::ProjectRepository& store) {
        store.createBranch("main");
        (void)commitValidated(store, "main", initialWithJunctions());
        store.createBranch("feature", "main");
        MapAnnotation left = note();
        left.text = QStringLiteral("Edited on main");
        NetworkHydraulicDistribution network;
        network.uuid = uuid(1);
        network.id = QStringLiteral("N1");
        HydraulicNodeJunction kept;
        kept.uuid = uuid(10);
        kept.id = QStringLiteral("J10");
        kept.coordinate_wgs84.longitude_deg = 12.0;
        kept.coordinate_wgs84.latitude_deg = 43.0;
        network.nodes_junctions.append(kept);
        std::vector<db::ModelChange> mainChanges = edit(left);
        std::map<db::Key, db::ModelRecord> before;
        for (const db::ModelChange& change : hydraulic::fullDraftHydraulicDistributionNetworkChanges(
                 [&]() {
                     NetworkHydraulicDistribution full = network;
                     HydraulicNodeJunction removed = kept;
                     removed.uuid = uuid(11);
                     removed.id = QStringLiteral("J11");
                     full.nodes_junctions.append(removed);
                     return full;
                 }()))
            before.emplace(change.key, *change.record);
        std::set<db::Key> after;
        for (const db::ModelChange& change : hydraulic::fullDraftHydraulicDistributionNetworkChanges(network)) {
            after.insert(change.key);
            const std::map<db::Key, db::ModelRecord>::const_iterator old = before.find(change.key);
            if (old == before.end() || old->second.payload != change.record->payload ||
                old->second.references != change.record->references)
                mainChanges.push_back(change);
        }
        for (const std::pair<const db::Key, db::ModelRecord>& entry : before)
            if (!after.contains(entry.first))
                mainChanges.push_back(db::ModelChange{entry.first, std::nullopt});
        const db::RevisionId mainHead = commitValidated(store, "main", mainChanges);
        const db::RevisionId featureHead = commitValidated(store, "feature", edit(attached(note(), 11)));
        bool refused = false;
        try {
            const db::MergeResult outcome = merge(store, mainHead, featureHead);
            refused = outcome.status != db::MergeStatus::Merged;
        } catch (const db::StorageError&) {
            refused = true;
        }
        require(refused && store.head("main") == mainHead,
                "a merge must not publish an anchor to a node deleted on the other branch");
    });
}

// A11: several members restored by object choices keep their baseline
// order: before the next surviving baseline member, and a restored LAST
// baseline member goes before the other branch's new additions.
void testRestoredMembersKeepBaselineOrder() {
    inRepository([](db::test::ProjectRepository& store) {
        store.createBranch("main");
        (void)commitValidated(store, "main", initial());
        const QList<MapAnnotation> base{note(), additionalNote(2301), additionalNote(2302),
                                        additionalNote(2303), additionalNote(2304)};
        (void)commitValidated(store, "main", annotationDelta({note()}, base));
        store.createBranch("feature", "main");
        const QList<MapAnnotation> left{note(), additionalNote(2302), additionalNote(2305)};
        const db::RevisionId mainHead = commitValidated(store, "main", annotationDelta(base, left));
        QList<MapAnnotation> right = base;
        for (const qsizetype index : {qsizetype{1}, qsizetype{3}, qsizetype{4}})
            right[index].text = QStringLiteral("Kept %1").arg(index);
        const db::RevisionId featureHead = commitValidated(store, "feature", annotationDelta(base, right));
        require(merge(store, mainHead, featureHead).status == db::MergeStatus::Conflicts,
                "delete-versus-edit of several members must conflict without choices");
        std::vector<hydraulic::HydraulicObjectChoice> keep;
        for (const qsizetype index : {qsizetype{1}, qsizetype{3}, qsizetype{4}})
            keep.push_back({right.at(index).uuid, hydraulic::HydraulicMergeSide::Source});
        require(merge(store, mainHead, featureHead, keep).status == db::MergeStatus::Merged,
                "object choices must restore the deleted members");
        const std::optional<ann::AnnotationLayerSnapshot> merged =
            ann::loadAnnotationLayerAtRevision(store.native(), store.project(),
                                               store.head("main"), layer().uuid);
        const QList<QUuid> expected{note().uuid, uuid(2301), uuid(2302), uuid(2303),
                                    uuid(2304), uuid(2305)};
        require(merged && merged->annotations.size() == expected.size(),
                "restored layer has the wrong member count");
        for (qsizetype index = 0; index < expected.size(); ++index)
            require(merged->annotations.at(index).uuid == expected.at(index),
                    "restored members lost their baseline order");
        require(merged->annotations.at(1).text == right.at(1).text &&
                merged->annotations.at(4).text == right.at(4).text,
                "restored members lost the chosen source edits");
    });
}

// A5: tips edited through the real materializer have chunk UUIDs that a
// canonical rebuild would not produce. The merge must delete the tips' actual
// old chunks, or the merged candidate keeps orphaned chunks and is refused.
std::vector<db::ModelChange> materialized(db::test::ProjectRepository& store, const char* branch,
                                          const std::vector<aowis::model::ModelEntityEdit>& edits) {
    const db::RevisionId head = store.head(branch);
    const std::function<std::optional<db::ModelRecord>(const db::Key&)> read =
        [&store, head](const db::Key& id) {
            return store.native().getModelAtRevision(store.project(), head, id);
        };
    return ann::materializeDraftAnnotationEdits(read, layer().uuid, edits);
}

aowis::model::ModelEntityEdit annotationEdit(const std::optional<MapAnnotation>& before,
                                             const std::optional<MapAnnotation>& after) {
    aowis::model::ModelEntityEdit edit;
    edit.entityId = before ? before->uuid : after->uuid;
    edit.entityType = QStringLiteral("map.annotation");
    edit.ownerLayerId = layer().uuid;
    if (before) edit.before = aowis::model::ModelCodec<MapAnnotation>::encode(*before);
    if (after) edit.after = aowis::model::ModelCodec<MapAnnotation>::encode(*after);
    return edit;
}

db::MergeCandidate candidateAt(db::test::ProjectRepository& store, const db::RevisionId& revision) {
    const std::shared_ptr<std::map<db::Key, db::ModelRecord>> records =
        std::make_shared<std::map<db::Key, db::ModelRecord>>();
    for (const std::pair<db::Key, db::Bytes>& entry :
         store.native().scanAtRevision(store.project(), revision))
        records->emplace(entry.first, db::decodeModelRecord(entry.second));
    db::MergeCandidate candidate;
    candidate.get = [records](const db::Key& id) -> std::optional<db::ModelRecord> {
        const std::map<db::Key, db::ModelRecord>::const_iterator found = records->find(id);
        return found == records->end() ? std::nullopt : std::optional<db::ModelRecord>(found->second);
    };
    candidate.forEach = [records](const std::function<void(const db::Key&, const db::ModelRecord&)>& visit) {
        for (const std::pair<const db::Key, db::ModelRecord>& entry : *records)
            visit(entry.first, entry.second);
    };
    return candidate;
}

void testMergeOfChunkLocalTips() {
    inRepository([](db::test::ProjectRepository& store) {
        store.createBranch("main");
        (void)commitValidated(store, "main", initial());
        QList<MapAnnotation> base{note()};
        // 513 members: chunks of 256, 256 and 1. Emptying the first chunk
        // never merges it with the full second one.
        for (std::uint32_t ordinal = 3000; ordinal < 3512; ++ordinal)
            base.append(additionalNote(ordinal));
        (void)commitValidated(store, "main", annotationDelta({note()}, base));
        store.createBranch("feature", "main");
        // Main empties the first chunk completely, so chunk serials 1 and 2
        // remain: a canonical rebuild would call them serials 0 and 1.
        std::vector<aowis::model::ModelEntityEdit> removals;
        for (qsizetype index = 0; index < 256; ++index)
            removals.push_back(annotationEdit(base.at(index), std::nullopt));
        const db::RevisionId mainHead =
            commitValidated(store, "main", materialized(store, "main", removals));
        const MapAnnotation added = additionalNote(3600);
        const db::RevisionId featureHead = commitValidated(store, "feature",
            materialized(store, "feature", {annotationEdit(std::nullopt, added)}));
        const db::MergeResult outcome = merge(store, mainHead, featureHead);
        require(outcome.status == db::MergeStatus::Merged,
                "a merge of chunk-local tips must succeed");
        const std::optional<ann::AnnotationLayerSnapshot> merged =
            ann::loadAnnotationLayerAtRevision(store.native(), store.project(),
                                               store.head("main"), layer().uuid);
        require(merged && merged->annotations.size() == base.size() - 256 + 1,
                "merged chunk-local layer has the wrong member count");
        for (qsizetype index = 0; index + 1 < merged->annotations.size(); ++index)
            require(merged->annotations.at(index).uuid == base.at(index + 256).uuid,
                    "merged chunk-local layer lost the member order");
        require(merged->annotations.last().uuid == added.uuid,
                "merged chunk-local layer lost the source addition");
        // Throws on any structural damage (orphans are caught by the merge
        // validator above; this checks the stored pack and indexes).
        (void)store.native().verifyRepository();
        // The merged revision must still validate as a complete candidate.
        require(!ann::validateAnnotationSnapshot(candidateAt(store, store.head("main"))).has_value(),
                "merged chunk-local revision is not a valid annotation candidate");
    });
}

void testInvalidFastForward() {
    const std::filesystem::path root = tempDirectory();
    try {
        db::test::ProjectRepository store(root);
        store.createBranch("main");
        const db::RevisionId base = commitValidated(store, "main", initial());
        store.createBranch("feature", "main");
        const std::vector<db::ModelChange> existing =
            ann::fullDraftAnnotationLayerChanges(layer(), {note()});
        std::vector<db::ModelChange> removals;
        for (const db::ModelChange& change : existing)
            removals.push_back(db::ModelChange{change.key, std::nullopt});
        // A malformed source revision cannot be manufactured through the
        // production writer anymore: it must fail before any HEAD/pack write.
        // The merge validator remains a separate gate for imported history.
        const std::uint64_t beforeObjects = store.objectCount();
        bool rejected = false;
        try {
            (void)commitValidated(store, "feature", removals);
        } catch (const db::StorageError& error) {
            rejected = error.kind() == db::StorageErrorKind::InvalidInput;
        }
        require(rejected && store.head("feature") == base &&
                store.head("main") == base && store.objectCount() == beforeObjects,
                "native Commit must reject a missing Distribution layer atomically");

        // The valid fast-forward path must still advance directly to the
        // validated source revision without producing a duplicate revision.
        const db::RevisionId validHead = commitValidated(store, "feature",
            annotationDelta({note()}, {note(), additionalNote(1900)}));
        const db::MergeResult outcome = merge(store, base, validHead);
        require(outcome.status == db::MergeStatus::FastForward &&
                store.head("main") == validHead,
                "valid annotation fast-forward must preserve the source revision");
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::remove_all(root);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        testIndependentFieldsAndHistoricalRevision();
        testSameFieldConflict();
        testAnnotationFieldChoices();
        testAnchorCombinesWithText();
        testPlacementIsOneUnit();
        testMergedAnchorTargetIsValidated();
        testInvalidFastForward();
        testConcurrentInsertAndCanonicalChunkSplit();
        testConcurrentFirstChunkCreation();
        testIndependentDeletionAndAddition();
        testDeleteVersusModifyRequiresObjectChoice();
        testRestoredMembersKeepBaselineOrder();
        testMergeOfChunkLocalTips();
        testConcurrentReorderIsRejected();
        std::cout << "DVMDB B4 annotation merge tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DVMDB B4 annotation merge tests: " << error.what() << '\n';
        return 1;
    }
}
