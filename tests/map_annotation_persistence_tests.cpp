#include "aowis/dvmdb/map_annotation_persistence.h"
#include "aowis/dvmdb/map_annotation_snapshot_validation.h"
#include "aowis/dvmdb/hydraulic_distribution_network_persistence.h"
#include "aowis/dvmdb/model_record_conversion.h"
#include "aowis/dvmdb/storage_error.h"
#include "project_repository.h"

#include <QCoreApplication>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
namespace ann = aowis::dvmdb::annotations;
namespace db = aowis::dvmdb;

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

MapAnnotationLayer distributionLayer() {
    MapAnnotationLayer layer;
    layer.uuid = uuid(2);
    layer.domain = MapAnnotationDomain::Distribution;
    layer.owner_network_uuid = uuid(1);
    return layer;
}

MapAnnotation annotation(std::uint32_t number) {
    MapAnnotation result;
    result.uuid = uuid(number + 1000);
    result.text = QStringLiteral("Annotation %1").arg(static_cast<int>(number));
    result.position_wgs84.latitude_deg = 11.25;
    result.position_wgs84.longitude_deg = 179.25;
    return result;
}

std::map<db::Key, db::ModelRecord> records(const std::vector<db::ModelChange>& changes) {
    std::map<db::Key, db::ModelRecord> result;
    for (const db::ModelChange& change : changes) {
        require(change.record.has_value(), "full builder emitted a deletion");
        require(result.emplace(change.key, *change.record).second, "duplicate record emitted");
    }
    return result;
}

std::function<std::optional<db::ModelRecord>(const db::Key&)> reader(
    const std::map<db::Key, db::ModelRecord>& model) {
    return [&model](const db::Key& key) -> std::optional<db::ModelRecord> {
        const std::map<db::Key, db::ModelRecord>::const_iterator it = model.find(key);
        if (it == model.end()) return std::nullopt;
        return it->second;
    };
}

template<typename Callback>
void mustReject(Callback&& callback, const char* message) {
    bool rejected = false;
    try { callback(); }
    catch (const db::StorageError&) { rejected = true; }
    require(rejected, message);
}

void testDeterministicDistributionLayerIdentity() {
    const QUuid first = ann::distributionLayerUuid(uuid(1));
    const QUuid same = ann::distributionLayerUuid(uuid(1));
    const QUuid different = ann::distributionLayerUuid(uuid(3));
    require(!first.isNull() && first == same && first != different &&
            first != uuid(1),
            "distribution annotation layer UUID must be stable, unique by network and distinct from owner");
    const QByteArray bytes = first.toRfc4122();
    require(bytes.size() == 16 &&
            ((static_cast<unsigned char>(bytes[6]) >> 4U) == 8U) &&
            ((static_cast<unsigned char>(bytes[8]) >> 6U) == 2U),
            "derived annotation layer must be an RFC 9562 UUIDv8");
    mustReject([&]() { ann::distributionLayerUuid(QUuid()); },
               "null network must not produce a layer identity");
}

void testMembershipSizes() {
    for (const int count : {0, 1, 255, 256, 257, 513}) {
        const MapAnnotationLayer layer = distributionLayer();
        QList<MapAnnotation> annotations;
        for (int number = 0; number < count; ++number)
            annotations.append(annotation(static_cast<std::uint32_t>(number)));
        const std::vector<db::ModelChange> changes = ann::fullDraftAnnotationLayerChanges(layer, annotations);
        const std::size_t expectedChunks = static_cast<std::size_t>((count + 255) / 256);
        require(changes.size() == static_cast<std::size_t>(count) + expectedChunks + 1,
                "incorrect member/chunk/root record count");
        std::map<db::Key, db::ModelRecord> model = records(changes);
        db::ModelRecord owner;
        owner.kind = 100;
        owner.payload = {1}; // B1 record-envelope fixture; owner manifest validation is B2.
        model.emplace(db::record::keyFromUuid(uuid(1)), owner);
        const std::optional<ann::AnnotationLayerSnapshot> loaded =
            ann::loadAnnotationLayer(reader(model), layer.uuid);
        require(loaded.has_value() && loaded->annotations.size() == count,
                "annotation layer round-trip lost members");
        for (int index = 0; index < count; ++index)
            require(loaded->annotations.at(index).uuid == annotations.at(index).uuid &&
                    loaded->annotations.at(index).text == annotations.at(index).text,
                    "annotation membership order or payload changed");
        require(!ann::loadAnnotationLayer(reader(model), uuid(987654)),
                "nonexistent layer must return absent");
    }
}

