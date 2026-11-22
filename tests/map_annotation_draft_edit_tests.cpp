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
#include <random>
#include <set>
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
    edit.ownerLayerId = makeLayer().uuid;
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
    model::ModelEntityEdit missingOwner = create(second);
    missingOwner.ownerLayerId.reset();
    mustReject([&]() { ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid,
                                                             {missingOwner}); },
               "annotation without durable layer provenance was accepted");
    model::ModelEntityEdit wrongOwner = create(second);
    wrongOwner.ownerLayerId = uuid(99999);
    mustReject([&]() { ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid,
                                                             {wrongOwner}); },
               "annotation claiming a foreign layer was accepted");
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
    foreign.kind = 212;
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

// ---- A5: chunk-local membership layout ----

std::vector<db::ModelChange> applyEdits(Records& records, const std::vector<model::ModelEntityEdit>& edits) {
    const std::vector<db::ModelChange> delta =
        ann::materializeDraftAnnotationEdits(read(records), makeLayer().uuid, edits);
    applyChanges(records, delta);
    return delta;
}

std::size_t chunkCount(const Records& records) {
    const std::optional<ann::AnnotationLayerSnapshot> layer =
        ann::loadAnnotationLayer(read(records), makeLayer().uuid);
    ensure(layer.has_value(), "annotation layer missing");
    return static_cast<std::size_t>(layer->chunkIds.size());
}

bool containsKey(const std::vector<db::ModelChange>& delta, const QUuid& id) {
    for (const db::ModelChange& change : delta)
        if (change.key == db::record::keyFromUuid(id)) return true;
    return false;
}

// Deleting the FIRST member of a large layer rewrites only its own chunk:
// the member tombstone and one chunk, no root and no later chunk.
void testDeleteIsLocal() {
    QList<MapAnnotation> items;
    QList<QUuid> order;
    for (std::uint32_t index = 0; index < 2048; ++index) {
        items.append(makeAnnotation(index));
        order.append(items.back().uuid);
    }
    Records records = initial(items);
    ensure(chunkCount(records) == 8, "2048 members must start in 8 full chunks");
    const std::vector<db::ModelChange> delta = applyEdits(records, {remove(items.first())});
    ensure(delta.size() == 2 && !containsKey(delta, makeLayer().uuid),
           "deleting the first of 2048 members must write exactly the member and its chunk");
    order.removeFirst();
    checkMemberOrder(records, order);
    MapAnnotation changed = items.at(1000);
    changed.text = QStringLiteral("Edited in a large layer");
    const std::vector<db::ModelChange> edit = applyEdits(records, {update(items.at(1000), changed)});
    ensure(edit.size() == 1 && edit.front().key == db::record::keyFromUuid(changed.uuid),
           "a text edit in a large layer must write only the member");
}

// A chunk below the merge threshold is merged with a neighbour when both fit;
// order is the concatenation of chunks before and after the merge.
void testSmallChunksMerge() {
    QList<MapAnnotation> items;
    QList<QUuid> order;
    for (std::uint32_t index = 0; index < 768; ++index) {
        items.append(makeAnnotation(index));
        order.append(items.back().uuid);
    }
    Records records = initial(items);
    ensure(chunkCount(records) == 3, "768 members must start in 3 full chunks");
    // Chunk 0: 256 -> 63 members. Its only neighbour (256) does not fit.
    for (int index = 0; index < 193; ++index) {
        const std::vector<db::ModelChange> delta = applyEdits(records, {remove(items.at(index))});
        ensure(delta.size() <= 2, "a delete without a merge writes at most member and chunk");
        order.removeOne(items.at(index).uuid);
    }
    ensure(chunkCount(records) == 3, "a small chunk must not merge with a full neighbour");
    // Chunk 1: 256 -> 63 members; now 63 + 63 fit, so chunk 1 merges into 0.
    std::vector<db::ModelChange> last;
    for (int index = 256; index < 449; ++index) {
        last = applyEdits(records, {remove(items.at(index))});
        order.removeOne(items.at(index).uuid);
    }
    ensure(chunkCount(records) == 2, "two small neighbours must merge into one chunk");
    ensure(last.size() <= 4 && containsKey(last, makeLayer().uuid),
           "a merging delete writes member, both chunks and the root at most");
    checkMemberOrder(records, order);
    // Removing every member drops the chunks one by one.
    for (int index = 0; index < items.size(); ++index) {
        if (!order.contains(items.at(index).uuid)) continue;
        (void)applyEdits(records, {remove(items.at(index))});
        order.removeOne(items.at(index).uuid);
    }
    ensure(chunkCount(records) == 0, "an emptied layer must have no chunks");
    checkMemberOrder(records, {});
}

