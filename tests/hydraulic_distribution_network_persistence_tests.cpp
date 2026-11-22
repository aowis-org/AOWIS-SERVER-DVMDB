#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/hydraulic_distribution_network_persistence.h"
#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/revision_record.h"
#include "aowis/dvmdb/map_annotation_snapshot_validation.h"
#include "aowis/dvmdb/map_annotation_persistence.h"
#include "aowis/dvmdb/map_annotation_action_replay.h"
#include <aowis/model/edit/hydraulic_distribution_root_edit.h>

#include <aowis/dvmdb/model_validation.h>

#include <QCoreApplication>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

using aowis::dvmdb::test::commitMetadata;

namespace {
namespace native = aowis::dvmdb;
namespace hydraulic = aowis::dvmdb::hydraulic;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// A hydraulic network cannot be published without its native annotation layer.
// The two records are one engineering snapshot and one atomic first revision.
std::vector<native::ModelChange> initialEngineeringSnapshot(
    const NetworkHydraulicDistribution& network) {
    std::vector<native::ModelChange> changes =
        hydraulic::fullDraftHydraulicDistributionNetworkChanges(network);
    MapAnnotationLayer layer;
    layer.uuid = native::annotations::distributionLayerUuid(network.uuid);
    layer.domain = MapAnnotationDomain::Distribution;
    layer.owner_network_uuid = network.uuid;
    std::vector<native::ModelChange> annotations =
        native::annotations::fullDraftAnnotationLayerChanges(layer, {});
    changes.insert(changes.end(), std::make_move_iterator(annotations.begin()),
                   std::make_move_iterator(annotations.end()));
    return changes;
}

QUuid uuid(int ordinal) {
    native::Key key{};
    key[0] = 0xA0;
    key[6] = 0x70;
    key[8] = 0x80;
    for (int i = 0; i < 4; ++i) {
        key[15 - i] = static_cast<std::uint8_t>(ordinal >> (8 * i));
    }
    return hydraulic::uuidFromKey(key);
}

template<typename T>
T item(int ordinal) {
    T result;
    result.uuid = uuid(ordinal);
    result.id = QStringLiteral("Entity-%1").arg(ordinal);
    return result;
}

std::filesystem::path tempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() /
                           "aowis-dvmdb-network-test-XXXXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* result = ::mkdtemp(writable.data());
    if (result == nullptr) throw std::runtime_error("mkdtemp failed");
    return result;
}

NetworkHydraulicDistribution completeNetwork() {
    NetworkHydraulicDistribution n;
    n.uuid = uuid(1);
    n.id = QStringLiteral("Network-1");
    n.title_line_1 = QStringLiteral("Réseau de Bitkine 水");
    n.duration_s = 123456;
    n.map_backdrop.file = QStringLiteral("background-map.png");
    MultiSpeciesSpecies species;
    species.id = QStringLiteral("chlorine");
    species.uuid = uuid(300);
    species.note = QStringLiteral("Dissolved Cl₂");
    n.multi_species.species.append(species);

    n.pipe_materials.append(item<HydraulicPipeMaterial>(10));
    n.patterns_time.append(item<HydraulicPatternTime>(11));
    n.patterns_time[0].multipliers = {1.0, 1.2, 0.75};
    n.curves_tank_volume.append(item<HydraulicCurveTankVolume>(12));
    n.curves_pump_head.append(item<HydraulicCurvePumpHead>(13));
    n.curves_pump_efficiency.append(item<HydraulicCurvePumpEfficiency>(14));
    n.curves_valve_headloss.append(item<HydraulicCurveValveHeadloss>(15));
    n.curves_valve_characteristic.append(item<HydraulicCurveValveCharacteristic>(16));
    n.curves_generic.append(item<HydraulicCurveGeneric>(28));
    n.curves_generic[0].id = QStringLiteral("Fixture curve Ω");
    n.nodes_reservoirs.append(item<HydraulicNodeReservoir>(18));
    n.nodes_junctions.append(item<HydraulicNodeJunction>(19));
    n.nodes_junctions.append(item<HydraulicNodeJunction>(20));
    n.nodes_junctions[0].metadata.description = QStringLiteral("Junction-α");
    n.nodes_tanks.append(item<HydraulicNodeTank>(21));
    n.nodes_tanks[0].volume_curve_uuid = uuid(12);
    n.demand_points.append(item<HydraulicDemandPoint>(22));
    n.demand_points[0].attachment.type = HydraulicDemandPointAttachmentType::Junction;
    n.demand_points[0].attachment.junction_uuid = uuid(19);
    n.links_pipes.append(item<HydraulicLinkPipe>(23));
    n.links_pipes[0].node_uuid_from = uuid(19);
    n.links_pipes[0].node_uuid_to = uuid(20);
    n.links_pipes[0].material_uuid = uuid(10);
    n.links_pipes[0].diameter_mm = 110.75;
    n.links_pumps.append(item<HydraulicLinkPump>(24));
    n.links_pumps[0].node_uuid_from = uuid(18);
    n.links_pumps[0].node_uuid_to = uuid(19);
    n.links_pumps[0].head_curve_uuid = uuid(13);
    n.links_pumps[0].speed_pattern_uuid = uuid(11);
    n.links_valves.append(item<HydraulicLinkValve>(25));
    n.links_valves[0].node_uuid_from = uuid(20);
    n.links_valves[0].node_uuid_to = uuid(21);
    n.links_valves[0].head_loss_curve_uuid = uuid(15);
    n.controls_simple.append(item<HydraulicControlSimple>(26));
    n.controls_simple[0].link_uuid = uuid(23);
    n.controls_simple[0].trigger_node_uuid = uuid(19);
    n.controls_rules.append(item<HydraulicControlRule>(27));
    HydraulicControlRuleAction action;
    action.link_uuid = uuid(24);
    n.controls_rules[0].actions_then.append(action);
    NetworkPresentationUnits units;
    units.name = QStringLiteral("Projekt-Einheiten µ");
    units.derived_from_profile = QStringLiteral("LPS");
    units.units.append(NetworkPresentationUnit{QStringLiteral("pressure_head"), QStringLiteral("bar")});
    units.units.append(NetworkPresentationUnit{QStringLiteral("volumetric_flow_rate"), QStringLiteral("L/s")});
    n.presentation_units = units;
    return n;
}

void compareNetworks(const NetworkHydraulicDistribution& source, const NetworkHydraulicDistribution& result) {
    require(aowis::dvmdb::ModelCodec<NetworkHydraulicDistribution>::encode(result) ==
            aowis::dvmdb::ModelCodec<NetworkHydraulicDistribution>::encode(source),
            "complete canonical NetworkHydraulicDistribution changed on reload");
    require(result.uuid == source.uuid && result.id == source.id &&
            result.title_line_1 == source.title_line_1 &&
            result.duration_s == source.duration_s &&
            result.map_backdrop.file == source.map_backdrop.file &&
            result.presentation_units.has_value() == source.presentation_units.has_value() &&
            (!source.presentation_units ||
             (result.presentation_units->name == source.presentation_units->name &&
              result.presentation_units->units.size() == source.presentation_units->units.size())) &&
            aowis::dvmdb::ModelCodec<NetworkMultiSpecies>::encode(result.multi_species) ==
            aowis::dvmdb::ModelCodec<NetworkMultiSpecies>::encode(source.multi_species),
            "network metadata changed on reload");
#define CHECK_LIST(name, type) \
    require(result.name.size() == source.name.size(), "network entity count changed: " #name); \
    for (qsizetype i = 0; i < source.name.size(); ++i) { \
        require(result.name[i].uuid == source.name[i].uuid, "network order changed: " #name); \
        require(aowis::dvmdb::ModelCodec<type>::encode(result.name[i]) == \
                aowis::dvmdb::ModelCodec<type>::encode(source.name[i]), \
                "canonical model payload changed: " #name); \
    }
    CHECK_LIST(pipe_materials, HydraulicPipeMaterial)
    CHECK_LIST(patterns_time, HydraulicPatternTime)
    CHECK_LIST(curves_tank_volume, HydraulicCurveTankVolume)
    CHECK_LIST(curves_pump_head, HydraulicCurvePumpHead)
    CHECK_LIST(curves_pump_efficiency, HydraulicCurvePumpEfficiency)
    CHECK_LIST(curves_valve_headloss, HydraulicCurveValveHeadloss)
    CHECK_LIST(curves_valve_characteristic, HydraulicCurveValveCharacteristic)
    CHECK_LIST(curves_generic, HydraulicCurveGeneric)
    CHECK_LIST(nodes_reservoirs, HydraulicNodeReservoir)
    CHECK_LIST(nodes_junctions, HydraulicNodeJunction)
    CHECK_LIST(nodes_tanks, HydraulicNodeTank)
    CHECK_LIST(demand_points, HydraulicDemandPoint)
    CHECK_LIST(links_pipes, HydraulicLinkPipe)
    CHECK_LIST(links_pumps, HydraulicLinkPump)
    CHECK_LIST(links_valves, HydraulicLinkValve)
    CHECK_LIST(controls_simple, HydraulicControlSimple)
    CHECK_LIST(controls_rules, HydraulicControlRule)
#undef CHECK_LIST
}

// Load APIs report every failure as a typed StorageError (never as text).
bool throwsStorageError(native::StorageErrorKind expected, const std::function<void()>& action) {
    try {
        action();
    } catch (const native::StorageError& error) {
        if (error.kind() != expected)
            throw std::runtime_error(std::string("load failure has kind ") +
                                     native::StorageError::kindName(error.kind()) + ": " + error.what());
        return true;
    }
    return false;
}

// End-to-end UUIDv7 hydraulic workflow: one pinned model snapshot powers
// every network, membership and indexed-ownership read. Model edits publish
// a distinct UUID revision; historical identities are never regenerated.
void exerciseUuidV7Hydraulic() {
    const std::filesystem::path folder = tempDir();
    try {
        const native::ProjectId project = native::ProjectId::generate();
        const NetworkHydraulicDistribution baseline = completeNetwork();
        native::RevisionId initial = native::RevisionId::generate();
        native::RevisionId saved = initial;
        native::RevisionId edited = initial;
        native::RevisionId scalarRewire = initial;
        native::RevisionId physicalHead = initial;
        native::RevisionId catalogueHead = initial;
        NetworkHydraulicDistribution scalarExpectedAfter = baseline;
        NetworkHydraulicDistribution physicalExpectedAfter = baseline;
        NetworkHydraulicDistribution catalogueExpectedAfter = baseline;
        {
            native::Repository repository(folder);
            initial = repository.createRootBranch(project, "main", commitMetadata());
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::loadHydraulicDistributionNetworkAtRevision(repository, project, initial, baseline.uuid);
            }), "initial raw UUIDv7 revision should reject model lookup");
            saved = hydraulic::saveHydraulicDistributionNetwork(repository, project, "main", baseline, initial, commitMetadata());
            require(saved != initial && repository.head(project, "main") == saved,
                    "UUIDv7 hydraulic save did not publish a fresh revision");
            // Distribution and presentation are one atomic revision, not two
            // independently published snapshots. The first native Save must
            // install a deterministic, initially empty annotation layer.
            const QUuid annotationLayerId = native::annotations::distributionLayerUuid(baseline.uuid);
            const std::optional<native::annotations::AnnotationLayerSnapshot> annotationLayer =
                native::annotations::loadAnnotationLayerAtRevision(
                    repository, project, saved, annotationLayerId);
            require(annotationLayer.has_value() &&
                    annotationLayer->layer.owner_network_uuid == baseline.uuid &&
                    annotationLayer->layer.uuid == annotationLayerId &&
                    annotationLayer->annotations.isEmpty(),
                    "initial network commit did not atomically bootstrap its annotation layer");
            const std::optional<NetworkHydraulicDistribution> loaded =
                hydraulic::loadHydraulicDistributionNetworkAtRevision(repository, project, saved, baseline.uuid);
            require(loaded.has_value(), "UUIDv7 hydraulic distribution network missing after save");
            compareNetworks(baseline, *loaded);
            const std::optional<QList<QUuid>> ids = hydraulic::listHydraulicEntityIdsAtRevisionLimited(
                repository, project, saved, baseline.uuid, hydraulic::HydraulicEntityType::Junction, 100);
            require(ids && ids->size() == baseline.nodes_junctions.size(),
                    "UUIDv7 membership list missing junctions");
            require(throwsStorageError(native::StorageErrorKind::LimitExceeded, [&] {
                (void)hydraulic::listHydraulicEntityIdsAtRevisionLimited(
                    repository, project, saved, baseline.uuid, hydraulic::HydraulicEntityType::Junction, 1);
            }), "UUIDv7 bounded membership did not reject excess members");
            const std::optional<hydraulic::HydraulicEntity> targeted =
                hydraulic::loadHydraulicEntityAtRevision(repository, project, saved,
                    baseline.uuid, baseline.nodes_junctions.front().uuid);
            require(targeted && std::holds_alternative<HydraulicNodeJunction>(*targeted),
                    "UUIDv7 indexed ownership lookup failed");
            require(!hydraulic::loadHydraulicEntityAtRevision(
                repository, project, saved, baseline.uuid, uuid(10000)),
                "UUIDv7 indexed lookup found an unowned entity");
            // UUIDv7 indexed physical updates must preserve the membership
            // manifest and reference index. A no-op retains the exact HEAD;
            // invalid values and stale HEADs cannot publish partial changes.
            require(repository.createBranch(project, "indexed-physical", "main") == saved,
                    "UUIDv7 physical branch did not inherit saved HEAD");
            const native::ModelRecord manifestBefore = *repository.getModelAtRevision(
                project, saved, hydraulic::keyFromUuid(baseline.uuid));
            NetworkHydraulicDistribution physicalNetwork = baseline;
            physicalNetwork.nodes_reservoirs[0].hydraulic_head_m += 12.5;
            physicalNetwork.nodes_tanks[0].terrain_elevation_m += 2.0;
            const std::vector<hydraulic::HydraulicEntityChange> physicalChanges{
                {physicalNetwork.nodes_reservoirs[0].uuid,
                 hydraulic::HydraulicEntity{physicalNetwork.nodes_reservoirs[0]}},
                {physicalNetwork.nodes_tanks[0].uuid,
                 hydraulic::HydraulicEntity{physicalNetwork.nodes_tanks[0]}}};
            hydraulic::HydraulicEditDiagnostics physicalStats;
            physicalHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "indexed-physical", baseline.uuid,
                physicalChanges, saved, commitMetadata(), &physicalStats);
            require(physicalHead != saved && physicalStats.indexedPhysicalFastPath &&
                    physicalStats.validatedEntities == physicalChanges.size(),
                    "UUIDv7 physical update did not use immutable indexed records");
            const native::ModelRecord physicalManifest = *repository.getModelAtRevision(
                project, physicalHead, hydraulic::keyFromUuid(baseline.uuid));
            require(native::encodeModelRecord(manifestBefore) ==
                        native::encodeModelRecord(physicalManifest),
                    "UUIDv7 indexed physical update rewrote network membership");
            compareNetworks(physicalNetwork, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, physicalHead, baseline.uuid));
            require(hydraulic::editHydraulicDistributionNetwork(repository, project, "indexed-physical",
                    baseline.uuid, physicalChanges, physicalHead, commitMetadata(), &physicalStats) == physicalHead &&
                    physicalStats.indexedPhysicalFastPath,
                    "UUIDv7 no-op physical edit created a revision");
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "indexed-physical",
                    baseline.uuid, physicalChanges, saved, commitMetadata(), &physicalStats);
            }), "UUIDv7 physical edit accepted stale expected HEAD");
            HydraulicNodeReservoir invalidPhysical = physicalNetwork.nodes_reservoirs[0];
            invalidPhysical.hydraulic_head_m = std::numeric_limits<double>::infinity();
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "indexed-physical",
                    baseline.uuid, {{invalidPhysical.uuid, hydraulic::HydraulicEntity{invalidPhysical}}},
                    physicalHead, commitMetadata(), &physicalStats);
            }) && repository.head(project, "indexed-physical") == physicalHead,
                    "UUIDv7 invalid physical value advanced HEAD");
            HydraulicLinkPump invalidRewire = physicalNetwork.links_pumps[0];
            invalidRewire.node_uuid_to = uuid(9999);
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "indexed-physical",
                    baseline.uuid, {{invalidRewire.uuid, hydraulic::HydraulicEntity{invalidRewire}}},
                    physicalHead, commitMetadata(), &physicalStats);
            }) && !physicalStats.indexedPhysicalFastPath &&
                repository.head(project, "indexed-physical") == physicalHead,
                "UUIDv7 physical edit bypassed topology validation");
            physicalExpectedAfter = physicalNetwork;

            // Existing catalogue payload updates require no structural writes.
            // Verify canonical replay and that a wrong-kind update falls back
            // to complete-network validation without publishing anything.
            require(repository.createBranch(project, "indexed-catalogue", "main") == saved,
                    "UUIDv7 catalogue branch did not inherit saved HEAD");
            NetworkHydraulicDistribution catalogueNetwork = baseline;
            catalogueNetwork.patterns_time[0].multipliers = {1.0, 1.15, 0.9};
            const std::vector<hydraulic::HydraulicEntityChange> catalogueChanges{
                {catalogueNetwork.patterns_time[0].uuid,
                 hydraulic::HydraulicEntity{catalogueNetwork.patterns_time[0]}}};
            hydraulic::HydraulicEditDiagnostics catalogueStats;
            catalogueHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "indexed-catalogue", baseline.uuid,
                catalogueChanges, saved, commitMetadata(), &catalogueStats);
            require(catalogueHead != saved && catalogueStats.indexedCatalogueFastPath &&
                    catalogueStats.validatedEntities == catalogueChanges.size(),
                    "UUIDv7 catalogue update did not use immutable indexed records");
            const native::ModelRecord catalogueManifest = *repository.getModelAtRevision(
                project, catalogueHead, hydraulic::keyFromUuid(baseline.uuid));
            require(native::encodeModelRecord(manifestBefore) ==
                        native::encodeModelRecord(catalogueManifest),
                    "UUIDv7 indexed catalogue update rewrote network membership");
            compareNetworks(catalogueNetwork, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, catalogueHead, baseline.uuid));
            require(hydraulic::editHydraulicDistributionNetwork(repository, project, "indexed-catalogue",
                    baseline.uuid, catalogueChanges, catalogueHead, commitMetadata(), &catalogueStats) == catalogueHead &&
                    catalogueStats.indexedCatalogueFastPath,
                    "UUIDv7 no-op catalogue edit created a revision");
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "indexed-catalogue",
                    baseline.uuid, catalogueChanges, saved, commitMetadata(), &catalogueStats);
            }), "UUIDv7 catalogue edit accepted stale expected HEAD");
            HydraulicPipeMaterial wrongKind = catalogueNetwork.pipe_materials[0];
            wrongKind.uuid = catalogueNetwork.patterns_time[0].uuid;
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "indexed-catalogue",
                    baseline.uuid, {{wrongKind.uuid, hydraulic::HydraulicEntity{wrongKind}}},
                    catalogueHead, commitMetadata(), &catalogueStats);
            }) && !catalogueStats.indexedCatalogueFastPath &&
                repository.head(project, "indexed-catalogue") == catalogueHead,
                "UUIDv7 catalogue edit changed entity type");
            catalogueExpectedAfter = catalogueNetwork;

            // The canonical UUIDv7 API must preserve bounded indexed scalar
            // editing rather than rewrite the entire network on every value
            // update. Membership/root bytes and the source branch remain
            // unchanged; both changed and no-op batches retain exact IDs.
            require(repository.createBranch(project, "indexed-scalars", "main") == saved,
                    "UUIDv7 scalar branch did not inherit saved HEAD");
            const native::ModelRecord scalarManifestBefore = *repository.getModelAtRevision(
                project, saved, hydraulic::keyFromUuid(baseline.uuid));
            NetworkHydraulicDistribution scalarNetwork = baseline;
            scalarNetwork.links_pipes[0].diameter_mm = 143.25;
            scalarNetwork.nodes_junctions[0].elevation_m = 17.0;
            const std::vector<hydraulic::HydraulicEntityChange> scalarChanges{
                {scalarNetwork.links_pipes[0].uuid,
                 hydraulic::HydraulicEntity{scalarNetwork.links_pipes[0]}},
                {scalarNetwork.nodes_junctions[0].uuid,
                 hydraulic::HydraulicEntity{scalarNetwork.nodes_junctions[0]}}};
            hydraulic::HydraulicEditDiagnostics scalarStats;
            const native::RevisionId scalarHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "indexed-scalars", baseline.uuid,
                scalarChanges, saved, commitMetadata(), &scalarStats);
            require(scalarHead != saved && scalarStats.indexedScalarFastPath &&
                    scalarStats.validatedEntities == 2,
                    "UUIDv7 safe scalar edit did not use indexed records");
            require(repository.head(project, "main") == saved,
                    "UUIDv7 scalar edit moved unrelated branch HEAD");
            const native::ModelRecord manifestAfter = *repository.getModelAtRevision(
                project, scalarHead, hydraulic::keyFromUuid(baseline.uuid));
            require(native::encodeModelRecord(scalarManifestBefore) ==
                        native::encodeModelRecord(manifestAfter),
                    "UUIDv7 indexed scalar edit rewrote membership manifest");
            compareNetworks(scalarNetwork, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, scalarHead, baseline.uuid));
            compareNetworks(baseline, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, saved, baseline.uuid));
            require(hydraulic::editHydraulicDistributionNetwork(repository, project, "indexed-scalars",
                    baseline.uuid, scalarChanges, scalarHead, commitMetadata(), &scalarStats) == scalarHead &&
                    scalarStats.indexedScalarFastPath,
                    "UUIDv7 no-op scalar edit created another revision");
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project,
                    "indexed-scalars", baseline.uuid, scalarChanges, saved, commitMetadata(), &scalarStats);
            }), "UUIDv7 indexed scalar edit accepted stale expected HEAD");
            HydraulicLinkPipe invalidScalarTopology = scalarNetwork.links_pipes[0];
            invalidScalarTopology.node_uuid_to = uuid(9999);
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project,
                    "indexed-scalars", baseline.uuid,
                    {{invalidScalarTopology.uuid,
                      hydraulic::HydraulicEntity{invalidScalarTopology}}},
                    scalarHead, commitMetadata(), &scalarStats);
            }) && !scalarStats.indexedScalarFastPath &&
                    repository.head(project, "indexed-scalars") == scalarHead,
                    "UUIDv7 topology edit incorrectly bypassed canonical validation");
            HydraulicLinkPipe validScalarTopology = scalarNetwork.links_pipes[0];
            validScalarTopology.node_uuid_from = uuid(18);
            const native::RevisionId rewireHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "indexed-scalars", baseline.uuid,
                {{validScalarTopology.uuid, hydraulic::HydraulicEntity{validScalarTopology}}},
                scalarHead, commitMetadata(), &scalarStats);
            require(rewireHead != scalarHead && !scalarStats.indexedScalarFastPath,
                    "UUIDv7 topology edit incorrectly used scalar fast path");
            scalarNetwork.links_pipes[0] = validScalarTopology;
            scalarRewire = rewireHead;
            scalarExpectedAfter = scalarNetwork;
            compareNetworks(scalarNetwork, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, rewireHead, baseline.uuid));
            HydraulicNodeJunction updated = baseline.nodes_junctions.front();
            updated.metadata.description = QStringLiteral("UUIDv7 updated junction");
            const std::vector<hydraulic::HydraulicEntityChange> changes{
                {updated.uuid, hydraulic::HydraulicEntity{updated}}};
            edited = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "main", baseline.uuid, changes, saved, commitMetadata());
            require(edited != saved && repository.head(project, "main") == edited,
                    "UUIDv7 hydraulic edit failed to advance HEAD");
            require(hydraulic::editHydraulicDistributionNetwork(
                repository, project, "main", baseline.uuid, changes, edited, commitMetadata()) == edited,
                    "UUIDv7 no-op edit created a new revision");
            NetworkHydraulicDistribution expected = baseline;
            expected.nodes_junctions.front() = updated;
            compareNetworks(expected, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, edited, baseline.uuid));
            compareNetworks(baseline, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, saved, baseline.uuid));
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(
                    repository, project, "main", baseline.uuid, changes, saved, commitMetadata());
            }), "UUIDv7 stale HEAD edit was accepted");
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::saveHydraulicDistributionNetwork(repository, project, "main", baseline, saved, commitMetadata());
            }), "UUIDv7 stale full save was accepted");
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "main", baseline.uuid,
                    {{uuid(9900), std::nullopt}}, edited, commitMetadata());
            }), "UUIDv7 unowned deletion was accepted");
            const native::RevisionId fork = repository.createBranchAtRevision(
                project, "history", "main", saved);
            require(fork == saved && repository.head(project, "history") == saved,
                    "historical UUIDv7 branch was not pinned to immutable revision");
            (void)repository.verifyRepository();
            (void)repository.compact();
        }
        {
            native::Repository reopened(folder);
            require(reopened.head(project, "main") == edited &&
                    reopened.head(project, "history") == saved &&
                    reopened.head(project, "indexed-scalars") == scalarRewire &&
                    reopened.head(project, "indexed-physical") == physicalHead &&
                    reopened.head(project, "indexed-catalogue") == catalogueHead,
                    "UUIDv7 hydraulic HEAD identities changed after reopen/compaction");
            compareNetworks(scalarExpectedAfter, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                reopened, project, scalarRewire, baseline.uuid));
            compareNetworks(physicalExpectedAfter, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                reopened, project, physicalHead, baseline.uuid));
            compareNetworks(catalogueExpectedAfter, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                reopened, project, catalogueHead, baseline.uuid));
            compareNetworks(baseline, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                reopened, project, saved, baseline.uuid));
            (void)reopened.verifyRepository();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// UUIDv7 indexed topology/dependent qualification: immutable revision reads,
