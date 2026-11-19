#include "aowis/dvmdb/engineering_delta_scope.h"

#include "aowis/dvmdb/storage_error.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace aowis::dvmdb {
namespace {

using RecordReader = std::function<std::optional<ModelRecord>(const Key&)>;
using ReferrerReader = std::function<std::vector<Key>(const Key&)>;

void invalid(bool condition, const char* message) {
    if (condition) throw StorageError(StorageErrorKind::InvalidInput, message);
}

bool sameRecord(const std::optional<ModelRecord>& first, const std::optional<ModelRecord>& second) {
    if (first.has_value() != second.has_value()) return false;
    if (!first) return true;
    return first->kind == second->kind && first->references == second->references &&
           first->payload == second->payload;
}

// The container kind that OWNS a record of this kind (exactly one owner).
std::optional<std::uint16_t> ownerKindFor(std::uint16_t kind) {
    if (kind == engineeringNetworkChunkKind) return engineeringNetworkKind;
    if (isHydraulicMemberKind(kind)) return engineeringNetworkChunkKind;
    if (kind == engineeringLayerChunkKind) return engineeringLayerKind;
    if (kind == engineeringAnnotationKind) return engineeringLayerChunkKind;
    return std::nullopt;
}

bool isContainerKind(std::uint16_t kind) {
    return kind == engineeringNetworkKind || kind == engineeringNetworkChunkKind ||
           kind == engineeringLayerKind || kind == engineeringLayerChunkKind;
}

// Kinds a container may reference. A layer root also references its owner
// network, which it does not own.
bool allowedChild(std::uint16_t container, std::uint16_t child) {
    switch (container) {
    case engineeringNetworkKind:
        return child == engineeringNetworkChunkKind;
    case engineeringNetworkChunkKind:
        return isHydraulicMemberKind(child);
    case engineeringLayerKind:
        return child == engineeringLayerChunkKind || child == engineeringNetworkKind;
    case engineeringLayerChunkKind:
        return child == engineeringAnnotationKind;
    default:
        return false;
    }
}

struct Entry final {
    std::optional<ModelRecord> before;
    std::optional<ModelRecord> after;
};

void insertAll(std::set<Key>& target, const std::vector<Key>& source) {
    target.insert(source.begin(), source.end());
}

} // namespace

EngineeringCandidateView overlayEngineeringCandidateView(
    RecordReader baseGet, ReferrerReader baseReferrers,
    const std::vector<std::vector<ModelChange>>& overlays) {
    struct State final {
        RecordReader baseGet;
        ReferrerReader baseReferrers;
        std::map<Key, std::optional<ModelRecord>> records;
        std::map<Key, std::set<Key>> incoming;
    };
    const std::shared_ptr<State> state = std::make_shared<State>();
    state->baseGet = std::move(baseGet);
    state->baseReferrers = std::move(baseReferrers);
    for (const std::vector<ModelChange>& overlay : overlays)
        for (const ModelChange& change : overlay)
            state->records[change.key] = change.record;
    for (const std::pair<const Key, std::optional<ModelRecord>>& entry : state->records) {
        if (!entry.second) continue;
        for (const Key& reference : entry.second->references)
            state->incoming[reference].insert(entry.first);
    }
    EngineeringCandidateView view;
    view.get = [state](const Key& key) -> std::optional<ModelRecord> {
        const std::map<Key, std::optional<ModelRecord>>::const_iterator found =
            state->records.find(key);
        if (found != state->records.end()) return found->second;
        if (!state->baseGet) return std::nullopt;
        return state->baseGet(key);
    };
    view.referrers = [state](const Key& key) -> std::vector<Key> {
        std::set<Key> result;
        if (state->baseReferrers) {
            // A base referrer replaced or deleted by an overlay is reported
            // only if its overlay record still references the key (below).
            for (const Key& referrer : state->baseReferrers(key))
                if (!state->records.contains(referrer)) result.insert(referrer);
        }
        const std::map<Key, std::set<Key>>::const_iterator found = state->incoming.find(key);
        if (found != state->incoming.end()) result.insert(found->second.begin(), found->second.end());
        return std::vector<Key>(result.begin(), result.end());
    };
    return view;
}

