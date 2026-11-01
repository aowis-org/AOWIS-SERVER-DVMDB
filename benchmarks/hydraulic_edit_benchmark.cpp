#include <aowis/dvmdb/hydraulic_network_persistence.h>
#include <aowis/dvmdb/revision_record.h>

#include <QCoreApplication>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <fstream>
#include <functional>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace native = aowis::dvmdb;
namespace hydraulic = aowis::dvmdb::hydraulic;

namespace {
QUuid uuid(std::uint32_t value) {
    native::Key key{};
    key[6] = 0x70;
    key[8] = 0x80;
    for (int i = 0; i < 4; ++i) {
        key[15 - i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
    return hydraulic::uuidFromKey(key);
}

// Every benchmark revision records the same author and message.
native::CommitMetadata benchmarkMetadata() {
    return native::CommitMetadata{*native::UuidV7::parse("018f84c0-0000-7000-8000-0000000be001"),
                                  "DVMDB benchmark", "benchmark revision"};
}

std::uint64_t peakResidentKiB() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (!line.starts_with("VmHWM:")) continue;
        std::istringstream parts(line.substr(6));
        std::uint64_t peak = 0;
        std::string units;
        if (parts >> peak >> units && peak > 0 && units == "kB") return peak;
    }
    throw std::runtime_error("Linux VmHWM is unavailable for memory measurement");
}

double elapsedMs(const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3 || argc > 6) {
        std::cerr << "usage: aowis-dvmdb-hydraulic-edit-benchmark NEW_REPOSITORY PIPES [SCALAR_EDITS=20] [TOPOLOGY_EDITS=3]\n";
        return 2;
    }
    try {
        const std::filesystem::path directory(argv[1]);
        if (std::filesystem::exists(directory))
            throw std::runtime_error("benchmark repository already exists");
        const int pipeCount = std::stoi(argv[2]);
        const int scalarEdits = argc >= 4 ? std::stoi(argv[3]) : 20;
        const int topologyEdits = argc >= 5 ? std::stoi(argv[4]) : 3;
        if (pipeCount < 2 || pipeCount > 1000000 || scalarEdits < 1 || topologyEdits < 1)
            throw std::runtime_error("invalid benchmark counts");
        if (argc != 3 && argc != 4 && argc != 5)
            throw std::runtime_error("unexpected benchmark arguments");

        NetworkHydraulic network;
        network.uuid = uuid(1);
        network.id = QStringLiteral("benchmark");
        network.nodes_junctions.reserve(pipeCount + 1);
        network.links_pipes.reserve(pipeCount);
        for (int i = 0; i <= pipeCount; ++i) {
            HydraulicNodeJunction node;
            node.uuid = uuid(static_cast<std::uint32_t>(100 + i));
            network.nodes_junctions.append(std::move(node));
        }
        for (int i = 0; i < pipeCount; ++i) {
            HydraulicLinkPipe pipe;
            pipe.uuid = uuid(static_cast<std::uint32_t>(1000100 + i));
            pipe.node_uuid_from = network.nodes_junctions[i].uuid;
            pipe.node_uuid_to = network.nodes_junctions[i+1].uuid;
            network.links_pipes.append(std::move(pipe));
        }
        HydraulicPipeMaterial baseMaterial;
        baseMaterial.uuid = uuid(5000000);
        baseMaterial.id = QStringLiteral("benchmark-material");
        network.pipe_materials.append(baseMaterial);
        HydraulicNodeReservoir physicalReservoir;
        physicalReservoir.uuid = uuid(6000000);
        network.nodes_reservoirs.append(physicalReservoir);
        HydraulicNodeTank physicalTank;
        physicalTank.uuid = uuid(6000001);
        physicalTank.water_level_minimum_m = 1.0;
        physicalTank.water_level_initial_m = 3.0;
        physicalTank.water_level_maximum_m = 10.0;
        network.nodes_tanks.append(physicalTank);
        HydraulicLinkPump physicalPump;
        physicalPump.uuid = uuid(6000002);
        physicalPump.node_uuid_from = network.nodes_junctions[0].uuid;
        physicalPump.node_uuid_to = physicalReservoir.uuid;
        network.links_pumps.append(physicalPump);
        HydraulicLinkValve physicalValve;
        physicalValve.uuid = uuid(6000003);
        physicalValve.node_uuid_from = network.nodes_junctions[1].uuid;
        physicalValve.node_uuid_to = physicalTank.uuid;
        network.links_valves.append(physicalValve);
        native::Repository repository(directory);
        const native::ProjectId project = native::ProjectId::generate();
        const native::RevisionId genesis = repository.createRootBranch(project, "main", benchmarkMetadata());
        const std::chrono::steady_clock::time_point importStart = std::chrono::steady_clock::now();
        native::RevisionId mainHead = hydraulic::saveHydraulicNetwork(
            repository, project, "main", network, genesis, benchmarkMetadata());
        const double importMs = elapsedMs(importStart);
        // Keep the precise expected HEAD after each indexed write, rather
        // than reading a moving branch reference as part of each timed call.
        const std::function<native::RevisionId(
            const std::vector<hydraulic::HydraulicEntityChange>&,
            hydraulic::HydraulicEditDiagnostics*)> editMain =
            [&](const std::vector<hydraulic::HydraulicEntityChange>& changes,
                hydraulic::HydraulicEditDiagnostics* diagnostics) -> native::RevisionId {
                mainHead = hydraulic::editHydraulicNetwork(
                    repository, project, "main", network.uuid, changes, mainHead, benchmarkMetadata(), diagnostics);
                return mainHead;
            };
        const std::uint64_t importPackBytes = repository.packSize();
        const std::uint64_t importIndexBytes = std::filesystem::file_size(directory / "objects.idx");

        HydraulicLinkPipe edited = network.links_pipes[0];
        const std::chrono::steady_clock::time_point scalarStart = std::chrono::steady_clock::now();
        for (int i = 0; i < scalarEdits; ++i) {
            edited.diameter_mm += 1.0;
            hydraulic::HydraulicEditDiagnostics diagnostic;
            editMain(
                {{edited.uuid, hydraulic::HydraulicEntity{edited}}}, &diagnostic);
            if (!diagnostic.indexedScalarFastPath || diagnostic.validatedEntities != 1)
                throw std::runtime_error("expected bounded indexed validation");
        }
        const double scalarMs = elapsedMs(scalarStart);

        const std::chrono::steady_clock::time_point topologyStart = std::chrono::steady_clock::now();
        for (int i = 0; i < topologyEdits; ++i) {
            edited.node_uuid_from = network.nodes_junctions[1 + (i % 2)].uuid;
            hydraulic::HydraulicEditDiagnostics diagnostic;
            editMain(
                {{edited.uuid, hydraulic::HydraulicEntity{edited}}}, &diagnostic);
            if (!diagnostic.indexedTopologyFastPath || diagnostic.indexedScalarFastPath ||
                diagnostic.validatedEntities != 1)
                throw std::runtime_error("independent pipe rewire did not use indexed validation");
        }
        const double topologyMs = elapsedMs(topologyStart);
        // Later dependent fixtures must use the pipe's *current* endpoints,
        // not the original endpoints in the imported network snapshot.
        const std::optional<hydraulic::HydraulicEntity> storedEditedPipe =
            hydraulic::loadHydraulicEntityAtRevision(repository, project, mainHead,
                network.uuid, edited.uuid);
        if (!storedEditedPipe ||
            !std::holds_alternative<HydraulicLinkPipe>(*storedEditedPipe) ||
            std::get<HydraulicLinkPipe>(*storedEditedPipe).node_uuid_from != edited.node_uuid_from ||
            std::get<HydraulicLinkPipe>(*storedEditedPipe).node_uuid_to != edited.node_uuid_to)
            throw std::runtime_error("benchmark topology fixture disagrees with stored pipe endpoints");
        // A single membership batch must not decode all pre-existing pipes.
        // Its UUID manifest is still O(network members) and is deliberately
        // measured separately from the O(log N) payload-only edit paths.
        HydraulicNodeJunction newNode;
        newNode.uuid = uuid(4000010);
        HydraulicLinkPipe newPipe;
        newPipe.uuid = uuid(4000011);
        newPipe.node_uuid_from = network.nodes_junctions[0].uuid;
        newPipe.node_uuid_to = newNode.uuid;
        HydraulicMapLabel newLabel;
        newLabel.uuid = uuid(4000012);
        newLabel.anchor_node_uuid = newNode.uuid;
        const std::chrono::steady_clock::time_point insertStart = std::chrono::steady_clock::now();
        hydraulic::HydraulicEditDiagnostics membershipStats;
        editMain(
            {{newPipe.uuid, hydraulic::HydraulicEntity{newPipe}},
             {newNode.uuid, hydraulic::HydraulicEntity{newNode}},
             {newLabel.uuid, hydraulic::HydraulicEntity{newLabel}}}, &membershipStats);
        if (!membershipStats.indexedMembershipFastPath || membershipStats.validatedEntities != 3)
            throw std::runtime_error("expected indexed membership insertion");
        const double insertionMs = elapsedMs(insertStart);
        const std::chrono::steady_clock::time_point removalStart = std::chrono::steady_clock::now();
        editMain(
            {{newLabel.uuid, std::nullopt}}, &membershipStats);
        if (!membershipStats.indexedMembershipFastPath || membershipStats.validatedEntities != 1)
            throw std::runtime_error("expected indexed label removal");
        const double removalMs = elapsedMs(removalStart);
        const std::chrono::steady_clock::time_point deleteBatchStart = std::chrono::steady_clock::now();
        editMain(
            {{newNode.uuid, std::nullopt}, {newPipe.uuid, std::nullopt}}, &membershipStats);
        if (!membershipStats.indexedMembershipFastPath || membershipStats.validatedEntities != 2)
            throw std::runtime_error("expected indexed pipe/junction deletion batch");
        const double deletionBatchMs = elapsedMs(deleteBatchStart);
        HydraulicPipeMaterial updatedMaterial = baseMaterial;
        const std::chrono::steady_clock::time_point catalogUpdateStart = std::chrono::steady_clock::now();
        hydraulic::HydraulicEditDiagnostics catalogStats;
        for (int i = 0; i < scalarEdits; ++i) {
            updatedMaterial.id = QStringLiteral("benchmark-material-%1").arg(i);
            editMain(
                {{updatedMaterial.uuid, hydraulic::HydraulicEntity{updatedMaterial}}}, &catalogStats);
            if (!catalogStats.indexedCatalogueFastPath || catalogStats.validatedEntities != 1)
                throw std::runtime_error("expected indexed catalogue update");
        }
        const double catalogUpdateMs = elapsedMs(catalogUpdateStart);
        HydraulicPipeMaterial addedMaterial;
        addedMaterial.uuid = uuid(5000001);
        HydraulicPatternTime addedPattern;
        addedPattern.uuid = uuid(5000002);
        HydraulicCurveGeneric addedGeneric;
        addedGeneric.uuid = uuid(5000003);
        const std::chrono::steady_clock::time_point catalogInsertStart = std::chrono::steady_clock::now();
        editMain(
            {{addedMaterial.uuid, hydraulic::HydraulicEntity{addedMaterial}},
             {addedPattern.uuid, hydraulic::HydraulicEntity{addedPattern}},
             {addedGeneric.uuid, hydraulic::HydraulicEntity{addedGeneric}}}, &catalogStats);
        if (!catalogStats.indexedMembershipFastPath || catalogStats.validatedEntities != 3)
            throw std::runtime_error("expected indexed catalogue insertion");
        const double catalogInsertMs = elapsedMs(catalogInsertStart);
        const std::chrono::steady_clock::time_point catalogDeleteStart = std::chrono::steady_clock::now();
        editMain(
            {{addedPattern.uuid, std::nullopt}, {addedMaterial.uuid, std::nullopt},
             {addedGeneric.uuid, std::nullopt}}, &catalogStats);
        if (!catalogStats.indexedMembershipFastPath || catalogStats.validatedEntities != 3)
            throw std::runtime_error("expected indexed catalogue deletion");
        const double catalogDeleteMs = elapsedMs(catalogDeleteStart);
        const std::chrono::steady_clock::time_point physicalStart = std::chrono::steady_clock::now();
        hydraulic::HydraulicEditDiagnostics physicalStats;
        for (int i = 0; i < scalarEdits; ++i) {
            physicalReservoir.hydraulic_head_m += 1.0;
            // Always remain within the tank's declared physical limits, even
            // for long edit sequences. Each iteration still changes the level.
            physicalTank.water_level_initial_m =
                3.0 + 0.25 * static_cast<double>((i % 24) + 1);
            physicalPump.initial_speed_ratio += 0.01;
            physicalValve.setting_pressure_head_m += 0.25;
            editMain(
                {{physicalReservoir.uuid, hydraulic::HydraulicEntity{physicalReservoir}},
                 {physicalTank.uuid, hydraulic::HydraulicEntity{physicalTank}},
                 {physicalPump.uuid, hydraulic::HydraulicEntity{physicalPump}},
                 {physicalValve.uuid, hydraulic::HydraulicEntity{physicalValve}}}, &physicalStats);
            if (!physicalStats.indexedPhysicalFastPath || physicalStats.validatedEntities != 4)
                throw std::runtime_error("expected indexed reservoir/tank/pump/valve update");
        }
        const double physicalMs = elapsedMs(physicalStart);
        // Structural batch with references to newly created nodes. Insertion
        // order deliberately puts the link objects first.
        HydraulicNodeReservoir addedReservoir;
        addedReservoir.uuid = uuid(7000000);
        HydraulicNodeTank addedTank;
        addedTank.uuid = uuid(7000001);
        HydraulicLinkPump addedPump;
        addedPump.uuid = uuid(7000002);
        addedPump.node_uuid_from = addedReservoir.uuid;
        addedPump.node_uuid_to = addedTank.uuid;
        HydraulicLinkValve addedValve;
        addedValve.uuid = uuid(7000003);
        addedValve.node_uuid_from = addedTank.uuid;
        addedValve.node_uuid_to = network.nodes_junctions[0].uuid;
        const std::chrono::steady_clock::time_point physicalInsertStart = std::chrono::steady_clock::now();
        editMain(
            {{addedPump.uuid, hydraulic::HydraulicEntity{addedPump}},
             {addedValve.uuid, hydraulic::HydraulicEntity{addedValve}},
             {addedTank.uuid, hydraulic::HydraulicEntity{addedTank}},
             {addedReservoir.uuid, hydraulic::HydraulicEntity{addedReservoir}}}, &physicalStats);
        const double physicalInsertMs = elapsedMs(physicalInsertStart);
        if (!physicalStats.indexedMembershipFastPath || physicalStats.validatedEntities != 4)
            throw std::runtime_error("expected indexed structural insertion");
        const std::chrono::steady_clock::time_point physicalDeleteStart = std::chrono::steady_clock::now();
        editMain(
            {{addedPump.uuid, std::nullopt}, {addedValve.uuid, std::nullopt},
             {addedTank.uuid, std::nullopt}, {addedReservoir.uuid, std::nullopt}}, &physicalStats);
        const double physicalDeleteMs = elapsedMs(physicalDeleteStart);
        if (!physicalStats.indexedMembershipFastPath || physicalStats.validatedEntities != 4)
            throw std::runtime_error("expected indexed structural deletion");

        HydraulicDemandPoint addedDemandPoint;
        addedDemandPoint.uuid = uuid(7100000);
        addedDemandPoint.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
        addedDemandPoint.attachment.pipe_uuid = network.links_pipes[0].uuid;
        addedDemandPoint.attachment.pipe_position = 0.35;
        addedDemandPoint.attachment.pipe_allocation_mode =
            HydraulicDemandPointPipeAllocationMode::AssignedJunction;
        addedDemandPoint.attachment.pipe_assigned_junction_uuid = edited.node_uuid_from;
        HydraulicControlSimple addedSimpleControl;
        addedSimpleControl.uuid = uuid(7100001);
        addedSimpleControl.link_uuid = network.links_pipes[0].uuid;
        addedSimpleControl.trigger_node_uuid = network.nodes_junctions[1].uuid;
        HydraulicControlRule addedRuleControl;
        addedRuleControl.uuid = uuid(7100002);
        HydraulicControlRulePremise rulePremise;
        rulePremise.object = HydraulicControlRuleObject::Link;
        rulePremise.object_uuid = network.links_pipes[0].uuid;
        addedRuleControl.premises.append(rulePremise);
        HydraulicControlRuleAction ruleAction;
        ruleAction.link_uuid = physicalPump.uuid;
        addedRuleControl.actions_then.append(ruleAction);
        hydraulic::HydraulicEditDiagnostics dependencyStats;
        const std::chrono::steady_clock::time_point dependentInsertStart =
            std::chrono::steady_clock::now();
        editMain(
            {{addedDemandPoint.uuid, hydraulic::HydraulicEntity{addedDemandPoint}},
             {addedSimpleControl.uuid, hydraulic::HydraulicEntity{addedSimpleControl}},
             {addedRuleControl.uuid, hydraulic::HydraulicEntity{addedRuleControl}}}, &dependencyStats);
        const double dependentInsertMs = elapsedMs(dependentInsertStart);
        if (!dependencyStats.indexedMembershipFastPath || dependencyStats.validatedEntities != 3)
            throw std::runtime_error("expected indexed demand/control insertion");
        // Three *existing* dependent entities switch reference targets in one
        // revision. The membership root must stay unchanged.
        addedDemandPoint.attachment.pipe_position = 0.70;
        addedDemandPoint.attachment.pipe_assigned_junction_uuid = edited.node_uuid_to;
        addedSimpleControl.link_uuid = physicalValve.uuid;
        addedSimpleControl.trigger_node_uuid = physicalTank.uuid;
        addedRuleControl.premises[0].object = HydraulicControlRuleObject::Node;
        addedRuleControl.premises[0].object_uuid = physicalReservoir.uuid;
        addedRuleControl.actions_then[0].link_uuid = physicalValve.uuid;
        const std::chrono::steady_clock::time_point dependentUpdateStart =
            std::chrono::steady_clock::now();
        editMain(
            {{addedDemandPoint.uuid, hydraulic::HydraulicEntity{addedDemandPoint}},
             {addedSimpleControl.uuid, hydraulic::HydraulicEntity{addedSimpleControl}},
             {addedRuleControl.uuid, hydraulic::HydraulicEntity{addedRuleControl}}}, &dependencyStats);
        const double dependentUpdateMs = elapsedMs(dependentUpdateStart);
        if (!dependencyStats.indexedDependentUpdateFastPath || dependencyStats.validatedEntities != 3)
            throw std::runtime_error("expected indexed existing demand/control update");
        const std::chrono::steady_clock::time_point dependentDeleteStart =
            std::chrono::steady_clock::now();
        editMain(
            {{addedDemandPoint.uuid, std::nullopt},
             {addedSimpleControl.uuid, std::nullopt},
             {addedRuleControl.uuid, std::nullopt}}, &dependencyStats);
        const double dependentDeleteMs = elapsedMs(dependentDeleteStart);
        if (!dependencyStats.indexedMembershipFastPath || dependencyStats.validatedEntities != 3)
            throw std::runtime_error("expected indexed demand/control deletion");
        // Representative model reads: a bounded, indexed entity lookup and
        // a complete reconstruction are separate operations with different
        // complexity. Never mix either cost into the edit-path timings.
        constexpr int readSamples = 100;
        const std::chrono::steady_clock::time_point targetedStart =
            std::chrono::steady_clock::now();
        for (int sample = 0; sample < readSamples; ++sample) {
            const std::optional<hydraulic::HydraulicEntity> entity =
                hydraulic::loadHydraulicEntityAtRevision(repository, project, mainHead,
                    network.uuid, network.links_pipes[sample % pipeCount].uuid);
            if (!entity || !std::holds_alternative<HydraulicLinkPipe>(*entity))
                throw std::runtime_error("targeted canonical pipe read failed");
        }
        const double targetedMs = elapsedMs(targetedStart);
        const std::chrono::steady_clock::time_point fullReadStart =
            std::chrono::steady_clock::now();
        const std::optional<NetworkHydraulic> reconstructed =
            hydraulic::loadHydraulicNetworkAtRevision(repository, project, mainHead, network.uuid);
        const double fullReadMs = elapsedMs(fullReadStart);
        if (!reconstructed || reconstructed->links_pipes.size() != pipeCount ||
            reconstructed->nodes_junctions.size() != pipeCount + 1)
            throw std::runtime_error("full canonical network reconstruction failed");

        // Two branches change DIFFERENT fields of the same existing pipe.
        // Validate the real hydraulic three-way merger, not the generic
        // object-level conflict resolver. The immutable baseline must survive.
        const native::RevisionId baselineId = mainHead;
        if (repository.createBranch(project, "baseline", "main") != baselineId ||
            repository.createBranch(project, "feature", "main") != baselineId)
            throw std::runtime_error("UUIDv7 benchmark branch fork changed base revision");
        native::RevisionId featureHead = baselineId;
        HydraulicLinkPipe targetPipe = edited;
        HydraulicLinkPipe featurePipe = edited;
        targetPipe.diameter_mm += 2.0;
        featurePipe.length_calculated_m += 10.0;
        editMain(
            {{targetPipe.uuid, hydraulic::HydraulicEntity{targetPipe}}}, nullptr);
        featureHead = hydraulic::editHydraulicNetwork(repository, project, "feature", network.uuid,
            {{featurePipe.uuid, hydraulic::HydraulicEntity{featurePipe}}}, featureHead, benchmarkMetadata());
        const std::chrono::steady_clock::time_point diffStart =
            std::chrono::steady_clock::now();
        const std::vector<native::Difference> differences =
            repository.diffRevisions(project, mainHead, featureHead, 1000000);
        const double diffMs = elapsedMs(diffStart);
        if (differences.empty())
            throw std::runtime_error("divergent hydraulic branches unexpectedly have no diff");
        const std::chrono::steady_clock::time_point mergeStart =
            std::chrono::steady_clock::now();
        const native::MergeResult merged = hydraulic::mergeHydraulicBranches(
            repository, project, "main", "feature",
            hydraulic::HydraulicMergeSelections{mainHead, featureHead, {}, {}}, benchmarkMetadata());
        const double mergeMs = elapsedMs(mergeStart);
        if (merged.status != native::MergeStatus::Merged || !merged.resultHead ||
            repository.head(project, "feature") != featureHead)
            throw std::runtime_error("expected an immutable UUIDv7 hydraulic merge");
        mainHead = *merged.resultHead;
        const std::optional<native::Bytes> mergeBytes = repository.readRevision(project, mainHead);
        if (!mergeBytes)
            throw std::runtime_error("missing committed UUIDv7 merge revision");
        const native::RevisionRecord mergeRecord = native::decodeRevisionRecord(*mergeBytes);
        if (mergeRecord.parents.size() != 2 || !merged.targetBefore || !merged.sourceHead ||
            mergeRecord.parents[0].id != *merged.targetBefore ||
            mergeRecord.parents[1].id != *merged.sourceHead)
            throw std::runtime_error("UUIDv7 merge lost its two immutable parents");
        const std::optional<hydraulic::HydraulicEntity> mergedPipe =
            hydraulic::loadHydraulicEntityAtRevision(repository, project, mainHead,
                network.uuid, edited.uuid);
        if (!mergedPipe || !std::holds_alternative<HydraulicLinkPipe>(*mergedPipe))
            throw std::runtime_error("merged canonical pipe is missing");
        const HydraulicLinkPipe& finalPipe = std::get<HydraulicLinkPipe>(*mergedPipe);
        if (finalPipe.diameter_mm != targetPipe.diameter_mm ||
            finalPipe.length_calculated_m != featurePipe.length_calculated_m)
            throw std::runtime_error("hydraulic merge lost a nonconflicting field");
        const std::optional<hydraulic::HydraulicEntity> historicalPipe =
            hydraulic::loadHydraulicEntityAtRevision(
                repository, project, baselineId, network.uuid, edited.uuid);
        if (!historicalPipe || !std::holds_alternative<HydraulicLinkPipe>(*historicalPipe) ||
            std::get<HydraulicLinkPipe>(*historicalPipe).diameter_mm != edited.diameter_mm ||
            std::get<HydraulicLinkPipe>(*historicalPipe).length_calculated_m !=
                edited.length_calculated_m)
            throw std::runtime_error("merged revision mutated the historical model");
        const std::uint64_t finalPackBytes = repository.packSize();
        const std::uint64_t finalIndexBytes =
            std::filesystem::file_size(directory / "objects.idx");
        const std::uint64_t finalObjectCount = repository.objectCount();
        if (finalPackBytes < importPackBytes || finalIndexBytes < importIndexBytes)
            throw std::runtime_error("append-only pack or index unexpectedly shrank");
        repository.verifyPack();
        const std::optional<native::ModelRecord> networkRoot = repository.getModelAtRevision(
            project, mainHead, hydraulic::keyFromUuid(network.uuid));
        if (!networkRoot || networkRoot->kind != hydraulic::hydraulicNetworkKind)
            throw std::runtime_error("missing indexed network root");
        const std::uint64_t peakRssKiB = peakResidentKiB();
        std::cout << std::fixed << std::setprecision(3)
                  << "engine=" << ("pack") << '\n'
                  << "fixture=canonical-hydraulic-network\n"
                  << "pipes=" << pipeCount << '\n'
                  << "scalar_edits=" << scalarEdits << '\n'
                  << "topology_edits=" << topologyEdits << '\n'
                  << "import_ms=" << importMs << '\n'
                  << "indexed_scalar_mean_ms=" << scalarMs / scalarEdits << '\n'
                  << "indexed_topology_mean_ms=" << topologyMs / topologyEdits << '\n'
                  << "indexed_membership_insert_batch_ms=" << insertionMs << '\n'
                  << "indexed_membership_delete_label_ms=" << removalMs << '\n'
                  << "indexed_membership_delete_pipe_junction_batch_ms=" << deletionBatchMs << '\n'
                  << "indexed_catalogue_update_mean_ms=" << catalogUpdateMs / scalarEdits << '\n'
                  << "indexed_catalogue_insert_batch_ms=" << catalogInsertMs << '\n'
                  << "indexed_catalogue_delete_batch_ms=" << catalogDeleteMs << '\n'
                  << "indexed_physical_update_batch_mean_ms=" << physicalMs / scalarEdits << '\n'
                  << "indexed_structural_insert_batch_ms=" << physicalInsertMs << '\n'
                  << "indexed_structural_delete_batch_ms=" << physicalDeleteMs << '\n'
                  << "indexed_dependent_insert_batch_ms=" << dependentInsertMs << '\n'
                  << "indexed_dependent_update_batch_ms=" << dependentUpdateMs << '\n'
                  << "indexed_dependent_delete_batch_ms=" << dependentDeleteMs << '\n'
                  << "network_membership_chunk_count=" << networkRoot->references.size() << '\n'
                  << "targeted_read_samples=" << readSamples << '\n'
                  << "targeted_read_mean_ms=" << targetedMs / readSamples << '\n'
                  << "full_network_read_ms=" << fullReadMs << '\n'
                  << "divergent_diff_ms=" << diffMs << '\n'
                  << "divergent_diff_entities=" << differences.size() << '\n'
                  << "canonical_merge_ms=" << mergeMs << '\n'
                  << "merge_parents=" << mergeRecord.parents.size() << '\n'
                  << "pack_bytes_after_import=" << importPackBytes << '\n'
                  << "pack_bytes_final=" << finalPackBytes << '\n'
                  << "pack_growth_bytes=" << finalPackBytes - importPackBytes << '\n'
                  << "index_bytes_after_import=" << importIndexBytes << '\n'
                  << "index_bytes_final=" << finalIndexBytes << '\n'
                  << "index_growth_bytes=" << finalIndexBytes - importIndexBytes << '\n'
                  << "physical_objects_final=" << finalObjectCount << '\n'
                  << "peak_rss_kib=" << peakRssKiB << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hydraulic edit benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