// reverse edges, bounded network ownership, full-validator fallbacks, stale
// pins, no-op edits and complete reconstruction after reopen/compaction.
void exerciseUuidV7IndexedReferences() {
    const std::filesystem::path folder = tempDir();
    const native::ProjectId project = native::ProjectId::generate();
    const NetworkHydraulicDistribution baseline = completeNetwork();
    native::RevisionId topologyHead = native::RevisionId::generate();
    native::RevisionId dependentHead = native::RevisionId::generate();
    NetworkHydraulicDistribution topologyExpected = baseline;
    NetworkHydraulicDistribution dependentExpected = baseline;
    try {
        {
            native::Repository repository(folder);
            const native::RevisionId genesis = repository.createRootBranch(project, "main", commitMetadata());
            const native::RevisionId saved = hydraulic::saveHydraulicDistributionNetwork(
                repository, project, "main", baseline, genesis, commitMetadata());
            require(repository.createBranch(project, "uuid-topology", "main") == saved,
                    "UUIDv7 topology fixture branch did not inherit HEAD");
            // A control depends on the original pipe. Such a rewire is
            // ineligible for the indexed shortcut, even when canonical full
            // validation can accept the resulting network.
            require(repository.createBranch(project, "uuid-referenced-pipe", "main") == saved,
                    "UUIDv7 referenced topology fixture branch did not inherit HEAD");
            HydraulicLinkPipe referencedPipe = baseline.links_pipes[0];
            referencedPipe.node_uuid_from = uuid(18);
            hydraulic::HydraulicEditDiagnostics referencedStats;
            const native::RevisionId referencedHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "uuid-referenced-pipe", baseline.uuid,
                {{referencedPipe.uuid, hydraulic::HydraulicEntity{referencedPipe}}},
                saved, commitMetadata(), &referencedStats);
            require(referencedHead != saved && !referencedStats.indexedTopologyFastPath,
                    "UUIDv7 topology bypassed an incoming dependent control");
            NetworkHydraulicDistribution referencedExpected = baseline;
            referencedExpected.links_pipes[0] = referencedPipe;
            compareNetworks(referencedExpected, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, referencedHead, baseline.uuid));
            // The baseline has a control referring to its pipe. Removing that
            // control is a full-network operation and permits an independent
            // rewire to take the bounded indexed path.
            topologyExpected.controls_simple.clear();
            const native::RevisionId topologyBase = hydraulic::saveHydraulicDistributionNetwork(
                repository, project, "uuid-topology", topologyExpected, saved, commitMetadata());
            const native::ModelRecord topologyRoot = *repository.getModelAtRevision(
                project, topologyBase, hydraulic::keyFromUuid(baseline.uuid));
            topologyExpected.links_pipes[0].node_uuid_from = uuid(18);
            const HydraulicLinkPipe rewire = topologyExpected.links_pipes[0];
            const std::vector<hydraulic::HydraulicEntityChange> topologyChanges{
                {rewire.uuid, hydraulic::HydraulicEntity{rewire}}};
            hydraulic::HydraulicEditDiagnostics topologyStats;
            topologyHead = hydraulic::editHydraulicDistributionNetwork(repository, project, "uuid-topology",
                baseline.uuid, topologyChanges, topologyBase, commitMetadata(), &topologyStats);
            require(topologyHead != topologyBase && topologyStats.indexedTopologyFastPath &&
                    topologyStats.validatedEntities == 1,
                    "UUIDv7 independent pipe rewire missed indexed fast path");
            require(native::encodeModelRecord(topologyRoot) == native::encodeModelRecord(
                *repository.getModelAtRevision(project, topologyHead,
                    hydraulic::keyFromUuid(baseline.uuid))),
                    "UUIDv7 indexed topology edit rewrote membership root");
            compareNetworks(topologyExpected, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, topologyHead, baseline.uuid));
            const std::vector<native::Key> oldIncoming = repository.referencingModelAtRevision(
                project, topologyHead, hydraulic::keyFromUuid(uuid(19)));
            require(std::find(oldIncoming.begin(), oldIncoming.end(),
                        hydraulic::keyFromUuid(rewire.uuid)) == oldIncoming.end(),
                    "UUIDv7 indexed topology kept old endpoint reference");
            const std::vector<native::Key> newIncoming = repository.referencingModelAtRevision(
                project, topologyHead, hydraulic::keyFromUuid(uuid(18)));
            require(std::find(newIncoming.begin(), newIncoming.end(),
                        hydraulic::keyFromUuid(rewire.uuid)) != newIncoming.end(),
                    "UUIDv7 indexed topology lost new endpoint reference");
            require(hydraulic::editHydraulicDistributionNetwork(repository, project, "uuid-topology",
                        baseline.uuid, topologyChanges, topologyHead, commitMetadata(), &topologyStats) == topologyHead &&
                    (topologyStats.indexedTopologyFastPath || topologyStats.indexedScalarFastPath),
                    "UUIDv7 no-op topology edit generated another revision");
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "uuid-topology",
                    baseline.uuid, topologyChanges, topologyBase, commitMetadata(), &topologyStats);
            }), "UUIDv7 topology edit accepted stale expected HEAD");
            HydraulicLinkPipe wrongKind = rewire;
            wrongKind.node_uuid_from = uuid(10); // Existing material, not a node.
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "uuid-topology",
                    baseline.uuid, {{wrongKind.uuid, hydraulic::HydraulicEntity{wrongKind}}},
                    topologyHead, commitMetadata(), &topologyStats);
            }) && !topologyStats.indexedTopologyFastPath &&
                    repository.head(project, "uuid-topology") == topologyHead,
                    "UUIDv7 topology accepted wrong-type endpoint");
            HydraulicLinkPipe missing = rewire;
            missing.node_uuid_from = uuid(9009);
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "uuid-topology",
                    baseline.uuid, {{missing.uuid, hydraulic::HydraulicEntity{missing}}},
                    topologyHead, commitMetadata(), &topologyStats);
            }) && !topologyStats.indexedTopologyFastPath,
                    "UUIDv7 topology accepted missing endpoint");
            HydraulicLinkPipe metadata = rewire;
            metadata.id = QStringLiteral("Full validation required");
            const native::RevisionId fullHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "uuid-topology", baseline.uuid,
                {{metadata.uuid, hydraulic::HydraulicEntity{metadata}}},
                topologyHead, commitMetadata(), &topologyStats);
            require(fullHead != topologyHead && !topologyStats.indexedTopologyFastPath,
                    "UUIDv7 topology fast path accepted metadata changes");
            topologyExpected.links_pipes[0] = metadata;
            topologyHead = fullHead;
            compareNetworks(topologyExpected, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, topologyHead, baseline.uuid));

            require(repository.createBranch(project, "uuid-dependent", "main") == saved,
                    "UUIDv7 dependent fixture branch did not inherit HEAD");
            const native::ModelRecord dependentRoot = *repository.getModelAtRevision(
                project, saved, hydraulic::keyFromUuid(baseline.uuid));
            HydraulicDemandPoint point = baseline.demand_points[0];
            point.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
            point.attachment.pipe_uuid = uuid(23);
            point.attachment.pipe_position = 0.65;
            point.attachment.pipe_allocation_mode =
                HydraulicDemandPointPipeAllocationMode::AssignedJunction;
            point.attachment.pipe_assigned_junction_uuid = uuid(20);
            HydraulicDemand demand;
            demand.pattern_uuid = uuid(11);
            point.demands.append(demand);
            HydraulicControlSimple control = baseline.controls_simple[0];
            control.link_uuid = uuid(25);
            control.trigger_node_uuid = uuid(21);
            HydraulicControlRule rule = baseline.controls_rules[0];
            HydraulicControlRulePremise premise;
            premise.object = HydraulicControlRuleObject::Node;
            premise.object_uuid = uuid(18);
            rule.premises.append(premise);
            rule.actions_then[0].link_uuid = uuid(23);
            HydraulicControlRuleAction elseAction;
            elseAction.link_uuid = uuid(25);
            rule.actions_else.append(elseAction);
            const std::vector<hydraulic::HydraulicEntityChange> dependentChanges{
                {rule.uuid, hydraulic::HydraulicEntity{rule}},
                {point.uuid, hydraulic::HydraulicEntity{point}},
                {control.uuid, hydraulic::HydraulicEntity{control}}};
            hydraulic::HydraulicEditDiagnostics dependentStats;
            dependentHead = hydraulic::editHydraulicDistributionNetwork(repository, project, "uuid-dependent",
                baseline.uuid, dependentChanges, saved, commitMetadata(), &dependentStats);
            require(dependentHead != saved && dependentStats.indexedDependentUpdateFastPath &&
                    dependentStats.validatedEntities == dependentChanges.size(),
                    "UUIDv7 dependent changes missed indexed fast path");
            dependentExpected.demand_points[0] = point;
            dependentExpected.controls_simple[0] = control;
            dependentExpected.controls_rules[0] = rule;
            require(native::validateHydraulicDistributionNetwork(dependentExpected).valid(),
                    "UUIDv7 indexed dependent fixture violates canonical validation");
            compareNetworks(dependentExpected, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, dependentHead, baseline.uuid));
            require(native::encodeModelRecord(dependentRoot) == native::encodeModelRecord(
                *repository.getModelAtRevision(project, dependentHead,
                    hydraulic::keyFromUuid(baseline.uuid))),
                    "UUIDv7 indexed dependent edit rewrote membership root");
            const std::vector<native::Key> pipeIncoming = repository.referencingModelAtRevision(
                project, dependentHead, hydraulic::keyFromUuid(uuid(23)));
            require(std::find(pipeIncoming.begin(), pipeIncoming.end(),
                        hydraulic::keyFromUuid(point.uuid)) != pipeIncoming.end(),
                    "UUIDv7 dependent edit omitted pipe attachment reference");
            require(hydraulic::editHydraulicDistributionNetwork(repository, project, "uuid-dependent",
                        baseline.uuid, dependentChanges, dependentHead, commitMetadata(), &dependentStats) == dependentHead &&
                    dependentStats.indexedDependentUpdateFastPath,
                    "UUIDv7 no-op dependent edit generated a revision");
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "uuid-dependent",
                    baseline.uuid, dependentChanges, saved, commitMetadata(), &dependentStats);
            }), "UUIDv7 dependent update accepted stale expected HEAD");
            HydraulicDemandPoint invalid = point;
            invalid.attachment.pipe_assigned_junction_uuid = uuid(21); // Not a pipe endpoint.
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "uuid-dependent",
                    baseline.uuid, {{invalid.uuid, hydraulic::HydraulicEntity{invalid}}},
                    dependentHead, commitMetadata(), &dependentStats);
            }) && !dependentStats.indexedDependentUpdateFastPath &&
                    repository.head(project, "uuid-dependent") == dependentHead,
                    "UUIDv7 dependent path accepted invalid assigned junction");
            // A mixed dependent/property batch cannot bypass whole-model
            // validation, and a rejected batch must not publish its valid part.
            HydraulicCurveGeneric changedLabel = dependentExpected.curves_generic[0];
            changedLabel.id = QStringLiteral("UUIDv7 reference fallback");
            HydraulicControlSimple changedControl = control;
            changedControl.trigger_node_uuid = uuid(20);
            const native::RevisionId mixedHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "uuid-dependent", baseline.uuid,
                {{changedControl.uuid, hydraulic::HydraulicEntity{changedControl}},
                 {changedLabel.uuid, hydraulic::HydraulicEntity{changedLabel}}},
                dependentHead, commitMetadata(), &dependentStats);
            require(mixedHead != dependentHead && !dependentStats.indexedDependentUpdateFastPath,
                    "UUIDv7 mixed dependent batch bypassed canonical validation");
            dependentExpected.controls_simple[0] = changedControl;
            dependentExpected.curves_generic[0] = changedLabel;
            dependentHead = mixedHead;
            compareNetworks(dependentExpected, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, dependentHead, baseline.uuid));
            HydraulicControlSimple invalidControl = changedControl;
            invalidControl.link_uuid = uuid(20); // A node is not a link.
            HydraulicDemandPoint validChangedPoint = point;
            validChangedPoint.attachment.pipe_position = 0.4;
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "uuid-dependent",
                    baseline.uuid,
                    {{validChangedPoint.uuid, hydraulic::HydraulicEntity{validChangedPoint}},
                     {invalidControl.uuid, hydraulic::HydraulicEntity{invalidControl}}},
                    dependentHead, commitMetadata(), &dependentStats);
            }) && repository.head(project, "uuid-dependent") == dependentHead,
                    "UUIDv7 invalid dependent batch committed its valid edit");
            compareNetworks(baseline, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, saved, baseline.uuid));
            (void)repository.verifyRepository();
            (void)repository.compact();
        }
        {
            native::Repository reopened(folder);
            require(reopened.head(project, "uuid-topology") == topologyHead &&
                    reopened.head(project, "uuid-dependent") == dependentHead,
                    "UUIDv7 indexed references changed HEAD after compaction/reopen");
            compareNetworks(topologyExpected, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                reopened, project, topologyHead, baseline.uuid));
            compareNetworks(dependentExpected, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                reopened, project, dependentHead, baseline.uuid));
            (void)reopened.verifyRepository();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// Project-scoped UUIDv7 bounded membership qualification. Structural edits
