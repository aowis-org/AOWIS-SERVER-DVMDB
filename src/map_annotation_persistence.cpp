#include "aowis/dvmdb/map_annotation_persistence.h"
#include "aowis/dvmdb/model_codec.h"
#include "aowis/dvmdb/model_record_conversion.h"
#include "aowis/dvmdb/storage_error.h"

#include <aowis/model/serialization/model_validation.h>

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>

#include <algorithm>
#include <set>
#include <utility>

namespace aowis::dvmdb::annotations {
namespace {

constexpr quint32 layerMagic = 0x414E4C31U; // ANL1
constexpr quint32 chunkMagic = 0x414E4331U; // ANC1
constexpr quint32 recordVersion = 1;
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
    stream << chunkMagic << recordVersion << layerId << serial << members;
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
    stream << layerMagic << recordVersion << layerPayload << chunkIds;
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

QList<QUuid> decodeChunk(const ModelRecord& record, const QUuid& layerId, quint32 serial) {
    reject(record.kind != annotationChunkKind || record.payload.size() > static_cast<std::size_t>(maxChunkBytes),
           "invalid annotation chunk record kind or size");
    const QByteArray payload = data(record.payload);
    QDataStream stream(payload);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint32 version = 0;
    quint32 storedSerial = 0;
    QUuid storedLayer;
    QList<QUuid> members;
    quint32 memberCount = 0;
    stream >> magic >> version >> storedLayer >> storedSerial >> memberCount;
    reject(stream.status() != QDataStream::Ok || memberCount == 0 ||
           memberCount > annotationChunkCapacity,
           "invalid annotation chunk member count");
    for (quint32 index = 0; index < memberCount; ++index) {
        QUuid member;
        stream >> member;
        members.append(member);
    }
    reject(stream.status() != QDataStream::Ok || !stream.atEnd() || magic != chunkMagic ||
           version != recordVersion || storedLayer != layerId || storedSerial != serial,
           "invalid annotation chunk encoding");
    reject(encodeChunk(layerId, serial, members).references != record.references,
           "annotation chunk membership references mismatch");
    reject(encodeChunk(layerId, serial, members).payload != record.payload,
           "noncanonical annotation chunk bytes");
    return members;
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
           version != recordVersion, "invalid annotation root encoding");
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

} // namespace

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

std::optional<AnnotationLayerSnapshot> loadAnnotationLayer(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& layerId) {
    const std::optional<ModelRecord> found = readCurrent(key(layerId));
    if (!found) return std::nullopt;
    const Root root = decodeRoot(*found, layerId);
    if (root.layer.owner_network_uuid) {
        const std::optional<ModelRecord> owner = readCurrent(key(*root.layer.owner_network_uuid));
        reject(!owner || owner->kind != 100, "missing or invalid distribution network owner");
    }
    AnnotationLayerSnapshot result;
    result.layer = root.layer;
    std::set<Key> seen;
    seen.insert(key(layerId));
    if (root.layer.owner_network_uuid) seen.insert(key(*root.layer.owner_network_uuid));
    for (qsizetype index = 0; index < root.chunkIds.size(); ++index) {
        const QUuid chunkId = root.chunkIds.at(index);
        reject(chunkId != chunkUuid(layerId, static_cast<quint32>(index)),
               "noncanonical annotation chunk UUID");
        reject(!seen.insert(key(chunkId)).second, "duplicate annotation chunk UUID");
        const std::optional<ModelRecord> chunk = readCurrent(key(chunkId));
        reject(!chunk, "missing annotation chunk");
        const QList<QUuid> members = decodeChunk(*chunk, layerId, static_cast<quint32>(index));
        for (const QUuid& id : members) {
            reject(!seen.insert(key(id)).second, "duplicate annotation member UUID");
            const std::optional<ModelRecord> item = readCurrent(key(id));
            reject(!item, "missing annotation member");
            const MapAnnotation decoded = decodeMember(*item, id);
            // B2 must prove that anchor targets have the declared type and
            // belong to this layer's owner; never silently drop missing ones.
            if (decoded.anchor) {
                reject(root.layer.domain != MapAnnotationDomain::Distribution,
                       "unowned/drainage annotation cannot have an anchor");
                const std::optional<ModelRecord> anchor = readCurrent(key(decoded.anchor->entity_uuid));
                reject(!anchor, "annotation anchor target is missing");
            }
            result.annotations.append(decoded);
        }
    }
    return result;
}

std::optional<AnnotationLayerSnapshot> loadAnnotationLayerAtRevision(
    Repository& store, const ProjectId& project, const RevisionId& revision,
    const QUuid& layerId) {
    return loadAnnotationLayer([&store, &project, &revision](const Key& recordKey) {
        return store.getModelAtRevision(project, revision, recordKey);
    }, layerId);
}

} // namespace aowis::dvmdb::annotations
