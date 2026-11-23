#include "aowis/dvmdb/map_annotation_persistence.h"

#include "aowis/dvmdb/engineering_delta_scope.h"
#include "aowis/dvmdb/model_codec.h"
#include "aowis/dvmdb/model_record_conversion.h"
#include "aowis/dvmdb/storage_error.h"

#include <aowis/model/serialization/model_validation.h>

#include <QCryptographicHash>
#include <QHash>
#include <QSet>
#include <QDataStream>
#include <QIODevice>

#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <utility>

namespace aowis::dvmdb::annotations {
namespace {

constexpr quint32 layerMagic = 0x414E4C31U; // ANL1
constexpr quint32 chunkMagic = 0x414E4331U; // ANC1
// Root version 2 (A5): chunks are edited in place and keep their serial, so a
// deletion rewrites only its own chunk. A new chunk takes the next serial
// after the highest listed one, so undoing an insertion restores the exact
// previous records (unborn History reverses its journal this way).
constexpr quint32 rootVersion = 2;
constexpr quint32 chunkVersion = 1;
// A chunk below this size is merged with a neighbour when both fit.
constexpr qsizetype chunkMergeThreshold = 64;
constexpr qsizetype maxRootBytes = 1024 * 1024;
constexpr qsizetype maxChunkBytes = 16 * 1024;
constexpr qsizetype maxMemberBytes = 256 * 1024;
constexpr std::size_t maxChunkCount = 4096;

Key key(const QUuid& uuid) { return record::keyFromUuid(uuid); }
Bytes bytes(const QByteArray& data) { return record::bytesFromQByteArray(data); }
QByteArray data(const Bytes& source) { return record::qByteArrayFromBytes(source); }

void reject(bool condition, const char* message) {
    if (condition) throw StorageError(StorageErrorKind::Corruption, message);
}

std::vector<Key> sortedReferences(const QList<QUuid>& ids) {
    std::set<Key> unique;
    for (const QUuid& uuid : ids) {
        reject(uuid.isNull(), "null annotation reference UUID");
        reject(!unique.insert(key(uuid)).second, "duplicate annotation reference UUID");
    }
    return std::vector<Key>(unique.begin(), unique.end());
}

QUuid chunkUuid(const QUuid& layerId, quint32 serial) {
    QByteArray seed("aowis-annotation-layer-chunk-v1");
    seed.append(layerId.toRfc4122());
    for (int shift = 24; shift >= 0; shift -= 8)
        seed.append(static_cast<char>((serial >> shift) & 0xffU));
    QByteArray digest = QCryptographicHash::hash(seed, QCryptographicHash::Sha256).left(16);
    digest[6] = static_cast<char>((static_cast<unsigned char>(digest[6]) & 0x0fU) | 0x80U);
    digest[8] = static_cast<char>((static_cast<unsigned char>(digest[8]) & 0x3fU) | 0x80U);
    return QUuid::fromRfc4122(digest);
}

void validateLayer(const MapAnnotationLayer& layer) {
    const aowis::model::ModelValidationReport report = aowis::model::validateMapAnnotationLayer(layer);
    if (!report.valid()) throw StorageError(StorageErrorKind::InvalidInput, report.summary().toStdString());
    if (layer.domain == MapAnnotationDomain::Distribution && !layer.owner_network_uuid)
        throw StorageError(StorageErrorKind::InvalidInput, "distribution annotation layer requires a network owner");
    if (layer.domain == MapAnnotationDomain::Drainage && layer.owner_network_uuid)
        throw StorageError(StorageErrorKind::InvalidInput, "drainage network ownership is not supported yet");
}

ModelRecord encodeMember(const MapAnnotation& annotation) {
    const aowis::model::ModelValidationReport report = aowis::model::validateMapAnnotation(annotation);
    if (!report.valid()) throw StorageError(StorageErrorKind::InvalidInput, report.summary().toStdString());
    const QByteArray encoded = ModelCodec<MapAnnotation>::encode(annotation);
    if (encoded.isEmpty() || encoded.size() > maxMemberBytes)
        throw StorageError(StorageErrorKind::InvalidInput, "invalid annotation MODEL payload");
    ModelRecord record;
    record.kind = annotationMemberKind;
    record.payload = bytes(encoded);
    if (annotation.anchor) record.references.push_back(key(annotation.anchor->entity_uuid));
    return record;
}

ModelRecord encodeChunk(const QUuid& layerId, quint32 serial, const QList<QUuid>& members) {
    if (layerId.isNull() || members.isEmpty() || members.size() > static_cast<qsizetype>(annotationChunkCapacity))
        throw StorageError(StorageErrorKind::InvalidInput, "invalid annotation membership chunk");
    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << chunkMagic << chunkVersion << layerId << serial << members;
    if (stream.status() != QDataStream::Ok || encoded.size() > maxChunkBytes)
        throw StorageError(StorageErrorKind::LimitExceeded, "annotation chunk exceeds size limit");
    ModelRecord record;
    record.kind = annotationChunkKind;
    record.payload = bytes(encoded);
    record.references = sortedReferences(members);
    return record;
}

ModelRecord encodeRoot(const MapAnnotationLayer& layer, const QList<QUuid>& chunkIds) {
    validateLayer(layer);
    if (chunkIds.size() > static_cast<qsizetype>(maxChunkCount))
        throw StorageError(StorageErrorKind::LimitExceeded, "too many annotation chunks");
    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    const QByteArray layerPayload = ModelCodec<MapAnnotationLayer>::encode(layer);
    if (layerPayload.isEmpty() || layerPayload.size() > 65536)
        throw StorageError(StorageErrorKind::InvalidInput, "invalid annotation layer metadata");
    stream << layerMagic << rootVersion << layerPayload << chunkIds;
    if (stream.status() != QDataStream::Ok || encoded.size() > maxRootBytes)
        throw StorageError(StorageErrorKind::LimitExceeded, "annotation root exceeds size limit");
    QList<QUuid> references = chunkIds;
    if (layer.owner_network_uuid) references.append(*layer.owner_network_uuid);
    ModelRecord record;
    record.kind = annotationLayerKind;
    record.references = sortedReferences(references);
    record.payload = bytes(encoded);
    return record;
}

MapAnnotation decodeMember(const ModelRecord& record, const QUuid& expectedId) {
    reject(record.kind != annotationMemberKind || record.payload.size() > static_cast<std::size_t>(maxMemberBytes),
           "invalid annotation member record kind or size");
    QString error;
    const std::optional<MapAnnotation> decoded = ModelCodec<MapAnnotation>::decode(data(record.payload), &error);
    reject(!decoded || decoded->uuid != expectedId,
           "invalid annotation member payload or UUID");
    const aowis::model::ModelValidationReport report = aowis::model::validateMapAnnotation(*decoded);
    reject(!report.valid() || bytes(ModelCodec<MapAnnotation>::encode(*decoded)) != record.payload,
           "noncanonical annotation member payload");
    QList<QUuid> references;
    if (decoded->anchor) references.append(decoded->anchor->entity_uuid);
    reject(sortedReferences(references) != record.references, "annotation anchor reference mismatch");
    return *decoded;
}

struct Chunk final {
    quint32 serial = 0;
    QList<QUuid> members;
};

// Canonical chunk of `layerId`; the caller checks that the chunk's record UUID
// is chunkUuid(layerId, serial) (see checkedChunk).
Chunk decodeChunk(const ModelRecord& record, const QUuid& layerId) {
    reject(record.kind != annotationChunkKind || record.payload.size() > static_cast<std::size_t>(maxChunkBytes),
           "invalid annotation chunk record kind or size");
    const QByteArray payload = data(record.payload);
    QDataStream stream(payload);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint32 version = 0;
    Chunk chunk;
    QUuid storedLayer;
    quint32 memberCount = 0;
    stream >> magic >> version >> storedLayer >> chunk.serial >> memberCount;
    reject(stream.status() != QDataStream::Ok || memberCount == 0 ||
           memberCount > annotationChunkCapacity,
           "invalid annotation chunk member count");
    for (quint32 index = 0; index < memberCount; ++index) {
        QUuid member;
        stream >> member;
        chunk.members.append(member);
    }
    reject(stream.status() != QDataStream::Ok || !stream.atEnd() || magic != chunkMagic ||
           version != chunkVersion || storedLayer != layerId,
           "invalid annotation chunk encoding");
    const ModelRecord canonical = encodeChunk(layerId, chunk.serial, chunk.members);
    reject(canonical.references != record.references,
           "annotation chunk membership references mismatch");
    reject(canonical.payload != record.payload, "noncanonical annotation chunk bytes");
    return chunk;
}

struct Root final {
    MapAnnotationLayer layer;
    QList<QUuid> chunkIds;
};

Root decodeRoot(const ModelRecord& record, const QUuid& expectedId) {
    reject(record.kind != annotationLayerKind || record.payload.size() > static_cast<std::size_t>(maxRootBytes),
           "invalid annotation root record kind or size");
    const QByteArray payload = data(record.payload);
    QDataStream stream(payload);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint32 version = 0;
    QByteArray layerPayload;
    Root root;
    quint32 metadataBytes = 0;
    stream >> magic >> version >> metadataBytes;
    // Untrusted record lengths must be checked before allocating any list.
    reject(stream.status() != QDataStream::Ok || metadataBytes == 0 || metadataBytes > 65536,
           "invalid annotation metadata length");
    layerPayload.resize(static_cast<qsizetype>(metadataBytes));
    reject(stream.readRawData(layerPayload.data(), static_cast<int>(metadataBytes)) !=
           static_cast<int>(metadataBytes), "truncated annotation metadata");
    quint32 chunkCount = 0;
    stream >> chunkCount;
    reject(stream.status() != QDataStream::Ok || chunkCount > maxChunkCount,
           "invalid annotation chunk count");
    for (quint32 index = 0; index < chunkCount; ++index) {
        QUuid chunkId;
        stream >> chunkId;
        root.chunkIds.append(chunkId);
    }
    reject(stream.status() != QDataStream::Ok || !stream.atEnd() || magic != layerMagic ||
           version != rootVersion, "invalid annotation root encoding");
    reject(QSet<QUuid>(root.chunkIds.cbegin(), root.chunkIds.cend()).size() != root.chunkIds.size(),
           "duplicate annotation chunk UUID");
    QString error;
    const std::optional<MapAnnotationLayer> layer = ModelCodec<MapAnnotationLayer>::decode(layerPayload, &error);
    reject(!layer || layer->uuid != expectedId ||
           bytes(ModelCodec<MapAnnotationLayer>::encode(*layer)) != bytes(layerPayload),
           "invalid annotation layer metadata");
    const aowis::model::ModelValidationReport report = aowis::model::validateMapAnnotationLayer(*layer);
    reject(!report.valid(), "invalid annotation layer values");
    // A stored version with unsupported domain ownership must be rejected.
    if (layer->domain == MapAnnotationDomain::Distribution)
        reject(!layer->owner_network_uuid, "missing distribution owner");
    else if (layer->domain == MapAnnotationDomain::Drainage)
        reject(layer->owner_network_uuid.has_value(), "unsupported drainage network owner");
    root.layer = *layer;
    QList<QUuid> references = root.chunkIds;
    if (root.layer.owner_network_uuid) references.append(*root.layer.owner_network_uuid);
    reject(sortedReferences(references) != record.references, "annotation root reference mismatch");
    reject(encodeRoot(root.layer, root.chunkIds).payload != record.payload,
           "noncanonical annotation root bytes");
    return root;
}

// Reads and checks one chunk listed by a root: canonical bytes, owned by this
// layer, record UUID derived from its serial.
Chunk checkedChunk(const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
                   const QUuid& layerId, const QUuid& chunkId) {
    const std::optional<ModelRecord> record = readCurrent(key(chunkId));
    reject(!record, "missing annotation chunk");
    Chunk chunk = decodeChunk(*record, layerId);
    reject(chunkUuid(layerId, chunk.serial) != chunkId, "noncanonical annotation chunk UUID");
    return chunk;
}

void checkLayerOwner(const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
                     const Root& root) {
    if (!root.layer.owner_network_uuid) return;
    const std::optional<ModelRecord> owner = readCurrent(key(*root.layer.owner_network_uuid));
    reject(!owner || owner->kind != engineeringNetworkKind,
           "missing or invalid distribution network owner");
}

// One member listed by `root`: canonical record and an existing anchor target.
// B2 must prove that anchor targets have the declared type and belong to this
// layer's owner; never silently drop missing ones.
MapAnnotation listedMember(const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
                           const Root& root, const QUuid& id) {
    const std::optional<ModelRecord> item = readCurrent(key(id));
    reject(!item, "missing annotation member");
    const MapAnnotation decoded = decodeMember(*item, id);
    if (decoded.anchor) {
        reject(root.layer.domain != MapAnnotationDomain::Distribution,
               "unowned/drainage annotation cannot have an anchor");
        const std::optional<ModelRecord> anchor = readCurrent(key(decoded.anchor->entity_uuid));
        reject(!anchor, "annotation anchor target is missing");
    }
    return decoded;
}

} // namespace

QUuid distributionLayerUuid(const QUuid& networkId) {
    if (networkId.isNull())
        throw StorageError(StorageErrorKind::InvalidInput, "null distribution network UUID");
    QByteArray seed("aowis-distribution-annotation-layer-v1");
    seed.append(networkId.toRfc4122());
    QByteArray digest = QCryptographicHash::hash(seed, QCryptographicHash::Sha256).left(16);
    digest[6] = static_cast<char>((static_cast<unsigned char>(digest[6]) & 0x0fU) | 0x80U);
    digest[8] = static_cast<char>((static_cast<unsigned char>(digest[8]) & 0x3fU) | 0x80U);
    return QUuid::fromRfc4122(digest);
}

std::vector<ModelChange> fullDraftAnnotationLayerChanges(
    const MapAnnotationLayer& layer, const QList<MapAnnotation>& annotations) {
    validateLayer(layer);
    if (annotations.size() > static_cast<qsizetype>(maxChunkCount * annotationChunkCapacity))
        throw StorageError(StorageErrorKind::LimitExceeded, "too many annotations");
    std::set<Key> seen;
    seen.insert(key(layer.uuid));
    if (layer.owner_network_uuid) {
        if (!seen.insert(key(*layer.owner_network_uuid)).second)
            throw StorageError(StorageErrorKind::InvalidInput, "layer ID must differ from owner network ID");
    }
    std::vector<ModelChange> changes;
    changes.reserve(static_cast<std::size_t>(annotations.size()) +
                    static_cast<std::size_t>((annotations.size() + 255) / 256) + 1);
    QList<QUuid> chunkIds;
    QList<QUuid> pending;
    quint32 serial = 0;
    const std::function<void()> flush = [&]() {
        if (pending.isEmpty()) return;
        const QUuid id = chunkUuid(layer.uuid, serial);
        if (!seen.insert(key(id)).second)
            throw StorageError(StorageErrorKind::InvalidInput, "annotation chunk ID collision");
        chunkIds.append(id);
        changes.push_back(ModelChange{key(id), encodeChunk(layer.uuid, serial, pending)});
        pending.clear();
        ++serial;
    };
    for (const MapAnnotation& annotation : annotations) {
        if (!seen.insert(key(annotation.uuid)).second)
            throw StorageError(StorageErrorKind::InvalidInput, "duplicate annotation or reserved UUID");
        changes.push_back(ModelChange{key(annotation.uuid), encodeMember(annotation)});
        pending.append(annotation.uuid);
        if (pending.size() == static_cast<qsizetype>(annotationChunkCapacity)) flush();
    }
    flush();
    changes.push_back(ModelChange{key(layer.uuid), encodeRoot(layer, chunkIds)});
    return changes;
}

std::vector<ModelChange> materializeDraftAnnotationEdits(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& layerId,
    const std::vector<aowis::model::ModelEntityEdit>& edits) {
    if (layerId.isNull() || edits.empty())
        throw StorageError(StorageErrorKind::InvalidInput,
                           "annotation edits require a layer UUID and nonempty batch");
    if (edits.size() > 4096)
        throw StorageError(StorageErrorKind::LimitExceeded,
                           "too many annotation edits in a draft batch");
    const std::optional<ModelRecord> rootRecord = readCurrent(key(layerId));
    if (!rootRecord)
        throw StorageError(StorageErrorKind::NotFound, "annotation layer not found in draft");
    const Root root = decodeRoot(*rootRecord, layerId);

    // A5: membership state comes from the root and the chunks only (UUID
    // lists); member payloads are read for edited members only. Chunks are
    // edited in place: an insertion appends to the last chunk (or a new chunk
    // whose serial follows the highest listed one), a deletion removes the
    // member from its own chunk; an empty chunk is dropped and a chunk below
    // the merge threshold is merged with a neighbour when both fit. Member
    // order is the concatenation of the chunks, so it is unchanged by any of
    // these steps.
    //
    // Exact inverse: removing the last member of the last chunk never merges
    // chunks (only drops the chunk when it empties). Together with the serial
    // rule this makes "delete the member just appended" restore the previous
    // records byte for byte, which unborn History relies on when it undoes
    // its journal.
    struct Slot final {
        QUuid id;
        quint32 serial = 0;
        QList<QUuid> members;
        std::optional<ModelRecord> original;
        bool alive = true;
        bool dirty = false;
    };
    std::vector<Slot> chunkSlots;
    chunkSlots.reserve(static_cast<std::size_t>(root.chunkIds.size()) + 1);
    QHash<QUuid, std::size_t> chunkOf;
    for (const QUuid& chunkId : root.chunkIds) {
        Slot slot;
        slot.id = chunkId;
        const Chunk chunk = checkedChunk(readCurrent, layerId, chunkId);
        slot.serial = chunk.serial;
        slot.members = chunk.members;
        slot.original = readCurrent(key(chunkId));
        for (const QUuid& member : chunk.members) {
            reject(chunkOf.contains(member), "duplicate annotation member UUID");
            chunkOf.insert(member, chunkSlots.size());
        }
        chunkSlots.push_back(std::move(slot));
    }
    const QSet<QUuid> originalMembers(chunkOf.keyBegin(), chunkOf.keyEnd());
    bool rootChanged = false;

    const std::function<std::optional<std::size_t>(std::size_t)> previousAlive =
        [&chunkSlots](std::size_t index) -> std::optional<std::size_t> {
        while (index > 0) {
            --index;
            if (chunkSlots[index].alive) return index;
        }
        return std::nullopt;
    };
    const std::function<std::optional<std::size_t>(std::size_t)> nextAlive =
        [&chunkSlots](std::size_t index) -> std::optional<std::size_t> {
        for (++index; index < chunkSlots.size(); ++index)
            if (chunkSlots[index].alive) return index;
        return std::nullopt;
    };
    const std::function<void(std::size_t, std::size_t)> absorb =
        [&chunkSlots, &chunkOf, &rootChanged](std::size_t into, std::size_t from) {
        for (const QUuid& member : chunkSlots[from].members) chunkOf.insert(member, into);
        chunkSlots[into].members.append(chunkSlots[from].members);
        chunkSlots[into].dirty = true;
        chunkSlots[from].members.clear();
        chunkSlots[from].alive = false;
        chunkSlots[from].dirty = true;
        rootChanged = true;
    };
    const std::function<void(std::size_t)> compact = [&](std::size_t index) {
        Slot& slot = chunkSlots[index];
        if (slot.members.isEmpty()) {
            slot.alive = false;
            rootChanged = true;
            return;
        }
        if (slot.members.size() >= chunkMergeThreshold) return;
        const qsizetype capacity = static_cast<qsizetype>(annotationChunkCapacity);
        const std::optional<std::size_t> previous = previousAlive(index);
        if (previous && chunkSlots[*previous].members.size() + slot.members.size() <= capacity) {
            absorb(*previous, index);
            return;
        }
        const std::optional<std::size_t> following = nextAlive(index);
        if (following && slot.members.size() + chunkSlots[*following].members.size() <= capacity)
            absorb(index, *following);
    };

    // Current value of every touched member, plus its pre-batch record.
    std::map<Key, std::optional<MapAnnotation>> current;
    std::map<Key, std::optional<ModelRecord>> originalRecords;
    for (const aowis::model::ModelEntityEdit& edit : edits) {
        if (edit.entityType != QStringLiteral("map.annotation") || edit.entityId.isNull() ||
            !edit.ownerLayerId || *edit.ownerLayerId != layerId ||
            (!edit.before && !edit.after))
            throw StorageError(StorageErrorKind::InvalidInput,
                               "invalid annotation MODEL entity edit");
        const Key identity = key(edit.entityId);
        if (!current.contains(identity)) {
            std::optional<MapAnnotation> value;
            std::optional<ModelRecord> record;
            if (originalMembers.contains(edit.entityId)) {
                record = readCurrent(identity);
                reject(!record, "missing annotation member");
                value = decodeMember(*record, edit.entityId);
            }
            current.emplace(identity, value);
            originalRecords.emplace(identity, record);
        }
        std::optional<MapAnnotation>& value = current.at(identity);
        if (value.has_value() != edit.before.has_value())
            throw StorageError(StorageErrorKind::Conflict,
                               "annotation edit before-image presence is stale");
        if (value && ModelCodec<MapAnnotation>::encode(*value) != *edit.before)
            throw StorageError(StorageErrorKind::Conflict,
                               "annotation edit before-image does not match draft");

        if (edit.after) {
            QString error;
            const std::optional<MapAnnotation> decoded =
                ModelCodec<MapAnnotation>::decode(*edit.after, &error);
            if (!decoded || decoded->uuid != edit.entityId ||
                ModelCodec<MapAnnotation>::encode(*decoded) != *edit.after)
                throw StorageError(StorageErrorKind::InvalidInput,
                                   "annotation after-image is invalid or noncanonical");
            const aowis::model::ModelValidationReport report =
                aowis::model::validateMapAnnotation(*decoded);
            if (!report.valid())
                throw StorageError(StorageErrorKind::InvalidInput,
                                   report.summary().toStdString());
            if (!value) {
                // A new member UUID must not name any other record (a member
                // removed earlier in this batch is no longer a collision).
                if (!originalMembers.contains(edit.entityId) && readCurrent(identity))
                    throw StorageError(StorageErrorKind::Conflict,
                                       "annotation edit collides with a foreign model record UUID");
                const std::optional<std::size_t> last =
                    chunkSlots.empty() ? std::nullopt
                                  : (chunkSlots.back().alive ? std::optional<std::size_t>(chunkSlots.size() - 1)
                                                        : previousAlive(chunkSlots.size() - 1));
                std::size_t target = 0;
                if (last && chunkSlots[*last].members.size() <
                                static_cast<qsizetype>(annotationChunkCapacity)) {
                    target = *last;
                } else {
                    std::optional<quint32> highest;
                    for (const Slot& slot : chunkSlots)
                        if (slot.alive && (!highest || slot.serial > *highest)) highest = slot.serial;
                    if (chunkSlots.size() >= maxChunkCount * 2 || (highest && *highest == 0xffffffffU))
                        throw StorageError(StorageErrorKind::LimitExceeded,
                                           "too many annotation chunks");
                    Slot fresh;
                    fresh.serial = highest ? *highest + 1 : 0;
                    fresh.id = chunkUuid(layerId, fresh.serial);
                    // A chunk of this layer dropped earlier in the batch may
                    // have the same UUID: the new chunk takes over its stored
                    // record, so the delta has one change (or none) per key.
                    bool reused = false;
                    for (Slot& dropped : chunkSlots) {
                        if (dropped.alive || dropped.id != fresh.id) continue;
                        // At most one dropped slot still holds the record.
                        if (dropped.original) fresh.original = std::move(dropped.original);
                        dropped.original.reset();
                        dropped.dirty = false;
                        reused = true;
                    }
                    if (!reused && readCurrent(key(fresh.id)))
                        throw StorageError(StorageErrorKind::Conflict,
                                           "annotation chunk UUID collides with a foreign model record");
                    chunkSlots.push_back(std::move(fresh));
                    target = chunkSlots.size() - 1;
                    rootChanged = true;
                }
                chunkSlots[target].members.append(edit.entityId);
                chunkSlots[target].dirty = true;
                chunkOf.insert(edit.entityId, target);
            }
            value = *decoded;
        } else {
            const QHash<QUuid, std::size_t>::const_iterator owner = chunkOf.constFind(edit.entityId);
            if (owner == chunkOf.cend())
                throw StorageError(StorageErrorKind::Internal,
                                   "annotation member has no membership chunk");
            const std::size_t index = owner.value();
            const bool trailing = chunkSlots[index].members.last() == edit.entityId &&
                                  !nextAlive(index).has_value();
            chunkOf.erase(owner);
            chunkSlots[index].members.removeOne(edit.entityId);
            chunkSlots[index].dirty = true;
            value.reset();
            if (!trailing) {
                compact(index);
            } else if (chunkSlots[index].members.isEmpty()) {
                chunkSlots[index].alive = false;
                rootChanged = true;
            }
        }
    }

    std::vector<ModelChange> delta;
    for (const std::pair<const Key, std::optional<MapAnnotation>>& entry : current) {
        const std::optional<ModelRecord>& before = originalRecords.at(entry.first);
        if (!entry.second) {
            if (before) delta.push_back(ModelChange{entry.first, std::nullopt});
            continue;
        }
        const ModelRecord updated = encodeMember(*entry.second);
        if (!before || before->kind != updated.kind || before->references != updated.references ||
            before->payload != updated.payload)
            delta.push_back(ModelChange{entry.first, updated});
    }
    QList<QUuid> chunkIds;
    for (const Slot& slot : chunkSlots) {
        if (slot.alive) chunkIds.append(slot.id);
        if (!slot.dirty) continue;
        if (!slot.alive) {
            if (slot.original) delta.push_back(ModelChange{key(slot.id), std::nullopt});
            continue;
        }
        const ModelRecord encoded = encodeChunk(layerId, slot.serial, slot.members);
        if (!slot.original || slot.original->references != encoded.references ||
            slot.original->payload != encoded.payload)
            delta.push_back(ModelChange{key(slot.id), encoded});
    }
    if (rootChanged || chunkIds != root.chunkIds) {
        const ModelRecord encodedRoot = encodeRoot(root.layer, chunkIds);
        if (encodedRoot.payload != rootRecord->payload ||
            encodedRoot.references != rootRecord->references)
            delta.push_back(ModelChange{key(layerId), encodedRoot});
    }
    return delta;
}

std::optional<AnnotationLayerSnapshot> loadAnnotationLayer(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& layerId) {
    const std::optional<ModelRecord> found = readCurrent(key(layerId));
    if (!found) return std::nullopt;
    const Root root = decodeRoot(*found, layerId);
    checkLayerOwner(readCurrent, root);
    AnnotationLayerSnapshot result;
    result.layer = root.layer;
    result.chunkIds = root.chunkIds;
    std::set<Key> seen;
    seen.insert(key(layerId));
    if (root.layer.owner_network_uuid) seen.insert(key(*root.layer.owner_network_uuid));
    for (const QUuid& chunkId : root.chunkIds) {
        reject(!seen.insert(key(chunkId)).second, "duplicate annotation chunk UUID");
        const QList<QUuid> members = checkedChunk(readCurrent, layerId, chunkId).members;
        for (const QUuid& id : members) {
            reject(!seen.insert(key(id)).second, "duplicate annotation member UUID");
            result.annotations.append(listedMember(readCurrent, root, id));
        }
    }
    return result;
}

std::optional<AnnotationLayerPage> loadAnnotationLayerPage(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& layerId, std::size_t fromChunk, std::optional<std::size_t> chunkLimit,
    std::size_t memberLimit) {
    if (chunkLimit && *chunkLimit == 0)
        throw StorageError(StorageErrorKind::InvalidInput,
                           "annotation layer page needs at least one chunk");
    const std::optional<ModelRecord> found = readCurrent(key(layerId));
    if (!found) return std::nullopt;
    const Root root = decodeRoot(*found, layerId);
    const std::size_t chunkCount = static_cast<std::size_t>(root.chunkIds.size());
    if (fromChunk > chunkCount)
        throw StorageError(StorageErrorKind::InvalidInput,
                           "annotation layer page starts after the last chunk");
    checkLayerOwner(readCurrent, root);
    const std::size_t remaining = chunkCount - fromChunk;
    const std::size_t taken = chunkLimit ? std::min(remaining, *chunkLimit) : remaining;
    AnnotationLayerPage page;
    page.layer = root.layer;
    page.chunkCount = chunkCount;
    page.fromChunk = fromChunk;
    page.nextChunk = fromChunk + taken;
    QList<QUuid> members;
    for (std::size_t index = fromChunk; index < page.nextChunk; ++index) {
        members.append(checkedChunk(readCurrent, layerId,
                                    root.chunkIds.at(static_cast<qsizetype>(index))).members);
        if (static_cast<std::size_t>(members.size()) > memberLimit)
            throw StorageError(StorageErrorKind::InvalidInput,
                               "annotation layer exceeds HTTP listing limit");
    }
    std::set<Key> seen;
    for (const QUuid& id : members) {
        reject(!seen.insert(key(id)).second, "duplicate annotation member UUID");
        page.annotations.append(listedMember(readCurrent, root, id));
    }
    return page;
}

std::optional<AnnotationLayerOrder> loadAnnotationLayerOrder(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& layerId, bool includeMembers) {
    const std::optional<ModelRecord> found = readCurrent(key(layerId));
    if (!found) return std::nullopt;
    const Root root = decodeRoot(*found, layerId);
    AnnotationLayerOrder order;
    order.layer = root.layer;
    order.chunkIds = root.chunkIds;
    if (!includeMembers) return order;
    QSet<QUuid> seen;
    for (const QUuid& chunkId : root.chunkIds) {
        const Chunk chunk = checkedChunk(readCurrent, layerId, chunkId);
        for (const QUuid& member : chunk.members) {
            reject(seen.contains(member), "duplicate annotation member UUID");
            seen.insert(member);
            order.members.append(member);
        }
    }
    return order;
}

std::optional<AnnotationLayerSummary> loadAnnotationLayerSummary(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& layerId) {
    const std::optional<ModelRecord> found = readCurrent(key(layerId));
    if (!found) return std::nullopt;
    const Root root = decodeRoot(*found, layerId);
    AnnotationLayerSummary summary;
    summary.layer = root.layer;
    for (const QUuid& chunkId : root.chunkIds)
        summary.memberCount += static_cast<std::size_t>(
            checkedChunk(readCurrent, layerId, chunkId).members.size());
    return summary;
}

std::optional<AnnotationLayerDelta> checkAnnotationLayerDelta(
    const EngineeringCandidateView& before, const EngineeringCandidateView& after,
    const QUuid& layerId, const std::set<Key>& changedKeys) {
    const std::optional<ModelRecord> nowRecord = after.get(key(layerId));
    if (!nowRecord) return std::nullopt;
    const Root now = decodeRoot(*nowRecord, layerId);
    if (now.layer.owner_network_uuid) {
        const std::optional<ModelRecord> owner = after.get(key(*now.layer.owner_network_uuid));
        reject(!owner || owner->kind != engineeringNetworkKind,
               "missing or invalid distribution network owner");
    }
    AnnotationLayerDelta result;
    result.layer = now.layer;
    const std::optional<ModelRecord> oldRecord = before.get(key(layerId));
    if (!oldRecord) {
        result.metadataChanged = true;
        return result;
    }
    const Root old = decodeRoot(*oldRecord, layerId);
    if (ModelCodec<MapAnnotationLayer>::encode(old.layer) !=
        ModelCodec<MapAnnotationLayer>::encode(now.layer)) {
        result.metadataChanged = true;
        return result;
    }
    // Unchanged chunks were checked when they were written; decodeRoot has
    // already refused a root that lists a chunk twice.
    const QSet<QUuid> oldChunks(old.chunkIds.cbegin(), old.chunkIds.cend());
    for (const QUuid& chunkId : now.chunkIds) {
        if (oldChunks.contains(chunkId) && !changedKeys.contains(key(chunkId))) continue;
        const Chunk chunk = checkedChunk(after.get, layerId, chunkId);
        for (const QUuid& member : chunk.members) {
            bool stayed = false;
            for (const Key& referrer : before.referrers(key(member))) {
                const std::optional<ModelRecord> previous = before.get(referrer);
                if (previous && previous->kind == annotationChunkKind &&
                    oldChunks.contains(record::uuidFromKey(referrer)))
                    stayed = true;
            }
            if (!stayed) result.enteredMembers.push_back(member);
        }
    }
    return result;
}

std::optional<MapAnnotation> loadAnnotationMember(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& annotationId) {
    const std::optional<ModelRecord> found = readCurrent(key(annotationId));
    if (!found) return std::nullopt;
    return decodeMember(*found, annotationId);
}

std::optional<MapAnnotationLayer> loadAnnotationLayerMetadata(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& layerId) {
    const std::optional<ModelRecord> found = readCurrent(key(layerId));
    if (!found) return std::nullopt;
    return decodeRoot(*found, layerId).layer;
}

std::optional<AnnotationLayerSnapshot> loadAnnotationLayerAtRevision(
    Repository& store, const ProjectId& project, const RevisionId& revision,
    const QUuid& layerId) {
    return loadAnnotationLayer([&store, &project, &revision](const Key& recordKey) {
        return store.getModelAtRevision(project, revision, recordKey);
    }, layerId);
}

} // namespace aowis::dvmdb::annotations