// update only the affected chunks, use immutable pinned reads and preserve
// the original branch. Unsafe dependencies must fall back to full validation.
void exerciseUuidV7IndexedMembership() {
    const std::filesystem::path folder = tempDir();
    const native::ProjectId project = native::ProjectId::generate();
    const NetworkHydraulicDistribution baseline = completeNetwork();
    native::RevisionId membershipHead = native::RevisionId::generate();
    NetworkHydraulicDistribution expected = baseline;
    try {
        {
            native::Repository repository(folder);
            const native::RevisionId genesis = repository.createRootBranch(project, "main", commitMetadata());
            const native::RevisionId saved = hydraulic::saveHydraulicDistributionNetwork(
                repository, project, "main", baseline, genesis, commitMetadata());
            require(repository.createBranch(project, "uuid-membership", "main") == saved,
                    "UUIDv7 membership branch did not inherit baseline");
            const HydraulicNodeJunction addedNode = item<HydraulicNodeJunction>(31110);
            HydraulicLinkPipe addedPipe = item<HydraulicLinkPipe>(31111);
            addedPipe.node_uuid_from = uuid(19);
            addedPipe.node_uuid_to = addedNode.uuid;
            addedPipe.material_uuid = uuid(10);
            HydraulicCurveGeneric addedLabel = item<HydraulicCurveGeneric>(31112);
            addedLabel.id = QStringLiteral("UUIDv7 new junction");
            const std::vector<hydraulic::HydraulicEntityChange> insertions{
                {addedPipe.uuid, hydraulic::HydraulicEntity{addedPipe}},
                {addedNode.uuid, hydraulic::HydraulicEntity{addedNode}},
                {addedLabel.uuid, hydraulic::HydraulicEntity{addedLabel}}};
            hydraulic::HydraulicEditDiagnostics stats;
            const native::ModelRecord oldRoot = *repository.getModelAtRevision(
                project, saved, hydraulic::keyFromUuid(baseline.uuid));
            membershipHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "uuid-membership", baseline.uuid,
                insertions, saved, commitMetadata(), &stats);
            require(membershipHead != saved && stats.indexedMembershipFastPath &&
                    stats.validatedEntities == insertions.size() &&
                    repository.head(project, "main") == saved,
                    "UUIDv7 insertion missed bounded membership path or changed source HEAD");
            expected.nodes_junctions.append(addedNode);
            expected.links_pipes.append(addedPipe);
            expected.curves_generic.append(addedLabel);
            compareNetworks(expected, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, membershipHead, baseline.uuid));
            const native::ModelRecord insertedRoot = *repository.getModelAtRevision(
                project, membershipHead, hydraulic::keyFromUuid(baseline.uuid));
            // The root stores chunk identities, not inline entity UUIDs. These
            // three insertions fit into existing chunks, so the root must stay
            // unchanged while the affected chunk records gain new members.
            require(native::encodeModelRecord(insertedRoot) == native::encodeModelRecord(oldRoot),
                    "UUIDv7 bounded insertion unnecessarily rewrote the root manifest");
            const std::set<native::Key> insertedKeys{
                hydraulic::keyFromUuid(addedNode.uuid),
                hydraulic::keyFromUuid(addedPipe.uuid),
                hydraulic::keyFromUuid(addedLabel.uuid)};
            std::set<native::Key> observedInsertedKeys;
            std::size_t updatedChunkCount = 0;
            for (const native::Key& chunkKey : insertedRoot.references) {
                const std::optional<native::ModelRecord> oldChunk =
                    repository.getModelAtRevision(project, saved, chunkKey);
                const std::optional<native::ModelRecord> newChunk =
                    repository.getModelAtRevision(project, membershipHead, chunkKey);
                require(newChunk.has_value(), "UUIDv7 insertion left a missing membership chunk");
                if (!oldChunk || native::encodeModelRecord(*oldChunk) !=
                                 native::encodeModelRecord(*newChunk)) {
                    ++updatedChunkCount;
                    for (const native::Key& memberKey : newChunk->references) {
                        if (insertedKeys.contains(memberKey))
                            observedInsertedKeys.insert(memberKey);
                    }
                }
            }
            require(updatedChunkCount == 3 && observedInsertedKeys == insertedKeys,
                    "UUIDv7 insertion did not update the three affected membership chunks");
            const std::vector<native::Key> incoming = repository.referencingModelAtRevision(
                project, membershipHead, hydraulic::keyFromUuid(addedNode.uuid));
            require(std::find(incoming.begin(), incoming.end(),
                             hydraulic::keyFromUuid(addedPipe.uuid)) != incoming.end(),
                    "UUIDv7 membership insertion lost dependent reference edges");
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "uuid-membership",
                    baseline.uuid, insertions, saved, commitMetadata(), &stats);
            }), "UUIDv7 membership insertion accepted stale expected HEAD");

            HydraulicLinkPipe invalidPipe = item<HydraulicLinkPipe>(31113);
            invalidPipe.node_uuid_from = uuid(10); // A material is not a node.
            invalidPipe.node_uuid_to = addedNode.uuid;
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "uuid-membership",
                    baseline.uuid, {{invalidPipe.uuid, hydraulic::HydraulicEntity{invalidPipe}}},
                    membershipHead, commitMetadata(), &stats);
            }) && !stats.indexedMembershipFastPath &&
                repository.head(project, "uuid-membership") == membershipHead,
                "UUIDv7 invalid inserted endpoint changed HEAD");
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "uuid-membership",
                    baseline.uuid, {{addedNode.uuid, std::nullopt}}, membershipHead, commitMetadata(), &stats);
            }) && !stats.indexedMembershipFastPath &&
                repository.head(project, "uuid-membership") == membershipHead,
                "UUIDv7 membership deleted a node with live incoming references");
            HydraulicNodeJunction invalidValue = item<HydraulicNodeJunction>(31115);
            invalidValue.elevation_m = std::numeric_limits<double>::infinity();
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "uuid-membership",
                    baseline.uuid, {{invalidValue.uuid, hydraulic::HydraulicEntity{invalidValue}}},
                    membershipHead, commitMetadata(), &stats);
            }) && !stats.indexedMembershipFastPath &&
                repository.head(project, "uuid-membership") == membershipHead,
                "UUIDv7 indexed membership accepted invalid physical values");

            const native::RevisionId afterLabel = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "uuid-membership", baseline.uuid,
                {{addedLabel.uuid, std::nullopt}}, membershipHead, commitMetadata(), &stats);
            require(afterLabel != membershipHead && stats.indexedMembershipFastPath &&
                    !repository.getModelAtRevision(
                        project, afterLabel, hydraulic::keyFromUuid(addedLabel.uuid)),
                    "UUIDv7 independent generic-curve deletion missed indexed membership path");
            expected.curves_generic.removeLast();
            membershipHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "uuid-membership", baseline.uuid,
                {{addedNode.uuid, std::nullopt}, {addedPipe.uuid, std::nullopt}},
                afterLabel, commitMetadata(), &stats);
            require(membershipHead != afterLabel && stats.indexedMembershipFastPath &&
                    stats.validatedEntities == 2,
                    "UUIDv7 dependent pipe-and-node removal missed indexed membership path");
            compareNetworks(baseline, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, membershipHead, baseline.uuid));
            require(!repository.getModelAtRevision(
                        project, membershipHead, hydraulic::keyFromUuid(addedPipe.uuid)) &&
                    !repository.getModelAtRevision(
                        project, membershipHead, hydraulic::keyFromUuid(addedNode.uuid)),
                    "UUIDv7 deleted membership records remain reachable");
            // Both added hydraulic members have now been removed. The next
            // mixed edit starts from the restored baseline, not the earlier
            // insertion expectation.
            expected = baseline;
            // Mixed existing-record update and insertion is not a membership
            // fast path; it must use the complete canonical validator.
            HydraulicCurveGeneric mixedLabel = item<HydraulicCurveGeneric>(31114);
            HydraulicNodeJunction changedNode = baseline.nodes_junctions[0];
            changedNode.metadata.description = QStringLiteral("mixed batch");
            const native::RevisionId mixedHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "uuid-membership", baseline.uuid,
                {{mixedLabel.uuid, hydraulic::HydraulicEntity{mixedLabel}},
                 {changedNode.uuid, hydraulic::HydraulicEntity{changedNode}}},
                membershipHead, commitMetadata(), &stats);
            require(mixedHead != membershipHead && !stats.indexedMembershipFastPath,
                    "UUIDv7 mixed structural batch bypassed full validation");
            expected.nodes_junctions[0] = changedNode;
            expected.curves_generic.append(mixedLabel);
            membershipHead = mixedHead;
            compareNetworks(expected, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, membershipHead, baseline.uuid));
            compareNetworks(baseline, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, saved, baseline.uuid));
            (void)repository.verifyRepository();
            (void)repository.compact();
        }
        {
            native::Repository reopened(folder);
            require(reopened.head(project, "uuid-membership") == membershipHead,
                    "UUIDv7 membership HEAD changed after reopen/compaction");
            compareNetworks(expected, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                reopened, project, membershipHead, baseline.uuid));
            (void)reopened.verifyRepository();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// Network-root metadata merges field by field and project units per
// quantity: independent changes on two branches combine without a conflict;
// the same quantity changed differently is a named, resolvable conflict.
void exerciseRootFieldMerges() {
    const std::filesystem::path folder = tempDir();
    const native::ProjectId project = native::ProjectId::generate();
    try {
        native::Repository repository(folder);
        const NetworkHydraulicDistribution baseline = completeNetwork();
        require(baseline.presentation_units.has_value() &&
                baseline.presentation_units->units.size() == 2,
                "root merge fixture lacks project units");
        const native::RevisionId root = repository.createRootBranch(project, "main", commitMetadata());
        const native::RevisionId base = hydraulic::saveHydraulicDistributionNetwork(
            repository, project, "main", baseline, root, commitMetadata());
        require(repository.createBranch(project, "feature", "main") == base,
                "root merge fork generated an extra revision");
        require(repository.createBranch(project, "rival", "main") == base,
                "root merge rival fork generated an extra revision");

        NetworkHydraulicDistribution ours = baseline;
        ours.title_line_2 = QStringLiteral("Main title");
        ours.presentation_units->units[1].unit = QStringLiteral("m3/h");    // flow
        const native::RevisionId target = hydraulic::saveHydraulicDistributionNetwork(
            repository, project, "main", ours, base, commitMetadata());
        NetworkHydraulicDistribution theirs = baseline;
        theirs.duration_s = 7200;
        theirs.options_hydraulic.maximum_trials = 77;
        theirs.presentation_units->units[0].unit = QStringLiteral("psi");   // pressure head
        const native::RevisionId source = hydraulic::saveHydraulicDistributionNetwork(
            repository, project, "feature", theirs, base, commitMetadata());

        const native::MergeResult combined = hydraulic::mergeHydraulicBranches(
            repository, project, "main", "feature",
            hydraulic::HydraulicMergeSelections{target, source, {}, {}}, commitMetadata());
        require(combined.status == native::MergeStatus::Merged && combined.resultHead,
                "independent root-field and per-quantity changes did not merge");
        const std::optional<NetworkHydraulicDistribution> merged = hydraulic::loadHydraulicDistributionNetworkAtRevision(
            repository, project, *combined.resultHead, baseline.uuid);
        require(merged && merged->title_line_2 == QStringLiteral("Main title") &&
                merged->duration_s == 7200 && merged->options_hydraulic.maximum_trials == 77 &&
                merged->presentation_units &&
                merged->presentation_units->units.at(0).unit == QStringLiteral("psi") &&
                merged->presentation_units->units.at(1).unit == QStringLiteral("m3/h") &&
                merged->nodes_junctions.size() == baseline.nodes_junctions.size(),
                "merged root lost a branch's field or quantity");

        // Same quantity, different units: one named conflict, both HEADs kept.
        NetworkHydraulicDistribution rival = baseline;
        rival.presentation_units->units[1].unit = QStringLiteral("ML/d");
        const native::RevisionId rivalHead = hydraulic::saveHydraulicDistributionNetwork(
            repository, project, "rival", rival, base, commitMetadata());
        const native::RevisionId mainHead = *combined.resultHead;
        hydraulic::HydraulicMergeSelections selections{mainHead, rivalHead, {}, {}};
        const native::MergeResult clash = hydraulic::mergeHydraulicBranches(
            repository, project, "main", "rival", selections, commitMetadata());
        const std::string flowField = "presentation_units.units.volumetric_flow_rate";
        require(clash.status == native::MergeStatus::Conflicts && clash.conflicts.size() == 1 &&
                clash.conflicts.front().reason.find(flowField) != std::string::npos &&
                repository.head(project, "main") == mainHead &&
                repository.head(project, "rival") == rivalHead,
                "per-quantity conflict was not reported by quantity");

        // A choice for a field that does not conflict is rejected.
        hydraulic::HydraulicMergeSelections stray = selections;
        stray.fields.push_back(hydraulic::HydraulicFieldChoice{
            baseline.uuid, QStringLiteral("presentation_units.units.pressure_head"),
            hydraulic::HydraulicMergeSide::Source});
        require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
            (void)hydraulic::mergeHydraulicBranches(repository, project,
                "main", "rival", stray, commitMetadata());
        }), "choice for a non-conflicting root field was accepted");

        selections.fields.push_back(hydraulic::HydraulicFieldChoice{
            baseline.uuid, QString::fromStdString(flowField), hydraulic::HydraulicMergeSide::Source});
        const native::MergeResult resolved = hydraulic::mergeHydraulicBranches(
            repository, project, "main", "rival", selections, commitMetadata());
        require(resolved.status == native::MergeStatus::Merged && resolved.resultHead,
                "explicit per-quantity choice did not resolve the conflict");
        const std::optional<NetworkHydraulicDistribution> finalNetwork = hydraulic::loadHydraulicDistributionNetworkAtRevision(
            repository, project, *resolved.resultHead, baseline.uuid);
        require(finalNetwork && finalNetwork->presentation_units &&
                finalNetwork->presentation_units->units.at(1).unit == QStringLiteral("ML/d") &&
                finalNetwork->presentation_units->units.at(0).unit == QStringLiteral("psi") &&
                finalNetwork->title_line_2 == QStringLiteral("Main title"),
                "per-quantity choice applied the wrong side");
        (void)repository.verifyRepository();
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

void exerciseUuidV7HydraulicMerges() {
    const std::filesystem::path folder = tempDir();
    const native::ProjectId project = native::ProjectId::generate();
    try {
        native::RevisionId resolved = native::RevisionId::generate();
        const NetworkHydraulicDistribution baseline = completeNetwork();
        {
            native::Repository repository(folder);
            const native::RevisionId root = repository.createRootBranch(project, "main", commitMetadata());
            const native::RevisionId base = hydraulic::saveHydraulicDistributionNetwork(
                repository, project, "main", baseline, root, commitMetadata());
            require(repository.createBranch(project, "feature", "main") == base,
                    "UUIDv7 hydraulic fork generated an extra revision");
            HydraulicNodeJunction targetJunction = baseline.nodes_junctions[0];
            targetJunction.metadata.description = QStringLiteral("hydraulic-target");
            const native::RevisionId target = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "main", baseline.uuid,
                {{targetJunction.uuid, hydraulic::HydraulicEntity{targetJunction}}}, base, commitMetadata());
            HydraulicNodeJunction sourceJunction = baseline.nodes_junctions[0];
            sourceJunction.metadata.description = QStringLiteral("hydraulic-source");
            const native::RevisionId source = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "feature", baseline.uuid,
                {{sourceJunction.uuid, hydraulic::HydraulicEntity{sourceJunction}}}, base, commitMetadata());
            hydraulic::HydraulicMergeSelections selections{target, source, {}, {}};
            const std::uint64_t before = repository.packSize();
            const native::MergeResult conflict = hydraulic::mergeHydraulicBranches(
                repository, project, "main", "feature", selections, commitMetadata());
            require(conflict.status == native::MergeStatus::Conflicts &&
                    !conflict.conflicts.empty() && repository.head(project, "main") == target &&
                    repository.head(project, "feature") == source &&
                    repository.packSize() == before,
                    "UUIDv7 hydraulic conflict changed immutable revisions");
            selections.fields.push_back(hydraulic::HydraulicFieldChoice{
                targetJunction.uuid, QStringLiteral("metadata.description"),
                hydraulic::HydraulicMergeSide::Source});
            const native::MergeResult merged = hydraulic::mergeHydraulicBranches(
                repository, project, "main", "feature", selections, commitMetadata());
            require(merged.status == native::MergeStatus::Merged && merged.resultHead &&
                    repository.head(project, "feature") == source,
                    "UUIDv7 hydraulic explicit field choice did not produce a merge");
            resolved = *merged.resultHead;
            const native::RevisionRecord record = native::decodeRevisionRecord(
                *repository.readRevision(project, resolved));
            require(record.parents.size() == 2 && record.parents[0].id == target &&
                    record.parents[1].id == source,
                    "UUIDv7 resolved hydraulic merge lost its two parent identities");
            NetworkHydraulicDistribution expected = baseline;
            expected.nodes_junctions[0] = sourceJunction;
            const std::optional<NetworkHydraulicDistribution> actual = hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, resolved, baseline.uuid);
            require(actual.has_value(), "resolved UUIDv7 network missing");
            compareNetworks(expected, *actual);
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::mergeHydraulicBranches(repository, project,
                    "main", "feature", selections, commitMetadata());
            }), "stale UUIDv7 hydraulic merge selections were accepted");

            // A chosen UUID must actually participate in the merge; silent
            // ignored choices would apply outdated GUI decisions to new data.
            hydraulic::HydraulicMergeSelections unused{resolved, source, {}, {}};
            unused.objects.push_back(hydraulic::HydraulicObjectChoice{
                uuid(9700), hydraulic::HydraulicMergeSide::Target});
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::mergeHydraulicBranches(repository, project,
                    "main", "feature", unused, commitMetadata());
            }), "unused UUIDv7 hydraulic object choice was silently accepted");

            // The native publication guard prevents an orphan hydraulic
            // member revision from being created at all. Previously this
            // fixture intentionally committed an invalid source branch and
            // depended on merge validation to reject it later.
            require(repository.createBranchAtRevision(project,
                "invalid-left", "main", base) == base, "cannot create invalid merge target");
            require(repository.createBranchAtRevision(project,
                "invalid-right", "main", base) == base, "cannot create invalid merge source");
            const native::Key existingKey = hydraulic::keyFromUuid(baseline.nodes_junctions[0].uuid);
            const std::optional<native::ModelRecord> member = repository.getModelAtRevision(
                project, base, existingKey);
            require(member.has_value(), "missing hydraulic member fixture");
            const std::uint64_t sizeBefore = repository.packSize();
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)repository.commitModel(
                    project, "invalid-right", {{hydraulic::keyFromUuid(uuid(9701)), *member}},
                    commitMetadata(), base, false, std::nullopt,
                    native::annotations::validateAnnotationSnapshot);
            }), "invalid orphan hydraulic commit was accepted");
            require(repository.head(project, "invalid-left") == base &&
                    repository.head(project, "invalid-right") == base &&
                    repository.packSize() == sizeBefore,
                    "rejected invalid engineering commit modified immutable history");
            (void)repository.verifyRepository();
            (void)repository.compact();
        }
        {
            native::Repository reopened(folder);
            require(reopened.head(project, "main") == resolved &&
                    reopened.revisionReachableFrom(project, resolved,
                        reopened.head(project, "feature")),
                    "resolved hydraulic UUIDv7 ancestry lost after compaction/reopen");
            (void)reopened.verifyRepository();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}