// A6: listings and HTTP limits use the root and membership chunks only.
void testLayerSummaryReadsNoMembers() {
    for (const int count : {0, 1, 256, 257, 513}) {
        const MapAnnotationLayer layer = distributionLayer();
        QList<MapAnnotation> annotations;
        std::set<db::Key> memberKeys;
        for (int number = 0; number < count; ++number) {
            annotations.append(annotation(static_cast<std::uint32_t>(number)));
            memberKeys.insert(db::record::keyFromUuid(annotations.back().uuid));
        }
        std::map<db::Key, db::ModelRecord> model =
            records(ann::fullDraftAnnotationLayerChanges(layer, annotations));
        std::vector<db::Key> visited;
        const std::function<std::optional<db::ModelRecord>(const db::Key&)> base = reader(model);
        const std::function<std::optional<db::ModelRecord>(const db::Key&)> counted =
            [&visited, &base](const db::Key& key) {
                visited.push_back(key);
                return base(key);
            };
        const std::optional<ann::AnnotationLayerSummary> summary =
            ann::loadAnnotationLayerSummary(counted, layer.uuid);
        require(summary.has_value() && summary->memberCount == static_cast<std::size_t>(count) &&
                summary->layer.uuid == layer.uuid &&
                summary->layer.owner_network_uuid == layer.owner_network_uuid,
                "layer summary lost metadata or member count");
        require(visited.size() == 1 + static_cast<std::size_t>((count + 255) / 256),
                "layer summary must read exactly the root and its chunks");
        for (const db::Key& key : visited)
            require(!memberKeys.contains(key), "layer summary read an annotation member");
        require(!ann::loadAnnotationLayerSummary(reader(model), uuid(987654)),
                "summary of a nonexistent layer must be absent");
        if (count == 0) continue;
        // A damaged chunk still fails closed without reading members.
        for (std::pair<const db::Key, db::ModelRecord>& entry : model) {
            if (entry.second.kind != ann::annotationChunkKind) continue;
            entry.second.payload.back() =
                static_cast<std::uint8_t>(entry.second.payload.back() ^ 0x01U);
            break;
        }
        mustReject([&]() { (void)ann::loadAnnotationLayerSummary(reader(model), layer.uuid); },
                   "layer summary accepted a damaged membership chunk");
    }
}

void testDrainageAndInvalidInput() {
    MapAnnotationLayer drainage;
    drainage.uuid = uuid(888);
    drainage.domain = MapAnnotationDomain::Drainage;
    const std::vector<db::ModelChange> changes =
        ann::fullDraftAnnotationLayerChanges(drainage, {annotation(7)});
    const std::map<db::Key, db::ModelRecord> model = records(changes);
    require(ann::loadAnnotationLayer(reader(model), drainage.uuid)->annotations.size() == 1,
            "standalone Drainage layer cannot be loaded");
    drainage.owner_network_uuid = uuid(1);
    mustReject([&]() { ann::fullDraftAnnotationLayerChanges(drainage, {}); },
               "drainage owner must not be accepted until network support exists");
    const MapAnnotationLayer distribution = distributionLayer();
    QList<MapAnnotation> duplicates{annotation(2), annotation(2)};
    mustReject([&]() { ann::fullDraftAnnotationLayerChanges(distribution, duplicates); },
               "duplicate annotation UUID must be rejected");
    MapAnnotation bad = annotation(1);
    bad.text.clear();
    mustReject([&]() { ann::fullDraftAnnotationLayerChanges(distribution, {bad}); },
               "invalid annotation MODEL value must be rejected");
}

void testCorruptReferences() {
    const MapAnnotationLayer layer = distributionLayer();
    std::vector<db::ModelChange> changes =
        ann::fullDraftAnnotationLayerChanges(layer, {annotation(9), annotation(10)});
    std::map<db::Key, db::ModelRecord> model = records(changes);
    db::ModelRecord owner;
    owner.kind = 100;
    model.emplace(db::record::keyFromUuid(uuid(1)), owner);
    const std::map<db::Key, db::ModelRecord> valid = model;
    model.at(db::record::keyFromUuid(annotation(9).uuid)).kind = 217;
    mustReject([&]() { ann::loadAnnotationLayer(reader(model), layer.uuid); },
               "wrong member kind must be rejected");
    model = valid;
    model.at(db::record::keyFromUuid(layer.uuid)).references.clear();
    mustReject([&]() { ann::loadAnnotationLayer(reader(model), layer.uuid); },
               "root references must match payload exactly");
    model = valid;
    model.erase(db::record::keyFromUuid(annotation(10).uuid));
    mustReject([&]() { ann::loadAnnotationLayer(reader(model), layer.uuid); },
               "missing annotation member must be rejected");
    model = valid;
    model.at(db::record::keyFromUuid(uuid(1))).kind = 112;
    mustReject([&]() { ann::loadAnnotationLayer(reader(model), layer.uuid); },
               "wrong network owner kind must be rejected");
}

