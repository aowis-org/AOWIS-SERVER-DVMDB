#include "aowis/dvmdb/map_annotation_action_replay.h"

#include "aowis/dvmdb/map_annotation_persistence.h"
#include "aowis/dvmdb/map_annotation_snapshot_validation.h"
#include "aowis/dvmdb/model_record_conversion.h"

#include <aowis/model/edit/model_edit_transaction.h>

#include <QByteArray>
#include <QSet>

#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace aowis::dvmdb::annotations {
namespace {

using Records = std::map<Key, ModelRecord>;
constexpr std::size_t maximumArchiveActions = 4096;
constexpr std::size_t maximumArchiveBytes = 8U * 1024U * 1024U;
constexpr std::size_t maximumSnapshotRecords = 200000;
constexpr std::size_t maximumSnapshotBytes = 256U * 1024U * 1024U;

bool equalRecord(const ModelRecord& first, const ModelRecord& second) {
    return first.kind == second.kind && first.payload == second.payload &&
           first.references == second.references;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

MergeCandidate view(const Records& records) {
    MergeCandidate candidate;
    candidate.get = [&records](const Key& key) -> std::optional<ModelRecord> {
        const Records::const_iterator found = records.find(key);
        return found != records.end() ? std::optional<ModelRecord>(found->second)
                                      : std::nullopt;
    };
    candidate.forEach = [&records](
            const std::function<void(const Key&, const ModelRecord&)>& visitor) {
        for (const std::pair<const Key, ModelRecord>& entry : records)
            visitor(entry.first, entry.second);
    };
    return candidate;
}

Records copyRecords(const MergeCandidate& candidate) {
    Records result;
    std::size_t bytes = 0;
    candidate.forEach([&result, &bytes](const Key& key, const ModelRecord& record) {
        require(result.size() < maximumSnapshotRecords, "annotation replay model record limit exceeded");
        require(record.payload.size() <= maximumSnapshotBytes - bytes,
                "annotation replay model byte limit exceeded");
        bytes += record.payload.size();
        require(result.emplace(key, record).second, "duplicate native model key in replay candidate");
    });
    return result;
}

void validate(const Records& records) {
    const std::optional<std::string> problem = validateAnnotationSnapshot(view(records));
    if (problem) throw std::runtime_error("invalid annotation replay snapshot: " + *problem);
}

std::vector<ModelChange> changesBetween(const Records& parent, const Records& child) {
    std::vector<ModelChange> changes;
    for (const std::pair<const Key, ModelRecord>& entry : child) {
        const Records::const_iterator found = parent.find(entry.first);
        if (found == parent.end() || !equalRecord(found->second, entry.second))
            changes.push_back(ModelChange{entry.first, entry.second});
    }
    for (const std::pair<const Key, ModelRecord>& entry : parent)
        if (!child.contains(entry.first))
            changes.push_back(ModelChange{entry.first, std::nullopt});
    return changes;
}

void requireSameChanges(const std::vector<ModelChange>& expected,
                        const std::vector<ModelChange>& supplied) {
    std::map<Key, std::optional<ModelRecord>> expectedByKey;
    std::map<Key, std::optional<ModelRecord>> suppliedByKey;
    for (const ModelChange& change : expected)
        require(expectedByKey.emplace(change.key, change.record).second,
                "duplicate expected annotation replay delta key");
    for (const ModelChange& change : supplied)
        require(suppliedByKey.emplace(change.key, change.record).second,
                "duplicate native witness delta key");
    require(expectedByKey.size() == suppliedByKey.size(),
            "archived annotation native delta lacks exact record coverage");
    for (const std::pair<const Key, std::optional<ModelRecord>>& entry : expectedByKey) {
        const std::map<Key, std::optional<ModelRecord>>::const_iterator found =
            suppliedByKey.find(entry.first);
        require(found != suppliedByKey.end() && entry.second.has_value() == found->second.has_value(),
                "native witness has an unexpected annotation delta key");
        if (entry.second)
            require(equalRecord(*entry.second, *found->second),
                    "native witness annotation delta bytes differ from immutable revision");
    }
}

// This is an attribution proof, not a heuristic. Members which exist at
// neither endpoint have no durable layer provenance in the current MODEL
// history envelope and must fail, even if only one layer is present.
std::map<Key, Key> knownOwners(const Records& parent, const Records& committed) {
    std::map<Key, Key> owners;
    const std::function<void(const Records&)> collect = [&owners](const Records& records) {
        const MergeCandidate candidate = view(records);
        for (const std::pair<const Key, ModelRecord>& entry : records) {
            if (entry.second.kind != annotationLayerKind) continue;
            const QUuid layerId = record::uuidFromKey(entry.first);
            const std::optional<AnnotationLayerSnapshot> layer =
                loadAnnotationLayer(candidate.get, layerId);
            require(layer.has_value(), "missing annotation layer during history attribution");
            for (const MapAnnotation& annotation : layer->annotations) {
                const Key member = record::keyFromUuid(annotation.uuid);
                const std::map<Key, Key>::const_iterator owner = owners.find(member);
                require(owner == owners.end() || owner->second == entry.first,
                        "annotation ownership changed between historical endpoints");
                owners[member] = entry.first;
            }
        }
    };
    collect(parent);
    collect(committed);
    return owners;
}

// The immutable local Commit sidecar authenticates this exact ordered chain.
// Replay must check it before applying actions: endpoint equality cannot prove
// the order of two causally independent edits.
Hash archiveActionChain(const std::vector<WorkingCopyEvent>& actions) {
    Hash chained{};
    for (const WorkingCopyEvent& action : actions) {
        Bytes input(chained.begin(), chained.end());
        input.insert(input.end(), action.encoded.begin(), action.encoded.end());
        chained = sha256(input);
    }
    return chained;
}

void applyChanges(Records& records, const std::vector<ModelChange>& changes) {
    for (const ModelChange& change : changes) {
        if (change.record) records[change.key] = *change.record;
        else records.erase(change.key);
    }
}

} // namespace

std::optional<VerifiedAnnotationActionBoundary> replayVerifiedAnnotationActionBoundary(
        const MergeCandidate& parent, const MergeCandidate& committed,
        const std::vector<WorkingCopyEvent>& actions,
        const std::vector<ModelChange>& actualNativeChanges,
        const Hash& authenticatedActionChain,
        std::size_t actionPosition, QString* error) {
    if (error) error->clear();
    const std::function<std::optional<VerifiedAnnotationActionBoundary>(const QString&)> fail =
        [error](const QString& reason) -> std::optional<VerifiedAnnotationActionBoundary> {
            if (error) *error = reason;
            return std::nullopt;
        };
    try {
        require(actions.size() <= maximumArchiveActions && actionPosition <= actions.size(),
                "annotation archive action boundary is out of range");
        // Check the authenticated action order before accepting any boundary.
        // Transaction identities are not sequence numbers, even for UUIDv7.
        std::size_t chainedBytes = 0;
        for (const WorkingCopyEvent& action : actions) {
            require(!action.encoded.empty() &&
                    action.encoded.size() <= maximumArchiveBytes - chainedBytes,
                    "annotation archive exceeds verified byte budget");
            chainedBytes += action.encoded.size();
        }
        require(archiveActionChain(actions) == authenticatedActionChain,
                "annotation archive action order differs from authenticated Commit chain");
        const Records before = copyRecords(parent);
        const Records after = copyRecords(committed);
        validate(before);
        validate(after);
        const std::map<Key, Key> owners = knownOwners(before, after);
        const std::vector<ModelChange> nativeDelta = changesBetween(before, after);
        requireSameChanges(nativeDelta, actualNativeChanges);

        Records current = before;
        Records selected = before;
        std::set<Key> changedLayers;
        QSet<QUuid> transactionsSeen;
        std::size_t totalBytes = 0;
        for (std::size_t index = 0; index < actions.size(); ++index) {
            const WorkingCopyEvent& archived = actions[index];
            require(!archived.encoded.empty() &&
                    archived.encoded.size() <= maximumArchiveBytes - totalBytes,
                    "annotation archive exceeds verified byte budget");
            totalBytes += archived.encoded.size();
            QString decodeError;
            const std::optional<aowis::model::ModelEditTransaction> transaction =
                aowis::model::ModelEditTransactionCodec::decode(
                    record::qByteArrayFromBytes(archived.encoded), &decodeError);
            require(transaction.has_value() &&
                    !transactionsSeen.contains(transaction->transactionId) &&
                    transaction->transactionId.toString(QUuid::WithoutBraces).toStdString() == archived.id,
                    "invalid or duplicate archived annotation transaction identity");
            transactionsSeen.insert(transaction->transactionId);
            require(!transaction->edits.empty(), "empty archived annotation transaction");
            std::set<Key> actionEntities;
            std::map<Key, std::vector<aowis::model::ModelEntityEdit>> byLayer;
            for (const aowis::model::ModelEntityEdit& edit : transaction->edits) {
                require(edit.entityType == QStringLiteral("map.annotation"),
                        "mixed/non-annotation archive needs multi-domain replay");
                const Key entityId = record::keyFromUuid(edit.entityId);
                require(!edit.entityId.isNull() && actionEntities.insert(entityId).second,
                        "repeated or null annotation UUID within archived transaction");
                const std::map<Key, Key>::const_iterator owner = owners.find(entityId);
                require(owner != owners.end(),
                        "annotation history lacks durable layer ownership provenance");
                byLayer[owner->second].push_back(edit);
            }
            // All layer-specific native deltas are materialized against the
            // same semantic event. Each layer retains its own membership order.
            for (const std::pair<const Key, std::vector<aowis::model::ModelEntityEdit>>& part : byLayer) {
                const QUuid layerId = record::uuidFromKey(part.first);
                const MergeCandidate pinned = view(current);
                const std::vector<ModelChange> delta =
                    materializeDraftAnnotationEdits(pinned.get, layerId, part.second);
                applyChanges(current, delta);
                changedLayers.insert(part.first);
            }
            validate(current);
            if (index + 1 == actionPosition) selected = current;
        }
        // Membership manifests, member bytes and unrelated native records
        // must ALL match the real immutable child, not just changed objects.
        require(current.size() == after.size(),
                "annotation MODEL archive does not cover the committed native snapshot");
        for (const std::pair<const Key, ModelRecord>& entry : after) {
            const Records::const_iterator reproduced = current.find(entry.first);
            require(reproduced != current.end() && equalRecord(reproduced->second, entry.second),
                    "annotation MODEL archive differs from committed native records");
        }
        VerifiedAnnotationActionBoundary result;
        result.actionCount = actions.size();
        result.changesFromParent = changesBetween(before, selected);
        for (const Key& key : changedLayers)
            result.changedLayerIds.push_back(record::uuidFromKey(key));
        return result;
    } catch (const std::exception& failure) {
        return fail(QString::fromUtf8(failure.what()));
    }
}

} // namespace aowis::dvmdb::annotations