// Unborn History undoes its journal: every insertion is undone by deleting
// the member it appended. That must restore the previous records exactly,
// including chunk UUIDs and the root, or the forward replay cannot prove the
// saved draft.
bool sameRecords(const Records& left, const Records& right) {
    if (left.size() != right.size()) return false;
    for (Records::const_iterator a = left.cbegin(), b = right.cbegin(); a != left.cend(); ++a, ++b) {
        if (a->first != b->first || a->second.kind != b->second.kind ||
            a->second.references != b->second.references || a->second.payload != b->second.payload)
            return false;
    }
    return true;
}

void checkInsertionUndoIsExact(const Records& start, const QList<MapAnnotation>& added,
                               const char* message, bool alsoPerEdit = true) {
    Records records = start;
    std::vector<model::ModelEntityEdit> forward;
    std::vector<model::ModelEntityEdit> backward;
    for (const MapAnnotation& annotation : added) forward.push_back(create(annotation));
    for (qsizetype index = added.size(); index > 0; --index)
        backward.push_back(remove(added.at(index - 1)));
    (void)applyEdits(records, forward);
    (void)applyEdits(records, backward);
    ensure(sameRecords(records, start), message);
    if (!alsoPerEdit) return;
    // The same, one edit per batch.
    for (const MapAnnotation& annotation : added) (void)applyEdits(records, {create(annotation)});
    for (qsizetype index = added.size(); index > 0; --index)
        (void)applyEdits(records, {remove(added.at(index - 1))});
    ensure(sameRecords(records, start), message);
}

void testInsertionUndoIsExact() {
    // Empty layer: the first chunk is created and dropped again.
    checkInsertionUndoIsExact(initial(), {makeAnnotation(1)}, "undo into an empty layer");
    // Full last chunk: the insertion opens a new chunk.
    QList<MapAnnotation> full;
    for (std::uint32_t index = 0; index < 256; ++index) full.append(makeAnnotation(index));
    checkInsertionUndoIsExact(initial(full), {makeAnnotation(900)}, "undo of a new chunk");
    // Re-creating after the only chunk was dropped gives the canonical layout.
    {
        const MapAnnotation first = makeAnnotation(1);
        const MapAnnotation second = makeAnnotation(2);
        Records records = initial({first});
        (void)applyEdits(records, {remove(first)});
        ensure(chunkCount(records) == 0, "removing the only member drops its chunk");
        (void)applyEdits(records, {create(second)});
        ensure(sameRecords(records, initial({second})), "a re-created layer must have the canonical layout");
    }
    // Small last chunk whose neighbour has room: [200, 50]. Removing the
    // appended member leaves 50 (< 64), which must NOT merge the two chunks.
    QList<MapAnnotation> layered;
    for (std::uint32_t index = 0; index < 306; ++index) layered.append(makeAnnotation(index));
    Records records = initial(layered);
    std::vector<model::ModelEntityEdit> shrink;
    for (qsizetype index = 0; index < 56; ++index) shrink.push_back(remove(layered.at(index)));
    (void)applyEdits(records, shrink);
    ensure(chunkCount(records) == 2, "fixture must keep chunks of 200 and 50");
    QList<MapAnnotation> many;
    for (std::uint32_t index = 0; index < 300; ++index) many.append(makeAnnotation(2000 + index));
    checkInsertionUndoIsExact(records, {makeAnnotation(1000)}, "undo next to a small neighbour");
    checkInsertionUndoIsExact(records, many, "undo of 300 insertions across chunk boundaries");
    // One batch that drops and re-creates the same chunk UUID twice: one
    // change per key, and the result equals the canonical layout.
    {
        const MapAnnotation a = makeAnnotation(1);
        const MapAnnotation b = makeAnnotation(2);
        const MapAnnotation c = makeAnnotation(3);
        Records twice = initial({a});
        const std::vector<db::ModelChange> delta =
            applyEdits(twice, {remove(a), create(b), remove(b), create(c)});
        std::set<db::Key> keys;
        for (const db::ModelChange& change : delta)
            ensure(keys.insert(change.key).second, "a batch emitted two changes for one key");
        ensure(sameRecords(twice, initial({c})), "drop/re-create batch lost the canonical layout");
    }
}