// UUIDv7 equivalent of the coordinated membership merge qualification. This
// intentionally uses only project-scoped immutable heads and pinned selections:
// no SHA branch history or implicit, moving-head hydraulic reads.
void exerciseUuidV7CoordinatedMembershipMerges() {
    const std::filesystem::path folder = tempDir();
    const native::ProjectId project = native::ProjectId::generate();
    const NetworkHydraulicDistribution baseline = completeNetwork();
    native::RevisionId mergedAppendHead = native::RevisionId::generate();
    native::RevisionId mergedDeleteHead = native::RevisionId::generate();
    native::RevisionId mergedInitialChunkHead = native::RevisionId::generate();
    try {
        {
            native::Repository repository(folder);
            const native::RevisionId raw = repository.createRootBranch(project, "main", commitMetadata());
            const native::RevisionId base = hydraulic::saveHydraulicDistributionNetwork(
                repository, project, "main", baseline, raw, commitMetadata());
            require(repository.createBranch(project, "append-left", "main") == base &&
                    repository.createBranch(project, "append-right", "main") == base,
                    "UUIDv7 membership merge branches did not share the same base");

            HydraulicCurveGeneric labelA = item<HydraulicCurveGeneric>(8501);
            HydraulicCurveGeneric labelB = item<HydraulicCurveGeneric>(8502);
            labelA.id = QStringLiteral("uuidv7 left append");
            labelB.id = QStringLiteral("uuidv7 right append");
            const native::RevisionId leftHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "append-left", baseline.uuid,
                {{labelA.uuid, hydraulic::HydraulicEntity{labelA}}}, base, commitMetadata());
            const native::RevisionId rightHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "append-right", baseline.uuid,
                {{labelB.uuid, hydraulic::HydraulicEntity{labelB}}}, base, commitMetadata());
            const native::MergeResult appended = hydraulic::mergeHydraulicBranches(
                repository, project, "append-left", "append-right",
                hydraulic::HydraulicMergeSelections{leftHead, rightHead, {}, {}}, commitMetadata());
            require(appended.status == native::MergeStatus::Merged && appended.resultHead &&
                    repository.head(project, "append-right") == rightHead,
                    "UUIDv7 concurrent appends failed or changed the source branch");
            mergedAppendHead = *appended.resultHead;
            const std::optional<native::Bytes> appendRecordBytes =
                repository.readRevision(project, mergedAppendHead);
            require(appendRecordBytes.has_value(), "UUIDv7 merge revision missing");
            const native::RevisionRecord appendRecord =
                native::decodeRevisionRecord(*appendRecordBytes);
            require(appendRecord.parents.size() == 2 &&
                    appendRecord.parents[0].id == leftHead &&
                    appendRecord.parents[1].id == rightHead,
                    "UUIDv7 concurrent append merge lost parent ancestry");
            NetworkHydraulicDistribution combined = baseline;
            combined.curves_generic.append(labelA);
            combined.curves_generic.append(labelB);
            const std::optional<NetworkHydraulicDistribution> mergedAppend =
                hydraulic::loadHydraulicDistributionNetworkAtRevision(
                    repository, project, mergedAppendHead, baseline.uuid);
            require(mergedAppend.has_value(), "UUIDv7 appended network is missing");
            compareNetworks(combined, *mergedAppend);
            const std::optional<NetworkHydraulicDistribution> immutableBase =
                hydraulic::loadHydraulicDistributionNetworkAtRevision(repository, project, base, baseline.uuid);
            require(immutableBase.has_value(), "UUIDv7 append mutated the pinned base");
            compareNetworks(baseline, *immutableBase);

            require(repository.createBranch(project, "remove-left", "append-left") ==
                    mergedAppendHead &&
                    repository.createBranch(project, "remove-right", "append-left") ==
                    mergedAppendHead, "UUIDv7 delete branches did not fork from merged head");
            const native::RevisionId removedLeft = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "remove-left", baseline.uuid,
                {{labelA.uuid, std::nullopt}}, mergedAppendHead, commitMetadata());
            const native::RevisionId removedRight = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "remove-right", baseline.uuid,
                {{labelB.uuid, std::nullopt}}, mergedAppendHead, commitMetadata());
            const native::MergeResult removed = hydraulic::mergeHydraulicBranches(
                repository, project, "remove-left", "remove-right",
                hydraulic::HydraulicMergeSelections{removedLeft, removedRight, {}, {}}, commitMetadata());
            require(removed.status == native::MergeStatus::Merged && removed.resultHead &&
                    repository.head(project, "remove-right") == removedRight,
                    "UUIDv7 independent deletions did not reconcile");
            mergedDeleteHead = *removed.resultHead;
            const std::optional<NetworkHydraulicDistribution> mergedDelete =
                hydraulic::loadHydraulicDistributionNetworkAtRevision(
                    repository, project, mergedDeleteHead, baseline.uuid);
            require(mergedDelete.has_value(), "UUIDv7 deleted network is missing");
            compareNetworks(baseline, *mergedDelete);

            // Both new branches create the same deterministic first chunk ID,
            // but with distinct contents. The merge must reconcile membership.
            require(repository.createBranch(project, "empty-labels", "main") == base,
                    "UUIDv7 empty-label branch has an unexpected base");
            NetworkHydraulicDistribution withoutLabels = baseline;
            withoutLabels.curves_generic.clear();
            const native::RevisionId emptyLabelsHead = hydraulic::saveHydraulicDistributionNetwork(
                repository, project, "empty-labels", withoutLabels, base, commitMetadata());
            require(repository.createBranch(project, "first-left", "empty-labels") ==
                    emptyLabelsHead &&
                    repository.createBranch(project, "first-right", "empty-labels") ==
                    emptyLabelsHead, "UUIDv7 initial-chunk branches have different bases");
            const native::RevisionId firstLeftHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "first-left", baseline.uuid,
                {{labelA.uuid, hydraulic::HydraulicEntity{labelA}}}, emptyLabelsHead, commitMetadata());
            const native::RevisionId firstRightHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "first-right", baseline.uuid,
                {{labelB.uuid, hydraulic::HydraulicEntity{labelB}}}, emptyLabelsHead, commitMetadata());
            const native::MergeResult firstChunk = hydraulic::mergeHydraulicBranches(
                repository, project, "first-left", "first-right",
                hydraulic::HydraulicMergeSelections{firstLeftHead, firstRightHead, {}, {}}, commitMetadata());
            require(firstChunk.status == native::MergeStatus::Merged && firstChunk.resultHead &&
                    repository.head(project, "first-right") == firstRightHead,
                    "UUIDv7 concurrent initial membership chunks did not reconcile");
            mergedInitialChunkHead = *firstChunk.resultHead;
            withoutLabels.curves_generic.append(labelA);
            withoutLabels.curves_generic.append(labelB);
            const std::optional<NetworkHydraulicDistribution> mergedInitialChunk =
                hydraulic::loadHydraulicDistributionNetworkAtRevision(
                    repository, project, mergedInitialChunkHead, baseline.uuid);
            require(mergedInitialChunk.has_value(), "UUIDv7 initial-chunk network missing");
            compareNetworks(withoutLabels, *mergedInitialChunk);

            // The conflicting branch changes list order rather than merely
            // appending; do not discard its change during append reconciliation.
            require(repository.createBranch(project, "order-left", "append-left") ==
                    mergedAppendHead &&
                    repository.createBranch(project, "order-right", "append-left") ==
                    mergedAppendHead, "UUIDv7 reorder branches have different bases");
            NetworkHydraulicDistribution reordered = combined;
            reordered.curves_generic.swapItemsAt(0, 1);
            const native::RevisionId orderLeft = hydraulic::saveHydraulicDistributionNetwork(
                repository, project, "order-left", reordered, mergedAppendHead, commitMetadata());
            HydraulicCurveGeneric labelC = item<HydraulicCurveGeneric>(8503);
            const native::RevisionId orderRight = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "order-right", baseline.uuid,
                {{labelC.uuid, hydraulic::HydraulicEntity{labelC}}}, mergedAppendHead, commitMetadata());
            const std::uint64_t beforeConflict = repository.packSize();
            const native::MergeResult orderConflict = hydraulic::mergeHydraulicBranches(
                repository, project, "order-left", "order-right",
                hydraulic::HydraulicMergeSelections{orderLeft, orderRight, {}, {}}, commitMetadata());
            require(orderConflict.status == native::MergeStatus::Conflicts &&
                    repository.head(project, "order-left") == orderLeft &&
                    repository.head(project, "order-right") == orderRight &&
                    repository.packSize() == beforeConflict,
                    "UUIDv7 membership reordering was silently discarded or published");
            (void)repository.verifyRepository();
            (void)repository.compact();
        }
        {
            native::Repository reopened(folder);
            require(reopened.head(project, "append-left") == mergedAppendHead &&
                    reopened.head(project, "remove-left") == mergedDeleteHead &&
                    reopened.head(project, "first-left") == mergedInitialChunkHead,
                    "UUIDv7 membership merges lost their HEADs after reopening");
            const std::optional<NetworkHydraulicDistribution> removed =
                hydraulic::loadHydraulicDistributionNetworkAtRevision(
                    reopened, project, mergedDeleteHead, baseline.uuid);
            require(removed.has_value(), "UUIDv7 merged removal missing after reopen");
            compareNetworks(baseline, *removed);
            (void)reopened.verifyRepository();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}


// Project-scoped UUIDv7 coverage for interactive field and
// whole-object merge-selection qualification. Rejected selections must never
// move a branch tip or append a pack record.
void exerciseUuidV7MergeChoiceQualification() {
    const std::filesystem::path folder = tempDir();
    const native::ProjectId project = native::ProjectId::generate();
    const NetworkHydraulicDistribution baseline = completeNetwork();
    try {
        {
            native::Repository repository(folder);
            const native::RevisionId root = repository.createRootBranch(project, "main", commitMetadata());
            const native::RevisionId base = hydraulic::saveHydraulicDistributionNetwork(
                repository, project, "main", baseline, root, commitMetadata());
            const std::function<void(const std::string&, const native::RevisionId&)> fork =
                [&repository, &project](const std::string& name, const native::RevisionId& parent) {
                    require(repository.createBranchAtRevision(project, name, "main", parent) == parent,
                            "UUIDv7 merge-choice branch origin was not preserved");
                };
            const std::function<void(const std::string&, const std::string&,
                                     const hydraulic::HydraulicMergeSelections&, const char*)> reject =
                [&repository, &project](const std::string& target, const std::string& source,
                                        const hydraulic::HydraulicMergeSelections& choice,
                                        const char* message) {
                    const native::RevisionId targetHead = repository.head(project, target);
                    const native::RevisionId sourceHead = repository.head(project, source);
                    const std::uint64_t bytesBefore = repository.packSize();
                    bool rejected = false;
                    try {
                        (void)hydraulic::mergeHydraulicBranches(repository, project,
                                                                target, source, choice, commitMetadata());
                    } catch (const std::exception&) {
                        rejected = true;
                    }
                    require(rejected && repository.head(project, target) == targetHead &&
                            repository.head(project, source) == sourceHead &&
                            repository.packSize() == bytesBefore, message);
                };

            // Concurrent changes to one field require a pinned, precise choice;
            // non-conflicting changes on the source must still be incorporated.
            fork("field-left", base);
            fork("field-right", base);
            HydraulicLinkPipe leftPipe = baseline.links_pipes[0];
            HydraulicLinkPipe rightPipe = baseline.links_pipes[0];
            leftPipe.diameter_mm = 190.0;
            rightPipe.diameter_mm = 200.0;
            rightPipe.length_calculated_m = 45.0;
            const native::RevisionId fieldLeft = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "field-left", baseline.uuid,
                {{leftPipe.uuid, hydraulic::HydraulicEntity{leftPipe}}}, base, commitMetadata());
            const native::RevisionId fieldRight = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "field-right", baseline.uuid,
                {{rightPipe.uuid, hydraulic::HydraulicEntity{rightPipe}}}, base, commitMetadata());
            hydraulic::HydraulicMergeSelections targetChoice{fieldLeft, fieldRight, {}, {}};
            targetChoice.fields.push_back({leftPipe.uuid, QStringLiteral("diameter_mm"),
                                           hydraulic::HydraulicMergeSide::Target});
            hydraulic::HydraulicMergeSelections stale = targetChoice;
            stale.expectedSourceHead = base;
            reject("field-left", "field-right", stale, "stale UUIDv7 field choice committed");
            hydraulic::HydraulicMergeSelections duplicate = targetChoice;
            duplicate.fields.push_back(targetChoice.fields[0]);
            reject("field-left", "field-right", duplicate, "duplicate UUIDv7 field choice committed");
            hydraulic::HydraulicMergeSelections unused = targetChoice;
            unused.fields[0].field = QStringLiteral("length_calculated_m");
            reject("field-left", "field-right", unused, "nonconflicting UUIDv7 field choice committed");
            const native::MergeResult fieldMerge = hydraulic::mergeHydraulicBranches(
                repository, project, "field-left", "field-right", targetChoice, commitMetadata());
            require(fieldMerge.status == native::MergeStatus::Merged && fieldMerge.resultHead &&
                    repository.head(project, "field-right") == fieldRight,
                    "UUIDv7 target-side field choice did not merge");
            NetworkHydraulicDistribution fieldExpected = baseline;
            fieldExpected.links_pipes[0] = rightPipe;
            fieldExpected.links_pipes[0].diameter_mm = leftPipe.diameter_mm;
            const std::optional<NetworkHydraulicDistribution> fieldActual = hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, *fieldMerge.resultHead, baseline.uuid);
            require(fieldActual.has_value(), "UUIDv7 field choice network missing");
            compareNetworks(fieldExpected, *fieldActual);
            const native::RevisionRecord fieldParents = native::decodeRevisionRecord(
                *repository.readRevision(project, *fieldMerge.resultHead));
            require(fieldParents.parents.size() == 2 && fieldParents.parents[0].id == fieldLeft &&
                    fieldParents.parents[1].id == fieldRight,
                    "UUIDv7 chosen field merge lost parent ancestry");
            reject("field-left", "field-right", targetChoice,
                   "already applied UUIDv7 field choice was replayed");

            // Delete-versus-modify conflicts require an explicit object choice.
            // Test both keeping the modified object and keeping its deletion.
            fork("object-deleted", base);
            fork("object-modified", base);
            HydraulicCurveGeneric modifiedLabel = baseline.curves_generic[0];
            modifiedLabel.id = QStringLiteral("edited label retained");
            const native::RevisionId deleted = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "object-deleted", baseline.uuid,
                {{modifiedLabel.uuid, std::nullopt}}, base, commitMetadata());
            const native::RevisionId modified = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "object-modified", baseline.uuid,
                {{modifiedLabel.uuid, hydraulic::HydraulicEntity{modifiedLabel}}}, base, commitMetadata());
            hydraulic::HydraulicMergeSelections deleteChoice{deleted, modified, {}, {}};
            deleteChoice.objects.push_back({modifiedLabel.uuid, hydraulic::HydraulicMergeSide::Target});
            hydraulic::HydraulicMergeSelections duplicateObject = deleteChoice;
            duplicateObject.objects.push_back(deleteChoice.objects[0]);
            reject("object-deleted", "object-modified", duplicateObject,
                   "duplicate UUIDv7 object choice was accepted");
            hydraulic::HydraulicMergeSelections overlapping = deleteChoice;
            overlapping.fields.push_back({modifiedLabel.uuid, QStringLiteral("id"),
                                          hydraulic::HydraulicMergeSide::Source});
            reject("object-deleted", "object-modified", overlapping,
                   "UUIDv7 field and object choice overlapped");
            hydraulic::HydraulicMergeSelections unknownObject = deleteChoice;
            unknownObject.objects[0].entityUuid = uuid(9992);
            reject("object-deleted", "object-modified", unknownObject,
                   "unknown UUIDv7 object choice was accepted");
            const native::MergeResult deleteMerge = hydraulic::mergeHydraulicBranches(
                repository, project, "object-deleted", "object-modified", deleteChoice, commitMetadata());
            require(deleteMerge.status == native::MergeStatus::Merged && deleteMerge.resultHead &&
                    repository.head(project, "object-modified") == modified,
                    "UUIDv7 delete-side object selection failed");
            NetworkHydraulicDistribution withoutLabel = baseline;
            withoutLabel.curves_generic.clear();
            const std::optional<NetworkHydraulicDistribution> deletion = hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, *deleteMerge.resultHead, baseline.uuid);
            require(deletion.has_value(), "UUIDv7 deleted label network missing");
            compareNetworks(withoutLabel, *deletion);

            fork("object-keep", base);
            fork("object-remove", base);
            const native::RevisionId kept = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "object-keep", baseline.uuid,
                {{modifiedLabel.uuid, hydraulic::HydraulicEntity{modifiedLabel}}}, base, commitMetadata());
            const native::RevisionId removed = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "object-remove", baseline.uuid,
                {{modifiedLabel.uuid, std::nullopt}}, base, commitMetadata());
            hydraulic::HydraulicMergeSelections keepChoice{kept, removed, {}, {}};
            keepChoice.objects.push_back({modifiedLabel.uuid, hydraulic::HydraulicMergeSide::Target});
            const native::MergeResult keepMerge = hydraulic::mergeHydraulicBranches(
                repository, project, "object-keep", "object-remove", keepChoice, commitMetadata());
            require(keepMerge.status == native::MergeStatus::Merged && keepMerge.resultHead &&
                    repository.head(project, "object-remove") == removed,
                    "UUIDv7 surviving-side object selection failed");
            NetworkHydraulicDistribution withLabel = baseline;
            withLabel.curves_generic[0] = modifiedLabel;
            const std::optional<NetworkHydraulicDistribution> surviving = hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, *keepMerge.resultHead, baseline.uuid);
            require(surviving.has_value(), "UUIDv7 kept label network missing");
            compareNetworks(withLabel, *surviving);
            (void)repository.verifyRepository();
            (void)repository.compact();
        }
        {
            native::Repository reopened(folder);
            const native::RevisionId deletedHead = reopened.head(project, "object-deleted");
            const native::RevisionId keptHead = reopened.head(project, "object-keep");
            const std::optional<NetworkHydraulicDistribution> deletedNetwork =
                hydraulic::loadHydraulicDistributionNetworkAtRevision(reopened, project, deletedHead, baseline.uuid);
            const std::optional<NetworkHydraulicDistribution> keptNetwork =
                hydraulic::loadHydraulicDistributionNetworkAtRevision(reopened, project, keptHead, baseline.uuid);
            require(deletedNetwork.has_value() && keptNetwork.has_value(),
                    "UUIDv7 chosen merge result disappeared after compaction");
            NetworkHydraulicDistribution withoutLabel = baseline;
            withoutLabel.curves_generic.clear();
            compareNetworks(withoutLabel, *deletedNetwork);
            NetworkHydraulicDistribution withLabel = baseline;
            withLabel.curves_generic[0].id = QStringLiteral("edited label retained");
            compareNetworks(withLabel, *keptNetwork);
            (void)reopened.verifyRepository();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}