std::vector<Key> engineeringReferrersOfKind(const EngineeringCandidateView& view,
                                            const Key& key, std::uint16_t kind) {
    std::vector<Key> result;
    for (const Key& referrer : view.referrers(key)) {
        const std::optional<ModelRecord> record = view.get(referrer);
        if (!record)
            throw StorageError(StorageErrorKind::Corruption,
                               "reverse-reference index names a missing record");
        if (record->kind == kind) result.push_back(referrer);
    }
    return result;
}

EngineeringDeltaScope engineeringDeltaScope(const EngineeringCandidateView& before,
                                            const EngineeringCandidateView& after,
                                            const std::vector<ModelChange>& delta) {
    EngineeringDeltaScope scope;
    std::map<Key, Entry> entries;
    std::set<Key> unique;
    for (const ModelChange& change : delta)
        invalid(!unique.insert(change.key).second, "duplicate record key in engineering delta");
    for (const ModelChange& change : delta) {
        Entry entry{before.get(change.key), after.get(change.key)};
        if (!sameRecord(entry.after, change.record))
            throw StorageError(StorageErrorKind::Internal,
                               "candidate view does not reflect the engineering delta");
        entries.emplace(change.key, std::move(entry));
    }
    for (const std::pair<const Key, Entry>& item : entries) {
        const Entry& entry = item.second;
        for (const std::optional<ModelRecord>* side : {&entry.before, &entry.after}) {
            // Generic records follow the conditional complete-candidate rule.
            if (*side && !isEngineeringRecordKind((*side)->kind)) {
                scope.fullValidationRequired = true;
                return scope;
            }
        }
        invalid(entry.before && entry.after && entry.before->kind != entry.after->kind,
                "native record kind cannot change in place");
        if (!entry.before && !entry.after) continue;
        const std::uint16_t kind = entry.after ? entry.after->kind : entry.before->kind;
        // Root creation/deletion changes project-wide cardinality (one layer
        // per network, at most one Drainage layer): complete validation.
        if ((kind == engineeringNetworkKind || kind == engineeringLayerKind) &&
            entry.before.has_value() != entry.after.has_value()) {
            scope.fullValidationRequired = true;
            return scope;
        }
    }

    std::set<Key> ownershipChecks;
    std::set<Key> deleted;
    std::set<Key> hydraulicTouched;
    std::set<Key> networkChunks;
    std::set<Key> layerChunks;
    std::set<Key> layerOwners;
    for (const std::pair<const Key, Entry>& item : entries) {
        const Key& id = item.first;
        const Entry& entry = item.second;
        if (sameRecord(entry.before, entry.after)) continue;
        const std::uint16_t kind = entry.after ? entry.after->kind : entry.before->kind;
        if (!entry.after) deleted.insert(id);
        if (entry.after) {
            for (const Key& reference : entry.after->references) {
                const std::optional<ModelRecord> target = after.get(reference);
                invalid(!target, "engineering record references a missing record");
                invalid(isContainerKind(kind) && !allowedChild(kind, target->kind),
                        "engineering container references a wrong-kind record");
            }
            if (ownerKindFor(kind)) ownershipChecks.insert(id);
        }
        if (isContainerKind(kind)) {
            // Every child added to or dropped from this container must end
            // with exactly one owner (or be deleted) in the candidate.
            std::set<Key> previous;
            std::set<Key> current;
            if (entry.before) previous.insert(entry.before->references.begin(), entry.before->references.end());
            if (entry.after) current.insert(entry.after->references.begin(), entry.after->references.end());
            std::vector<Key> moved;
            std::set_symmetric_difference(previous.begin(), previous.end(),
                                          current.begin(), current.end(),
                                          std::back_inserter(moved));
            for (const Key& child : moved) {
                const std::optional<ModelRecord> record = after.get(child);
                if (!record || ownerKindFor(record->kind) != kind) continue;
                ownershipChecks.insert(child);
                if (isHydraulicMemberKind(record->kind)) hydraulicTouched.insert(child);
                if (record->kind == engineeringNetworkChunkKind) networkChunks.insert(child);
                if (record->kind == engineeringLayerChunkKind) layerChunks.insert(child);
            }
        }
        if (kind == engineeringNetworkKind) {
            scope.networks.insert(id);
        } else if (kind == engineeringNetworkChunkKind) {
            networkChunks.insert(id);
            if (entry.before)
                insertAll(scope.networks, engineeringReferrersOfKind(before, id, engineeringNetworkKind));
        } else if (isHydraulicMemberKind(kind)) {
            hydraulicTouched.insert(id);
            if (entry.before) {
                for (const Key& chunk : engineeringReferrersOfKind(before, id, engineeringNetworkChunkKind))
                    insertAll(scope.networks, engineeringReferrersOfKind(before, chunk, engineeringNetworkKind));
            }
            if (entry.after) {
                for (const Key& chunk : engineeringReferrersOfKind(after, id, engineeringNetworkChunkKind))
                    networkChunks.insert(chunk);
            }
        } else if (kind == engineeringLayerKind) {
            scope.layers.insert(id);
            // A changed owner reference changes the layer count of the old
            // and the new owner network.
            for (const std::optional<ModelRecord>* side : {&entry.before, &entry.after}) {
                if (!*side) continue;
                for (const Key& reference : (*side)->references) {
                    const std::optional<ModelRecord> target = after.get(reference);
                    if (target && target->kind == engineeringNetworkKind)
                        layerOwners.insert(reference);
                }
            }
        } else if (kind == engineeringLayerChunkKind) {
            layerChunks.insert(id);
            if (entry.before)
                insertAll(scope.layers, engineeringReferrersOfKind(before, id, engineeringLayerKind));
        } else if (kind == engineeringAnnotationKind) {
            if (entry.after) scope.annotations.insert(id);
        }
    }

    // No surviving record may still reference a deleted record: this covers
    // a chunk listing a removed member, a pipe naming a removed node and an
    // annotation anchored to a removed target.
    for (const Key& id : deleted)
        invalid(!after.referrers(id).empty(), "deleted engineering record is still referenced");
    for (const Key& id : ownershipChecks) {
        const std::optional<ModelRecord> record = after.get(id);
        if (!record) continue;
        const std::uint16_t ownerKind = *ownerKindFor(record->kind);
        const std::vector<Key> owners = engineeringReferrersOfKind(after, id, ownerKind);
        invalid(owners.empty(), "engineering record has no owner in the candidate");
        invalid(owners.size() > 1, "engineering record has multiple owners in the candidate");
    }
    for (const Key& chunk : networkChunks) {
        if (!after.get(chunk)) continue;
        insertAll(scope.networks, engineeringReferrersOfKind(after, chunk, engineeringNetworkKind));
    }
    for (const Key& chunk : layerChunks) {
        if (!after.get(chunk)) continue;
        insertAll(scope.layers, engineeringReferrersOfKind(after, chunk, engineeringLayerKind));
    }
    // A changed or moved anchor target may invalidate annotations anchored to
    // it (type, owning network). Re-check exactly those members.
    for (const Key& member : hydraulicTouched) {
        if (!after.get(member)) continue;
        insertAll(scope.annotations, engineeringReferrersOfKind(after, member, engineeringAnnotationKind));
    }
    for (const Key& network : scope.networks) {
        if (!after.get(network)) {
            scope.fullValidationRequired = true;
            return scope;
        }
    }
    layerOwners.insert(scope.networks.begin(), scope.networks.end());
    for (const Key& network : layerOwners) {
        const std::vector<Key> layers = engineeringReferrersOfKind(after, network, engineeringLayerKind);
        invalid(layers.size() != 1, "distribution network must have exactly one annotation layer");
    }
    for (const Key& layer : scope.layers) {
        if (!after.get(layer)) {
            scope.fullValidationRequired = true;
            return scope;
        }
    }
    return scope;
}

} // namespace aowis::dvmdb
