#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/hydraulic_network_persistence.h"
#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/revision_record.h"
#include <aowis/model/edit/hydraulic_root_edit.h>

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

NetworkHydraulic completeNetwork() {
    NetworkHydraulic n;
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
    n.curves_generic.append(item<HydraulicCurveGeneric>(17));
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
    n.map_labels.append(item<HydraulicMapLabel>(28));
    n.map_labels[0].anchor_node_uuid = uuid(20);
    n.map_labels[0].text = QStringLiteral("Well Ω");
    return n;
}

void compareNetworks(const NetworkHydraulic& source, const NetworkHydraulic& result) {
    require(aowis::dvmdb::ModelCodec<NetworkHydraulic>::encode(result) ==
            aowis::dvmdb::ModelCodec<NetworkHydraulic>::encode(source),
            "complete canonical NetworkHydraulic changed on reload");
    require(result.uuid == source.uuid && result.id == source.id &&
            result.title_line_1 == source.title_line_1 &&
            result.duration_s == source.duration_s &&
            result.map_backdrop.file == source.map_backdrop.file &&
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
    CHECK_LIST(map_labels, HydraulicMapLabel)
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
        const NetworkHydraulic baseline = completeNetwork();
        native::RevisionId initial = native::RevisionId::generate();
        native::RevisionId saved = initial;
        native::RevisionId edited = initial;
        native::RevisionId scalarRewire = initial;
        native::RevisionId physicalHead = initial;
        native::RevisionId catalogueHead = initial;
        NetworkHydraulic scalarExpectedAfter = baseline;
        NetworkHydraulic physicalExpectedAfter = baseline;
        NetworkHydraulic catalogueExpectedAfter = baseline;
        {
            native::Repository repository(folder);
            initial = repository.createRootBranch(project, "main", commitMetadata());
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::loadHydraulicNetworkAtRevision(repository, project, initial, baseline.uuid);
            }), "initial raw UUIDv7 revision should reject model lookup");
            saved = hydraulic::saveHydraulicNetwork(repository, project, "main", baseline, initial, commitMetadata());
            require(saved != initial && repository.head(project, "main") == saved,
                    "UUIDv7 hydraulic save did not publish a fresh revision");
            const std::optional<NetworkHydraulic> loaded =
                hydraulic::loadHydraulicNetworkAtRevision(repository, project, saved, baseline.uuid);
            require(loaded.has_value(), "UUIDv7 hydraulic network missing after save");
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
            NetworkHydraulic physicalNetwork = baseline;
            physicalNetwork.nodes_reservoirs[0].hydraulic_head_m += 12.5;
            physicalNetwork.nodes_tanks[0].terrain_elevation_m += 2.0;
            const std::vector<hydraulic::HydraulicEntityChange> physicalChanges{
                {physicalNetwork.nodes_reservoirs[0].uuid,
                 hydraulic::HydraulicEntity{physicalNetwork.nodes_reservoirs[0]}},
                {physicalNetwork.nodes_tanks[0].uuid,
                 hydraulic::HydraulicEntity{physicalNetwork.nodes_tanks[0]}}};
            hydraulic::HydraulicEditDiagnostics physicalStats;
            physicalHead = hydraulic::editHydraulicNetwork(
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
            compareNetworks(physicalNetwork, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, physicalHead, baseline.uuid));
            require(hydraulic::editHydraulicNetwork(repository, project, "indexed-physical",
                    baseline.uuid, physicalChanges, physicalHead, commitMetadata(), &physicalStats) == physicalHead &&
                    physicalStats.indexedPhysicalFastPath,
                    "UUIDv7 no-op physical edit created a revision");
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "indexed-physical",
                    baseline.uuid, physicalChanges, saved, commitMetadata(), &physicalStats);
            }), "UUIDv7 physical edit accepted stale expected HEAD");
            HydraulicNodeReservoir invalidPhysical = physicalNetwork.nodes_reservoirs[0];
            invalidPhysical.hydraulic_head_m = std::numeric_limits<double>::infinity();
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "indexed-physical",
                    baseline.uuid, {{invalidPhysical.uuid, hydraulic::HydraulicEntity{invalidPhysical}}},
                    physicalHead, commitMetadata(), &physicalStats);
            }) && repository.head(project, "indexed-physical") == physicalHead,
                    "UUIDv7 invalid physical value advanced HEAD");
            HydraulicLinkPump invalidRewire = physicalNetwork.links_pumps[0];
            invalidRewire.node_uuid_to = uuid(9999);
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "indexed-physical",
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
            NetworkHydraulic catalogueNetwork = baseline;
            catalogueNetwork.patterns_time[0].multipliers = {1.0, 1.15, 0.9};
            const std::vector<hydraulic::HydraulicEntityChange> catalogueChanges{
                {catalogueNetwork.patterns_time[0].uuid,
                 hydraulic::HydraulicEntity{catalogueNetwork.patterns_time[0]}}};
            hydraulic::HydraulicEditDiagnostics catalogueStats;
            catalogueHead = hydraulic::editHydraulicNetwork(
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
            compareNetworks(catalogueNetwork, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, catalogueHead, baseline.uuid));
            require(hydraulic::editHydraulicNetwork(repository, project, "indexed-catalogue",
                    baseline.uuid, catalogueChanges, catalogueHead, commitMetadata(), &catalogueStats) == catalogueHead &&
                    catalogueStats.indexedCatalogueFastPath,
                    "UUIDv7 no-op catalogue edit created a revision");
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "indexed-catalogue",
                    baseline.uuid, catalogueChanges, saved, commitMetadata(), &catalogueStats);
            }), "UUIDv7 catalogue edit accepted stale expected HEAD");
            HydraulicPipeMaterial wrongKind = catalogueNetwork.pipe_materials[0];
            wrongKind.uuid = catalogueNetwork.patterns_time[0].uuid;
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "indexed-catalogue",
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
            NetworkHydraulic scalarNetwork = baseline;
            scalarNetwork.links_pipes[0].diameter_mm = 143.25;
            scalarNetwork.nodes_junctions[0].elevation_m = 17.0;
            scalarNetwork.map_labels[0].text = QStringLiteral("UUIDv7 indexed label");
            const std::vector<hydraulic::HydraulicEntityChange> scalarChanges{
                {scalarNetwork.links_pipes[0].uuid,
                 hydraulic::HydraulicEntity{scalarNetwork.links_pipes[0]}},
                {scalarNetwork.nodes_junctions[0].uuid,
                 hydraulic::HydraulicEntity{scalarNetwork.nodes_junctions[0]}},
                {scalarNetwork.map_labels[0].uuid,
                 hydraulic::HydraulicEntity{scalarNetwork.map_labels[0]}}};
            hydraulic::HydraulicEditDiagnostics scalarStats;
            const native::RevisionId scalarHead = hydraulic::editHydraulicNetwork(
                repository, project, "indexed-scalars", baseline.uuid,
                scalarChanges, saved, commitMetadata(), &scalarStats);
            require(scalarHead != saved && scalarStats.indexedScalarFastPath &&
                    scalarStats.validatedEntities == 3,
                    "UUIDv7 safe scalar edit did not use indexed records");
            require(repository.head(project, "main") == saved,
                    "UUIDv7 scalar edit moved unrelated branch HEAD");
            const native::ModelRecord manifestAfter = *repository.getModelAtRevision(
                project, scalarHead, hydraulic::keyFromUuid(baseline.uuid));
            require(native::encodeModelRecord(scalarManifestBefore) ==
                        native::encodeModelRecord(manifestAfter),
                    "UUIDv7 indexed scalar edit rewrote membership manifest");
            compareNetworks(scalarNetwork, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, scalarHead, baseline.uuid));
            compareNetworks(baseline, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, saved, baseline.uuid));
            require(hydraulic::editHydraulicNetwork(repository, project, "indexed-scalars",
                    baseline.uuid, scalarChanges, scalarHead, commitMetadata(), &scalarStats) == scalarHead &&
                    scalarStats.indexedScalarFastPath,
                    "UUIDv7 no-op scalar edit created another revision");
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project,
                    "indexed-scalars", baseline.uuid, scalarChanges, saved, commitMetadata(), &scalarStats);
            }), "UUIDv7 indexed scalar edit accepted stale expected HEAD");
            HydraulicLinkPipe invalidScalarTopology = scalarNetwork.links_pipes[0];
            invalidScalarTopology.node_uuid_to = uuid(9999);
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project,
                    "indexed-scalars", baseline.uuid,
                    {{invalidScalarTopology.uuid,
                      hydraulic::HydraulicEntity{invalidScalarTopology}}},
                    scalarHead, commitMetadata(), &scalarStats);
            }) && !scalarStats.indexedScalarFastPath &&
                    repository.head(project, "indexed-scalars") == scalarHead,
                    "UUIDv7 topology edit incorrectly bypassed canonical validation");
            HydraulicLinkPipe validScalarTopology = scalarNetwork.links_pipes[0];
            validScalarTopology.node_uuid_from = uuid(18);
            const native::RevisionId rewireHead = hydraulic::editHydraulicNetwork(
                repository, project, "indexed-scalars", baseline.uuid,
                {{validScalarTopology.uuid, hydraulic::HydraulicEntity{validScalarTopology}}},
                scalarHead, commitMetadata(), &scalarStats);
            require(rewireHead != scalarHead && !scalarStats.indexedScalarFastPath,
                    "UUIDv7 topology edit incorrectly used scalar fast path");
            scalarNetwork.links_pipes[0] = validScalarTopology;
            scalarRewire = rewireHead;
            scalarExpectedAfter = scalarNetwork;
            compareNetworks(scalarNetwork, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, rewireHead, baseline.uuid));
            HydraulicNodeJunction updated = baseline.nodes_junctions.front();
            updated.metadata.description = QStringLiteral("UUIDv7 updated junction");
            const std::vector<hydraulic::HydraulicEntityChange> changes{
                {updated.uuid, hydraulic::HydraulicEntity{updated}}};
            edited = hydraulic::editHydraulicNetwork(
                repository, project, "main", baseline.uuid, changes, saved, commitMetadata());
            require(edited != saved && repository.head(project, "main") == edited,
                    "UUIDv7 hydraulic edit failed to advance HEAD");
            require(hydraulic::editHydraulicNetwork(
                repository, project, "main", baseline.uuid, changes, edited, commitMetadata()) == edited,
                    "UUIDv7 no-op edit created a new revision");
            NetworkHydraulic expected = baseline;
            expected.nodes_junctions.front() = updated;
            compareNetworks(expected, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, edited, baseline.uuid));
            compareNetworks(baseline, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, saved, baseline.uuid));
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::editHydraulicNetwork(
                    repository, project, "main", baseline.uuid, changes, saved, commitMetadata());
            }), "UUIDv7 stale HEAD edit was accepted");
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::saveHydraulicNetwork(repository, project, "main", baseline, saved, commitMetadata());
            }), "UUIDv7 stale full save was accepted");
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "main", baseline.uuid,
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
            compareNetworks(scalarExpectedAfter, *hydraulic::loadHydraulicNetworkAtRevision(
                reopened, project, scalarRewire, baseline.uuid));
            compareNetworks(physicalExpectedAfter, *hydraulic::loadHydraulicNetworkAtRevision(
                reopened, project, physicalHead, baseline.uuid));
            compareNetworks(catalogueExpectedAfter, *hydraulic::loadHydraulicNetworkAtRevision(
                reopened, project, catalogueHead, baseline.uuid));
            compareNetworks(baseline, *hydraulic::loadHydraulicNetworkAtRevision(
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
    const NetworkHydraulic baseline = completeNetwork();
    native::RevisionId topologyHead = native::RevisionId::generate();
    native::RevisionId dependentHead = native::RevisionId::generate();
    NetworkHydraulic topologyExpected = baseline;
    NetworkHydraulic dependentExpected = baseline;
    try {
        {
            native::Repository repository(folder);
            const native::RevisionId genesis = repository.createRootBranch(project, "main", commitMetadata());
            const native::RevisionId saved = hydraulic::saveHydraulicNetwork(
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
            const native::RevisionId referencedHead = hydraulic::editHydraulicNetwork(
                repository, project, "uuid-referenced-pipe", baseline.uuid,
                {{referencedPipe.uuid, hydraulic::HydraulicEntity{referencedPipe}}},
                saved, commitMetadata(), &referencedStats);
            require(referencedHead != saved && !referencedStats.indexedTopologyFastPath,
                    "UUIDv7 topology bypassed an incoming dependent control");
            NetworkHydraulic referencedExpected = baseline;
            referencedExpected.links_pipes[0] = referencedPipe;
            compareNetworks(referencedExpected, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, referencedHead, baseline.uuid));
            // The baseline has a control referring to its pipe. Removing that
            // control is a full-network operation and permits an independent
            // rewire to take the bounded indexed path.
            topologyExpected.controls_simple.clear();
            const native::RevisionId topologyBase = hydraulic::saveHydraulicNetwork(
                repository, project, "uuid-topology", topologyExpected, saved, commitMetadata());
            const native::ModelRecord topologyRoot = *repository.getModelAtRevision(
                project, topologyBase, hydraulic::keyFromUuid(baseline.uuid));
            topologyExpected.links_pipes[0].node_uuid_from = uuid(18);
            const HydraulicLinkPipe rewire = topologyExpected.links_pipes[0];
            const std::vector<hydraulic::HydraulicEntityChange> topologyChanges{
                {rewire.uuid, hydraulic::HydraulicEntity{rewire}}};
            hydraulic::HydraulicEditDiagnostics topologyStats;
            topologyHead = hydraulic::editHydraulicNetwork(repository, project, "uuid-topology",
                baseline.uuid, topologyChanges, topologyBase, commitMetadata(), &topologyStats);
            require(topologyHead != topologyBase && topologyStats.indexedTopologyFastPath &&
                    topologyStats.validatedEntities == 1,
                    "UUIDv7 independent pipe rewire missed indexed fast path");
            require(native::encodeModelRecord(topologyRoot) == native::encodeModelRecord(
                *repository.getModelAtRevision(project, topologyHead,
                    hydraulic::keyFromUuid(baseline.uuid))),
                    "UUIDv7 indexed topology edit rewrote membership root");
            compareNetworks(topologyExpected, *hydraulic::loadHydraulicNetworkAtRevision(
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
            require(hydraulic::editHydraulicNetwork(repository, project, "uuid-topology",
                        baseline.uuid, topologyChanges, topologyHead, commitMetadata(), &topologyStats) == topologyHead &&
                    (topologyStats.indexedTopologyFastPath || topologyStats.indexedScalarFastPath),
                    "UUIDv7 no-op topology edit generated another revision");
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "uuid-topology",
                    baseline.uuid, topologyChanges, topologyBase, commitMetadata(), &topologyStats);
            }), "UUIDv7 topology edit accepted stale expected HEAD");
            HydraulicLinkPipe wrongKind = rewire;
            wrongKind.node_uuid_from = uuid(10); // Existing material, not a node.
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "uuid-topology",
                    baseline.uuid, {{wrongKind.uuid, hydraulic::HydraulicEntity{wrongKind}}},
                    topologyHead, commitMetadata(), &topologyStats);
            }) && !topologyStats.indexedTopologyFastPath &&
                    repository.head(project, "uuid-topology") == topologyHead,
                    "UUIDv7 topology accepted wrong-type endpoint");
            HydraulicLinkPipe missing = rewire;
            missing.node_uuid_from = uuid(9009);
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "uuid-topology",
                    baseline.uuid, {{missing.uuid, hydraulic::HydraulicEntity{missing}}},
                    topologyHead, commitMetadata(), &topologyStats);
            }) && !topologyStats.indexedTopologyFastPath,
                    "UUIDv7 topology accepted missing endpoint");
            HydraulicLinkPipe metadata = rewire;
            metadata.id = QStringLiteral("Full validation required");
            const native::RevisionId fullHead = hydraulic::editHydraulicNetwork(
                repository, project, "uuid-topology", baseline.uuid,
                {{metadata.uuid, hydraulic::HydraulicEntity{metadata}}},
                topologyHead, commitMetadata(), &topologyStats);
            require(fullHead != topologyHead && !topologyStats.indexedTopologyFastPath,
                    "UUIDv7 topology fast path accepted metadata changes");
            topologyExpected.links_pipes[0] = metadata;
            topologyHead = fullHead;
            compareNetworks(topologyExpected, *hydraulic::loadHydraulicNetworkAtRevision(
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
            dependentHead = hydraulic::editHydraulicNetwork(repository, project, "uuid-dependent",
                baseline.uuid, dependentChanges, saved, commitMetadata(), &dependentStats);
            require(dependentHead != saved && dependentStats.indexedDependentUpdateFastPath &&
                    dependentStats.validatedEntities == dependentChanges.size(),
                    "UUIDv7 dependent changes missed indexed fast path");
            dependentExpected.demand_points[0] = point;
            dependentExpected.controls_simple[0] = control;
            dependentExpected.controls_rules[0] = rule;
            require(native::validateHydraulicNetwork(dependentExpected).valid(),
                    "UUIDv7 indexed dependent fixture violates canonical validation");
            compareNetworks(dependentExpected, *hydraulic::loadHydraulicNetworkAtRevision(
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
            require(hydraulic::editHydraulicNetwork(repository, project, "uuid-dependent",
                        baseline.uuid, dependentChanges, dependentHead, commitMetadata(), &dependentStats) == dependentHead &&
                    dependentStats.indexedDependentUpdateFastPath,
                    "UUIDv7 no-op dependent edit generated a revision");
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "uuid-dependent",
                    baseline.uuid, dependentChanges, saved, commitMetadata(), &dependentStats);
            }), "UUIDv7 dependent update accepted stale expected HEAD");
            HydraulicDemandPoint invalid = point;
            invalid.attachment.pipe_assigned_junction_uuid = uuid(21); // Not a pipe endpoint.
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "uuid-dependent",
                    baseline.uuid, {{invalid.uuid, hydraulic::HydraulicEntity{invalid}}},
                    dependentHead, commitMetadata(), &dependentStats);
            }) && !dependentStats.indexedDependentUpdateFastPath &&
                    repository.head(project, "uuid-dependent") == dependentHead,
                    "UUIDv7 dependent path accepted invalid assigned junction");
            // A mixed dependent/property batch cannot bypass whole-model
            // validation, and a rejected batch must not publish its valid part.
            HydraulicMapLabel changedLabel = dependentExpected.map_labels[0];
            changedLabel.text = QStringLiteral("UUIDv7 reference fallback");
            HydraulicControlSimple changedControl = control;
            changedControl.trigger_node_uuid = uuid(20);
            const native::RevisionId mixedHead = hydraulic::editHydraulicNetwork(
                repository, project, "uuid-dependent", baseline.uuid,
                {{changedControl.uuid, hydraulic::HydraulicEntity{changedControl}},
                 {changedLabel.uuid, hydraulic::HydraulicEntity{changedLabel}}},
                dependentHead, commitMetadata(), &dependentStats);
            require(mixedHead != dependentHead && !dependentStats.indexedDependentUpdateFastPath,
                    "UUIDv7 mixed dependent batch bypassed canonical validation");
            dependentExpected.controls_simple[0] = changedControl;
            dependentExpected.map_labels[0] = changedLabel;
            dependentHead = mixedHead;
            compareNetworks(dependentExpected, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, dependentHead, baseline.uuid));
            HydraulicControlSimple invalidControl = changedControl;
            invalidControl.link_uuid = uuid(20); // A node is not a link.
            HydraulicDemandPoint validChangedPoint = point;
            validChangedPoint.attachment.pipe_position = 0.4;
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "uuid-dependent",
                    baseline.uuid,
                    {{validChangedPoint.uuid, hydraulic::HydraulicEntity{validChangedPoint}},
                     {invalidControl.uuid, hydraulic::HydraulicEntity{invalidControl}}},
                    dependentHead, commitMetadata(), &dependentStats);
            }) && repository.head(project, "uuid-dependent") == dependentHead,
                    "UUIDv7 invalid dependent batch committed its valid edit");
            compareNetworks(baseline, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, saved, baseline.uuid));
            (void)repository.verifyRepository();
            (void)repository.compact();
        }
        {
            native::Repository reopened(folder);
            require(reopened.head(project, "uuid-topology") == topologyHead &&
                    reopened.head(project, "uuid-dependent") == dependentHead,
                    "UUIDv7 indexed references changed HEAD after compaction/reopen");
            compareNetworks(topologyExpected, *hydraulic::loadHydraulicNetworkAtRevision(
                reopened, project, topologyHead, baseline.uuid));
            compareNetworks(dependentExpected, *hydraulic::loadHydraulicNetworkAtRevision(
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
    const NetworkHydraulic baseline = completeNetwork();
    native::RevisionId membershipHead = native::RevisionId::generate();
    NetworkHydraulic expected = baseline;
    try {
        {
            native::Repository repository(folder);
            const native::RevisionId genesis = repository.createRootBranch(project, "main", commitMetadata());
            const native::RevisionId saved = hydraulic::saveHydraulicNetwork(
                repository, project, "main", baseline, genesis, commitMetadata());
            require(repository.createBranch(project, "uuid-membership", "main") == saved,
                    "UUIDv7 membership branch did not inherit baseline");
            const HydraulicNodeJunction addedNode = item<HydraulicNodeJunction>(31110);
            HydraulicLinkPipe addedPipe = item<HydraulicLinkPipe>(31111);
            addedPipe.node_uuid_from = uuid(19);
            addedPipe.node_uuid_to = addedNode.uuid;
            addedPipe.material_uuid = uuid(10);
            HydraulicMapLabel addedLabel = item<HydraulicMapLabel>(31112);
            addedLabel.anchor_node_uuid = addedNode.uuid;
            addedLabel.text = QStringLiteral("UUIDv7 new junction");
            const std::vector<hydraulic::HydraulicEntityChange> insertions{
                {addedPipe.uuid, hydraulic::HydraulicEntity{addedPipe}},
                {addedNode.uuid, hydraulic::HydraulicEntity{addedNode}},
                {addedLabel.uuid, hydraulic::HydraulicEntity{addedLabel}}};
            hydraulic::HydraulicEditDiagnostics stats;
            const native::ModelRecord oldRoot = *repository.getModelAtRevision(
                project, saved, hydraulic::keyFromUuid(baseline.uuid));
            membershipHead = hydraulic::editHydraulicNetwork(
                repository, project, "uuid-membership", baseline.uuid,
                insertions, saved, commitMetadata(), &stats);
            require(membershipHead != saved && stats.indexedMembershipFastPath &&
                    stats.validatedEntities == insertions.size() &&
                    repository.head(project, "main") == saved,
                    "UUIDv7 insertion missed bounded membership path or changed source HEAD");
            expected.nodes_junctions.append(addedNode);
            expected.links_pipes.append(addedPipe);
            expected.map_labels.append(addedLabel);
            compareNetworks(expected, *hydraulic::loadHydraulicNetworkAtRevision(
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
                             hydraulic::keyFromUuid(addedPipe.uuid)) != incoming.end() &&
                    std::find(incoming.begin(), incoming.end(),
                             hydraulic::keyFromUuid(addedLabel.uuid)) != incoming.end(),
                    "UUIDv7 membership insertion lost dependent reference edges");
            require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "uuid-membership",
                    baseline.uuid, insertions, saved, commitMetadata(), &stats);
            }), "UUIDv7 membership insertion accepted stale expected HEAD");

            HydraulicLinkPipe invalidPipe = item<HydraulicLinkPipe>(31113);
            invalidPipe.node_uuid_from = uuid(10); // A material is not a node.
            invalidPipe.node_uuid_to = addedNode.uuid;
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "uuid-membership",
                    baseline.uuid, {{invalidPipe.uuid, hydraulic::HydraulicEntity{invalidPipe}}},
                    membershipHead, commitMetadata(), &stats);
            }) && !stats.indexedMembershipFastPath &&
                repository.head(project, "uuid-membership") == membershipHead,
                "UUIDv7 invalid inserted endpoint changed HEAD");
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "uuid-membership",
                    baseline.uuid, {{addedNode.uuid, std::nullopt}}, membershipHead, commitMetadata(), &stats);
            }) && !stats.indexedMembershipFastPath &&
                repository.head(project, "uuid-membership") == membershipHead,
                "UUIDv7 membership deleted a node with live incoming references");
            HydraulicNodeJunction invalidValue = item<HydraulicNodeJunction>(31115);
            invalidValue.elevation_m = std::numeric_limits<double>::infinity();
            require(throwsStorageError(native::StorageErrorKind::InvalidInput, [&] {
                (void)hydraulic::editHydraulicNetwork(repository, project, "uuid-membership",
                    baseline.uuid, {{invalidValue.uuid, hydraulic::HydraulicEntity{invalidValue}}},
                    membershipHead, commitMetadata(), &stats);
            }) && !stats.indexedMembershipFastPath &&
                repository.head(project, "uuid-membership") == membershipHead,
                "UUIDv7 indexed membership accepted invalid physical values");

            const native::RevisionId afterLabel = hydraulic::editHydraulicNetwork(
                repository, project, "uuid-membership", baseline.uuid,
                {{addedLabel.uuid, std::nullopt}}, membershipHead, commitMetadata(), &stats);
            require(afterLabel != membershipHead && stats.indexedMembershipFastPath &&
                    !repository.getModelAtRevision(
                        project, afterLabel, hydraulic::keyFromUuid(addedLabel.uuid)),
                    "UUIDv7 independent label deletion missed indexed membership path");
            expected.map_labels.removeLast();
            membershipHead = hydraulic::editHydraulicNetwork(
                repository, project, "uuid-membership", baseline.uuid,
                {{addedNode.uuid, std::nullopt}, {addedPipe.uuid, std::nullopt}},
                afterLabel, commitMetadata(), &stats);
            require(membershipHead != afterLabel && stats.indexedMembershipFastPath &&
                    stats.validatedEntities == 2,
                    "UUIDv7 dependent pipe-and-node removal missed indexed membership path");
            compareNetworks(baseline, *hydraulic::loadHydraulicNetworkAtRevision(
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
            HydraulicMapLabel mixedLabel = item<HydraulicMapLabel>(31114);
            mixedLabel.anchor_node_uuid = uuid(20);
            HydraulicNodeJunction changedNode = baseline.nodes_junctions[0];
            changedNode.metadata.description = QStringLiteral("mixed batch");
            const native::RevisionId mixedHead = hydraulic::editHydraulicNetwork(
                repository, project, "uuid-membership", baseline.uuid,
                {{mixedLabel.uuid, hydraulic::HydraulicEntity{mixedLabel}},
                 {changedNode.uuid, hydraulic::HydraulicEntity{changedNode}}},
                membershipHead, commitMetadata(), &stats);
            require(mixedHead != membershipHead && !stats.indexedMembershipFastPath,
                    "UUIDv7 mixed structural batch bypassed full validation");
            expected.nodes_junctions[0] = changedNode;
            expected.map_labels.append(mixedLabel);
            membershipHead = mixedHead;
            compareNetworks(expected, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, membershipHead, baseline.uuid));
            compareNetworks(baseline, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, saved, baseline.uuid));
            (void)repository.verifyRepository();
            (void)repository.compact();
        }
        {
            native::Repository reopened(folder);
            require(reopened.head(project, "uuid-membership") == membershipHead,
                    "UUIDv7 membership HEAD changed after reopen/compaction");
            compareNetworks(expected, *hydraulic::loadHydraulicNetworkAtRevision(
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

void exerciseUuidV7HydraulicMerges() {
    const std::filesystem::path folder = tempDir();
    const native::ProjectId project = native::ProjectId::generate();
    try {
        native::RevisionId resolved = native::RevisionId::generate();
        const NetworkHydraulic baseline = completeNetwork();
        {
            native::Repository repository(folder);
            const native::RevisionId root = repository.createRootBranch(project, "main", commitMetadata());
            const native::RevisionId base = hydraulic::saveHydraulicNetwork(
                repository, project, "main", baseline, root, commitMetadata());
            require(repository.createBranch(project, "feature", "main") == base,
                    "UUIDv7 hydraulic fork generated an extra revision");
            HydraulicNodeJunction targetJunction = baseline.nodes_junctions[0];
            targetJunction.metadata.description = QStringLiteral("hydraulic-target");
            const native::RevisionId target = hydraulic::editHydraulicNetwork(
                repository, project, "main", baseline.uuid,
                {{targetJunction.uuid, hydraulic::HydraulicEntity{targetJunction}}}, base, commitMetadata());
            HydraulicNodeJunction sourceJunction = baseline.nodes_junctions[0];
            sourceJunction.metadata.description = QStringLiteral("hydraulic-source");
            const native::RevisionId source = hydraulic::editHydraulicNetwork(
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
            NetworkHydraulic expected = baseline;
            expected.nodes_junctions[0] = sourceJunction;
            const std::optional<NetworkHydraulic> actual = hydraulic::loadHydraulicNetworkAtRevision(
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

            // A generic model record can be valid for the native model index
            // but invalid as a hydraulic-network member. Fast-forward must
            // run the whole-network validator before moving the target HEAD.
            require(repository.createBranchAtRevision(project,
                "invalid-left", "main", base) == base, "cannot create invalid merge target");
            require(repository.createBranchAtRevision(project,
                "invalid-right", "main", base) == base, "cannot create invalid merge source");
            const native::Key existingKey = hydraulic::keyFromUuid(baseline.nodes_junctions[0].uuid);
            const std::optional<native::ModelRecord> member = repository.getModelAtRevision(
                project, base, existingKey);
            require(member.has_value(), "missing hydraulic member fixture");
            const native::RevisionId badTip = repository.commitModel(
                project, "invalid-right", {{hydraulic::keyFromUuid(uuid(9701)), *member}}, commitMetadata(), base);
            const std::uint64_t sizeBefore = repository.packSize();
            const native::MergeResult invalid = hydraulic::mergeHydraulicBranches(
                repository, project, "invalid-left", "invalid-right",
                hydraulic::HydraulicMergeSelections{base, badTip, {}, {}}, commitMetadata());
            require(invalid.status == native::MergeStatus::Conflicts &&
                    repository.head(project, "invalid-left") == base &&
                    repository.head(project, "invalid-right") == badTip &&
                    repository.packSize() == sizeBefore,
                    "invalid UUIDv7 hydraulic fast-forward published a corrupt network");
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
    const NetworkHydraulic baseline = completeNetwork();
    native::RevisionId mergedAppendHead = native::RevisionId::generate();
    native::RevisionId mergedDeleteHead = native::RevisionId::generate();
    native::RevisionId mergedInitialChunkHead = native::RevisionId::generate();
    try {
        {
            native::Repository repository(folder);
            const native::RevisionId raw = repository.createRootBranch(project, "main", commitMetadata());
            const native::RevisionId base = hydraulic::saveHydraulicNetwork(
                repository, project, "main", baseline, raw, commitMetadata());
            require(repository.createBranch(project, "append-left", "main") == base &&
                    repository.createBranch(project, "append-right", "main") == base,
                    "UUIDv7 membership merge branches did not share the same base");

            HydraulicMapLabel labelA = item<HydraulicMapLabel>(8501);
            HydraulicMapLabel labelB = item<HydraulicMapLabel>(8502);
            labelA.text = QStringLiteral("uuidv7 left append");
            labelB.text = QStringLiteral("uuidv7 right append");
            labelA.anchor_node_uuid = baseline.nodes_junctions[0].uuid;
            labelB.anchor_node_uuid = baseline.nodes_junctions[0].uuid;
            const native::RevisionId leftHead = hydraulic::editHydraulicNetwork(
                repository, project, "append-left", baseline.uuid,
                {{labelA.uuid, hydraulic::HydraulicEntity{labelA}}}, base, commitMetadata());
            const native::RevisionId rightHead = hydraulic::editHydraulicNetwork(
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
            NetworkHydraulic combined = baseline;
            combined.map_labels.append(labelA);
            combined.map_labels.append(labelB);
            const std::optional<NetworkHydraulic> mergedAppend =
                hydraulic::loadHydraulicNetworkAtRevision(
                    repository, project, mergedAppendHead, baseline.uuid);
            require(mergedAppend.has_value(), "UUIDv7 appended network is missing");
            compareNetworks(combined, *mergedAppend);
            const std::optional<NetworkHydraulic> immutableBase =
                hydraulic::loadHydraulicNetworkAtRevision(repository, project, base, baseline.uuid);
            require(immutableBase.has_value(), "UUIDv7 append mutated the pinned base");
            compareNetworks(baseline, *immutableBase);

            require(repository.createBranch(project, "remove-left", "append-left") ==
                    mergedAppendHead &&
                    repository.createBranch(project, "remove-right", "append-left") ==
                    mergedAppendHead, "UUIDv7 delete branches did not fork from merged head");
            const native::RevisionId removedLeft = hydraulic::editHydraulicNetwork(
                repository, project, "remove-left", baseline.uuid,
                {{labelA.uuid, std::nullopt}}, mergedAppendHead, commitMetadata());
            const native::RevisionId removedRight = hydraulic::editHydraulicNetwork(
                repository, project, "remove-right", baseline.uuid,
                {{labelB.uuid, std::nullopt}}, mergedAppendHead, commitMetadata());
            const native::MergeResult removed = hydraulic::mergeHydraulicBranches(
                repository, project, "remove-left", "remove-right",
                hydraulic::HydraulicMergeSelections{removedLeft, removedRight, {}, {}}, commitMetadata());
            require(removed.status == native::MergeStatus::Merged && removed.resultHead &&
                    repository.head(project, "remove-right") == removedRight,
                    "UUIDv7 independent deletions did not reconcile");
            mergedDeleteHead = *removed.resultHead;
            const std::optional<NetworkHydraulic> mergedDelete =
                hydraulic::loadHydraulicNetworkAtRevision(
                    repository, project, mergedDeleteHead, baseline.uuid);
            require(mergedDelete.has_value(), "UUIDv7 deleted network is missing");
            compareNetworks(baseline, *mergedDelete);

            // Both new branches create the same deterministic first chunk ID,
            // but with distinct contents. The merge must reconcile membership.
            require(repository.createBranch(project, "empty-labels", "main") == base,
                    "UUIDv7 empty-label branch has an unexpected base");
            NetworkHydraulic withoutLabels = baseline;
            withoutLabels.map_labels.clear();
            const native::RevisionId emptyLabelsHead = hydraulic::saveHydraulicNetwork(
                repository, project, "empty-labels", withoutLabels, base, commitMetadata());
            require(repository.createBranch(project, "first-left", "empty-labels") ==
                    emptyLabelsHead &&
                    repository.createBranch(project, "first-right", "empty-labels") ==
                    emptyLabelsHead, "UUIDv7 initial-chunk branches have different bases");
            const native::RevisionId firstLeftHead = hydraulic::editHydraulicNetwork(
                repository, project, "first-left", baseline.uuid,
                {{labelA.uuid, hydraulic::HydraulicEntity{labelA}}}, emptyLabelsHead, commitMetadata());
            const native::RevisionId firstRightHead = hydraulic::editHydraulicNetwork(
                repository, project, "first-right", baseline.uuid,
                {{labelB.uuid, hydraulic::HydraulicEntity{labelB}}}, emptyLabelsHead, commitMetadata());
            const native::MergeResult firstChunk = hydraulic::mergeHydraulicBranches(
                repository, project, "first-left", "first-right",
                hydraulic::HydraulicMergeSelections{firstLeftHead, firstRightHead, {}, {}}, commitMetadata());
            require(firstChunk.status == native::MergeStatus::Merged && firstChunk.resultHead &&
                    repository.head(project, "first-right") == firstRightHead,
                    "UUIDv7 concurrent initial membership chunks did not reconcile");
            mergedInitialChunkHead = *firstChunk.resultHead;
            withoutLabels.map_labels.append(labelA);
            withoutLabels.map_labels.append(labelB);
            const std::optional<NetworkHydraulic> mergedInitialChunk =
                hydraulic::loadHydraulicNetworkAtRevision(
                    repository, project, mergedInitialChunkHead, baseline.uuid);
            require(mergedInitialChunk.has_value(), "UUIDv7 initial-chunk network missing");
            compareNetworks(withoutLabels, *mergedInitialChunk);

            // The conflicting branch changes list order rather than merely
            // appending; do not discard its change during append reconciliation.
            require(repository.createBranch(project, "order-left", "append-left") ==
                    mergedAppendHead &&
                    repository.createBranch(project, "order-right", "append-left") ==
                    mergedAppendHead, "UUIDv7 reorder branches have different bases");
            NetworkHydraulic reordered = combined;
            reordered.map_labels.swapItemsAt(0, 1);
            const native::RevisionId orderLeft = hydraulic::saveHydraulicNetwork(
                repository, project, "order-left", reordered, mergedAppendHead, commitMetadata());
            HydraulicMapLabel labelC = item<HydraulicMapLabel>(8503);
            labelC.anchor_node_uuid = baseline.nodes_junctions[1].uuid;
            const native::RevisionId orderRight = hydraulic::editHydraulicNetwork(
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
            const std::optional<NetworkHydraulic> removed =
                hydraulic::loadHydraulicNetworkAtRevision(
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
    const NetworkHydraulic baseline = completeNetwork();
    try {
        {
            native::Repository repository(folder);
            const native::RevisionId root = repository.createRootBranch(project, "main", commitMetadata());
            const native::RevisionId base = hydraulic::saveHydraulicNetwork(
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
            const native::RevisionId fieldLeft = hydraulic::editHydraulicNetwork(
                repository, project, "field-left", baseline.uuid,
                {{leftPipe.uuid, hydraulic::HydraulicEntity{leftPipe}}}, base, commitMetadata());
            const native::RevisionId fieldRight = hydraulic::editHydraulicNetwork(
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
            NetworkHydraulic fieldExpected = baseline;
            fieldExpected.links_pipes[0] = rightPipe;
            fieldExpected.links_pipes[0].diameter_mm = leftPipe.diameter_mm;
            const std::optional<NetworkHydraulic> fieldActual = hydraulic::loadHydraulicNetworkAtRevision(
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
            HydraulicMapLabel modifiedLabel = baseline.map_labels[0];
            modifiedLabel.text = QStringLiteral("edited label retained");
            const native::RevisionId deleted = hydraulic::editHydraulicNetwork(
                repository, project, "object-deleted", baseline.uuid,
                {{modifiedLabel.uuid, std::nullopt}}, base, commitMetadata());
            const native::RevisionId modified = hydraulic::editHydraulicNetwork(
                repository, project, "object-modified", baseline.uuid,
                {{modifiedLabel.uuid, hydraulic::HydraulicEntity{modifiedLabel}}}, base, commitMetadata());
            hydraulic::HydraulicMergeSelections deleteChoice{deleted, modified, {}, {}};
            deleteChoice.objects.push_back({modifiedLabel.uuid, hydraulic::HydraulicMergeSide::Target});
            hydraulic::HydraulicMergeSelections duplicateObject = deleteChoice;
            duplicateObject.objects.push_back(deleteChoice.objects[0]);
            reject("object-deleted", "object-modified", duplicateObject,
                   "duplicate UUIDv7 object choice was accepted");
            hydraulic::HydraulicMergeSelections overlapping = deleteChoice;
            overlapping.fields.push_back({modifiedLabel.uuid, QStringLiteral("text"),
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
            NetworkHydraulic withoutLabel = baseline;
            withoutLabel.map_labels.clear();
            const std::optional<NetworkHydraulic> deletion = hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, *deleteMerge.resultHead, baseline.uuid);
            require(deletion.has_value(), "UUIDv7 deleted label network missing");
            compareNetworks(withoutLabel, *deletion);

            fork("object-keep", base);
            fork("object-remove", base);
            const native::RevisionId kept = hydraulic::editHydraulicNetwork(
                repository, project, "object-keep", baseline.uuid,
                {{modifiedLabel.uuid, hydraulic::HydraulicEntity{modifiedLabel}}}, base, commitMetadata());
            const native::RevisionId removed = hydraulic::editHydraulicNetwork(
                repository, project, "object-remove", baseline.uuid,
                {{modifiedLabel.uuid, std::nullopt}}, base, commitMetadata());
            hydraulic::HydraulicMergeSelections keepChoice{kept, removed, {}, {}};
            keepChoice.objects.push_back({modifiedLabel.uuid, hydraulic::HydraulicMergeSide::Target});
            const native::MergeResult keepMerge = hydraulic::mergeHydraulicBranches(
                repository, project, "object-keep", "object-remove", keepChoice, commitMetadata());
            require(keepMerge.status == native::MergeStatus::Merged && keepMerge.resultHead &&
                    repository.head(project, "object-remove") == removed,
                    "UUIDv7 surviving-side object selection failed");
            NetworkHydraulic withLabel = baseline;
            withLabel.map_labels[0] = modifiedLabel;
            const std::optional<NetworkHydraulic> surviving = hydraulic::loadHydraulicNetworkAtRevision(
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
            const std::optional<NetworkHydraulic> deletedNetwork =
                hydraulic::loadHydraulicNetworkAtRevision(reopened, project, deletedHead, baseline.uuid);
            const std::optional<NetworkHydraulic> keptNetwork =
                hydraulic::loadHydraulicNetworkAtRevision(reopened, project, keptHead, baseline.uuid);
            require(deletedNetwork.has_value() && keptNetwork.has_value(),
                    "UUIDv7 chosen merge result disappeared after compaction");
            NetworkHydraulic withoutLabel = baseline;
            withoutLabel.map_labels.clear();
            compareNetworks(withoutLabel, *deletedNetwork);
            NetworkHydraulic withLabel = baseline;
            withLabel.map_labels[0].text = QStringLiteral("edited label retained");
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
        const NetworkHydraulic network = completeNetwork();
        const native::RevisionId raw = repository.createRootBranch(project, "main", commitMetadata());
        const native::RevisionId initial = hydraulic::saveHydraulicNetwork(
            repository, project, "main", network, raw, commitMetadata());
        require(initial != raw && repository.head(project, "main") == initial,
                "UUIDv7 first pinned network save did not commit");
        require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
            (void)hydraulic::saveHydraulicNetwork(repository, project, "main", network, raw, commitMetadata());
        }) && repository.head(project, "main") == initial,
                "UUIDv7 full-network save accepted stale HEAD");

        HydraulicNodeJunction junction = network.nodes_junctions[0];
        junction.elevation_m += 2.0;
        const std::vector<hydraulic::HydraulicEntityChange> changes{
            {junction.uuid, hydraulic::HydraulicEntity{junction}}};
        const native::RevisionId edited = hydraulic::editHydraulicNetwork(
            repository, project, "main", network.uuid, changes, initial, commitMetadata());
        require(edited != initial && repository.head(project, "main") == edited,
                "UUIDv7 caller-pinned indexed edit did not commit");
        require(throwsStorageError(native::StorageErrorKind::Conflict, [&] {
            (void)hydraulic::editHydraulicNetwork(repository, project, "main",
                network.uuid, changes, initial, commitMetadata());
        }) && throwsStorageError(native::StorageErrorKind::Conflict, [&] {
            (void)hydraulic::editHydraulicNetwork(repository, project, "main",
                network.uuid, {}, initial, commitMetadata());
        }) && repository.head(project, "main") == edited,
                "UUIDv7 stale edit or empty edit accepted an outdated HEAD");
        const std::uint64_t packBefore = repository.packSize();
        const native::RevisionId noop = hydraulic::editHydraulicNetwork(
            repository, project, "main", network.uuid, {}, edited, commitMetadata());
        require(noop == edited && repository.head(project, "main") == edited &&
                repository.packSize() == packBefore,
                "UUIDv7 pinned empty edit created a revision");
        NetworkHydraulic expected = network;
        expected.nodes_junctions[0] = junction;
        const std::optional<NetworkHydraulic> restored = hydraulic::loadHydraulicNetworkAtRevision(
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
        NetworkHydraulic baseline = completeNetwork();
        baseline.title_line_1 = QStringLiteral("2");
        baseline.nodes_junctions[0].metadata.description = QStringLiteral("2");
        baseline.map_labels[0].text = QStringLiteral("2");
        HydraulicMapLabel secondLabel = item<HydraulicMapLabel>(9102);
        secondLabel.text = QStringLiteral("2");
        secondLabel.anchor_node_uuid = baseline.nodes_junctions[1].uuid;
        baseline.map_labels.append(secondLabel);
        {
            native::Repository repository(folder);
            const native::RevisionId raw = repository.createRootBranch(project, "main", commitMetadata());
            const native::RevisionId base = hydraulic::saveHydraulicNetwork(
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
                        const std::optional<NetworkHydraulic> observed =
                            hydraulic::loadHydraulicNetworkAtRevision(
                                repository, project, pinned, baseline.uuid);
                        require(observed.has_value(), "UUIDv7 concurrent snapshot missing");
                        const QString marker = observed->title_line_1;
                        bool valid = false;
                        const int generation = marker.toInt(&valid);
                        require(valid && generation >= 2 && generation <= 24 &&
                                observed->nodes_junctions[0].metadata.description == marker &&
                                observed->map_labels[0].text == marker &&
                                observed->map_labels.size() == (generation % 2 == 0 ? 2 : 1),
                                "UUIDv7 pinned aggregate mixed generations");
                        const std::optional<QList<QUuid>> ids =
                            hydraulic::listHydraulicEntityIdsAtRevisionLimited(
                                repository, project, pinned, baseline.uuid,
                                hydraulic::HydraulicEntityType::MapLabel, 2);
                        const std::optional<hydraulic::HydraulicEntity> label =
                            hydraulic::loadHydraulicEntityAtRevision(
                                repository, project, pinned, baseline.uuid,
                                baseline.map_labels[0].uuid);
                        require(ids && ids->size() == observed->map_labels.size() &&
                                ids->front() == baseline.map_labels[0].uuid &&
                                label && std::holds_alternative<HydraulicMapLabel>(*label) &&
                                std::get<HydraulicMapLabel>(*label).text == marker,
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
                    NetworkHydraulic next = baseline;
                    begin.store(true);
                    for (int generation = 3; generation <= 24; ++generation) {
                        const QString marker = QString::number(generation);
                        next.title_line_1 = marker;
                        next.nodes_junctions[0].metadata.description = marker;
                        next.map_labels[0].text = marker;
                        if (generation % 2 != 0) next.map_labels.removeLast();
                        else if (next.map_labels.size() == 1) {
                            secondLabel.text = marker;
                            next.map_labels.append(secondLabel);
                        } else {
                            next.map_labels[1].text = marker;
                        }
                        expectedHead = hydraulic::saveHydraulicNetwork(
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
            const std::optional<NetworkHydraulic> original =
                hydraulic::loadHydraulicNetworkAtRevision(
                    repository, project, base, baseline.uuid);
            require(original.has_value(), "UUIDv7 original snapshot disappeared");
            compareNetworks(baseline, *original);
            (void)repository.verifyRepository();
            (void)repository.compact();
        }
        {
            native::Repository reopened(folder);
            const native::RevisionId latest = reopened.head(project, "main");
            const std::optional<NetworkHydraulic> last =
                hydraulic::loadHydraulicNetworkAtRevision(
                    reopened, project, latest, baseline.uuid);
            require(last && last->title_line_1 == QStringLiteral("24") &&
                    last->nodes_junctions[0].metadata.description == QStringLiteral("24") &&
                    last->map_labels.size() == 2 &&
                    last->map_labels[0].text == QStringLiteral("24"),
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
    NetworkHydraulic baseline;
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
        baseline.map_labels.append(item<HydraulicMapLabel>(803000 + index));
    require(aowis::dvmdb::validateHydraulicNetwork(baseline).valid(),
            "UUIDv7 chunk fixture failed MODEL validation");
    native::RevisionId saved = native::RevisionId::generate();
    native::RevisionId finalHead = native::RevisionId::generate();
    NetworkHydraulic expected = baseline;
    try {
        {
            native::Repository repository(folder);
            const native::RevisionId genesis = repository.createRootBranch(project, "main", commitMetadata());
            saved = hydraulic::saveHydraulicNetwork(repository, project, "main", baseline, genesis, commitMetadata());
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
            finalHead = hydraulic::editHydraulicNetwork(
                repository, project, "feature", baseline.uuid,
                {{addedNode.uuid, hydraulic::HydraulicEntity{addedNode}},
                 {addedPipe.uuid, hydraulic::HydraulicEntity{addedPipe}}}, saved, commitMetadata(), &stats);
            require(stats.indexedMembershipFastPath, "UUIDv7 bounded member insertion missed indexed path");
            expected.nodes_junctions.append(addedNode);
            expected.links_pipes.append(addedPipe);
            for (int index = 0; index < 7; ++index) {
                finalHead = hydraulic::editHydraulicNetwork(
                    repository, project, "feature", baseline.uuid,
                    {{baseline.map_labels[index].uuid, std::nullopt}}, finalHead, commitMetadata(), &stats);
                require(stats.indexedMembershipFastPath, "UUIDv7 chunk label deletion missed indexed path");
                expected.map_labels.removeFirst();
            }
            std::vector<hydraulic::HydraulicEntityChange> finalChunkRemoval;
            for (int index = 256; index < 260; ++index)
                finalChunkRemoval.push_back({baseline.map_labels[index].uuid, std::nullopt});
            finalHead = hydraulic::editHydraulicNetwork(
                repository, project, "feature", baseline.uuid, finalChunkRemoval, finalHead, commitMetadata(), &stats);
            require(stats.indexedMembershipFastPath, "UUIDv7 empty final chunk missed indexed retirement");
            for (int index = 0; index < 4; ++index) expected.map_labels.removeLast();
            const std::optional<native::ModelRecord> root = repository.getModelAtRevision(
                project, finalHead, hydraulic::keyFromUuid(baseline.uuid));
            require(root && root->references.size() == 7,
                    "UUIDv7 empty chunk descriptor was not retired");
            compareNetworks(expected, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, finalHead, baseline.uuid));
            compareNetworks(baseline, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, saved, baseline.uuid));
            const std::optional<QList<QUuid>> labels = hydraulic::listHydraulicEntityIdsAtRevisionLimited(
                repository, project, finalHead, baseline.uuid, hydraulic::HydraulicEntityType::MapLabel, 255);
            require(labels && labels->size() == expected.map_labels.size() &&
                    labels->first() == expected.map_labels.first().uuid,
                    "UUIDv7 chunked membership order was not preserved");
            require(throwsStorageError(native::StorageErrorKind::LimitExceeded, [&] {
                (void)hydraulic::listHydraulicEntityIdsAtRevisionLimited(
                    repository, project, saved, baseline.uuid, hydraulic::HydraulicEntityType::Junction, 256);
            }), "UUIDv7 bounded list accepted more than 256 members");
            HydraulicLinkPipe rewired = expected.links_pipes[0];
            rewired.node_uuid_to = expected.nodes_junctions[2].uuid;
            finalHead = hydraulic::editHydraulicNetwork(
                repository, project, "feature", baseline.uuid,
                {{rewired.uuid, hydraulic::HydraulicEntity{rewired}}}, finalHead, commitMetadata(), &stats);
            require(stats.indexedTopologyFastPath,
                    "UUIDv7 chunked owner lookup blocked independent indexed rewire");
            expected.links_pipes[0] = rewired;
            compareNetworks(expected, *hydraulic::loadHydraulicNetworkAtRevision(
                repository, project, finalHead, baseline.uuid));
            (void)repository.verifyRepository();
            (void)repository.compact();
        }
        {
            native::Repository reopened(folder);
            require(reopened.head(project, "main") == saved &&
                    reopened.head(project, "feature") == finalHead,
                    "UUIDv7 chunked branch HEAD changed after compaction/reopen");
            compareNetworks(baseline, *hydraulic::loadHydraulicNetworkAtRevision(
                reopened, project, saved, baseline.uuid));
            compareNetworks(expected, *hydraulic::loadHydraulicNetworkAtRevision(
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
        const NetworkHydraulic baseline = completeNetwork();
        const native::RevisionId base = hydraulic::saveHydraulicNetwork(
            repository, project, "main", baseline, genesis, commitMetadata());
        require(repository.createBranch(project, "target", "main") == base &&
                repository.createBranch(project, "source", "main") == base,
                "UUIDv7 fast-forward fixture branches did not share an immutable base");
        HydraulicNodeJunction updated = baseline.nodes_junctions[0];
        updated.elevation_m += 5.0;
        const native::RevisionId sourceTip = hydraulic::editHydraulicNetwork(
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
        NetworkHydraulic expected = baseline;
        expected.nodes_junctions[0] = updated;
        compareNetworks(expected, *hydraulic::loadHydraulicNetworkAtRevision(
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
        NetworkHydraulic incomplete;
        incomplete.uuid = uuid(4500);
        incomplete.id = QStringLiteral("Unfinished network - no water source");
        incomplete.nodes_junctions.append(item<HydraulicNodeJunction>(4501));
        incomplete.nodes_junctions.append(item<HydraulicNodeJunction>(4502));
        // There is deliberately no reservoir, tank, pipe or pump. The
        // independent solver has no supply, but all stored IDs are canonical.
        require(incomplete.nodes_reservoirs.isEmpty() && incomplete.nodes_tanks.isEmpty(),
                "INT-00 test accidentally supplied a hydraulic source");

        const std::vector<native::ModelChange> draftChanges =
            hydraulic::fullDraftHydraulicNetworkChanges(incomplete);
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
        const std::optional<NetworkHydraulic> draftNetwork =
            hydraulic::loadDraftHydraulicNetwork(reader, incomplete.uuid);
        require(draftNetwork.has_value(), "INT-00 incomplete draft was not readable");
        compareNetworks(incomplete, *draftNetwork);

        const native::ProjectId project = native::ProjectId::generate();
        native::RevisionId committed = native::RevisionId::generate();
        {
            native::Repository repository(folder);
            const native::RevisionId root =
                repository.createRootBranch(project, "main", commitMetadata());
            committed = repository.commitModel(project, "main", draftChanges,
                                               commitMetadata(), root);
            require(repository.createBranchAtRevision(project, "explore-fix", "main", committed) ==
                    committed, "INT-00 could not branch from incomplete network");
            require(repository.head(project, "main") == committed &&
                    repository.head(project, "explore-fix") == committed,
                    "INT-00 branch changed the source history");
            const std::optional<NetworkHydraulic> restored =
                hydraulic::loadHydraulicNetworkAtRevision(repository, project, committed, incomplete.uuid);
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
            const std::optional<NetworkHydraulic> restored =
                hydraulic::loadHydraulicNetworkAtRevision(reopened, project, committed, incomplete.uuid);
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
    const NetworkHydraulic before = completeNetwork();
    const std::vector<native::ModelChange> records =
        hydraulic::fullDraftHydraulicNetworkChanges(before);
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
    NetworkHydraulic changed = before;
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
    changed.multi_species.species[0].note = QStringLiteral("Updated MSX root note");
    aowis::model::ModelEntityEdit edit;
    edit.entityId = before.uuid;
    edit.entityType = QString::fromLatin1(aowis::model::hydraulicRootEditType);
    edit.before = aowis::model::hydraulicRootImage(before);
    edit.after = aowis::model::hydraulicRootImage(changed);
    const native::ModelChange materialized =
        hydraulic::materializeDraftRootEdit(reader, edit);
    require(materialized.record.has_value() &&
            materialized.record->kind == hydraulic::hydraulicNetworkKind &&
            materialized.record->references == reader(materialized.key)->references,
            "root transaction retains native membership chunk references");
    state[materialized.key] = *materialized.record;
    const std::optional<NetworkHydraulic> saved =
        hydraulic::loadDraftHydraulicNetwork(reader, before.uuid);
    require(saved.has_value() &&
            saved->options_hydraulic.headloss_formula == HydraulicHeadlossFormula::DarcyWeisbach &&
            saved->title_line_2 == changed.title_line_2 &&
            saved->id == changed.id &&
            saved->nodes_junctions.size() == before.nodes_junctions.size() &&
            saved->links_pipes.size() == before.links_pipes.size(),
            "root edit updates saved options without changing entity collections");
    require(saved.has_value() &&
            aowis::dvmdb::ModelCodec<NetworkHydraulic>::encode(*saved) ==
            aowis::dvmdb::ModelCodec<NetworkHydraulic>::encode(changed),
            "INT-05 metadata update preserves all 18 collections and every root byte");
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
            const native::RevisionId originalRevision = repository.commitModel(
                project, "main", records, commitMetadata(), emptyHead);
            updatedRevision = repository.commitModel(
                project, "main", {materialized}, commitMetadata(), originalRevision);
            const std::optional<NetworkHydraulic> committed =
                hydraulic::loadHydraulicNetworkAtRevision(
                    repository, project, updatedRevision, changed.uuid);
            require(committed.has_value(), "INT-05 committed metadata root not found");
            compareNetworks(changed, *committed);
            (void)repository.verifyRepository();
        }
        {
            native::Repository reopened(rootPath);
            require(reopened.head(project, "main") == updatedRevision,
                    "INT-05 committed metadata HEAD lost after restart");
            const std::optional<NetworkHydraulic> restored =
                hydraulic::loadHydraulicNetworkAtRevision(
                    reopened, project, updatedRevision, changed.uuid);
            require(restored.has_value(), "INT-05 complete network lost after restart");
            compareNetworks(changed, *restored);
            (void)reopened.verifyRepository();
            (void)reopened.compact();
            const std::optional<NetworkHydraulic> compacted =
                hydraulic::loadHydraulicNetworkAtRevision(
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
    edit.before = aowis::model::hydraulicRootImage(*saved);
    NetworkHydraulic forged = *saved;
    forged.uuid = uuid(999);
    edit.after = aowis::model::hydraulicRootImage(forged);
    bool identityRejected = false;
    try { (void)hydraulic::materializeDraftRootEdit(reader, edit); }
    catch (const std::exception&) { identityRejected = true; }
    require(identityRejected, "root UUID replacement rejected");
}

// HIST-03c2: prove image continuity before any draft cursor could use it.
// This coverage is intentionally about already-existing objects (no
// collection-position guessing), including all 18 canonical MODEL kinds.
void exerciseHistoricalScalarActionPreview() {
    const NetworkHydraulic original = completeNetwork();
    NetworkHydraulic edited = original;
    aowis::model::ModelEditTransaction first;
    first.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    first.operation = QStringLiteral("hydraulic.history.all-collections");
#define HISTORY_UPDATE_COLLECTION(name, type, kind) \
    { \
        const type before = original.name.first(); \
        type &after = edited.name.first(); \
        after.id = before.id + QStringLiteral("-history"); \
        first.edits.append(aowis::model::ModelEntityEdit{ \
            before.uuid, QStringLiteral(#type), \
            native::ModelCodec<type>::encode(before), \
            native::ModelCodec<type>::encode(after)}); \
    }
    HISTORY_UPDATE_COLLECTION(pipe_materials, HydraulicPipeMaterial, 8)
    HISTORY_UPDATE_COLLECTION(patterns_time, HydraulicPatternTime, 9)
    HISTORY_UPDATE_COLLECTION(curves_tank_volume, HydraulicCurveTankVolume, 10)
    HISTORY_UPDATE_COLLECTION(curves_pump_head, HydraulicCurvePumpHead, 11)
    HISTORY_UPDATE_COLLECTION(curves_pump_efficiency, HydraulicCurvePumpEfficiency, 12)
    HISTORY_UPDATE_COLLECTION(curves_valve_headloss, HydraulicCurveValveHeadloss, 13)
    HISTORY_UPDATE_COLLECTION(curves_valve_characteristic, HydraulicCurveValveCharacteristic, 14)
    HISTORY_UPDATE_COLLECTION(curves_generic, HydraulicCurveGeneric, 15)
    HISTORY_UPDATE_COLLECTION(nodes_reservoirs, HydraulicNodeReservoir, 2)
    HISTORY_UPDATE_COLLECTION(nodes_junctions, HydraulicNodeJunction, 1)
    HISTORY_UPDATE_COLLECTION(nodes_tanks, HydraulicNodeTank, 3)
    HISTORY_UPDATE_COLLECTION(demand_points, HydraulicDemandPoint, 7)
    HISTORY_UPDATE_COLLECTION(links_pipes, HydraulicLinkPipe, 4)
    HISTORY_UPDATE_COLLECTION(links_pumps, HydraulicLinkPump, 5)
    HISTORY_UPDATE_COLLECTION(links_valves, HydraulicLinkValve, 6)
    HISTORY_UPDATE_COLLECTION(controls_simple, HydraulicControlSimple, 16)
    HISTORY_UPDATE_COLLECTION(controls_rules, HydraulicControlRule, 17)
    HISTORY_UPDATE_COLLECTION(map_labels, HydraulicMapLabel, 18)
#undef HISTORY_UPDATE_COLLECTION
    require(first.edits.size() == 18, "HIST-03c2 must cover all 18 collections");
    const QByteArray firstBytes = aowis::model::ModelEditTransactionCodec::encode(first);
    require(!firstBytes.isEmpty(), "HIST-03c2 18-collection MODEL action not encoded");

    aowis::model::ModelEditTransaction second;
    second.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    second.operation = QStringLiteral("hydraulic.history.root-metadata");
    const NetworkHydraulic afterFirst = edited;
    edited.title_line_2 = QStringLiteral("Historical root after-image");
    edited.nodes_junctions.first().id += QStringLiteral("-twice");
    second.edits.append(aowis::model::ModelEntityEdit{
        edited.uuid, QString::fromLatin1(aowis::model::hydraulicRootEditType),
        aowis::model::hydraulicRootImage(afterFirst),
        aowis::model::hydraulicRootImage(edited)});
    second.edits.append(aowis::model::ModelEntityEdit{
        edited.nodes_junctions.first().uuid, QStringLiteral("HydraulicNodeJunction"),
        native::ModelCodec<HydraulicNodeJunction>::encode(
            afterFirst.nodes_junctions.first()),
        native::ModelCodec<HydraulicNodeJunction>::encode(
            edited.nodes_junctions.first())});
    const QByteArray secondBytes = aowis::model::ModelEditTransactionCodec::encode(second);
    require(!secondBytes.isEmpty(), "HIST-03c2 root MODEL action not encoded");
    const native::Bytes frameFirst = hydraulic::bytesFromQByteArray(firstBytes);
    const native::Bytes frameSecond = hydraulic::bytesFromQByteArray(secondBytes);
    const QByteArray baselineBytes = native::ModelCodec<NetworkHydraulic>::encode(original);
    const QByteArray intermediateBytes = native::ModelCodec<NetworkHydraulic>::encode(afterFirst);
    const QByteArray finalBytes = native::ModelCodec<NetworkHydraulic>::encode(edited);
    QString error;
    const std::optional<NetworkHydraulic> afterRootUndo =
        hydraulic::previewReverseHydraulicActions(edited, {frameSecond}, &error);
    require(afterRootUndo.has_value() && error.isEmpty() &&
            native::ModelCodec<NetworkHydraulic>::encode(*afterRootUndo) == intermediateBytes,
            "HIST-03c2 root undo did not recover exact intermediate bytes");
    const std::optional<NetworkHydraulic> beforeBoth =
        hydraulic::previewReverseHydraulicActions(edited, {frameSecond, frameFirst}, &error);
    require(beforeBoth.has_value() && error.isEmpty() &&
            native::ModelCodec<NetworkHydraulic>::encode(*beforeBoth) == baselineBytes &&
            native::ModelCodec<NetworkHydraulic>::encode(edited) == finalBytes,
            "HIST-03c2 complete reverse replay changed the source or lost bytes");
    require(!hydraulic::previewReverseHydraulicActions(edited, {frameFirst}, &error) &&
            error.contains(QStringLiteral("after-image")),
            "HIST-03c2 accepted out-of-order original events");
    NetworkHydraulic tampered = edited;
    tampered.nodes_junctions.first().id = QStringLiteral("stale");
    require(!hydraulic::previewReverseHydraulicActions(tampered,
            {frameSecond, frameFirst}, &error) && !error.isEmpty(),
            "HIST-03c2 accepted stale entity images");
    aowis::model::ModelEditTransaction creation;
    creation.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    creation.operation = QStringLiteral("hydraulic.junction.add");
    HydraulicNodeJunction newJunction = item<HydraulicNodeJunction>(551);
    creation.edits.append(aowis::model::ModelEntityEdit{
        newJunction.uuid, QStringLiteral("HydraulicNodeJunction"), std::nullopt,
        native::ModelCodec<HydraulicNodeJunction>::encode(newJunction)});
    const QByteArray creationBytes = aowis::model::ModelEditTransactionCodec::encode(creation);
    require(!creationBytes.isEmpty() &&
            !hydraulic::previewReverseHydraulicActions(edited,
                {hydraulic::bytesFromQByteArray(creationBytes)}, &error) &&
            error.contains(QStringLiteral("membership-order")),
            "HIST-03c2 guessed a missing original insertion position");
    native::Bytes invalid = frameFirst;
    invalid.back() ^= 0xff;
    require(!hydraulic::previewReverseHydraulicActions(edited, {invalid}, &error),
            "HIST-03c2 accepted corrupted archived MODEL bytes");
}

// HIST-03c2b: an immutable parent + *complete* forward action archive is
// sufficient to reconstruct ordered structural intermediate states. Native
// draft member insertion appends; a deletion preserves survivor order.
// An unjournaled change or a different historical ordering must fail closed.
void exerciseVerifiedHydraulicActionBoundary() {
    const NetworkHydraulic parent = completeNetwork();
    NetworkHydraulic current = parent;
    std::vector<native::Bytes> archived;
    std::vector<NetworkHydraulic> states{parent};
    const std::function<void(const aowis::model::ModelEditTransaction&)> archive =
        [&archived, &states, &current](const aowis::model::ModelEditTransaction& event) {
            const QByteArray bytes = aowis::model::ModelEditTransactionCodec::encode(event);
            require(!bytes.isEmpty(), "HIST-03c2b action did not encode");
            archived.push_back(hydraulic::bytesFromQByteArray(bytes));
            states.push_back(current);
        };

    // One logical action updates *all 18* existing collection types.
    aowis::model::ModelEditTransaction fields;
    fields.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    fields.operation = QStringLiteral("hydraulic.history.verified.all-collections");
#define HISTORY_VERIFY_UPDATE(name, type) \
    { \
        const type before = current.name.first(); \
        type& after = current.name.first(); \
        after.id += QStringLiteral("-verified"); \
        fields.edits.append(aowis::model::ModelEntityEdit{ \
            before.uuid, QStringLiteral(#type), native::ModelCodec<type>::encode(before), \
            native::ModelCodec<type>::encode(after)}); \
    }
    HISTORY_VERIFY_UPDATE(pipe_materials, HydraulicPipeMaterial)
    HISTORY_VERIFY_UPDATE(patterns_time, HydraulicPatternTime)
    HISTORY_VERIFY_UPDATE(curves_tank_volume, HydraulicCurveTankVolume)
    HISTORY_VERIFY_UPDATE(curves_pump_head, HydraulicCurvePumpHead)
    HISTORY_VERIFY_UPDATE(curves_pump_efficiency, HydraulicCurvePumpEfficiency)
    HISTORY_VERIFY_UPDATE(curves_valve_headloss, HydraulicCurveValveHeadloss)
    HISTORY_VERIFY_UPDATE(curves_valve_characteristic, HydraulicCurveValveCharacteristic)
    HISTORY_VERIFY_UPDATE(curves_generic, HydraulicCurveGeneric)
    HISTORY_VERIFY_UPDATE(nodes_reservoirs, HydraulicNodeReservoir)
    HISTORY_VERIFY_UPDATE(nodes_junctions, HydraulicNodeJunction)
    HISTORY_VERIFY_UPDATE(nodes_tanks, HydraulicNodeTank)
    HISTORY_VERIFY_UPDATE(demand_points, HydraulicDemandPoint)
    HISTORY_VERIFY_UPDATE(links_pipes, HydraulicLinkPipe)
    HISTORY_VERIFY_UPDATE(links_pumps, HydraulicLinkPump)
    HISTORY_VERIFY_UPDATE(links_valves, HydraulicLinkValve)
    HISTORY_VERIFY_UPDATE(controls_simple, HydraulicControlSimple)
    HISTORY_VERIFY_UPDATE(controls_rules, HydraulicControlRule)
    HISTORY_VERIFY_UPDATE(map_labels, HydraulicMapLabel)
#undef HISTORY_VERIFY_UPDATE
    require(fields.edits.size() == 18, "HIST-03c2b must update all collections");
    archive(fields);

    // One mixed root+creation event. The new member must be appended after
    // the original two junctions, including when chunks have been rebalanced.
    aowis::model::ModelEditTransaction mixed;
    mixed.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    mixed.operation = QStringLiteral("hydraulic.history.verified.mixed");
    const NetworkHydraulic beforeMixed = current;
    current.title_line_2 = QStringLiteral("HIST-03c2b archived mixed edit");
    const HydraulicNodeJunction firstNew = item<HydraulicNodeJunction>(551);
    current.nodes_junctions.append(firstNew);
    mixed.edits.append(aowis::model::ModelEntityEdit{
        current.uuid, QString::fromLatin1(aowis::model::hydraulicRootEditType),
        aowis::model::hydraulicRootImage(beforeMixed),
        aowis::model::hydraulicRootImage(current)});
    mixed.edits.append(aowis::model::ModelEntityEdit{
        firstNew.uuid, QStringLiteral("HydraulicNodeJunction"), std::nullopt,
        native::ModelCodec<HydraulicNodeJunction>::encode(firstNew)});
    archive(mixed);

    // Create one member and delete an earlier appended member in a single
    // action, retaining the two original junctions in their exact order.
    aowis::model::ModelEditTransaction swap;
    swap.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    swap.operation = QStringLiteral("hydraulic.history.verified.swap");
    const HydraulicNodeJunction secondNew = item<HydraulicNodeJunction>(552);
    swap.edits.append(aowis::model::ModelEntityEdit{
        secondNew.uuid, QStringLiteral("HydraulicNodeJunction"), std::nullopt,
        native::ModelCodec<HydraulicNodeJunction>::encode(secondNew)});
    swap.edits.append(aowis::model::ModelEntityEdit{
        firstNew.uuid, QStringLiteral("HydraulicNodeJunction"),
        native::ModelCodec<HydraulicNodeJunction>::encode(firstNew), std::nullopt});
    current.nodes_junctions.append(secondNew);
    current.nodes_junctions.removeAt(2);
    archive(swap);

    // Deleting an original ordered member and then introducing another new
    // member must reconstruct the parent order at earlier boundaries.
    aowis::model::ModelEditTransaction removal;
    removal.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    removal.operation = QStringLiteral("hydraulic.history.verified.remove");
    const HydraulicMapLabel oldLabel = current.map_labels.first();
    removal.edits.append(aowis::model::ModelEntityEdit{
        oldLabel.uuid, QStringLiteral("HydraulicMapLabel"),
        native::ModelCodec<HydraulicMapLabel>::encode(oldLabel), std::nullopt});
    current.map_labels.clear();
    archive(removal);

    aowis::model::ModelEditTransaction insertion;
    insertion.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    insertion.operation = QStringLiteral("hydraulic.history.verified.insert");
    HydraulicMapLabel newLabel = item<HydraulicMapLabel>(553);
    newLabel.anchor_node_uuid = uuid(20);
    newLabel.text = QStringLiteral("Historical label");
    insertion.edits.append(aowis::model::ModelEntityEdit{
        newLabel.uuid, QStringLiteral("HydraulicMapLabel"), std::nullopt,
        native::ModelCodec<HydraulicMapLabel>::encode(newLabel)});
    current.map_labels.append(newLabel);
    archive(insertion);

    const NetworkHydraulic committed = current;
    QString error;
    for (std::size_t boundary = 0; boundary <= archived.size(); ++boundary) {
        const std::optional<NetworkHydraulic> reconstructed =
            hydraulic::previewVerifiedHydraulicActionBoundary(
                parent, committed, archived, boundary, &error);
        require(reconstructed.has_value() && error.isEmpty() &&
                native::ModelCodec<NetworkHydraulic>::encode(*reconstructed) ==
                native::ModelCodec<NetworkHydraulic>::encode(states[boundary]) &&
                native::ModelCodec<NetworkHydraulic>::encode(parent) ==
                native::ModelCodec<NetworkHydraulic>::encode(states.front()),
                "HIST-03c2b failed byte-exact ordered intermediate reconstruction");
    }
    require(!hydraulic::previewVerifiedHydraulicActionBoundary(
                parent, committed, archived, archived.size() + 1, &error) &&
            error.contains(QStringLiteral("boundary")),
            "HIST-03c2b accepted an out-of-range historical position");

    // If an early action is missing, a later action may encounter a stale
    // before-image before the final checkpoint comparison. That is also a
    // correct fail-closed result, not evidence that coverage was accepted.
    std::vector<native::Bytes> missingPrefix = archived;
    missingPrefix.erase(missingPrefix.begin());
    require(!hydraulic::previewVerifiedHydraulicActionBoundary(
                parent, committed, missingPrefix, 0, &error) &&
            error.contains(QStringLiteral("before-image mismatch")),
            "HIST-03c2b did not reject an omitted earlier action");
    // Removing the *last* action leaves a valid prefix that can be replayed
    // without stale before-images. Only the final immutable snapshot witness
    // detects its missing edit; that coverage check is essential.
    std::vector<native::Bytes> incomplete = archived;
    incomplete.pop_back();
    require(!hydraulic::previewVerifiedHydraulicActionBoundary(
                parent, committed, incomplete, 0, &error) &&
            error.contains(QStringLiteral("does not cover")),
            "HIST-03c2b accepted incomplete archived action coverage");
    NetworkHydraulic unjournaled = committed;
    unjournaled.title_line_3 = QStringLiteral("Unjournaled native change");
    require(!hydraulic::previewVerifiedHydraulicActionBoundary(
                parent, unjournaled, archived, 1, &error) &&
            error.contains(QStringLiteral("does not cover")),
            "HIST-03c2b accepted unjournaled root metadata");

    NetworkHydraulic outOfOrder = committed;
    std::swap(outOfOrder.nodes_junctions[0], outOfOrder.nodes_junctions[1]);
    require(!hydraulic::previewVerifiedHydraulicActionBoundary(
                parent, outOfOrder, archived, 2, &error) &&
            error.contains(QStringLiteral("does not cover")),
            "HIST-03c2b guessed a different historical membership order");
    std::vector<native::Bytes> corrupted = archived;
    corrupted[2].back() ^= 0xff;
    require(!hydraulic::previewVerifiedHydraulicActionBoundary(
                parent, committed, corrupted, 0, &error) &&
            error.contains(QStringLiteral("Invalid archived MODEL")),
            "HIST-03c2b accepted a corrupt historical action frame");
    std::vector<native::Bytes> duplicated = archived;
    duplicated.push_back(archived.front());
    require(!hydraulic::previewVerifiedHydraulicActionBoundary(
                parent, committed, duplicated, 0, &error) && !error.isEmpty(),
            "HIST-03c2b accepted a duplicate historical action");
    aowis::model::ModelEditTransaction collision;
    collision.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    collision.operation = QStringLiteral("hydraulic.history.verified.uuid-collision");
    HydraulicMapLabel duplicate = item<HydraulicMapLabel>(19);
    duplicate.anchor_node_uuid = uuid(20);
    collision.edits.append(aowis::model::ModelEntityEdit{
        duplicate.uuid, QStringLiteral("HydraulicMapLabel"), std::nullopt,
        native::ModelCodec<HydraulicMapLabel>::encode(duplicate)});
    const QByteArray collisionBytes = aowis::model::ModelEditTransactionCodec::encode(collision);
    require(!collisionBytes.isEmpty() &&
            !hydraulic::previewVerifiedHydraulicActionBoundary(
                parent, parent, {hydraulic::bytesFromQByteArray(collisionBytes)}, 0, &error) &&
            error.contains(QStringLiteral("collides")),
            "HIST-03c2b accepted a member UUID already owned by another collection");

    // Use real immutable native revisions as the before/after witnesses.
    // This also proves collection ordering survives manifest chunk materialization.
    const std::filesystem::path folder = tempDir();
    try {
        const native::ProjectId project = native::ProjectId::generate();
        native::Repository repository(folder);
        const native::RevisionId unborn =
            repository.createRootBranch(project, "history-verified", commitMetadata());
        const native::RevisionId parentRevision = hydraulic::saveHydraulicNetwork(
            repository, project, "history-verified", parent, unborn, commitMetadata());
        const native::RevisionId childRevision = hydraulic::saveHydraulicNetwork(
            repository, project, "history-verified", committed, parentRevision, commitMetadata());
        const std::optional<NetworkHydraulic> storedParent =
            hydraulic::loadHydraulicNetworkAtRevision(repository, project, parentRevision, parent.uuid);
        const std::optional<NetworkHydraulic> storedChild =
            hydraulic::loadHydraulicNetworkAtRevision(repository, project, childRevision, parent.uuid);
        require(storedParent.has_value() && storedChild.has_value(),
                "HIST-03c2b could not load immutable checkpoint witnesses");
        const std::optional<NetworkHydraulic> fromRevisions =
            hydraulic::previewVerifiedHydraulicActionBoundary(
                *storedParent, *storedChild, archived, 3, &error);
        require(fromRevisions.has_value() && error.isEmpty() &&
                native::ModelCodec<NetworkHydraulic>::encode(*fromRevisions) ==
                native::ModelCodec<NetworkHydraulic>::encode(states[3]),
                "HIST-03c2b native revision witnesses disagree with archived action ordering");
        (void)repository.verifyRepository();
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);

    // A complete archive may span several HTTP pages. Verification must
    // only succeed with every frame, never with the first 100 alone.
    NetworkHydraulic longEnd = parent;
    std::vector<native::Bytes> longFrames;
    for (int index = 0; index < 126; ++index) {
        aowis::model::ModelEditTransaction action;
        action.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        action.operation = QStringLiteral("hydraulic.history.verified.root-step");
        const QByteArray before = aowis::model::hydraulicRootImage(longEnd);
        longEnd.title_line_2 = QStringLiteral("Action %1").arg(index);
        action.edits.append(aowis::model::ModelEntityEdit{
            longEnd.uuid, QString::fromLatin1(aowis::model::hydraulicRootEditType),
            before, aowis::model::hydraulicRootImage(longEnd)});
        const QByteArray bytes = aowis::model::ModelEditTransactionCodec::encode(action);
        require(!bytes.isEmpty(), "HIST-03c2b failed to encode paged action archive");
        longFrames.push_back(hydraulic::bytesFromQByteArray(bytes));
    }
    require(hydraulic::previewVerifiedHydraulicActionBoundary(
                parent, longEnd, longFrames, 126, &error).has_value() && error.isEmpty(),
            "HIST-03c2b failed archive spanning multiple read pages");
    std::vector<native::Bytes> firstPage(longFrames.begin(), longFrames.begin() + 100);
    require(!hydraulic::previewVerifiedHydraulicActionBoundary(
                parent, longEnd, firstPage, 100, &error) &&
            error.contains(QStringLiteral("does not cover")),
            "HIST-03c2b accepted a truncated first archive page");

    const NetworkHydraulic untouchedParent = parent;
    const NetworkHydraulic untouchedChild = committed;
    require(native::ModelCodec<NetworkHydraulic>::encode(parent) ==
                native::ModelCodec<NetworkHydraulic>::encode(untouchedParent) &&
            native::ModelCodec<NetworkHydraulic>::encode(committed) ==
                native::ModelCodec<NetworkHydraulic>::encode(untouchedChild),
            "HIST-03c2b mutated immutable caller checkpoints");
}

} // namespace

// HIST-03c3b: the indexed native witness alone is not enough. Exercise a
// real local Commit with a single MODEL action touching two independent
// hydraulic networks, including a native-owned root/chunk membership update.
// An unrelated native write must remain ineligible for history navigation.
void exerciseVerifiedRevisionHydraulicCoverage() {
    const std::filesystem::path folder = tempDir();
    try {
        std::unique_ptr<native::ProjectService> service =
            std::make_unique<native::ProjectService>(folder);
        service->configureRepositoryRole(native::RepositoryRole::Private);
        const std::string projectId = service->createUnbornProject(
            "HIST-03c3b multi-network action coverage").id;
        const std::string session = native::UuidV7::generate().toString();
        NetworkHydraulic a;
        a.uuid = uuid(930000);
        a.id = QStringLiteral("Network A");
        a.nodes_junctions.append(item<HydraulicNodeJunction>(930001));
        NetworkHydraulic b;
        b.uuid = uuid(940000);
        b.id = QStringLiteral("Network B");
        b.nodes_junctions.append(item<HydraulicNodeJunction>(940001));
        std::vector<native::ModelChange> initial =
            hydraulic::fullDraftHydraulicNetworkChanges(a);
        const std::vector<native::ModelChange> other =
            hydraulic::fullDraftHydraulicNetworkChanges(b);
        initial.insert(initial.end(), other.begin(), other.end());
        const native::RevisionId baseline = service->commitModelAtExpectedHead(
            projectId, "main", initial, commitMetadata(), std::nullopt);

        const std::string draft = service->createWorkingCopy(
            projectId, "main", baseline, session).id;
        const std::string token = service->acquireWorkingCopyLease(draft, session, 120).token;
        const HydraulicNodeJunction added = item<HydraulicNodeJunction>(930002);
        HydraulicNodeJunction changedB = b.nodes_junctions.first();
        changedB.elevation_m = 16;
        NetworkHydraulic changedRootB = b;
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
            b.uuid, QString::fromLatin1(aowis::model::hydraulicRootEditType),
            aowis::model::hydraulicRootImage(b),
            aowis::model::hydraulicRootImage(changedRootB)});
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
                hydraulic::keyFromUuid(a.uuid), hydraulic::keyFromUuid(added.uuid), 1, 9, true}},
            changes);
        changes.push_back(hydraulic::materializeDraftRootEdit(readBase,
            action.edits.last()));
        require(service->saveWorkingCopyTransactions(draft, session, token, 0,
                    changes, {hydraulic::bytesFromQByteArray(eventBytes)}).generation == 1,
                "HIST-03c3b could not Save a mixed two-network action");
        const native::LocalCommitInfo committed = service->commitWorkingCopy(
            native::UuidV7::generate().toString(), draft, session, token, 1,
            baseline, commitMetadata());
        require(committed.state == native::LocalCommitState::Succeeded,
                "HIST-03c3b local Commit did not succeed");
        const native::RevisionId revision =
            *native::RevisionId::parse(committed.reservedRevisionId);
        QString error;
        const std::optional<hydraulic::VerifiedHydraulicRevisionCoverage> verified =
            hydraulic::verifyHydraulicRevisionActionCoverage(
                *service, projectId, "main", revision, &error);
        require(verified && error.isEmpty() && verified->actionCount == 1 &&
                verified->changedNetworkIds.size() == 2 &&
                verified->changedNetworkIds[0] == a.uuid &&
                verified->changedNetworkIds[1] == b.uuid,
                "HIST-03c3b failed to attribute complete two-network native delta");
        const native::RevisionNativeActionWitness witness =
            service->verifiedRevisionNativeActionWitness(projectId, "main", revision);
        require(witness.hydraulicRootKeys.size() == 2 &&
                witness.actualNativeChanges.size() >= 4,
                "HIST-03c3b native witness omitted server-owned root/chunk changes");
        // HIST-03c4a: turn authenticated full-history action coverage into
        // COMPLETE draft overlays, both before and after the mixed action.
        // Two networks must move at the same global action boundary while
        // retaining canonical collection order and root metadata.
        const std::string navigationDraft = service->createWorkingCopy(
            projectId, "main", revision, session).id;
        const std::optional<hydraulic::VerifiedHydraulicActionNavigationPlan> beforeAction =
            hydraulic::planVerifiedHydraulicActionNavigation(
                *service, navigationDraft, revision, 0, 0, &error);
        const std::optional<hydraulic::VerifiedHydraulicActionNavigationPlan> afterAction =
            hydraulic::planVerifiedHydraulicActionNavigation(
                *service, navigationDraft, revision, 1, 0, &error);
        require(beforeAction && afterAction && error.isEmpty() &&
                beforeAction->expectedHead == revision &&
                afterAction->expectedHead == revision &&
                beforeAction->actionPosition == 0 && afterAction->actionPosition == 1 &&
                beforeAction->commitOperationId == witness.commitOperationId &&
                afterAction->candidateChanges.size() > 0,
                "HIST-03c4a could not plan both verified multi-network action boundaries");
        const std::function<NetworkHydraulic(
            const hydraulic::VerifiedHydraulicActionNavigationPlan&, const QUuid&)>
            plannedNetwork = [&service, &projectId, &revision](
                const hydraulic::VerifiedHydraulicActionNavigationPlan& plan,
                const QUuid& id) -> NetworkHydraulic {
                std::map<native::Key, std::optional<native::ModelRecord>> overlay;
                for (const native::ModelChange& change : plan.candidateChanges)
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
                const std::optional<NetworkHydraulic> network =
                    hydraulic::loadDraftHydraulicNetwork(read, id);
                require(network.has_value(), "HIST-03c4a candidate network cannot be reopened");
                return *network;
            };
        NetworkHydraulic expectedA = a;
        expectedA.nodes_junctions.append(added);
        NetworkHydraulic expectedB = changedRootB;
        expectedB.nodes_junctions.first() = changedB;
        require(native::ModelCodec<NetworkHydraulic>::encode(
                    plannedNetwork(*beforeAction, a.uuid)) ==
                    native::ModelCodec<NetworkHydraulic>::encode(a) &&
                native::ModelCodec<NetworkHydraulic>::encode(
                    plannedNetwork(*beforeAction, b.uuid)) ==
                    native::ModelCodec<NetworkHydraulic>::encode(b) &&
                native::ModelCodec<NetworkHydraulic>::encode(
                    plannedNetwork(*afterAction, a.uuid)) ==
                    native::ModelCodec<NetworkHydraulic>::encode(expectedA) &&
                native::ModelCodec<NetworkHydraulic>::encode(
                    plannedNetwork(*afterAction, b.uuid)) ==
                    native::ModelCodec<NetworkHydraulic>::encode(expectedB),
                "HIST-03c4a reconstructed an incorrect draft-relative action boundary");
        require(service->workingCopy(navigationDraft).generation == 0 &&
                !service->workingCopy(navigationDraft).dirty &&
                !service->workingCopy(navigationDraft).historyCheckpointRevisionId,
                "HIST-03c4a read-only planning modified the actual draft");
        require(!hydraulic::planVerifiedHydraulicActionNavigation(
                    *service, navigationDraft, revision, 2, 0, &error) &&
                !error.isEmpty() &&
                !hydraulic::planVerifiedHydraulicActionNavigation(
                    *service, navigationDraft, revision, 0, 1, &error) &&
                !error.isEmpty(),
                "HIST-03c4a accepted invalid action positions or stale draft generations");

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
        native::ModelRecord foreign;
        foreign.kind = 55;
        foreign.payload = {42};
        const native::ModelChange foreignChange{hydraulic::keyFromUuid(uuid(950000)), foreign};
        require(service->saveWorkingCopyTransactions(
                    unrelatedDraft, session, unrelatedToken, 0,
                    {hydraulic::changeFor(nextA), foreignChange},
                    {hydraulic::bytesFromQByteArray(nextFrame)}).generation == 1,
                "HIST-03c3b unjournaled native Save failed");
        const native::LocalCommitInfo unrelatedCommit = service->commitWorkingCopy(
            native::UuidV7::generate().toString(), unrelatedDraft, session,
            unrelatedToken, 1, revision, commitMetadata());
        require(unrelatedCommit.state == native::LocalCommitState::Succeeded,
                "HIST-03c3b unjournaled native Commit failed");
        const native::RevisionId later =
            *native::RevisionId::parse(unrelatedCommit.reservedRevisionId);
        // Planning cannot race a newly published branch HEAD, even if the
        // selected older action archive itself remains immutable and readable.
        require(!hydraulic::planVerifiedHydraulicActionNavigation(
                    *service, navigationDraft, revision, 0, 0, &error) &&
                error.contains(QStringLiteral("HEAD")),
                "HIST-03c4a did not reject a draft after its branch HEAD advanced");
        require(!hydraulic::verifyHydraulicRevisionActionCoverage(
                    *service, projectId, "main", later, &error) &&
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
            missingToken, 1, later, commitMetadata());
        require(missingCommit.state == native::LocalCommitState::Succeeded,
                "HIST-03c3b missing hydraulic action fixture Commit failed");
        require(!hydraulic::verifyHydraulicRevisionActionCoverage(
                    *service, projectId, "main",
                    *native::RevisionId::parse(missingCommit.reservedRevisionId), &error) &&
                error.contains(QStringLiteral("unjournaled hydraulic network")),
                "HIST-03c3b accepted a changed hydraulic network with no MODEL event");
        require(!hydraulic::planVerifiedHydraulicActionNavigation(
                    *service, missingDraft,
                    *native::RevisionId::parse(missingCommit.reservedRevisionId),
                    0, 2, &error) && !error.isEmpty(),
                "HIST-03c4a built a candidate from an incomplete MODEL archive");
        // ProjectService owns an exclusive root lock. Simulate a real server
        // restart by destroying the first service before opening the second.
        service.reset();
        {
            native::ProjectService reopened(folder);
            const std::optional<hydraulic::VerifiedHydraulicRevisionCoverage> recovered =
                hydraulic::verifyHydraulicRevisionActionCoverage(
                    reopened, projectId, "main", revision, &error);
            require(recovered && recovered->actionCount == 1 &&
                    recovered->changedNetworkIds.size() == 2,
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

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        exerciseHistoricalScalarActionPreview();
        exerciseVerifiedHydraulicActionBoundary();
        exerciseVerifiedRevisionHydraulicCoverage();
        exerciseAtomicRootMetadataEdit();
        exerciseInt00IncompleteNetworkRoundTrip();
        exerciseUuidV7Hydraulic();
        exerciseUuidV7IndexedReferences();
        exerciseUuidV7IndexedMembership();
        exerciseUuidV7HydraulicMerges();
        exerciseUuidV7CoordinatedMembershipMerges();
        exerciseUuidV7MergeChoiceQualification();
        exerciseUuidV7CallerPinnedWrites();
        exerciseUuidV7ConcurrentSnapshots();
        exerciseUuidV7ChunkBoundaries();
        exerciseUuidV7FastForward();
        std::cout << "Complete hydraulic network persistence tests passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Hydraulic network persistence test failed: " << failure.what() << '\n';
        return 1;
    }
}
