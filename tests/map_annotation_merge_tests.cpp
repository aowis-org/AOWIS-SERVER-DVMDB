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
                      const std::vector<hydraulic::HydraulicObjectChoice>& objects = {}) {
    const hydraulic::HydraulicMergeSelections selections{mainHead, featureHead, {}, objects};
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
        testInvalidFastForward();
        testConcurrentInsertAndCanonicalChunkSplit();
        testConcurrentFirstChunkCreation();
        testIndependentDeletionAndAddition();
        testDeleteVersusModifyRequiresObjectChoice();
        testConcurrentReorderIsRejected();
        std::cout << "DVMDB B4 annotation merge tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DVMDB B4 annotation merge tests: " << error.what() << '\n';
        return 1;
    }
}
