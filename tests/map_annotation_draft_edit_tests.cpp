#include "aowis/dvmdb/map_annotation_persistence.h"
#include "aowis/dvmdb/model_record_conversion.h"
#include "aowis/dvmdb/storage_error.h"
#include "aowis/dvmdb/model_codec.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QString>

#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
namespace ann = aowis::dvmdb::annotations;
namespace db = aowis::dvmdb;
namespace model = aowis::model;

void ensure(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

QUuid uuid(std::uint32_t number) {
    db::Key bytes{};
    bytes[0] = 0x42;
    bytes[6] = 0x70;
    bytes[8] = 0x80;
    for (unsigned index = 0; index < 4; ++index)
        bytes[15 - index] = static_cast<std::uint8_t>(number >> (index * 8U));
    return db::record::uuidFromKey(bytes);
}

MapAnnotation makeAnnotation(std::uint32_t ordinal) {
    MapAnnotation item;
    item.uuid = uuid(ordinal + 1000);
    item.text = QStringLiteral("Test annotation %1").arg(static_cast<int>(ordinal));
    item.position_wgs84.latitude_deg = 14.1;
    item.position_wgs84.longitude_deg = 32.2;
    return item;
}

MapAnnotationLayer makeLayer() {
    MapAnnotationLayer layer;
    layer.uuid = uuid(2);
    layer.domain = MapAnnotationDomain::Distribution;
    layer.owner_network_uuid = uuid(1);
    return layer;
}

using Records = std::map<db::Key, db::ModelRecord>;

void applyChanges(Records& records, const std::vector<db::ModelChange>& changes) {
    for (const db::ModelChange& change : changes) {
        if (change.record) records[change.key] = *change.record;
        else records.erase(change.key);
    }
}

std::function<std::optional<db::ModelRecord>(const db::Key&)> read(const Records& records) {
    return [&records](const db::Key& id) -> std::optional<db::ModelRecord> {
        const Records::const_iterator item = records.find(id);
        if (item == records.end()) return std::nullopt;
        return item->second;
    };
}

Records initial(const QList<MapAnnotation>& annotations = {}) {
    Records records;
    db::ModelRecord owner;
    owner.kind = 100;
    records.emplace(db::record::keyFromUuid(uuid(1)), owner);
    applyChanges(records, ann::fullDraftAnnotationLayerChanges(makeLayer(), annotations));
    return records;
}

model::ModelEntityEdit create(const MapAnnotation& value) {
    model::ModelEntityEdit edit;
    edit.entityId = value.uuid;
    edit.entityType = QStringLiteral("map.annotation");
    edit.after = db::ModelCodec<MapAnnotation>::encode(value);
    return edit;
}

model::ModelEntityEdit update(const MapAnnotation& before, const MapAnnotation& after) {
    model::ModelEntityEdit edit = create(after);
    edit.before = db::ModelCodec<MapAnnotation>::encode(before);
    return edit;
}

model::ModelEntityEdit remove(const MapAnnotation& value) {
    model::ModelEntityEdit edit = create(value);
    edit.before = edit.after;
    edit.after.reset();
    return edit;
}

template <typename Callback>
void mustReject(Callback&& callback, const char* message) {
    bool rejected = false;
    try { callback(); }
    catch (const db::StorageError&) { rejected = true; }
    ensure(rejected, message);
}

void checkMemberOrder(const Records& records, const QList<QUuid>& ids) {
    const std::optional<ann::AnnotationLayerSnapshot> snapshot =
        ann::loadAnnotationLayer(read(records), makeLayer().uuid);
    ensure(snapshot.has_value(), "annotation layer missing after editing");
    ensure(snapshot->annotations.size() == ids.size(), "annotation edit changed membership count");
    for (qsizetype index = 0; index < ids.size(); ++index)
        ensure(snapshot->annotations.at(index).uuid == ids.at(index),
               "annotation edit changed stable membership order");
}

void testCreateUpdateDelete() {
    Records records = initial();
    const MapAnnotation first = makeAnnotation(1);
    const MapAnnotation second = makeAnnotation(2);
    std::vector<db::ModelChange> delta =
        ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid,
                                            {create(first), create(second)});
    ensure(delta.size() == 4, "two insertions should change members, chunk, and root");
    applyChanges(records, delta);
    checkMemberOrder(records, {first.uuid, second.uuid});

    MapAnnotation changed = first;
    changed.text = QStringLiteral("Edited text");
    delta = ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid,
                                                  {update(first, changed)});
    ensure(delta.size() == 1 && delta.front().key == db::record::keyFromUuid(first.uuid),
           "scalar edit must not rewrite chunk or layer root");
    applyChanges(records, delta);
    ensure(ann::loadAnnotationLayer(read(records), makeLayer().uuid)->annotations.first().text == changed.text,
           "text edit did not persist");

    delta = ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid,
                                                  {remove(changed)});
    ensure(delta.size() == 2, "deletion within one chunk must update that chunk and tombstone member");
    applyChanges(records, delta);
    checkMemberOrder(records, {second.uuid});
    ensure(!read(records)(db::record::keyFromUuid(first.uuid)),
           "removed annotation still has a native record");
}

