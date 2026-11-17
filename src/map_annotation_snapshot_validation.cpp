#include "aowis/dvmdb/map_annotation_snapshot_validation.h"

#include "aowis/dvmdb/hydraulic_distribution_network_persistence.h"
#include "aowis/dvmdb/map_annotation_persistence.h"
#include "aowis/dvmdb/model_record_conversion.h"
#include "aowis/dvmdb/storage_error.h"

#include <aowis/model/map/map_annotation.h>

#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace aowis::dvmdb::annotations {
namespace {

using AnchorTypes = std::map<Key, QString>;

void ensure(bool condition, const char* description) {
    if (!condition)
        throw StorageError(StorageErrorKind::Corruption, description);
}

bool isHydraulicMemberKind(std::uint16_t kind) {
    return kind >= 1 && kind <= 17;
}

bool isKnownKind(std::uint16_t kind) {
    return isHydraulicMemberKind(kind) || kind == 100 || kind == 101 ||
           kind == annotationLayerKind || kind == annotationChunkKind ||
           kind == annotationMemberKind;
}

void own(std::set<Key>& owned, const Key& id, const char* description) {
    ensure(owned.insert(id).second, description);
}

void insertAnchor(AnchorTypes& anchors, const QUuid& uuid, const QString& type) {
    ensure(anchors.emplace(record::keyFromUuid(uuid), type).second,
           "duplicate anchor-capable hydraulic entity UUID");
}

AnchorTypes allowedAnchors(const NetworkHydraulicDistribution& network) {
    AnchorTypes anchors;
    for (const HydraulicNodeJunction& item : network.nodes_junctions)
        insertAnchor(anchors, item.uuid, QStringLiteral("HydraulicNodeJunction"));
    for (const HydraulicNodeReservoir& item : network.nodes_reservoirs)
        insertAnchor(anchors, item.uuid, QStringLiteral("HydraulicNodeReservoir"));
    for (const HydraulicNodeTank& item : network.nodes_tanks)
        insertAnchor(anchors, item.uuid, QStringLiteral("HydraulicNodeTank"));
    for (const HydraulicDemandPoint& item : network.demand_points)
        insertAnchor(anchors, item.uuid, QStringLiteral("HydraulicDemandPoint"));
    return anchors;
}

void collectHydraulicOwnership(const MergeCandidate& candidate, const Key& rootId,
                               std::set<Key>& owned) {
    const std::optional<ModelRecord> root = candidate.get(rootId);
    ensure(root && root->kind == hydraulic::hydraulicDistributionNetworkKind,
           "missing or invalid hydraulic root in candidate");
    own(owned, rootId, "duplicate hydraulic network root ownership");
    // The hydraulic loader verifies these native manifest/chunk references
    // against canonical MODEL payloads before we use the indexed closure.
    for (const Key& chunkId : root->references) {
        const std::optional<ModelRecord> chunk = candidate.get(chunkId);
        ensure(chunk && chunk->kind == 101,
               "missing or invalid hydraulic membership chunk");
        own(owned, chunkId, "hydraulic membership chunk belongs to multiple networks");
        for (const Key& memberId : chunk->references) {
            const std::optional<ModelRecord> member = candidate.get(memberId);
            ensure(member && isHydraulicMemberKind(member->kind),
                   "missing or invalid hydraulic collection member");
            own(owned, memberId, "hydraulic entity belongs to multiple networks");
        }
    }
}

void collectAnnotationOwnership(const MergeCandidate& candidate, const Key& layerId,
                                std::set<Key>& owned) {
    const std::optional<ModelRecord> root = candidate.get(layerId);
    ensure(root && root->kind == annotationLayerKind,
           "missing or invalid annotation layer root");
    own(owned, layerId, "duplicate annotation layer ownership");
    // Root references also contain its network owner. It is NOT part of the
    // annotation-owned record set; only membership chunks are owned here.
    for (const Key& reference : root->references) {
        const std::optional<ModelRecord> child = candidate.get(reference);
        ensure(child.has_value(), "missing annotation layer dependency");
        if (child->kind == 100) continue;
        ensure(child->kind == annotationChunkKind,
               "annotation layer references a wrong-kind chunk");
        own(owned, reference, "annotation membership chunk belongs to multiple layers");
        for (const Key& memberId : child->references) {
            const std::optional<ModelRecord> member = candidate.get(memberId);
            ensure(member && member->kind == annotationMemberKind,
                   "annotation chunk contains missing or wrong-kind member");
            own(owned, memberId, "annotation belongs to multiple layers or chunks");
        }
    }
}

} // namespace