// UUIDv7 caller-pinned write qualification.
// Even an empty edit must validate the caller's pinned branch tip before it
// decides there is nothing to publish.
void exerciseUuidV7CallerPinnedWrites() {
    const std::filesystem::path folder = tempDir();
    try {
        native::Repository repository(folder);
        const native::ProjectId project = native::ProjectId::generate();
        const NetworkHydraulicDistribution network = completeNetwork();
        const native::RevisionId raw = repository.createRootBranch(project, "main", commitMetadata());
        const native::RevisionId initial = hydraulic::saveHydraulicDistributionNetwork(
            repository, project, "main", network, raw, commitMetadata());
        require(initial != raw && repository.head(project, "main") == initial,
                "UUIDv7 first pinned network save did not commit");
        require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
            (void)hydraulic::saveHydraulicDistributionNetwork(repository, project, "main", network, raw, commitMetadata());
        }) && repository.head(project, "main") == initial,
                "UUIDv7 full-network save accepted stale HEAD");

        HydraulicNodeJunction junction = network.nodes_junctions[0];
        junction.elevation_m += 2.0;
        const std::vector<hydraulic::HydraulicEntityChange> changes{
            {junction.uuid, hydraulic::HydraulicEntity{junction}}};
        const native::RevisionId edited = hydraulic::editHydraulicDistributionNetwork(
            repository, project, "main", network.uuid, changes, initial, commitMetadata());
        require(edited != initial && repository.head(project, "main") == edited,
                "UUIDv7 caller-pinned indexed edit did not commit");
        require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
            (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "main",
                network.uuid, changes, initial, commitMetadata());
        }) && throwsStorageError(native::StorageErrorKind::Conflict, [&] {
            (void)hydraulic::editHydraulicDistributionNetwork(repository, project, "main",
                network.uuid, {}, initial, commitMetadata());
        }) && repository.head(project, "main") == edited,
                "UUIDv7 stale edit or empty edit accepted an outdated HEAD");
        const std::uint64_t packBefore = repository.packSize();
        const native::RevisionId noop = hydraulic::editHydraulicDistributionNetwork(
            repository, project, "main", network.uuid, {}, edited, commitMetadata());
        require(noop == edited && repository.head(project, "main") == edited &&
                repository.packSize() == packBefore,
                "UUIDv7 pinned empty edit created a revision");
        NetworkHydraulicDistribution expected = network;
        expected.nodes_junctions[0] = junction;
        const std::optional<NetworkHydraulicDistribution> restored = hydraulic::loadHydraulicDistributionNetworkAtRevision(
            repository, project, edited, network.uuid);
        require(restored.has_value(), "UUIDv7 pinned edit network missing");
        compareNetworks(expected, *restored);
        (void)repository.verifyRepository();
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// UUIDv7 concurrent-snapshot qualification.
// Readers pin one immutable revision before loading the complete aggregate,
// its bounded membership and a targeted indexed entity. The writer changes
// both root metadata and membership, so mixing revisions would be observable.
void exerciseUuidV7ConcurrentSnapshots() {
    const std::filesystem::path folder = tempDir();
    try {
        const native::ProjectId project = native::ProjectId::generate();
        NetworkHydraulicDistribution baseline = completeNetwork();
        baseline.title_line_1 = QStringLiteral("2");
        baseline.nodes_junctions[0].metadata.description = QStringLiteral("2");
        baseline.curves_generic[0].id = QStringLiteral("2");
        HydraulicCurveGeneric secondLabel = item<HydraulicCurveGeneric>(9102);
        secondLabel.id = QStringLiteral("2");
        baseline.curves_generic.append(secondLabel);
        {
            native::Repository repository(folder);
            const native::RevisionId raw = repository.createRootBranch(project, "main", commitMetadata());
            const native::RevisionId base = hydraulic::saveHydraulicDistributionNetwork(
                repository, project, "main", baseline, raw, commitMetadata());
            std::atomic<bool> begin{false};
            std::atomic<int> checks{0};
            std::exception_ptr failure;
            std::mutex failureMutex;
            const std::function<void(std::exception_ptr)> reportFailure =
                [&failure, &failureMutex](std::exception_ptr error) {
                    std::lock_guard<std::mutex> guard(failureMutex);
                    if (!failure) failure = error;
                };
            const std::function<void()> reader = [&]() {
                try {
                    while (!begin.load()) std::this_thread::yield();
                    for (int index = 0; index < 80; ++index) {
                        const native::RevisionId pinned = repository.head(project, "main");
                        const std::optional<NetworkHydraulicDistribution> observed =
                            hydraulic::loadHydraulicDistributionNetworkAtRevision(
                                repository, project, pinned, baseline.uuid);
                        require(observed.has_value(), "UUIDv7 concurrent snapshot missing");
                        const QString marker = observed->title_line_1;
                        bool valid = false;
                        const int generation = marker.toInt(&valid);
                        require(valid && generation >= 2 && generation <= 24 &&
                                observed->nodes_junctions[0].metadata.description == marker &&
                                observed->curves_generic[0].id == marker &&
                                observed->curves_generic.size() == (generation % 2 == 0 ? 2 : 1),
                                "UUIDv7 pinned aggregate mixed generations");
                        const std::optional<QList<QUuid>> ids =
                            hydraulic::listHydraulicEntityIdsAtRevisionLimited(
                                repository, project, pinned, baseline.uuid,
                                hydraulic::HydraulicEntityType::GenericCurve, 2);
                        const std::optional<hydraulic::HydraulicEntity> label =
                            hydraulic::loadHydraulicEntityAtRevision(
                                repository, project, pinned, baseline.uuid,
                                baseline.curves_generic[0].uuid);
                        require(ids && ids->size() == observed->curves_generic.size() &&
                                ids->front() == baseline.curves_generic[0].uuid &&
                                label && std::holds_alternative<HydraulicCurveGeneric>(*label) &&
                                std::get<HydraulicCurveGeneric>(*label).id == marker,
                                "UUIDv7 revision-pinned indexed readers mixed generations");
                        ++checks;
                        std::this_thread::yield();
                    }
                } catch (...) { reportFailure(std::current_exception()); }
            };
            std::thread first(reader);
            std::thread second(reader);
            std::thread writer([&]() {
                try {
                    native::RevisionId expectedHead = base;
                    NetworkHydraulicDistribution next = baseline;
                    begin.store(true);
                    for (int generation = 3; generation <= 24; ++generation) {
                        const QString marker = QString::number(generation);
                        next.title_line_1 = marker;
                        next.nodes_junctions[0].metadata.description = marker;
                        next.curves_generic[0].id = marker;
                        if (generation % 2 != 0) next.curves_generic.removeLast();
                        else if (next.curves_generic.size() == 1) {
                            secondLabel.id = marker;
                            next.curves_generic.append(secondLabel);
                        } else {
                            next.curves_generic[1].id = marker;
                        }
                        expectedHead = hydraulic::saveHydraulicDistributionNetwork(
                            repository, project, "main", next, expectedHead, commitMetadata());
                        std::this_thread::yield();
                    }
                } catch (...) { reportFailure(std::current_exception()); }
            });
            first.join();
            second.join();
            writer.join();
            if (failure) std::rethrow_exception(failure);
            require(checks == 160, "UUIDv7 concurrent snapshot readers did not finish");
            const std::optional<NetworkHydraulicDistribution> original =
                hydraulic::loadHydraulicDistributionNetworkAtRevision(
                    repository, project, base, baseline.uuid);
            require(original.has_value(), "UUIDv7 original snapshot disappeared");
            compareNetworks(baseline, *original);
            (void)repository.verifyRepository();
            (void)repository.compact();
        }
        {
            native::Repository reopened(folder);
            const native::RevisionId latest = reopened.head(project, "main");
            const std::optional<NetworkHydraulicDistribution> last =
                hydraulic::loadHydraulicDistributionNetworkAtRevision(
                    reopened, project, latest, baseline.uuid);
            require(last && last->title_line_1 == QStringLiteral("24") &&
                    last->nodes_junctions[0].metadata.description == QStringLiteral("24") &&
                    last->curves_generic.size() == 2 &&
                    last->curves_generic[0].id == QStringLiteral("24"),
                    "UUIDv7 concurrent writer result missing after compaction/reopen");
            (void)reopened.verifyRepository();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}


// Canonical UUIDv7 qualification for the bounded 256-member chunk boundary.
// These assertions protect multi-chunk ownership and retirement behavior.
void exerciseUuidV7ChunkBoundaries() {
    const std::filesystem::path folder = tempDir();
    const native::ProjectId project = native::ProjectId::generate();
    NetworkHydraulicDistribution baseline;
    baseline.uuid = uuid(800000);
    baseline.id = QStringLiteral("uuidv7-chunk-boundaries");
    for (int index = 0; index < 514; ++index) {
        baseline.nodes_junctions.append(item<HydraulicNodeJunction>(801000 + index));
        if (index > 0) {
            HydraulicLinkPipe pipe = item<HydraulicLinkPipe>(802000 + index);
            pipe.node_uuid_from = baseline.nodes_junctions[index - 1].uuid;
            pipe.node_uuid_to = baseline.nodes_junctions[index].uuid;
            baseline.links_pipes.append(pipe);
        }
    }
    for (int index = 0; index < 260; ++index)
        baseline.curves_generic.append(item<HydraulicCurveGeneric>(803000 + index));
    require(aowis::dvmdb::validateHydraulicDistributionNetwork(baseline).valid(),
            "UUIDv7 chunk fixture failed MODEL validation");
    native::RevisionId saved = native::RevisionId::generate();
    native::RevisionId finalHead = native::RevisionId::generate();
    NetworkHydraulicDistribution expected = baseline;
    try {
        {
            native::Repository repository(folder);
            const native::RevisionId genesis = repository.createRootBranch(project, "main", commitMetadata());
            saved = hydraulic::saveHydraulicDistributionNetwork(repository, project, "main", baseline, genesis, commitMetadata());
            const std::optional<native::ModelRecord> originalRoot = repository.getModelAtRevision(
                project, saved, hydraulic::keyFromUuid(baseline.uuid));
            require(originalRoot && originalRoot->references.size() == 8,
                    "UUIDv7 membership did not split 514/513/260 entities into bounded chunks");
            require(repository.createBranch(project, "feature", "main") == saved,
                    "UUIDv7 chunked branch did not retain immutable base");
            HydraulicNodeJunction addedNode = item<HydraulicNodeJunction>(804000);
            HydraulicLinkPipe addedPipe = item<HydraulicLinkPipe>(804001);
            addedPipe.node_uuid_from = baseline.nodes_junctions[0].uuid;
            addedPipe.node_uuid_to = addedNode.uuid;
            hydraulic::HydraulicEditDiagnostics stats;
            finalHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "feature", baseline.uuid,
                {{addedNode.uuid, hydraulic::HydraulicEntity{addedNode}},
                 {addedPipe.uuid, hydraulic::HydraulicEntity{addedPipe}}}, saved, commitMetadata(), &stats);
            require(stats.indexedMembershipFastPath, "UUIDv7 bounded member insertion missed indexed path");
            expected.nodes_junctions.append(addedNode);
            expected.links_pipes.append(addedPipe);
            for (int index = 0; index < 7; ++index) {
                finalHead = hydraulic::editHydraulicDistributionNetwork(
                    repository, project, "feature", baseline.uuid,
                    {{baseline.curves_generic[index].uuid, std::nullopt}}, finalHead, commitMetadata(), &stats);
                require(stats.indexedMembershipFastPath, "UUIDv7 chunk generic-curve deletion missed indexed path");
                expected.curves_generic.removeFirst();
            }
            std::vector<hydraulic::HydraulicEntityChange> finalChunkRemoval;
            for (int index = 256; index < 260; ++index)
                finalChunkRemoval.push_back({baseline.curves_generic[index].uuid, std::nullopt});
            finalHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "feature", baseline.uuid, finalChunkRemoval, finalHead, commitMetadata(), &stats);
            require(stats.indexedMembershipFastPath, "UUIDv7 empty final chunk missed indexed retirement");
            for (int index = 0; index < 4; ++index) expected.curves_generic.removeLast();
            const std::optional<native::ModelRecord> root = repository.getModelAtRevision(
                project, finalHead, hydraulic::keyFromUuid(baseline.uuid));
            require(root && root->references.size() == 7,
                    "UUIDv7 empty chunk descriptor was not retired");
            compareNetworks(expected, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, finalHead, baseline.uuid));
            compareNetworks(baseline, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, saved, baseline.uuid));
            const std::optional<QList<QUuid>> labels = hydraulic::listHydraulicEntityIdsAtRevisionLimited(
                repository, project, finalHead, baseline.uuid, hydraulic::HydraulicEntityType::GenericCurve, 255);
            require(labels && labels->size() == expected.curves_generic.size() &&
                    labels->first() == expected.curves_generic.first().uuid,
                    "UUIDv7 chunked membership order was not preserved");
            require(throwsStorageError(native::StorageErrorKind::LimitExceeded, [&] {
                (void)hydraulic::listHydraulicEntityIdsAtRevisionLimited(
                    repository, project, saved, baseline.uuid, hydraulic::HydraulicEntityType::Junction, 256);
            }), "UUIDv7 bounded list accepted more than 256 members");
            HydraulicLinkPipe rewired = expected.links_pipes[0];
            rewired.node_uuid_to = expected.nodes_junctions[2].uuid;
            finalHead = hydraulic::editHydraulicDistributionNetwork(
                repository, project, "feature", baseline.uuid,
                {{rewired.uuid, hydraulic::HydraulicEntity{rewired}}}, finalHead, commitMetadata(), &stats);
            require(stats.indexedTopologyFastPath,
                    "UUIDv7 chunked owner lookup blocked independent indexed rewire");
            expected.links_pipes[0] = rewired;
            compareNetworks(expected, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                repository, project, finalHead, baseline.uuid));
            (void)repository.verifyRepository();
            (void)repository.compact();
        }
        {
            native::Repository reopened(folder);
            require(reopened.head(project, "main") == saved &&
                    reopened.head(project, "feature") == finalHead,
                    "UUIDv7 chunked branch HEAD changed after compaction/reopen");
            compareNetworks(baseline, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                reopened, project, saved, baseline.uuid));
            compareNetworks(expected, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
                reopened, project, finalHead, baseline.uuid));
            require(hydraulic::loadHydraulicEntityAtRevision(
                reopened, project, finalHead, baseline.uuid, uuid(804000)).has_value(),
                "UUIDv7 added member disappeared after reopen");
            (void)reopened.verifyRepository();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// Explicitly qualify the two valid fast-forward outcomes. Corrupt source