void testSequentialEditsAndBoundary() {
    QList<MapAnnotation> items;
    for (std::uint32_t index = 1; index <= 256; ++index)
        items.append(makeAnnotation(index));
    Records records = initial(items);
    MapAnnotation newItem = makeAnnotation(257);
    MapAnnotation changed = newItem;
    changed.text = QStringLiteral("Updated in the same batch");
    std::vector<db::ModelChange> delta = ann::materializeDraftAnnotationEdits(
        read(records), makeLayer().uuid, {create(newItem), update(newItem, changed)});
    ensure(delta.size() == 3, "256->257 must add one member, one chunk, root");
    applyChanges(records, delta);
    ensure(ann::loadAnnotationLayer(read(records), makeLayer().uuid)->annotations.last().text == changed.text,
           "sequential batch did not retain final after-image");
    delta = ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid, {remove(changed)});
    ensure(delta.size() == 3, "257->256 must tombstone second chunk and member and update root");
    applyChanges(records, delta);
    ensure(ann::loadAnnotationLayer(read(records), makeLayer().uuid)->annotations.size() == 256,
           "chunk boundary lost original members");
    ensure(ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid,
                                                 {update(items.first(), items.first())}).empty(),
           "identical edit must not fabricate a native delta");
}

void testBadEdits() {
    Records records = initial({makeAnnotation(1)});
    const Records before = records;
    const MapAnnotation first = makeAnnotation(1);
    const MapAnnotation second = makeAnnotation(2);
    const MapAnnotation stale = makeAnnotation(3);
    mustReject([&]() { ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid, {}); },
               "empty batch was accepted");
    mustReject([&]() { ann::materializeDraftAnnotationEdits(read(records), uuid(99), {create(second)}); },
               "nonexistent layer was accepted");
    mustReject([&]() { ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid,
                                                             {create(first)}); },
               "duplicate insertion was accepted");
    mustReject([&]() { ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid,
                                                             {remove(second)}); },
               "removal without matching stored before-image was accepted");
    mustReject([&]() { ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid,
                                                             {update(stale, first)}); },
               "stale image was accepted");
    mustReject([&]() { ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid,
                                                             {create(second), update(first, second)}); },
               "partially valid batch with stale second edit was accepted");
    model::ModelEntityEdit badType = remove(first);
    badType.entityType = QStringLiteral("HydraulicNodeJunction");
    mustReject([&]() { ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid,
                                                             {badType}); },
               "foreign MODEL entity type was accepted");
    model::ModelEntityEdit badUuid = create(second);
    badUuid.entityId = uuid(12345);
    mustReject([&]() { ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid,
                                                             {badUuid}); },
               "payload UUID mismatch was accepted");
    model::ModelEntityEdit badBytes = create(second);
    badBytes.after = QByteArray("invalid canonical bytes");
    mustReject([&]() { ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid,
                                                             {badBytes}); },
               "invalid canonical payload was accepted");
    db::ModelRecord foreign;
    foreign.kind = 12;
    records.emplace(db::record::keyFromUuid(second.uuid), foreign);
    mustReject([&]() { ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid,
                                                             {create(second)}); },
               "foreign UUID collision was accepted");
    records.erase(db::record::keyFromUuid(second.uuid));
    ensure(records.size() == before.size(), "rejected edit changed record count");
    for (const std::pair<const db::Key, db::ModelRecord>& entry : before) {
        const Records::const_iterator found = records.find(entry.first);
        ensure(found != records.end() && found->second.kind == entry.second.kind &&
               found->second.references == entry.second.references &&
               found->second.payload == entry.second.payload,
               "rejected edit modified caller-owned records");
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        testCreateUpdateDelete();
        testSequentialEditsAndBoundary();
        testBadEdits();
        std::cout << "B3.1 annotation draft edit materialization tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "B3.1 annotation draft edit tests: " << error.what() << '\n';
        return 1;
    }
}