std::filesystem::path tempDirectory() {
    std::string pattern = (std::filesystem::temp_directory_path() / "aowis-ann-b1-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* directory = ::mkdtemp(buffer.data());
    if (!directory) throw std::runtime_error("mkdtemp failed");
    return directory;
}

// The B1 revision fixtures must use the same fail-closed Commit path as
// application callers, not skip semantic validation in the shared test store.
void commitValidated(db::test::ProjectRepository& store,
                     std::vector<db::ModelChange> changes) {
    (void)store.native().commitModel(
        store.project(), "main", std::move(changes),
        db::test::commitMetadata("validated annotation fixture"),
        std::nullopt, false, std::nullopt, ann::validateAnnotationSnapshot);
}

void testNativeRevisionRoundtrip() {
    const std::filesystem::path location = tempDirectory();
    try {
        db::test::ProjectRepository store(location);
        store.createBranch("main");
        const MapAnnotationLayer layer = distributionLayer();
        // The owner is an actual 17-collection, format-3 hydraulic root,
        // committed atomically with its annotation layer and members.
        std::vector<db::ModelChange> changes = ann::fullDraftAnnotationLayerChanges(
            layer, {annotation(11), annotation(12)});
        changes.insert(changes.begin(),
            aowis::dvmdb::hydraulic::emptyDraftHydraulicDistributionNetworkRoot(
                uuid(1), QStringLiteral("B1 distribution network")));
        commitValidated(store, changes);
        const db::RevisionId revision = store.head("main");
        const std::optional<ann::AnnotationLayerSnapshot> loaded =
            ann::loadAnnotationLayerAtRevision(store.native(), store.project(), revision, layer.uuid);
        require(loaded && loaded->annotations.size() == 2, "immutable revision lost annotation records");
        const std::optional<NetworkHydraulicDistribution> network =
            aowis::dvmdb::hydraulic::loadHydraulicDistributionNetworkAtRevision(
                store.native(), store.project(), revision, uuid(1));
        require(network && network->uuid == uuid(1),
                "immutable revision lost owning hydraulic network");
        // A later revision changes only presentation data. Historical
        // annotation and network reads must remain pinned to their revision.
        MapAnnotation modified = annotation(11);
        modified.text = QStringLiteral("Annotation edited in newer revision");
        commitValidated(store, ann::fullDraftAnnotationLayerChanges(
            layer, {modified, annotation(12)}));
        const db::RevisionId newerRevision = store.head("main");
        require(newerRevision != revision, "annotation edit did not create a new revision");
        const std::optional<ann::AnnotationLayerSnapshot> previous =
            ann::loadAnnotationLayerAtRevision(store.native(), store.project(), revision, layer.uuid);
        const std::optional<ann::AnnotationLayerSnapshot> newer =
            ann::loadAnnotationLayerAtRevision(store.native(), store.project(), newerRevision, layer.uuid);
        require(previous && newer &&
                previous->annotations.first().text == annotation(11).text &&
                newer->annotations.first().text == modified.text,
                "newer annotation revision modified historical records");
        const std::optional<NetworkHydraulicDistribution> unchangedNetwork =
            aowis::dvmdb::hydraulic::loadHydraulicDistributionNetworkAtRevision(
                store.native(), store.project(), newerRevision, uuid(1));
        require(unchangedNetwork && unchangedNetwork->uuid == uuid(1),
                "annotation-only revision lost its hydraulic network");
        require(store.verifyRepository().revisions >= 1, "native reverse-reference verification failed");
    } catch (...) {
        std::filesystem::remove_all(location);
        throw;
    }
    std::filesystem::remove_all(location);
}
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        testDeterministicDistributionLayerIdentity();
        testMembershipSizes();
        testLayerSummaryReadsNoMembers();
        testDrainageAndInvalidInput();
        testCorruptReferences();
        testNativeRevisionRoundtrip();
        std::cout << "DVMDB native annotation records B1 tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DVMDB native annotation records B1 tests failed: " << error.what() << '\n';
        return 1;
    }
}