// Random layer states (random deletions make small chunks), each followed by
// a random insertion batch and its exact undo.
void testRandomInsertionUndoIsExact() {
    QList<MapAnnotation> current;
    std::uint32_t nextOrdinal = 0;
    for (; nextOrdinal < 700; ++nextOrdinal) current.append(makeAnnotation(nextOrdinal));
    Records records = initial(current);
    std::mt19937 random(0x5EEDU);
    // One-edit-per-batch undo is costly (one materialization per edit), so the
    // random rounds check it on small batches only; batched undo every round.
    for (int round = 0; round < 150; ++round) {
        std::vector<model::ModelEntityEdit> removals;
        const unsigned removeCount = static_cast<unsigned>(random() % 40U);
        for (unsigned step = 0; step < removeCount && !current.isEmpty(); ++step) {
            const qsizetype index =
                static_cast<qsizetype>(random() % static_cast<unsigned>(current.size()));
            removals.push_back(remove(current.at(index)));
            current.removeAt(index);
        }
        if (!removals.empty()) (void)applyEdits(records, removals);
        std::vector<model::ModelEntityEdit> creations;
        const unsigned createCount = static_cast<unsigned>(random() % 40U);
        for (unsigned step = 0; step < createCount; ++step) {
            current.append(makeAnnotation(nextOrdinal++));
            creations.push_back(create(current.back()));
        }
        if (!creations.empty()) (void)applyEdits(records, creations);
        QList<MapAnnotation> added;
        const bool perEdit = round % 10 == 0;
        const unsigned addCount = 1U + static_cast<unsigned>(random() % (perEdit ? 20U : 300U));
        for (unsigned step = 0; step < addCount; ++step) added.append(makeAnnotation(nextOrdinal++));
        checkInsertionUndoIsExact(records, added, "random insertion undo is not exact", perEdit);
    }
    QList<QUuid> ids;
    for (const MapAnnotation& annotation : current) ids.append(annotation.uuid);
    checkMemberOrder(records, ids);
}

// Random create/update/delete batches against a reference order. Every
// state is loaded and checked, and a single-edit batch writes at most four
// records (member, two chunks, root) regardless of the layer size.
void testRandomEditsKeepOrder() {
    QList<MapAnnotation> current;
    std::uint32_t nextOrdinal = 0;
    for (; nextOrdinal < 600; ++nextOrdinal) current.append(makeAnnotation(nextOrdinal));
    Records records = initial(current);
    std::mt19937 random(0xA5A5U);
    for (int round = 0; round < 4000; ++round) {
        const unsigned choice = static_cast<unsigned>(random() % 10U);
        std::vector<model::ModelEntityEdit> edits;
        if ((choice < 4 || current.isEmpty()) && current.size() < 1500) {
            const MapAnnotation created = makeAnnotation(nextOrdinal++);
            edits.push_back(create(created));
            current.append(created);
        } else if (choice < 8 && !current.isEmpty()) {
            const qsizetype index = static_cast<qsizetype>(random() % static_cast<unsigned>(current.size()));
            edits.push_back(remove(current.at(index)));
            current.removeAt(index);
        } else if (!current.isEmpty()) {
            const qsizetype index = static_cast<qsizetype>(random() % static_cast<unsigned>(current.size()));
            MapAnnotation changed = current.at(index);
            changed.text = QStringLiteral("Round %1").arg(round);
            edits.push_back(update(current.at(index), changed));
            current[index] = changed;
        } else {
            continue;
        }
        const std::vector<db::ModelChange> delta = applyEdits(records, edits);
        ensure(delta.size() <= 4, "a single annotation edit wrote more than four records");
        if (round % 97 == 0 || round == 3999) {
            QList<QUuid> ids;
            for (const MapAnnotation& annotation : current) ids.append(annotation.uuid);
            checkMemberOrder(records, ids);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        testCreateUpdateDelete();
        testSequentialEditsAndBoundary();
        testBadEdits();
        testDeleteIsLocal();
        testSmallChunksMerge();
        testInsertionUndoIsExact();
        testRandomInsertionUndoIsExact();
        testRandomEditsKeepOrder();
        std::cout << "B3.1 annotation draft edit materialization tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "B3.1 annotation draft edit tests: " << error.what() << '\n';
        return 1;
    }
}
