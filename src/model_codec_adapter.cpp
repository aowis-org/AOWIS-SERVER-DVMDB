#include "aowis/dvmdb/model_codec_adapter.h"
#include "aowis/dvmdb/model_record_conversion.h"
#include "aowis/dvmdb/storage_error.h"

#include <algorithm>
#include <utility>

namespace aowis::dvmdb::hydraulic {

Key keyFromUuid(const QUuid& uuid) { return record::keyFromUuid(uuid); }
QUuid uuidFromKey(const Key& key) { return record::uuidFromKey(key); }
Bytes bytesFromQByteArray(const QByteArray& bytes) { return record::bytesFromQByteArray(bytes); }
QByteArray qByteArrayFromBytes(const Bytes& bytes) { return record::qByteArrayFromBytes(bytes); }

ModelChange changeFor(const HydraulicNodeJunction& junction) {
    ModelRecord record;
    record.kind = junctionKind;
    record.payload = bytesFromQByteArray(aowis::dvmdb::ModelCodec<HydraulicNodeJunction>::encode(junction));
    if (record.payload.empty()) throw StorageError(StorageErrorKind::InvalidInput, "junction has a non-finite number or an undeclared enumerator");
    return ModelChange{keyFromUuid(junction.uuid), std::move(record)};
}

ModelChange changeFor(const HydraulicLinkPipe& pipe) {
    ModelRecord record;
    record.kind = pipeKind;
    record.references.push_back(keyFromUuid(pipe.node_uuid_from));
    record.references.push_back(keyFromUuid(pipe.node_uuid_to));
    record.payload = bytesFromQByteArray(aowis::dvmdb::ModelCodec<HydraulicLinkPipe>::encode(pipe));
    if (record.payload.empty()) throw StorageError(StorageErrorKind::InvalidInput, "pipe has a non-finite number or an undeclared enumerator");
    return ModelChange{keyFromUuid(pipe.uuid), std::move(record)};
}

std::optional<HydraulicNodeJunction> readJunction(Repository& store, const ProjectId& project,
                                                 const RevisionId& revision, const Key& key,
                                                 QString* error) {
    const std::optional<ModelRecord> record = store.getModelAtRevision(project, revision, key);
    if (!record) return std::nullopt;
    if (record->kind != junctionKind) {
        if (error) *error = QStringLiteral("Model record kind is not a junction");
        return std::nullopt;
    }
    std::optional<HydraulicNodeJunction> result =
        aowis::dvmdb::ModelCodec<HydraulicNodeJunction>::decode(qByteArrayFromBytes(record->payload), error);
    if (result && result->uuid != uuidFromKey(key)) {
        if (error) *error = QStringLiteral("Model record UUID differs from its tree key");
        return std::nullopt;
    }
    return result;
}

std::optional<HydraulicLinkPipe> readPipe(Repository& store, const ProjectId& project,
                                          const RevisionId& revision, const Key& key,
                                          QString* error) {
    const std::optional<ModelRecord> record = store.getModelAtRevision(project, revision, key);
    if (!record) return std::nullopt;
    if (record->kind != pipeKind) {
        if (error) *error = QStringLiteral("Model record kind is not a pipe");
        return std::nullopt;
    }
    std::optional<HydraulicLinkPipe> result =
        aowis::dvmdb::ModelCodec<HydraulicLinkPipe>::decode(qByteArrayFromBytes(record->payload), error);
    if (result) {
        if (result->uuid != uuidFromKey(key) ||
            result->node_uuid_from.isNull() || result->node_uuid_to.isNull()) {
            if (error) *error = QStringLiteral("Pipe identity or endpoints invalid");
            return std::nullopt;
        }
        std::vector<Key> expected{keyFromUuid(result->node_uuid_from),
                                  keyFromUuid(result->node_uuid_to)};
        std::sort(expected.begin(), expected.end());
        expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
        if (record->references != expected) {
            if (error) *error = QStringLiteral("Pipe endpoint references differ from canonical model payload");
            return std::nullopt;
        }
    }
    return result;
}

}  // namespace aowis::dvmdb::hydraulic