std::optional<std::string> validateAnnotationSnapshot(const MergeCandidate& candidate) {
    try {
        std::map<Key, std::uint16_t> existing;
        std::set<Key> networkRoots;
        std::set<Key> layerRoots;
        candidate.forEach([&](const Key& id, const ModelRecord& entry) {
            ensure(isKnownKind(entry.kind), "unsupported native model record kind in annotation candidate");
            ensure(existing.emplace(id, entry.kind).second,
                   "duplicate UUID in annotation candidate enumeration");
            if (entry.kind == hydraulic::hydraulicDistributionNetworkKind)
                networkRoots.insert(id);
            if (entry.kind == annotationLayerKind)
                layerRoots.insert(id);
        });

        std::map<Key, AnchorTypes> networkAnchors;
        std::set<Key> owned;
        for (const Key& id : networkRoots) {
            const std::optional<NetworkHydraulicDistribution> network =
                hydraulic::loadDraftHydraulicDistributionNetwork(
                    candidate.get, record::uuidFromKey(id));
            ensure(network.has_value(), "invalid candidate distribution network");
            networkAnchors.emplace(id, allowedAnchors(*network));
            collectHydraulicOwnership(candidate, id, owned);
        }

        std::set<Key> assignedNetworks;
        bool drainageLayerSeen = false;
        for (const Key& id : layerRoots) {
            const std::optional<AnnotationLayerSnapshot> layer =
                loadAnnotationLayer(candidate.get, record::uuidFromKey(id));
            ensure(layer.has_value(), "invalid candidate annotation layer");
            if (layer->layer.domain == MapAnnotationDomain::Distribution) {
                ensure(layer->layer.owner_network_uuid.has_value(),
                       "distribution annotation layer lacks network owner");
                const Key ownerId = record::keyFromUuid(*layer->layer.owner_network_uuid);
                const std::map<Key, AnchorTypes>::const_iterator network = networkAnchors.find(ownerId);
                ensure(network != networkAnchors.end(),
                       "annotation layer belongs to an absent or foreign distribution network");
                ensure(assignedNetworks.insert(ownerId).second,
                       "distribution network has multiple annotation layers");
                for (const MapAnnotation& annotation : layer->annotations) {
                    if (!annotation.anchor) continue;
                    const Key anchorId = record::keyFromUuid(annotation.anchor->entity_uuid);
                    const AnchorTypes::const_iterator found = network->second.find(anchorId);
                    ensure(found != network->second.end(),
                           "annotation anchor is not owned by this distribution network");
                    ensure(found->second == annotation.anchor->entity_type,
                           "annotation anchor has the wrong canonical entity type");
                }
            } else if (layer->layer.domain == MapAnnotationDomain::Drainage) {
                ensure(!drainageLayerSeen, "project has multiple pre-network drainage annotation layers");
                drainageLayerSeen = true;
                for (const MapAnnotation& annotation : layer->annotations)
                    ensure(!annotation.anchor, "pre-network drainage annotation must not have an anchor");
            } else {
                ensure(false, "unsupported annotation layer domain");
            }
            collectAnnotationOwnership(candidate, id, owned);
        }
        ensure(assignedNetworks.size() == networkRoots.size(),
               "distribution network is missing its required annotation layer");
        ensure(owned.size() == existing.size(),
               "candidate contains unowned or orphaned native model records");
        for (const std::pair<const Key, std::uint16_t>& entry : existing)
            ensure(owned.contains(entry.first),
                   "candidate contains unowned or orphaned native model record");
        return std::nullopt;
    } catch (const std::exception& error) {
        return std::string(error.what());
    }
}

} // namespace aowis::dvmdb::annotations