// snapshots and stale pins are already covered by exerciseUuidV7HydraulicMerges.
void exerciseUuidV7FastForward() {
    const std::filesystem::path folder = tempDir();
    const native::ProjectId project = native::ProjectId::generate();
    try {
        native::Repository repository(folder);
        const native::RevisionId genesis = repository.createRootBranch(project, "main", commitMetadata());
        const NetworkHydraulicDistribution baseline = completeNetwork();
        const native::RevisionId base = hydraulic::saveHydraulicDistributionNetwork(
            repository, project, "main", baseline, genesis, commitMetadata());
        require(repository.createBranch(project, "target", "main") == base &&
                repository.createBranch(project, "source", "main") == base,
                "UUIDv7 fast-forward fixture branches did not share an immutable base");
        HydraulicNodeJunction updated = baseline.nodes_junctions[0];
        updated.elevation_m += 5.0;
        const native::RevisionId sourceTip = hydraulic::editHydraulicDistributionNetwork(
            repository, project, "source", baseline.uuid,
            {{updated.uuid, hydraulic::HydraulicEntity{updated}}}, base, commitMetadata());
        const std::uint64_t bytesBefore = repository.packSize();
        const native::MergeResult fastForward = hydraulic::mergeHydraulicBranches(
            repository, project, "target", "source",
            hydraulic::HydraulicMergeSelections{base, sourceTip, {}, {}}, commitMetadata());
        require(fastForward.status == native::MergeStatus::FastForward &&
                fastForward.resultHead && *fastForward.resultHead == sourceTip &&
                repository.head(project, "target") == sourceTip &&
                repository.head(project, "source") == sourceTip &&
                repository.packSize() == bytesBefore,
                "UUIDv7 hydraulic fast-forward did not reuse immutable source revision");
        NetworkHydraulicDistribution expected = baseline;
        expected.nodes_junctions[0] = updated;
        compareNetworks(expected, *hydraulic::loadHydraulicDistributionNetworkAtRevision(
            repository, project, sourceTip, baseline.uuid));
        const native::MergeResult current = hydraulic::mergeHydraulicBranches(
            repository, project, "target", "source",
            hydraulic::HydraulicMergeSelections{sourceTip, sourceTip, {}, {}}, commitMetadata());
        require(current.status == native::MergeStatus::UpToDate &&
                repository.head(project, "target") == sourceTip &&
                repository.packSize() == bytesBefore,
                "UUIDv7 same-tip hydraulic merge unexpectedly appended objects");
        (void)repository.verifyRepository();
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// INT-00: a representable but non-simulatable engineering state is still a
// legitimate immutable revision. A junction-only network has no hydraulic
// supply. Neither the draft transport nor the version-control layer may
// demand a successful hydraulic simulation before preserving that state.
void exerciseInt00IncompleteNetworkRoundTrip() {
    const std::filesystem::path folder = tempDir();
    try {
        NetworkHydraulicDistribution incomplete;
        incomplete.uuid = uuid(4500);
        incomplete.id = QStringLiteral("Unfinished network - no water source");
        incomplete.nodes_junctions.append(item<HydraulicNodeJunction>(4501));
        incomplete.nodes_junctions.append(item<HydraulicNodeJunction>(4502));
        // There is deliberately no reservoir, tank, pipe or pump. The
        // independent solver has no supply, but all stored IDs are canonical.
        require(incomplete.nodes_reservoirs.isEmpty() && incomplete.nodes_tanks.isEmpty(),
                "INT-00 test accidentally supplied a hydraulic source");

        const std::vector<native::ModelChange> draftChanges =
            hydraulic::fullDraftHydraulicDistributionNetworkChanges(incomplete);
        std::map<native::Key, native::ModelRecord> draft;
        for (const native::ModelChange& change : draftChanges) {
            require(change.record.has_value(), "INT-00 draft contained deletion");
            require(draft.emplace(change.key, *change.record).second,
                    "INT-00 draft contains duplicate UUID");
        }
        const std::function<std::optional<native::ModelRecord>(const native::Key&)> reader =
            [&draft](const native::Key& key) -> std::optional<native::ModelRecord> {
                const std::map<native::Key, native::ModelRecord>::const_iterator found =
                    draft.find(key);
                return found == draft.end() ? std::nullopt :
                    std::optional<native::ModelRecord>(found->second);
            };
        const std::optional<NetworkHydraulicDistribution> draftNetwork =
            hydraulic::loadDraftHydraulicDistributionNetwork(reader, incomplete.uuid);
        require(draftNetwork.has_value(), "INT-00 incomplete draft was not readable");
        compareNetworks(incomplete, *draftNetwork);

        const native::ProjectId project = native::ProjectId::generate();
        native::RevisionId committed = native::RevisionId::generate();
        {
            native::Repository repository(folder);
            const native::RevisionId root =
                repository.createRootBranch(project, "main", commitMetadata());
            committed = hydraulic::saveHydraulicDistributionNetwork(
                repository, project, "main", incomplete, root, commitMetadata());
            require(repository.createBranchAtRevision(project, "explore-fix", "main", committed) ==
                    committed, "INT-00 could not branch from incomplete network");
            require(repository.head(project, "main") == committed &&
                    repository.head(project, "explore-fix") == committed,
                    "INT-00 branch changed the source history");
            const std::optional<NetworkHydraulicDistribution> restored =
                hydraulic::loadHydraulicDistributionNetworkAtRevision(repository, project, committed, incomplete.uuid);
            require(restored.has_value(), "INT-00 committed network missing");
            compareNetworks(incomplete, *restored);
            (void)repository.verifyRepository();
            (void)repository.compact();
        }
        {
            native::Repository reopened(folder);
            require(reopened.head(project, "main") == committed &&
                    reopened.head(project, "explore-fix") == committed,
                    "INT-00 incomplete network history changed after restart");
            const std::optional<NetworkHydraulicDistribution> restored =
                hydraulic::loadHydraulicDistributionNetworkAtRevision(reopened, project, committed, incomplete.uuid);
            require(restored.has_value(), "INT-00 incomplete network lost on restart");
            compareNetworks(incomplete, *restored);
            (void)reopened.verifyRepository();
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

void exerciseAtomicRootMetadataEdit() {
    const NetworkHydraulicDistribution before = completeNetwork();
    const std::vector<native::ModelChange> records =
        hydraulic::fullDraftHydraulicDistributionNetworkChanges(before);
    std::map<native::Key, native::ModelRecord> state;
    for (const native::ModelChange& change : records) {
        if (change.record) state.emplace(change.key, *change.record);
    }
    const std::function<std::optional<native::ModelRecord>(const native::Key&)> reader =
        [&state](const native::Key& key) -> std::optional<native::ModelRecord> {
        const std::map<native::Key, native::ModelRecord>::const_iterator found = state.find(key);
        if (found == state.end()) return std::nullopt;
        return found->second;
    };
    NetworkHydraulicDistribution changed = before;
    changed.title_line_2 = QStringLiteral("Root edit saved atomically");
    changed.id = QStringLiteral("Renamed network ID");
    changed.options_hydraulic.headloss_formula = HydraulicHeadlossFormula::DarcyWeisbach;
    changed.title_line_3 = QStringLiteral("All root metadata persisted");
    changed.duration_s = 86400;
    changed.timestep_hydraulic_s = 900;
    changed.timestep_quality_s = 300;
    changed.timestep_pattern_s = 1800;
    changed.timestep_report_s = 1800;
    changed.timestep_rule_s = 300;
    changed.start_time_of_day_s = 3600;
    changed.report_statistic = HydraulicSimulationReportStatistic::Average;
    changed.options_energy.currency_iso4217 = QStringLiteral("EUR");
    changed.options_report.summary = false;
    changed.map_backdrop.file = QStringLiteral("int05-backdrop.png");
    // Project units are root metadata: a unit change is a root edit that the
    // draft persists and the next Commit records.
    NetworkPresentationUnits changedUnits;
    changedUnits.name = QStringLiteral("US customary");
    changedUnits.derived_from_profile = QStringLiteral("GPM");
    changedUnits.units.append(NetworkPresentationUnit{QStringLiteral("pressure_head"), QStringLiteral("psi")});
    changedUnits.units.append(NetworkPresentationUnit{QStringLiteral("volumetric_flow_rate"), QStringLiteral("US gal/min")});
    changed.presentation_units = changedUnits;
    changed.multi_species.species[0].note = QStringLiteral("Updated MSX root note");
    aowis::model::ModelEntityEdit edit;
    edit.entityId = before.uuid;
    edit.entityType = QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType);
    edit.before = aowis::model::hydraulicDistributionRootImage(before);
    edit.after = aowis::model::hydraulicDistributionRootImage(changed);
    const native::ModelChange materialized =
        hydraulic::materializeDraftRootEdit(reader, edit);
    require(materialized.record.has_value() &&
            materialized.record->kind == hydraulic::hydraulicDistributionNetworkKind &&
            materialized.record->references == reader(materialized.key)->references,
            "root transaction retains native membership chunk references");
    state[materialized.key] = *materialized.record;
    const std::optional<NetworkHydraulicDistribution> saved =
        hydraulic::loadDraftHydraulicDistributionNetwork(reader, before.uuid);
    require(saved.has_value() &&
            saved->options_hydraulic.headloss_formula == HydraulicHeadlossFormula::DarcyWeisbach &&
            saved->title_line_2 == changed.title_line_2 &&
            saved->id == changed.id &&
            saved->presentation_units.has_value() &&
            saved->presentation_units->name == QStringLiteral("US customary") &&
            saved->presentation_units->units.size() == 2 &&
            saved->presentation_units->units.at(1).unit == QStringLiteral("US gal/min") &&
            saved->nodes_junctions.size() == before.nodes_junctions.size() &&
            saved->links_pipes.size() == before.links_pipes.size(),
            "root edit updates saved options without changing entity collections");
    require(saved.has_value() &&
            aowis::dvmdb::ModelCodec<NetworkHydraulicDistribution>::encode(*saved) ==
            aowis::dvmdb::ModelCodec<NetworkHydraulicDistribution>::encode(changed),
            "INT-05 metadata update preserves all 17 collections and every root byte");
    // Exercise native persistence, immutable Commit and a newly opened
    // repository, not only an in-memory manifest materialization.
    const std::filesystem::path rootPath = tempDir();
    try {
        const native::ProjectId project = native::ProjectId::generate();
        native::RevisionId updatedRevision = native::RevisionId::generate();
        {
            native::Repository repository(rootPath);
            const native::RevisionId emptyHead =
                repository.createRootBranch(project, "main", commitMetadata());
            const native::RevisionId originalRevision =
                hydraulic::saveHydraulicDistributionNetwork(
                    repository, project, "main", before, emptyHead, commitMetadata());
            updatedRevision = repository.commitModel(
                project, "main", {materialized}, commitMetadata(), originalRevision,
                false, std::nullopt, native::annotations::validateAnnotationSnapshot);
            const std::optional<NetworkHydraulicDistribution> committed =
                hydraulic::loadHydraulicDistributionNetworkAtRevision(
                    repository, project, updatedRevision, changed.uuid);
            require(committed.has_value(), "INT-05 committed metadata root not found");
            compareNetworks(changed, *committed);
            (void)repository.verifyRepository();
        }
        {
            native::Repository reopened(rootPath);
            require(reopened.head(project, "main") == updatedRevision,
                    "INT-05 committed metadata HEAD lost after restart");
            const std::optional<NetworkHydraulicDistribution> restored =
                hydraulic::loadHydraulicDistributionNetworkAtRevision(
                    reopened, project, updatedRevision, changed.uuid);
            require(restored.has_value(), "INT-05 complete network lost after restart");
            compareNetworks(changed, *restored);
            (void)reopened.verifyRepository();
            (void)reopened.compact();
            const std::optional<NetworkHydraulicDistribution> compacted =
                hydraulic::loadHydraulicDistributionNetworkAtRevision(
                    reopened, project, updatedRevision, changed.uuid);
            require(compacted.has_value(), "INT-05 network lost during compaction");
            compareNetworks(changed, *compacted);
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(rootPath, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(rootPath, ignored);
    bool staleRejected = false;
    try { (void)hydraulic::materializeDraftRootEdit(reader, edit); }
    catch (const std::exception&) { staleRejected = true; }
    require(staleRejected, "stale root before-image rejected");
    edit.before = aowis::model::hydraulicDistributionRootImage(*saved);
    NetworkHydraulicDistribution forged = *saved;
    forged.uuid = uuid(999);
    edit.after = aowis::model::hydraulicDistributionRootImage(forged);
    bool identityRejected = false;
    try { (void)hydraulic::materializeDraftRootEdit(reader, edit); }
    catch (const std::exception&) { identityRejected = true; }
    require(identityRejected, "root UUID replacement rejected");
}

} // namespace

// HIST-03c3b: the indexed native witness alone is not enough. Exercise a
// real local Commit with a single MODEL action touching two independent
// hydraulic distribution networks, including a native-owned root/chunk membership update.
// An unjournaled but semantically valid edit must remain ineligible for history navigation.
void exerciseVerifiedRevisionHydraulicCoverage() {
    const std::filesystem::path folder = tempDir();
    try {
        std::unique_ptr<native::ProjectService> service =
            std::make_unique<native::ProjectService>(folder);
        service->configureRepositoryRole(native::RepositoryRole::Private);
        const std::string projectId = service->createUnbornProject(
            "HIST-03c3b multi-network action coverage").id;
        const std::string session = native::UuidV7::generate().toString();
        NetworkHydraulicDistribution a;
        a.uuid = uuid(930000);
        a.id = QStringLiteral("Network A");
        a.nodes_junctions.append(item<HydraulicNodeJunction>(930001));
        NetworkHydraulicDistribution b;
        b.uuid = uuid(940000);
        b.id = QStringLiteral("Network B");
        b.nodes_junctions.append(item<HydraulicNodeJunction>(940001));
        std::vector<native::ModelChange> initial =
            initialEngineeringSnapshot(a);
        const std::vector<native::ModelChange> other =
            initialEngineeringSnapshot(b);
        initial.insert(initial.end(), other.begin(), other.end());
        const native::RevisionId baseline = service->commitModelAtExpectedHead(
            projectId, "main", initial, commitMetadata(), std::nullopt,
            native::annotations::validateAnnotationSnapshot);

        const std::string draft = service->createWorkingCopy(
            projectId, "main", baseline, session).id;
        const std::string token = service->acquireWorkingCopyLease(draft, session, 120).token;
        const HydraulicNodeJunction added = item<HydraulicNodeJunction>(930002);
        HydraulicNodeJunction changedB = b.nodes_junctions.first();
        changedB.elevation_m = 16;
        NetworkHydraulicDistribution changedRootB = b;
        changedRootB.title_line_2 = QStringLiteral("Edited in one action with Network A");
        aowis::model::ModelEditTransaction action;
        action.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        action.operation = QStringLiteral("hydraulic.history.multi-network");
        action.edits.append(aowis::model::ModelEntityEdit{
            added.uuid, QStringLiteral("HydraulicNodeJunction"), std::nullopt,
            native::ModelCodec<HydraulicNodeJunction>::encode(added)});
        action.edits.append(aowis::model::ModelEntityEdit{
            changedB.uuid, QStringLiteral("HydraulicNodeJunction"),
            native::ModelCodec<HydraulicNodeJunction>::encode(b.nodes_junctions.first()),
            native::ModelCodec<HydraulicNodeJunction>::encode(changedB)});
        action.edits.append(aowis::model::ModelEntityEdit{
            b.uuid, QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType),
            aowis::model::hydraulicDistributionRootImage(b),
            aowis::model::hydraulicDistributionRootImage(changedRootB)});
        const QByteArray eventBytes = aowis::model::ModelEditTransactionCodec::encode(action);
        require(!eventBytes.isEmpty(), "HIST-03c3b could not encode multi-network action");
        std::vector<native::ModelChange> changes{
            hydraulic::changeFor(added), hydraulic::changeFor(changedB)};
        const std::function<std::optional<native::ModelRecord>(const native::Key&)> readBase =
            [&service, &projectId, &baseline](const native::Key& key)
                    -> std::optional<native::ModelRecord> {
                return service->getModelAtRevision(projectId, baseline, key);
            };
        hydraulic::materializeDraftMembership(readBase,
            {hydraulic::DraftMembershipIntent{
                hydraulic::keyFromUuid(a.uuid), hydraulic::keyFromUuid(added.uuid), 201, 9, true}},
            changes);
        changes.push_back(hydraulic::materializeDraftRootEdit(readBase,
            action.edits.last()));
        require(service->saveWorkingCopyTransactions(draft, session, token, 0,
                    changes, {hydraulic::bytesFromQByteArray(eventBytes)}).generation == 1,
                "HIST-03c3b could not Save a mixed two-network action");
        const native::LocalCommitInfo committed = service->commitWorkingCopy(
            native::UuidV7::generate().toString(), draft, session, token, 1,
            baseline, commitMetadata(),
            native::annotations::validateAnnotationSnapshot);
        require(committed.state == native::LocalCommitState::Succeeded,
                "HIST-03c3b local Commit did not succeed");
        const native::RevisionId revision =
            *native::RevisionId::parse(committed.reservedRevisionId);
        QString error;
        const std::optional<native::annotations::VerifiedIndexedAnnotationActionBoundary> verified =
            native::annotations::verifyIndexedCombinedActionBoundary(
                *service, projectId, "main", revision, 1, &error);
        // The tip boundary must carry the edits of BOTH networks: the new
        // member of A, the changed member of B and B's root metadata.
        const std::function<bool(const native::annotations::VerifiedIndexedAnnotationActionBoundary&)>
            coversBothNetworks = [&added, &changedB, &b](
                const native::annotations::VerifiedIndexedAnnotationActionBoundary& proof) {
                bool member = false, other = false, root = false;
                for (const native::ModelChange& change : proof.boundary.changesFromParent) {
                    member = member || change.key == hydraulic::keyFromUuid(added.uuid);
                    other = other || change.key == hydraulic::keyFromUuid(changedB.uuid);
                    root = root || change.key == hydraulic::keyFromUuid(b.uuid);
                }
                return member && other && root;
            };
        require(verified && error.isEmpty() && verified->boundary.actionCount == 1 &&
                coversBothNetworks(*verified),
                "HIST-03c3b failed to attribute complete two-network native delta");
        const native::RevisionNativeActionWitness witness =
            service->verifiedRevisionNativeActionWitness(projectId, "main", revision);
        require(witness.hydraulicDistributionRootKeys.size() == 2 &&
                witness.actualNativeChanges.size() >= 4,
                "HIST-03c3b native witness omitted server-owned root/chunk changes");
        // HIST-03c4a: turn authenticated full-history action coverage into
        // COMPLETE draft overlays, both before and after the mixed action.
        // Two networks must move at the same global action boundary while
        // retaining canonical collection order and root metadata.
        const std::string navigationDraft = service->createWorkingCopy(
            projectId, "main", revision, session).id;
        const std::optional<std::vector<native::ModelChange>> beforeAction =
            native::annotations::planVerifiedArchivedAction(
                *service, navigationDraft, revision, revision, 0, 0, &error);
        const std::optional<std::vector<native::ModelChange>> afterAction =
            native::annotations::planVerifiedArchivedAction(
                *service, navigationDraft, revision, revision, 1, 0, &error);
        require(beforeAction && afterAction && error.isEmpty() &&
                !(*beforeAction).empty() &&
                (*afterAction).empty(),
                "HIST-03c4a overlays must be relative to HEAD, without no-op records");
        const std::function<NetworkHydraulicDistribution(
            const std::vector<native::ModelChange>&, const QUuid&)>
            plannedNetwork = [&service, &projectId, &revision](
                const std::vector<native::ModelChange>& plan,
                const QUuid& id) -> NetworkHydraulicDistribution {
                std::map<native::Key, std::optional<native::ModelRecord>> overlay;
                for (const native::ModelChange& change : plan)
                    require(overlay.emplace(change.key, change.record).second,
                            "duplicate key in HIST-03c4a candidate overlay");
                const std::function<std::optional<native::ModelRecord>(const native::Key&)> read =
                    [&service, &projectId, &revision, &overlay](const native::Key& key)
                    -> std::optional<native::ModelRecord> {
                        const std::map<native::Key, std::optional<native::ModelRecord>>::const_iterator
                            found = overlay.find(key);
                        if (found != overlay.end()) return found->second;
                        return service->getModelAtRevision(projectId, revision, key);
                    };
                const std::optional<NetworkHydraulicDistribution> network =
                    hydraulic::loadDraftHydraulicDistributionNetwork(read, id);
                require(network.has_value(), "HIST-03c4a candidate network cannot be reopened");
                return *network;
            };
        NetworkHydraulicDistribution expectedA = a;
        expectedA.nodes_junctions.append(added);
        NetworkHydraulicDistribution expectedB = changedRootB;
        expectedB.nodes_junctions.first() = changedB;
        require(native::ModelCodec<NetworkHydraulicDistribution>::encode(
                    plannedNetwork(*beforeAction, a.uuid)) ==
                    native::ModelCodec<NetworkHydraulicDistribution>::encode(a) &&
                native::ModelCodec<NetworkHydraulicDistribution>::encode(
                    plannedNetwork(*beforeAction, b.uuid)) ==
                    native::ModelCodec<NetworkHydraulicDistribution>::encode(b) &&
                native::ModelCodec<NetworkHydraulicDistribution>::encode(
                    plannedNetwork(*afterAction, a.uuid)) ==
                    native::ModelCodec<NetworkHydraulicDistribution>::encode(expectedA) &&
                native::ModelCodec<NetworkHydraulicDistribution>::encode(
                    plannedNetwork(*afterAction, b.uuid)) ==
                    native::ModelCodec<NetworkHydraulicDistribution>::encode(expectedB),
                "HIST-03c4a reconstructed an incorrect draft-relative action boundary");
        require(service->workingCopy(navigationDraft).generation == 0 &&
                !service->workingCopy(navigationDraft).dirty &&
                !service->workingCopy(navigationDraft).historyCheckpointRevisionId,
                "HIST-03c4a read-only planning modified the actual draft");
        require(!native::annotations::planVerifiedArchivedAction(
                    *service, navigationDraft, revision, revision, 2, 0, &error) &&
                !error.isEmpty() &&
                !native::annotations::planVerifiedArchivedAction(
                    *service, navigationDraft, revision, revision, 0, 1, &error) &&
                !error.isEmpty(),
                "HIST-03c4a accepted invalid action positions or stale draft generations");

        // HIST-03c4b: publish a completely verified action boundary, with a
        // durable action coordinate and one complete native candidate overlay.
        const std::string navLease = service->acquireWorkingCopyLease(
            navigationDraft, session, 120).token;
        const std::string navOperation0 = native::UuidV7::generate().toString();
        const std::string navOperation1 = native::UuidV7::generate().toString();
        const std::optional<native::WorkingCopyInfo> selectedBefore =
            native::annotations::navigateVerifiedCombinedAction(*service, navOperation0,
                navigationDraft, session, navLease, revision, revision, 0, 0, &error);
        require(selectedBefore && error.isEmpty() && selectedBefore->generation == 1 &&
                selectedBefore->historyActionPosition == 0 &&
                selectedBefore->historyCheckpointRevisionId == revision.toString() &&
                selectedBefore->historyArchiveCommitOperationId == witness.commitOperationId &&
                selectedBefore->lastHistoryNavigationOperationId == navOperation0 &&
                selectedBefore->journalCount == 0,
                "HIST-03c4b failed atomic action boundary zero publication");
        const std::optional<native::WorkingCopyInfo> retryBefore =
            native::annotations::navigateVerifiedCombinedAction(*service, navOperation0,
                navigationDraft, session, navLease, revision, revision, 0, 0, &error);
        require(retryBefore && retryBefore->generation == 1,
                "HIST-03c4b retried action navigation advanced generation");
        require(!native::annotations::navigateVerifiedCombinedAction(*service, navOperation0,
                    navigationDraft, session, navLease, revision, revision, 1, 0, &error) &&
                !error.isEmpty(), "HIST-03c4b reused operation ID at a different boundary");
        const std::optional<native::WorkingCopyInfo> selectedAfter =
            native::annotations::navigateVerifiedCombinedAction(*service, navOperation1,
                navigationDraft, session, navLease, revision, revision, 1, 1, &error);
        const std::string forwardError =
            "HIST-03c4b could not publish forward navigation: " + error.toStdString();
        require(selectedAfter.has_value(), forwardError.c_str());
        require(error.isEmpty() && selectedAfter->generation == 2 &&
                selectedAfter->historyActionPosition == 1 &&
                selectedAfter->historyArchiveCommitOperationId == witness.commitOperationId &&
                selectedAfter->candidateChanges.empty() && !selectedAfter->dirty,
                "HIST-03c4b failed atomic action forward navigation");
        require(!native::annotations::navigateVerifiedCombinedAction(*service,
                    native::UuidV7::generate().toString(), navigationDraft,
                    session, navLease, revision, revision, 0, 1, &error) &&
                !error.isEmpty(), "HIST-03c4b accepted stale generation after navigation");
        const std::optional<native::WorkingCopyInfo> selectedAgain =
            native::annotations::navigateVerifiedCombinedAction(*service,
                native::UuidV7::generate().toString(), navigationDraft,
                session, navLease, revision, revision, 0, 2, &error);
        require(selectedAgain && selectedAgain->generation == 3 &&
                selectedAgain->historyActionPosition == 0,
                "HIST-03c4b failed reverse action navigation after forward navigation");

        const std::string unrelatedDraft = service->createWorkingCopy(
            projectId, "main", revision, session).id;
        const std::string unrelatedToken = service->acquireWorkingCopyLease(
            unrelatedDraft, session, 120).token;
        HydraulicNodeJunction nextA = a.nodes_junctions.first();
        nextA.elevation_m = 41;
        aowis::model::ModelEditTransaction nextAction;
        nextAction.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        nextAction.operation = QStringLiteral("hydraulic.history.unjournaled-native");
        nextAction.edits.append(aowis::model::ModelEntityEdit{
            nextA.uuid, QStringLiteral("HydraulicNodeJunction"),
            native::ModelCodec<HydraulicNodeJunction>::encode(a.nodes_junctions.first()),
            native::ModelCodec<HydraulicNodeJunction>::encode(nextA)});
        const QByteArray nextFrame = aowis::model::ModelEditTransactionCodec::encode(nextAction);
        require(!nextFrame.isEmpty(), "HIST-03c3b unjournaled fixture action failed to encode");
        // An arbitrary kind-55 record is now correctly rejected by the
        // engineering semantic validator. Keep this history-coverage test
        // meaningful by adding an UNJOURNALED but otherwise valid update to
        // an existing member of the second network.
        HydraulicNodeJunction unjournaledB = changedB;
        unjournaledB.elevation_m = 47;
        require(service->saveWorkingCopyTransactions(
                    unrelatedDraft, session, unrelatedToken, 0,
                    {hydraulic::changeFor(nextA), hydraulic::changeFor(unjournaledB)},
                    {hydraulic::bytesFromQByteArray(nextFrame)}).generation == 1,
                "HIST-03c3b unjournaled native Save failed");
        const native::LocalCommitInfo unrelatedCommit = service->commitWorkingCopy(
            native::UuidV7::generate().toString(), unrelatedDraft, session,
            unrelatedToken, 1, revision, commitMetadata(),
            native::annotations::validateAnnotationSnapshot);
        require(unrelatedCommit.state == native::LocalCommitState::Succeeded,
                "HIST-03c3b unjournaled native Commit failed");
        const native::RevisionId later =
            *native::RevisionId::parse(unrelatedCommit.reservedRevisionId);
        // Planning cannot race a newly published branch HEAD, even if the
        // selected older action archive itself remains immutable and readable.
        require(!native::annotations::planVerifiedArchivedAction(
                    *service, navigationDraft, revision, revision, 0, 0, &error) &&
                error.contains(QStringLiteral("HEAD")),
                "HIST-03c4a did not reject a draft after its branch HEAD advanced");
        require(!native::annotations::verifyIndexedCombinedActionBoundary(
                    *service, projectId, "main", later, 1, &error) &&
                error.contains(QStringLiteral("unjournaled")),
                "HIST-03c3b accepted a native record with no MODEL action");
        // Also reject a *valid hydraulic* native record omitted from the
        // archived event when a different network has a genuine event.
        const std::string missingDraft = service->createWorkingCopy(
            projectId, "main", later, session).id;
        const std::string missingToken = service->acquireWorkingCopyLease(
            missingDraft, session, 120).token;
        HydraulicNodeJunction newestA = nextA;
        newestA.elevation_m = 42;
        HydraulicNodeJunction hiddenB = changedB;
        hiddenB.elevation_m = 73;
        aowis::model::ModelEditTransaction onlyA;
        onlyA.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        onlyA.operation = QStringLiteral("hydraulic.history.only-one-network");
        onlyA.edits.append(aowis::model::ModelEntityEdit{
            newestA.uuid, QStringLiteral("HydraulicNodeJunction"),
            native::ModelCodec<HydraulicNodeJunction>::encode(nextA),
            native::ModelCodec<HydraulicNodeJunction>::encode(newestA)});
        const QByteArray onlyAFrame = aowis::model::ModelEditTransactionCodec::encode(onlyA);
        require(!onlyAFrame.isEmpty() &&
                service->saveWorkingCopyTransactions(missingDraft, session, missingToken, 0,
                    {hydraulic::changeFor(newestA), hydraulic::changeFor(hiddenB)},
                    {hydraulic::bytesFromQByteArray(onlyAFrame)}).generation == 1,
                "HIST-03c3b missing hydraulic action fixture Save failed");
        const native::LocalCommitInfo missingCommit = service->commitWorkingCopy(
            native::UuidV7::generate().toString(), missingDraft, session,
            missingToken, 1, later, commitMetadata(),
            native::annotations::validateAnnotationSnapshot);
        require(missingCommit.state == native::LocalCommitState::Succeeded,
                "HIST-03c3b missing hydraulic action fixture Commit failed");
        require(!native::annotations::verifyIndexedCombinedActionBoundary(
                    *service, projectId, "main",
                    *native::RevisionId::parse(missingCommit.reservedRevisionId), 1, &error) &&
                error.contains(QStringLiteral("unjournaled")),
                "HIST-03c3b accepted a changed hydraulic distribution network with no MODEL event");
        require(!native::annotations::planVerifiedArchivedAction(
                    *service, missingDraft, later,
                    *native::RevisionId::parse(missingCommit.reservedRevisionId),
                    0, 2, &error) && !error.isEmpty(),
                "HIST-03c4a built a candidate from an incomplete MODEL archive");
        // ProjectService owns an exclusive root lock. Simulate a real server
        // restart by destroying the first service before opening the second.
        service.reset();
        {
            native::ProjectService reopened(folder);
            const native::WorkingCopyInfo persistentCursor = reopened.workingCopy(navigationDraft);
            require(persistentCursor.generation == 3 &&
                    persistentCursor.historyActionPosition == 0 &&
                    persistentCursor.historyCheckpointRevisionId == revision.toString() &&
                    persistentCursor.historyArchiveCommitOperationId == witness.commitOperationId,
                    "HIST-03c4b action cursor was not durable across DVMDB restart");
            const std::optional<native::annotations::VerifiedIndexedAnnotationActionBoundary> recovered =
                native::annotations::verifyIndexedCombinedActionBoundary(
                    reopened, projectId, "main", revision, 1, &error);
            require(recovered && recovered->boundary.actionCount == 1 &&
                    coversBothNetworks(*recovered),
                    "HIST-03c3b archive/native attribution was lost after restart");
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// HDB-03b: the public MODEL-aware adapter must prove that original active
// journal frames describe the ACTUAL draft, not just a plausible sequence.
// A published position keeps the redo frames, and moving to position zero
// must yield a clean HEAD-relative candidate even after structural edits.
void exerciseVerifiedActiveDraftActionNavigation() {
    const std::filesystem::path folder = tempDir();
    try {
        std::unique_ptr<native::ProjectService> service =
            std::make_unique<native::ProjectService>(folder);
        service->configureRepositoryRole(native::RepositoryRole::Private);
        const std::string projectId = service->createUnbornProject(
            "HDB-03b verified live journal").id;
        const std::string session = native::UuidV7::generate().toString();
        const NetworkHydraulicDistribution base = completeNetwork();
        const native::RevisionId head = service->commitModelAtExpectedHead(
            projectId, "main", initialEngineeringSnapshot(base),
            commitMetadata(), std::nullopt,
            native::annotations::validateAnnotationSnapshot);
        const std::string draftId = service->createWorkingCopy(
            projectId, "main", head, session).id;
        const std::string token = service->acquireWorkingCopyLease(
            draftId, session, 120).token;
        const std::function<std::optional<native::ModelRecord>(const native::Key&)> readBase =
            [&service, &projectId, &head](const native::Key& key)
                    -> std::optional<native::ModelRecord> {
                return service->getModelAtRevision(projectId, head, key);
            };
        const std::function<NetworkHydraulicDistribution()> readDraft =
            [&service, &draftId, &readBase, &base]() -> NetworkHydraulicDistribution {
                std::map<native::Key, std::optional<native::ModelRecord>> changes;
                const native::WorkingCopyInfo current = service->workingCopy(draftId, false);
                for (const native::ModelChange& change : current.candidateChanges)
                    require(changes.emplace(change.key, change.record).second,
                            "HDB-03b duplicate draft candidate key");
                const std::function<std::optional<native::ModelRecord>(const native::Key&)> read =
                    [&changes, &readBase](const native::Key& key)
                            -> std::optional<native::ModelRecord> {
                        const std::map<native::Key, std::optional<native::ModelRecord>>::const_iterator
                            item = changes.find(key);
                        return item != changes.end() ? item->second : readBase(key);
                    };
                const std::optional<NetworkHydraulicDistribution> value =
                    hydraulic::loadDraftHydraulicDistributionNetwork(read, base.uuid);
                require(value.has_value(), "HDB-03b could not materialize selected draft");
                return *value;
            };
        const std::function<aowis::model::ModelEditTransaction(const QString&)> newAction =
            [](const QString& operation) -> aowis::model::ModelEditTransaction {
                aowis::model::ModelEditTransaction event;
                event.transactionId = QUuid(QString::fromStdString(
                    native::UuidV7::generate().toString()));
                event.operation = operation;
                return event;
            };
        NetworkHydraulicDistribution stepOne = base;
        stepOne.curves_generic[0].id = QStringLiteral("History journal edit");
        aowis::model::ModelEditTransaction first = newAction(QStringLiteral("hydraulic.curve.update"));
        first.edits.append(aowis::model::ModelEntityEdit{
            stepOne.curves_generic[0].uuid, QStringLiteral("HydraulicCurveGeneric"),
            native::ModelCodec<HydraulicCurveGeneric>::encode(base.curves_generic[0]),
            native::ModelCodec<HydraulicCurveGeneric>::encode(stepOne.curves_generic[0])});
        const QByteArray firstBytes = aowis::model::ModelEditTransactionCodec::encode(first);
        native::ModelChange labelChange;
        labelChange.key = hydraulic::keyFromUuid(stepOne.curves_generic[0].uuid);
        labelChange.record = readBase(labelChange.key);
        require(labelChange.record.has_value() && labelChange.record->kind == 215,
                "HDB-03b missing canonical generic-curve native record");
        labelChange.record->payload = hydraulic::bytesFromQByteArray(
            native::ModelCodec<HydraulicCurveGeneric>::encode(stepOne.curves_generic[0]));
        require(!firstBytes.isEmpty() &&
                service->saveWorkingCopyTransactions(draftId, session, token, 0,
                    {labelChange},
                    {hydraulic::bytesFromQByteArray(firstBytes)}).generation == 1,
                "HDB-03b failed to Save first active MODEL event");

        NetworkHydraulicDistribution stepTwo = stepOne;
        const HydraulicNodeJunction added = item<HydraulicNodeJunction>(990201);
        stepTwo.nodes_junctions.append(added);
        aowis::model::ModelEditTransaction second = newAction(QStringLiteral("hydraulic.junction.add"));
        second.edits.append(aowis::model::ModelEntityEdit{
            added.uuid, QStringLiteral("HydraulicNodeJunction"), std::nullopt,
            native::ModelCodec<HydraulicNodeJunction>::encode(added)});
        const QByteArray secondBytes = aowis::model::ModelEditTransactionCodec::encode(second);
        std::vector<native::ModelChange> secondChanges{hydraulic::changeFor(added)};
        hydraulic::materializeDraftMembership(readBase,
            {hydraulic::DraftMembershipIntent{
                hydraulic::keyFromUuid(base.uuid), hydraulic::keyFromUuid(added.uuid),
                201, 9, true}}, secondChanges);
        require(!secondBytes.isEmpty() &&
                service->saveWorkingCopyTransactions(draftId, session, token, 1,
                    secondChanges, {hydraulic::bytesFromQByteArray(secondBytes)}).generation == 2,
                "HDB-03b failed to Save structural MODEL event");

        NetworkHydraulicDistribution stepThree = stepTwo;
        stepThree.title_line_2 = QStringLiteral("History root metadata");
        aowis::model::ModelEditTransaction third = newAction(QStringLiteral("hydraulic.root.update"));
        third.edits.append(aowis::model::ModelEntityEdit{
            base.uuid, QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType),
            aowis::model::hydraulicDistributionRootImage(stepTwo),
            aowis::model::hydraulicDistributionRootImage(stepThree)});
        const QByteArray thirdBytes = aowis::model::ModelEditTransactionCodec::encode(third);
        const native::WorkingCopyInfo beforeThird = service->workingCopy(draftId, false);
        std::map<native::Key, std::optional<native::ModelRecord>> beforeThirdChanges;
        for (const native::ModelChange& change : beforeThird.candidateChanges)
            beforeThirdChanges.emplace(change.key, change.record);
        const std::function<std::optional<native::ModelRecord>(const native::Key&)> beforeThirdRead =
            [&beforeThirdChanges, &readBase](const native::Key& key)
                    -> std::optional<native::ModelRecord> {
                const std::map<native::Key, std::optional<native::ModelRecord>>::const_iterator
                    item = beforeThirdChanges.find(key);
                return item == beforeThirdChanges.end() ? readBase(key) : item->second;
            };
        const native::ModelChange thirdChange =
            hydraulic::materializeDraftRootEdit(beforeThirdRead, third.edits.first());
        require(!thirdBytes.isEmpty() &&
                service->saveWorkingCopyTransactions(draftId, session, token, 2,
                    {thirdChange}, {hydraulic::bytesFromQByteArray(thirdBytes)}).generation == 3,
                "HDB-03b failed to Save network-root MODEL event");
        compareNetworks(stepThree, readDraft());

        QString error;
        const std::optional<std::vector<native::ModelChange>> cleanPlan =
            native::annotations::planVerifiedActiveDraftAction(
                *service, draftId, head, 0, 3, &error);
        const std::optional<std::vector<native::ModelChange>> middlePlan =
            native::annotations::planVerifiedActiveDraftAction(
                *service, draftId, head, 2, 3, &error);
        require(cleanPlan && (*cleanPlan).empty() &&
                middlePlan && !(*middlePlan).empty() &&
                service->workingCopy(draftId, true).journal.size() == 3,
                "HDB-03b planning mutated the draft or failed to rebuild a clean base");
        require(!native::annotations::planVerifiedActiveDraftAction(
                    *service, draftId, head, 4, 3, &error) && !error.isEmpty(),
                "HDB-03b accepted an out-of-range action position");
        const std::string firstMove = native::UuidV7::generate().toString();
        const std::optional<native::WorkingCopyInfo> toMiddle =
            native::annotations::navigateVerifiedActiveDraftAction(
                *service, firstMove, draftId, session, token, head, 2, 3, &error);
        require(toMiddle && toMiddle->generation == 4 &&
                toMiddle->draftActionPosition == 2 && toMiddle->journalCount == 3,
                "HDB-03b failed verified backward active-journal navigation");
        compareNetworks(stepTwo, readDraft());
        const std::optional<native::WorkingCopyInfo> retry =
            native::annotations::navigateVerifiedActiveDraftAction(
                *service, firstMove, draftId, session, token, head, 2, 3, &error);
        require(retry && retry->generation == 4,
                "HDB-03b repeated operation changed the draft generation");
        const std::optional<native::WorkingCopyInfo> toBase =
            native::annotations::navigateVerifiedActiveDraftAction(
                *service, native::UuidV7::generate().toString(),
                draftId, session, token, head, 0, 4, &error);
        require(toBase && toBase->generation == 5 && !toBase->dirty &&
                toBase->candidateChanges.empty(),
                "HDB-03b did not restore the clean committed base");
        compareNetworks(base, readDraft());
        const std::optional<native::WorkingCopyInfo> forward =
            native::annotations::navigateVerifiedActiveDraftAction(
                *service, native::UuidV7::generate().toString(),
                draftId, session, token, head, 3, 5, &error);
        require(forward && forward->generation == 6 &&
                forward->draftActionPosition == 3 && forward->journalCount == 3 &&
                service->optionalHead(projectId, "main") == head,
                "HDB-03b failed verified forward active-journal navigation");
        compareNetworks(stepThree, readDraft());

        // An ordinary native Save is not an archived MODEL edit. Even with a
        // valid retained MODEL journal, action navigation must not silently
        // replace that unjournaled record with a plausible MODEL-only state.
        const std::string unjournaled = service->createWorkingCopy(
            projectId, "main", head, session).id;
        const std::string unjournaledToken = service->acquireWorkingCopyLease(
            unjournaled, session, 120).token;
        // Native-only edits are legal before the first MODEL event. Once a
        // journal exists the service correctly requires atomic event Saves,
        // so create the mixed draft in that permitted order instead.
        native::ModelChange foreign;
        foreign.key = hydraulic::keyFromUuid(uuid(990999));
        native::ModelRecord foreignRecord;
        foreignRecord.kind = 999;
        foreignRecord.payload = {42};
        foreign.record = foreignRecord;
        require(service->saveWorkingCopyChanges(unjournaled, session,
                    unjournaledToken, 0, {foreign}).generation == 1,
                "HDB-03b could not Save negative-control unjournaled native change");
        require(service->saveWorkingCopyTransactions(unjournaled, session,
                    unjournaledToken, 1,
                    {labelChange},
                    {hydraulic::bytesFromQByteArray(firstBytes)}).generation == 2,
                "HDB-03b could not Save negative-control MODEL action");
        require(!native::annotations::planVerifiedActiveDraftAction(
                    *service, unjournaled, head, 0, 2, &error) && !error.isEmpty(),
                "HDB-03b accepted an unjournaled foreign native draft change");
        service.reset();
        {
            native::ProjectService reopened(folder);
            const native::WorkingCopyInfo recovered = reopened.workingCopy(draftId, true);
            require(recovered.generation == 6 && recovered.draftActionPosition == 3 &&
                    recovered.journal.size() == 3 &&
                    reopened.optionalHead(projectId, "main") == head,
                    "HDB-03b lost verified active journal cursor after restart");
            const std::optional<std::vector<native::ModelChange>> again =
                native::annotations::planVerifiedActiveDraftAction(
                    reopened, draftId, head, 1, 6, &error);
            require(again && !(*again).empty(),
                    "HDB-03b cannot reconstruct saved journal after restart");
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}


// History undoes an unborn journal by deleting what it appended and then
// replays it forward: deleting the member just added must restore the
// previous root and chunk records byte for byte, also when the addition
// opened a new chunk, and a dropped chunk may be re-created in one batch.
void exerciseMembershipUndoIsExact() {
    using Records = std::map<native::Key, native::ModelRecord>;
    const QUuid networkId = uuid(991000);
    const native::ModelChange root =
        hydraulic::emptyDraftHydraulicDistributionNetworkRoot(networkId, QStringLiteral("Undo"));
    Records records{{root.key, *root.record}};
    const std::function<std::optional<native::ModelRecord>(const native::Key&)> read =
        [&records](const native::Key& key) -> std::optional<native::ModelRecord> {
            const Records::const_iterator found = records.find(key);
            return found == records.end() ? std::nullopt
                                          : std::optional<native::ModelRecord>(found->second);
        };
    const std::function<void(const std::vector<HydraulicNodeJunction>&, bool)> edit =
        [&](const std::vector<HydraulicNodeJunction>& junctions, bool insert) {
            std::vector<native::ModelChange> changes;
            std::vector<hydraulic::DraftMembershipIntent> intents;
            for (const HydraulicNodeJunction& junction : junctions) {
                native::ModelChange change = hydraulic::changeFor(junction);
                if (!insert) change.record.reset();
                changes.push_back(change);
                intents.push_back(hydraulic::DraftMembershipIntent{
                    hydraulic::keyFromUuid(networkId), hydraulic::keyFromUuid(junction.uuid),
                    201, 9, insert});
            }
            hydraulic::materializeDraftMembership(read, intents, changes);
            for (const native::ModelChange& change : changes) {
                if (change.record) records[change.key] = *change.record;
                else records.erase(change.key);
            }
        };
    const std::function<bool(const Records&, const Records&)> same =
        [](const Records& left, const Records& right) {
            if (left.size() != right.size()) return false;
            for (Records::const_iterator a = left.begin(), b = right.begin(); a != left.end(); ++a, ++b)
                if (a->first != b->first || a->second.kind != b->second.kind ||
                    a->second.payload != b->second.payload ||
                    a->second.references != b->second.references) return false;
            return true;
        };
    const Records empty = records;
    const HydraulicNodeJunction first = item<HydraulicNodeJunction>(991001);
    edit({first}, true);
    edit({first}, false);
    require(same(records, empty), "undoing the first member must restore the empty network exactly");
    // Fill one chunk (256), then one more member opens a second chunk.
    std::vector<HydraulicNodeJunction> batch;
    for (int ordinal = 0; ordinal < 256; ++ordinal) {
        batch.push_back(item<HydraulicNodeJunction>(991100 + ordinal));
        if (batch.size() == 128) {
            edit(batch, true);
            batch.clear();
        }
    }
    const Records full = records;
    const HydraulicNodeJunction overflow = item<HydraulicNodeJunction>(991999);
    edit({overflow}, true);
    edit({overflow}, false);
    require(same(records, full), "undoing a member that opened a new chunk must restore the network exactly");
    // Drop and re-create the same chunk in one batch: a valid single delta.
    edit({overflow}, true);
    std::vector<native::ModelChange> changes{native::ModelChange{
        hydraulic::keyFromUuid(overflow.uuid), std::nullopt}};
    const HydraulicNodeJunction again = item<HydraulicNodeJunction>(991998);
    changes.push_back(hydraulic::changeFor(again));
    hydraulic::materializeDraftMembership(read,
        {hydraulic::DraftMembershipIntent{hydraulic::keyFromUuid(networkId),
             hydraulic::keyFromUuid(overflow.uuid), 201, 9, false},
         hydraulic::DraftMembershipIntent{hydraulic::keyFromUuid(networkId),
             hydraulic::keyFromUuid(again.uuid), 201, 9, true}}, changes);
    std::set<native::Key> keys;
    for (const native::ModelChange& change : changes)
        require(keys.insert(change.key).second, "a membership batch emitted two changes for one key");
}

// The first hydraulic distribution network is initialized in an unborn private working
// copy without inventing a Commit. Its subsequent MODEL actions must still
// support Undo/Redo, including atomic membership removal/restoration.
void exerciseUnbornFirstTankHistory() {
    const std::filesystem::path folder = tempDir();
    try {
        std::unique_ptr<native::ProjectService> service =
            std::make_unique<native::ProjectService>(folder);
        service->configureRepositoryRole(native::RepositoryRole::Private);
        const std::string projectId = service->createUnbornProject("Unborn tank history").id;
        const std::string session = native::UuidV7::generate().toString();
        const std::string draftId = service->createWorkingCopy(
            projectId, "main", std::nullopt, session).id;
        const std::string token = service->acquireWorkingCopyLease(
            draftId, session, 120).token;
        const QUuid networkId = uuid(990301);
        const native::ModelChange root = hydraulic::emptyDraftHydraulicDistributionNetworkRoot(
            networkId, QStringLiteral("New network"));
        // Initialize like the server does: the network root together with its
        // (empty) Distribution annotation layer. Every distribution network
        // must own exactly one layer, and History checks the unborn seed
        // with the complete candidate validator.
        std::vector<native::ModelChange> initialization{root};
        MapAnnotationLayer initialLayer;
        initialLayer.uuid = native::annotations::distributionLayerUuid(networkId);
        initialLayer.domain = MapAnnotationDomain::Distribution;
        initialLayer.owner_network_uuid = networkId;
        for (const native::ModelChange& change :
             native::annotations::fullDraftAnnotationLayerChanges(initialLayer, {}))
            initialization.push_back(change);
        require(service->saveWorkingCopyChanges(draftId, session, token, 0,
                    initialization).generation == 1,
                "Unborn History could not initialize its first network");
        const HydraulicNodeTank tank = item<HydraulicNodeTank>(990302);
        aowis::model::ModelEditTransaction action;
        action.transactionId = QUuid(QString::fromStdString(
            native::UuidV7::generate().toString()));
        action.operation = QStringLiteral("hydraulic.tank.create");
        action.edits.append(aowis::model::ModelEntityEdit{
            tank.uuid, QStringLiteral("HydraulicNodeTank"), std::nullopt,
            native::ModelCodec<HydraulicNodeTank>::encode(tank)});
        const QByteArray frame = aowis::model::ModelEditTransactionCodec::encode(action);
        require(!frame.isEmpty(), "Unborn History tank action cannot be encoded");
        const std::function<std::optional<native::ModelRecord>(const native::Key&)> initial =
            [&root](const native::Key& key) -> std::optional<native::ModelRecord> {
                return key == root.key ? root.record : std::nullopt;
            };
        // The small changeFor() convenience overloads cover junctions and
        // pipes only. Build the tank's canonical native record with its
        // declared kind and outbound reference index explicitly.
        native::ModelRecord tankRecord;
        tankRecord.kind = 203;
        tankRecord.payload = hydraulic::bytesFromQByteArray(
            native::ModelCodec<HydraulicNodeTank>::encode(tank));
        require(!tankRecord.payload.empty(),
                "Unborn History cannot encode the canonical tank record");
        if (!tank.volume_curve_uuid.isNull())
            tankRecord.references.push_back(hydraulic::keyFromUuid(tank.volume_curve_uuid));
        if (!tank.quality_source.pattern_uuid.isNull())
            tankRecord.references.push_back(hydraulic::keyFromUuid(tank.quality_source.pattern_uuid));
        std::sort(tankRecord.references.begin(), tankRecord.references.end());
        tankRecord.references.erase(
            std::unique(tankRecord.references.begin(), tankRecord.references.end()),
            tankRecord.references.end());
        std::vector<native::ModelChange> additions{
            native::ModelChange{hydraulic::keyFromUuid(tank.uuid), std::move(tankRecord)}};
        hydraulic::materializeDraftMembership(initial,
            {hydraulic::DraftMembershipIntent{
                hydraulic::keyFromUuid(networkId), hydraulic::keyFromUuid(tank.uuid),
                203, 10, true}}, additions);
        require(service->saveWorkingCopyTransactions(draftId, session, token, 1,
                    additions, {hydraulic::bytesFromQByteArray(frame)}).generation == 2,
                "Unborn History could not save tank creation");
        const std::function<NetworkHydraulicDistribution()> readDraft =
            [&service, &draftId, &networkId]() -> NetworkHydraulicDistribution {
                std::map<native::Key, std::optional<native::ModelRecord>> changes;
                const native::WorkingCopyInfo draft = service->workingCopy(draftId, false);
                for (const native::ModelChange& change : draft.candidateChanges)
                    require(changes.emplace(change.key, change.record).second,
                            "Unborn History candidate has duplicate keys");
                const std::function<std::optional<native::ModelRecord>(const native::Key&)> read =
                    [&changes](const native::Key& key) -> std::optional<native::ModelRecord> {
                        const std::map<native::Key, std::optional<native::ModelRecord>>::const_iterator
                            found = changes.find(key);
                        return found == changes.end() ? std::nullopt : found->second;
                    };
                const std::optional<NetworkHydraulicDistribution> network =
                    hydraulic::loadDraftHydraulicDistributionNetwork(read, networkId);
                require(network.has_value(), "Unborn History network could not be loaded");
                return *network;
            };
        require(readDraft().nodes_tanks.size() == 1,
                "Unborn History tank was not persisted");
        QString error;
        const std::optional<std::vector<native::ModelChange>> plan =
            native::annotations::planVerifiedActiveDraftAction(
                *service, draftId, std::nullopt, 0, 2, &error);
        require(plan && !(*plan).empty(),
                "Unborn History could not verify the first Undo");
        const std::optional<native::WorkingCopyInfo> undo =
            native::annotations::navigateVerifiedActiveDraftAction(
                *service, native::UuidV7::generate().toString(), draftId,
                session, token, std::nullopt, 0, 2, &error);
        require(undo && undo->generation == 3 && undo->draftActionPosition == 0 &&
                undo->journalCount == 1 && !undo->baseRevision,
                "Unborn History failed to publish first Undo");
        require(readDraft().nodes_tanks.isEmpty(),
                "Unborn History Undo did not remove the tank");
        const std::optional<native::WorkingCopyInfo> redo =
            native::annotations::navigateVerifiedActiveDraftAction(
                *service, native::UuidV7::generate().toString(), draftId,
                session, token, std::nullopt, 1, 3, &error);
        require(redo && redo->generation == 4 && redo->draftActionPosition == 1,
                "Unborn History failed to publish first Redo");
        require(readDraft().nodes_tanks.size() == 1 &&
                readDraft().nodes_tanks.first().uuid == tank.uuid,
                "Unborn History Redo did not restore the original tank");
        service.reset();
        native::ProjectService recovered(folder);
        const native::WorkingCopyInfo after = recovered.workingCopy(draftId, true);
        require(!after.baseRevision && after.journalCount == 1 &&
                after.draftActionPosition == 1 && after.generation == 4,
                "Unborn History position was not recovered after restart");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}


// History editing must remain possible after Undo crosses immutable Commits.
// New MODEL actions begin at the verified archived coordinate, not at the
// native first parent of the eventual Commit. Old Commits remain immutable,
// while their abandoned actions leave the *active* Undo/Redo timeline.
void exerciseArchivedHistoryForkedHydraulicEdit() {
    const std::filesystem::path folder = tempDir();
    try {
        std::unique_ptr<native::ProjectService> service =
            std::make_unique<native::ProjectService>(folder);
        service->configureRepositoryRole(native::RepositoryRole::Private);
        const std::string projectId = service->createUnbornProject(
            "Hydraulic editor forks after crossing Commit markers").id;
        const std::string session = native::UuidV7::generate().toString();
        const NetworkHydraulicDistribution original = completeNetwork();
        require(!original.curves_generic.isEmpty(), "History fork fixture needs a generic curve");
        const native::RevisionId baseline = service->commitModelAtExpectedHead(
            projectId, "main", initialEngineeringSnapshot(original),
            commitMetadata(), std::nullopt,
            native::annotations::validateAnnotationSnapshot);

        const std::function<aowis::model::ModelEditTransaction(
            const HydraulicCurveGeneric&, const HydraulicCurveGeneric&)> encodeEdit =
            [](const HydraulicCurveGeneric& before, const HydraulicCurveGeneric& after)
                    -> aowis::model::ModelEditTransaction {
                aowis::model::ModelEditTransaction event;
                event.transactionId = QUuid(QString::fromStdString(
                    native::UuidV7::generate().toString()));
                event.operation = QStringLiteral("hydraulic.generic-curve.update");
                event.edits.append(aowis::model::ModelEntityEdit{
                    before.uuid, QStringLiteral("HydraulicCurveGeneric"),
                    native::ModelCodec<HydraulicCurveGeneric>::encode(before),
                    native::ModelCodec<HydraulicCurveGeneric>::encode(after)});
                return event;
            };
        const std::function<native::ModelChange(const HydraulicCurveGeneric&,
            const native::RevisionId&)> nativeLabelChange =
            [&service, &projectId](const HydraulicCurveGeneric& label,
                const native::RevisionId& source) -> native::ModelChange {
                native::ModelChange change;
                change.key = hydraulic::keyFromUuid(label.uuid);
                change.record = service->getModelAtRevision(projectId, source, change.key);
                require(change.record && change.record->kind == 215,
                        "History fork fixture has no stored generic-curve record");
                change.record->payload = hydraulic::bytesFromQByteArray(
                    native::ModelCodec<HydraulicCurveGeneric>::encode(label));
                return change;
            };

        NetworkHydraulicDistribution firstNetwork = original;
        firstNetwork.curves_generic[0].id = QStringLiteral("Committed action one");
        const aowis::model::ModelEditTransaction firstAction =
            encodeEdit(original.curves_generic[0], firstNetwork.curves_generic[0]);
        const QByteArray firstFrame =
            aowis::model::ModelEditTransactionCodec::encode(firstAction);
        const std::string firstDraft = service->createWorkingCopy(
            projectId, "main", baseline, session).id;
        const std::string firstToken = service->acquireWorkingCopyLease(
            firstDraft, session, 120).token;
        require(!firstFrame.isEmpty() &&
                service->saveWorkingCopyTransactions(firstDraft, session, firstToken, 0,
                    {nativeLabelChange(firstNetwork.curves_generic[0], baseline)},
                    {hydraulic::bytesFromQByteArray(firstFrame)}).generation == 1,
                "Cannot Save first MODEL generic-curve action");
        const std::string firstOperation = native::UuidV7::generate().toString();
        const native::LocalCommitInfo firstCommit = service->commitWorkingCopy(
            firstOperation, firstDraft, session, firstToken, 1, baseline,
            commitMetadata(),
            native::annotations::validateAnnotationSnapshot);
        require(firstCommit.state == native::LocalCommitState::Succeeded,
                "Cannot Commit first MODEL action");
        const std::optional<native::RevisionId> firstRevision =
            native::RevisionId::parse(firstCommit.reservedRevisionId);
        require(firstRevision.has_value(), "First MODEL revision was not created");

        NetworkHydraulicDistribution secondNetwork = firstNetwork;
        secondNetwork.curves_generic[0].id = QStringLiteral("Abandoned committed action");
        const aowis::model::ModelEditTransaction secondAction =
            encodeEdit(firstNetwork.curves_generic[0], secondNetwork.curves_generic[0]);
        const QByteArray secondFrame =
            aowis::model::ModelEditTransactionCodec::encode(secondAction);
        const std::string secondDraft = service->createWorkingCopy(
            projectId, "main", *firstRevision, session).id;
        const std::string secondToken = service->acquireWorkingCopyLease(
            secondDraft, session, 120).token;
        require(!secondFrame.isEmpty() &&
                service->saveWorkingCopyTransactions(secondDraft, session, secondToken, 0,
                    {nativeLabelChange(secondNetwork.curves_generic[0], *firstRevision)},
                    {hydraulic::bytesFromQByteArray(secondFrame)}).generation == 1,
                "Cannot Save second MODEL generic-curve action");
        const native::LocalCommitInfo secondCommit = service->commitWorkingCopy(
            native::UuidV7::generate().toString(), secondDraft, session,
            secondToken, 1, *firstRevision, commitMetadata(),
            native::annotations::validateAnnotationSnapshot);
        require(secondCommit.state == native::LocalCommitState::Succeeded,
                "Cannot Commit second MODEL action");
        const std::optional<native::RevisionId> secondRevision =
            native::RevisionId::parse(secondCommit.reservedRevisionId);
        require(secondRevision.has_value(), "Second MODEL revision was not created");

        const std::string forkDraft = service->createWorkingCopy(
            projectId, "main", *secondRevision, session).id;
        const std::string forkToken = service->acquireWorkingCopyLease(
            forkDraft, session, 120).token;
        QString error;
        const std::optional<native::WorkingCopyInfo> selected =
            native::annotations::navigateVerifiedCombinedAction(*service,
                native::UuidV7::generate().toString(), forkDraft, session,
                forkToken, *secondRevision, *firstRevision, 0, 0, &error);
        require(selected && selected->generation == 1 &&
                selected->historyActionPosition == 0,
                ("Cannot Undo across Commit marker: " + error.toStdString()).c_str());

        NetworkHydraulicDistribution alternativeNetwork = original;
        alternativeNetwork.curves_generic[0].id = QStringLiteral("New edit from old History");
        const aowis::model::ModelEditTransaction alternativeAction =
            encodeEdit(original.curves_generic[0], alternativeNetwork.curves_generic[0]);
        const QByteArray alternativeFrame =
            aowis::model::ModelEditTransactionCodec::encode(alternativeAction);
        require(!alternativeFrame.isEmpty(), "Cannot encode alternative MODEL action");
        const native::WorkingCopyInfo saved = service->saveWorkingCopyTransactions(
            forkDraft, session, forkToken, 1,
            {nativeLabelChange(alternativeNetwork.curves_generic[0], *secondRevision)},
            {hydraulic::bytesFromQByteArray(alternativeFrame)});
        require(saved.generation == 2 && saved.journalCount == 1 &&
                saved.historyForkRevisionId == firstRevision->toString() &&
                saved.historyForkActionPosition == 0 &&
                service->optionalHead(projectId, "main") == secondRevision,
                "Forked MODEL Save discarded origin or changed immutable HEAD");
        const std::optional<std::vector<native::ModelChange>> undoPlan =
            native::annotations::planVerifiedActiveDraftAction(
                *service, forkDraft, *secondRevision, 0, 2, &error);
        const std::optional<std::vector<native::ModelChange>> redoPlan =
            native::annotations::planVerifiedActiveDraftAction(
                *service, forkDraft, *secondRevision, 1, 2, &error);
        require(undoPlan && redoPlan,
                ("Cannot replay new forked MODEL action: " + error.toStdString()).c_str());
        const std::optional<native::WorkingCopyInfo> undone =
            native::annotations::navigateVerifiedActiveDraftAction(*service,
                native::UuidV7::generate().toString(), forkDraft, session, forkToken,
                *secondRevision, 0, 2, &error);
        require(undone && undone->generation == 3 && undone->draftActionPosition == 0,
                ("Cannot Undo newly forked MODEL action: " + error.toStdString()).c_str());
        const std::optional<native::WorkingCopyInfo> redone =
            native::annotations::navigateVerifiedActiveDraftAction(*service,
                native::UuidV7::generate().toString(), forkDraft, session, forkToken,
                *secondRevision, 1, 3, &error);
        require(redone && redone->generation == 4 && redone->draftActionPosition == 1,
                ("Cannot Redo newly forked MODEL action: " + error.toStdString()).c_str());
        const native::LocalCommitInfo forkCommit = service->commitWorkingCopy(
            native::UuidV7::generate().toString(), forkDraft, session,
            forkToken, 4, *secondRevision, commitMetadata(),
            native::annotations::validateAnnotationSnapshot);
        require(forkCommit.state == native::LocalCommitState::Succeeded,
                "Cannot Commit MODEL edit made after archived Undo");
        const std::optional<native::RevisionId> forkRevision =
            native::RevisionId::parse(forkCommit.reservedRevisionId);
        require(forkRevision.has_value(), "Fork Commit lacks immutable revision");
        const std::optional<native::annotations::VerifiedIndexedAnnotationActionBoundary> verified =
            native::annotations::verifyIndexedCombinedActionBoundary(
                *service, projectId, "main", *forkRevision, 1, &error);
        // The forked archive starts at the first Commit's action 0, i.e. at
        // that Commit's parent, not at its own first parent (the second Commit).
        require(verified && verified->boundary.actionCount == 1 &&
                verified->originRevision && *verified->originRevision != *secondRevision &&
                *verified->originRevision != *firstRevision,
                ("Cannot replay forked Commit actions: " + error.toStdString()).c_str());
        const native::BranchActionHistoryPage history =
            service->branchActionHistory(projectId, "main", 10);
        require(history.commits.size() >= 2 &&
                history.commits.front().revision.id == *forkRevision &&
                history.commits.front().logicalParentRevisionId == firstRevision->toString(),
                "New logical editing History still includes abandoned Commit actions");
        const std::optional<native::ModelRecord> immutableSecond =
            service->getModelAtRevision(projectId, *secondRevision,
                hydraulic::keyFromUuid(original.curves_generic[0].uuid));
        require(immutableSecond && immutableSecond->payload ==
                hydraulic::bytesFromQByteArray(
                    native::ModelCodec<HydraulicCurveGeneric>::encode(secondNetwork.curves_generic[0])),
                "New editing History modified the abandoned immutable Commit");
        service.reset();
        native::ProjectService recovered(folder);
        const std::optional<native::annotations::VerifiedIndexedAnnotationActionBoundary> afterRestart =
            native::annotations::verifyIndexedCombinedActionBoundary(
                recovered, projectId, "main", *forkRevision, 1, &error);
        require(afterRestart && afterRestart->boundary.actionCount == 1,
                "Forked MODEL archive cannot be verified after DVMDB restart");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        exerciseVerifiedRevisionHydraulicCoverage();
        exerciseVerifiedActiveDraftActionNavigation();
        exerciseMembershipUndoIsExact();
        exerciseUnbornFirstTankHistory();
        exerciseArchivedHistoryForkedHydraulicEdit();
        exerciseAtomicRootMetadataEdit();
        exerciseInt00IncompleteNetworkRoundTrip();
        exerciseUuidV7Hydraulic();
        exerciseUuidV7IndexedReferences();
        exerciseUuidV7IndexedMembership();
        exerciseUuidV7HydraulicMerges();
        exerciseRootFieldMerges();
        exerciseUuidV7CoordinatedMembershipMerges();
        exerciseUuidV7MergeChoiceQualification();
        exerciseUuidV7CallerPinnedWrites();
        exerciseUuidV7ConcurrentSnapshots();
        exerciseUuidV7ChunkBoundaries();
        exerciseUuidV7FastForward();
        std::cout << "Complete hydraulic distribution network persistence tests passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Hydraulic distribution network persistence test failed: " << failure.what() << '\n';
        return 1;
    }
}
