#include "aowis/dvmdb/model_codec_adapter.h"
#include "aowis/dvmdb/hydraulic_distribution_network_persistence.h"
#include "commit_metadata_fixture.h"
#include "project_repository.h"

#include <QCoreApplication>
#include <QDate>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
using aowis::dvmdb::Bytes;
using aowis::dvmdb::Key;
using aowis::dvmdb::ModelChange;
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::test::ProjectRepository;
namespace hydraulic = aowis::dvmdb::hydraulic;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

QUuid fixedUuid(std::uint8_t kind, std::uint64_t ordinal) {
    Key key{};
    key[0] = kind;
    key[6] = 0x70;
    key[8] = 0x80;
    for (unsigned i = 0; i < 6; ++i) {
        key[15 - i] = static_cast<std::uint8_t>(ordinal >> (8U * i));
    }
    return hydraulic::uuidFromKey(key);
}

std::filesystem::path uniqueDirectory() {
    std::string pattern = (std::filesystem::temp_directory_path() / "aowis-dvmdb-codec-test-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    char* directory = ::mkdtemp(name.data());
    if (!directory) throw std::runtime_error("mkdtemp failed");
    return std::filesystem::path(directory);
}

void testCanonicalCodecAndRepository() {
    const std::filesystem::path path = uniqueDirectory();
    try {
        const QUuid idA = fixedUuid(1, 1);
        const QUuid idB = fixedUuid(1, 2);
        HydraulicNodeJunction a;
        a.uuid = idA;
        a.id = QStringLiteral("water–α");
        a.elevation_m = 109.625;
        a.coordinate_wgs84.latitude_deg = 11.987654321;
        a.metadata.description = QStringLiteral("Test junction with UTF-8 É 水");
        a.metadata.date_installed = QDate(2021, 4, 2);
        HydraulicNodeJunction b;
        b.uuid = idB;
        b.id = QStringLiteral("J-2");
        HydraulicLinkPipe pipe;
        pipe.uuid = fixedUuid(2, 99);
        pipe.node_uuid_from = idA;
        pipe.node_uuid_to = idB;
        pipe.length_measured_m = 123.125;
        pipe.diameter_mm = 90.625;
        pipe.roughness_darcy_weisbach_mm = 0.025;
        const Bytes expectedA = hydraulic::bytesFromQByteArray(
            aowis::dvmdb::ModelCodec<HydraulicNodeJunction>::encode(a));
        expect(!expectedA.empty(), "native model codec returned empty junction");
        expect(hydraulic::qByteArrayFromBytes(expectedA) ==
               aowis::dvmdb::ModelCodec<HydraulicNodeJunction>::encode(a),
               "canonical codec bytes changed on roundtrip");
        expect(hydraulic::uuidFromKey(hydraulic::keyFromUuid(idA)) == idA,
               "UUID key transform inconsistent");
        {
            ProjectRepository store(path);
            // The first hydraulic snapshot requires an actual committed base;
            // an unborn branch has no revision to pin during native Save.
            const RevisionId emptyHead = store.native().createRootBranch(
                store.project(), "main", aowis::dvmdb::test::commitMetadata());
            NetworkHydraulicDistribution network;
            network.uuid = fixedUuid(3, 1);
            network.id = QStringLiteral("Codec network");
            network.nodes_junctions = {a, b};
            network.links_pipes = {pipe};
            (void)hydraulic::saveHydraulicDistributionNetwork(
                store.native(), store.project(), "main", network,
                emptyHead, aowis::dvmdb::test::commitMetadata());
            const std::optional<aowis::dvmdb::ModelRecord> stored = store.getModel("main", hydraulic::keyFromUuid(idA));
            expect(stored.has_value() && stored->kind == hydraulic::junctionKind &&
                   stored->payload == expectedA, "native store modified canonical native model bytes");
            QString error;
            const std::optional<HydraulicNodeJunction> readA =
                hydraulic::readJunction(store.native(), store.project(), store.head("main"), hydraulic::keyFromUuid(idA), &error);
            expect(readA.has_value() && readA->id == a.id && readA->elevation_m == a.elevation_m,
                   "native model junction roundtrip failed");
            const std::optional<HydraulicLinkPipe> readPipe =
                hydraulic::readPipe(store.native(), store.project(), store.head("main"), hydraulic::keyFromUuid(pipe.uuid), &error);
            expect(readPipe.has_value() && readPipe->node_uuid_from == idA &&
                   readPipe->node_uuid_to == idB && readPipe->diameter_mm == pipe.diameter_mm,
                   "native model pipe roundtrip failed");
            expect(!hydraulic::readJunction(store.native(), store.project(), store.head("main"), hydraulic::keyFromUuid(pipe.uuid), &error),
                   "reading a pipe as a junction must fail");
            const RevisionId mainRevision = store.head("main");
            store.createBranch("other", "main");
            pipe.diameter_mm = 112.75;
            (void)hydraulic::editHydraulicDistributionNetwork(
                store.native(), store.project(), "other", network.uuid,
                {{pipe.uuid, hydraulic::HydraulicEntity{pipe}}},
                store.head("other"), aowis::dvmdb::test::commitMetadata());
            expect(hydraulic::readPipe(store.native(), store.project(), store.head("main"), hydraulic::keyFromUuid(pipe.uuid))->diameter_mm == 90.625,
                   "branch isolation failed");
            expect(hydraulic::readPipe(store.native(), store.project(), store.head("other"), hydraulic::keyFromUuid(pipe.uuid))->diameter_mm == 112.75,
                   "branch update failed");
            expect(hydraulic::readPipe(store.native(), store.project(), mainRevision,
                                       hydraulic::keyFromUuid(pipe.uuid))->diameter_mm == 90.625,
                   "historical revision read changed after branch edit");
            expect(store.diffBranches("main", "other").size() == 1,
                   "codec-level branch diff failed");
            bool rejected = false;
            try {
                (void)hydraulic::editHydraulicDistributionNetwork(
                    store.native(), store.project(), "other", network.uuid,
                    {{idA, std::nullopt}}, store.head("other"),
                    aowis::dvmdb::test::commitMetadata());
            } catch (const std::exception&) { rejected = true; }
            expect(rejected, "referenced junction deletion was accepted");
        }
        {
            ProjectRepository reopened(path);
            const std::optional<HydraulicLinkPipe> stored =
                hydraulic::readPipe(reopened.native(), reopened.project(), reopened.head("other"), hydraulic::keyFromUuid(pipe.uuid));
            expect(stored.has_value() && stored->diameter_mm == 112.75,
                   "reopened canonical model pipe missing");
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        testCanonicalCodecAndRepository();
        std::cout << "Canonical native model codec/native repository test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Canonical native model codec/native repository test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
