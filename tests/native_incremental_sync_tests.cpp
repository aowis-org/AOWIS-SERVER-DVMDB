// Review finding F3: incremental synchronization work is proportional to the
// change, not to the history. Export walks only the base's revision ancestry
// plus the changed trie paths of new revisions; import treats objects that are
// already stored locally as verified boundaries. Forged bundles that graft
// local objects into the wrong position must still be rejected.
#include "aowis/dvmdb/native_store.h"
#include "project_repository.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::test::ProjectRepository;
using aowis::dvmdb::BundleImportProfile;
using aowis::dvmdb::RevisionBundleImport;
using aowis::dvmdb::BundleImportStatus;
using aowis::dvmdb::Bytes;
using aowis::dvmdb::Change;
using aowis::dvmdb::Hash;
using aowis::dvmdb::Key;
using aowis::dvmdb::MergeResult;
using aowis::dvmdb::MergeStatus;
using aowis::dvmdb::ModelChange;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::Repository;
using aowis::dvmdb::ProjectId;
using aowis::dvmdb::test::CommittedRevision;
using aowis::dvmdb::StorageError;
using aowis::dvmdb::StorageErrorKind;

void expect(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

struct TestDirectory {
    explicit TestDirectory(std::string_view label) {
        static unsigned counter = 0;
        this->path = std::filesystem::temp_directory_path() /
            ("aowis-dvmdb-incremental-" + std::string(label) + "-" + std::to_string(::getpid()) +
             "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++counter));
        std::filesystem::create_directories(this->path);
    }
    ~TestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(this->path, error);
    }
    std::filesystem::path path;
};

// Well-spread UUIDs, so tries have realistic shapes.
Key key(std::uint32_t number) {
    const Bytes seed{static_cast<std::uint8_t>(number >> 24U), static_cast<std::uint8_t>(number >> 16U),
                     static_cast<std::uint8_t>(number >> 8U), static_cast<std::uint8_t>(number), 'k'};
    const Hash digest = aowis::dvmdb::sha256(seed);
    Key result{};
    std::copy_n(digest.begin(), result.size(), result.begin());
    return result;
}

Bytes payload(std::uint32_t number, std::uint32_t version) {
    Bytes bytes(24, static_cast<std::uint8_t>(number));
    for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<std::uint8_t>(version >> (8U * i));
    return bytes;
}

ModelChange put(std::uint32_t number, std::uint32_t version, std::vector<Key> references = {}) {
    return ModelChange{key(number), ModelRecord{1, std::move(references), payload(number, version)}};
}

void expectKind(StorageErrorKind expected, const std::string& label,
                const std::function<void()>& action) {
    try {
        action();
    } catch (const StorageError& error) {
        expect(error.kind() == expected, label + ": expected " + StorageError::kindName(expected) +
               ", got " + StorageError::kindName(error.kind()) + " (" + error.what() + ")");
        return;
    }
    throw std::runtime_error(label + ": accepted");
}

std::uint64_t readU64(const Bytes& bytes, std::size_t offset) {
    std::uint64_t number = 0;
    for (unsigned i = 0; i < 8; ++i) number = (number << 8U) | bytes[offset + i];
    return number;
}

void appendU64(Bytes& bytes, std::uint64_t number) {
    for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(number >> (56U - 8U * i)));
}

Bytes readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

std::uint64_t bundleObjectCount(const std::filesystem::path& bundle) {
    return aowis::dvmdb::test::bundleLayout(bundle).objects;
}

// Stored payload of `hash`, located through its pack record header
// (AOBJ\1 | SHA-256 | big-endian size | payload).
Bytes storedObject(const std::filesystem::path& repository, const Hash& hash) {
    const Bytes pack = readFile(repository / "objects.pack");
    Bytes header{'A', 'O', 'B', 'J', 1};
    header.insert(header.end(), hash.begin(), hash.end());
    const Bytes::const_iterator found = std::search(pack.begin(), pack.end(), header.begin(), header.end());
    expect(found != pack.end(), "object not stored locally");
    const std::size_t offset = static_cast<std::size_t>(found - pack.begin());
    const std::uint64_t size = readU64(pack, offset + 37);
    return Bytes(pack.begin() + static_cast<std::ptrdiff_t>(offset + 45),
                 pack.begin() + static_cast<std::ptrdiff_t>(offset + 45 + size));
}

// A forged revision record (UUIDv7, 'T',3 layout) and its identity.
struct ForgedRevision {
    RevisionId id;
    Bytes bytes;
};

ForgedRevision revisionObject(const ProjectId& project, const Hash& root, const Hash& referencesRoot,
                              bool model, const std::vector<std::pair<RevisionId, Hash>>& parents) {
    ForgedRevision result{RevisionId::generate(), Bytes{'T', 3}};
    for (const aowis::dvmdb::UuidV7* uuid : {&result.id.uuid(), &project.uuid()})
        result.bytes.insert(result.bytes.end(), uuid->bytes.begin(), uuid->bytes.end());
    result.bytes.insert(result.bytes.end(), root.begin(), root.end());
    result.bytes.insert(result.bytes.end(), referencesRoot.begin(), referencesRoot.end());
    result.bytes.push_back(model ? 1 : 0);
    result.bytes.push_back(static_cast<std::uint8_t>(parents.size()));
    for (const std::pair<RevisionId, Hash>& parent : parents) {
        result.bytes.insert(result.bytes.end(), parent.first.uuid().bytes.begin(),
                            parent.first.uuid().bytes.end());
        result.bytes.insert(result.bytes.end(), parent.second.begin(), parent.second.end());
    }
    aowis::dvmdb::test::appendRecordMetadata(result.bytes);
    return result;
}

std::pair<RevisionId, Hash> parentOf(const CommittedRevision& revision) {
    return {revision.id, revision.recordHash};
}

Bytes leafObject(const Key& leafKey, const Hash& value) {
    Bytes bytes{'L'};
    bytes.insert(bytes.end(), leafKey.begin(), leafKey.end());
    bytes.insert(bytes.end(), value.begin(), value.end());
    return bytes;
}

Bytes branchObject(std::uint8_t depth, const Key& prefix, const std::map<unsigned, Hash>& children) {
    Bytes bytes{'B', depth};
    bytes.insert(bytes.end(), prefix.begin(), prefix.end());
    for (unsigned slot = 0; slot < 16; ++slot) {
        const std::map<unsigned, Hash>::const_iterator child = children.find(slot);
        const Hash hash = child == children.end() ? Hash{} : child->second;
        bytes.insert(bytes.end(), hash.begin(), hash.end());
    }
    return bytes;
}

Bytes modelValue(const ModelRecord& record) {
    Bytes bytes{'V'};
    const Bytes envelope = aowis::dvmdb::encodeModelRecord(record);
    bytes.insert(bytes.end(), envelope.begin(), envelope.end());
    return bytes;
}

Bytes referrersValue(const std::vector<Key>& sources) {
    Bytes bytes{'I', 'N', 'R', '1'};
    appendU64(bytes, sources.size());
    bytes.erase(bytes.begin() + 4, bytes.begin() + 8); // 32-bit count
    for (const Key& source : sources) bytes.insert(bytes.end(), source.begin(), source.end());
    return bytes;
}

unsigned nibble(const Key& value, unsigned position) {
    const std::uint8_t byte = value[position / 2U];
    return (position % 2U) == 0U ? (byte >> 4U) : (byte & 0x0fU);
}

// An incremental (AOWREV22) bundle with arbitrary (hash-valid) objects.
void writeBundle(const std::filesystem::path& path, const ProjectId& project,
                 const ForgedRevision& tip, const CommittedRevision& base,
                 const std::vector<Bytes>& objects, const aowis::dvmdb::BranchId& branchId) {
    std::map<Hash, Bytes> sorted;
    for (const Bytes& object : objects) sorted.emplace(aowis::dvmdb::sha256(object), object);
    const Hash tipHash = aowis::dvmdb::sha256(tip.bytes);
    Bytes bundle{'A', 'O', 'W', 'R', 'E', 'V', '2', '2'};
    for (const aowis::dvmdb::UuidV7* uuid : {&project.uuid(), &branchId.uuid(), &tip.id.uuid()})
        bundle.insert(bundle.end(), uuid->bytes.begin(), uuid->bytes.end());
    bundle.insert(bundle.end(), tipHash.begin(), tipHash.end());
    appendU64(bundle, sorted.size());
    bundle.insert(bundle.end(), base.id.uuid().bytes.begin(), base.id.uuid().bytes.end());
    bundle.insert(bundle.end(), base.recordHash.begin(), base.recordHash.end());
    for (const std::pair<const Hash, Bytes>& entry : sorted) {
        bundle.insert(bundle.end(), entry.first.begin(), entry.first.end());
        appendU64(bundle, entry.second.size());
        bundle.insert(bundle.end(), entry.second.begin(), entry.second.end());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bundle.data()), static_cast<std::streamsize>(bundle.size()));
    expect(output.good(), "cannot write forged bundle");
}

// Exports main relative to the recipient's head and imports it; checks the
// recipient ends with exactly the source's state.
struct Transfer {
    RevisionBundleImport result{BundleImportStatus::UpToDate, RevisionId::generate(), 0};
    BundleImportProfile profile;
    std::uint64_t objects = 0;
};

Transfer synchronize(ProjectRepository& source, ProjectRepository& recipient, const std::filesystem::path& bundle,
                     std::string_view branch = "main") {
    std::error_code ignored;
    std::filesystem::remove(bundle, ignored);
    source.exportIncrementalBundle(branch, recipient.head(branch), bundle);
    Transfer transfer;
    transfer.objects = bundleObjectCount(bundle);
    transfer.result = recipient.importBundle(bundle, branch, {}, &transfer.profile);
    expect(recipient.head(branch) == source.head(branch), "recipient head differs after import");
    expect(recipient.scan(branch) == source.scan(branch), "recipient content differs after import");
    return transfer;
}

// 2000 model records (every seventh refers to its predecessor), then
// `historyLength` commits of three record updates each.
void buildModelHistory(ProjectRepository& repository, unsigned historyLength) {
    repository.createBranch("main");
    std::vector<ModelChange> initial;
    for (std::uint32_t i = 1; i <= 2000; ++i) {
        initial.push_back(i % 7 == 0 ? put(i, 0, {key(i - 1)}) : put(i, 0));
    }
    repository.commitModel("main", initial);
    for (unsigned commit = 1; commit <= historyLength; ++commit) {
        std::vector<ModelChange> changes;
        for (unsigned j = 0; j < 3; ++j) {
            const std::uint32_t number = 1U + (commit * 37U + j * 613U) % 2000U;
            changes.push_back(number % 7 == 0 ? put(number, commit, {key(number - 1)}) : put(number, commit));
        }
        repository.commitModel("main", changes);
    }
}

void testWorkIndependentOfHistory() {
    std::vector<std::uint64_t> reads;
    std::vector<std::uint64_t> sizes;
    for (unsigned historyLength : {50U, 400U}) {
        TestDirectory directory("history-" + std::to_string(historyLength));
        ProjectRepository source(directory.path / "source");
        buildModelHistory(source, historyLength);
        source.exportBundle("main", directory.path / "full.bundle");
        ProjectRepository recipient(directory.path / "recipient");
        recipient.importBundle(directory.path / "full.bundle", "main");
        // One updated record, one new record with a new reference.
        source.commitModel("main", {put(5, 9999), put(5000, 0, {key(5)})});
        const Transfer transfer = synchronize(source, recipient, directory.path / "delta.bundle");
        expect(transfer.result.status == BundleImportStatus::FastForward, "delta import not a fast-forward");
        reads.push_back(transfer.profile.localObjectReads);
        sizes.push_back(transfer.objects);
        const aowis::dvmdb::VerificationReport report = recipient.verifyRepository();
        expect(report.revisions == historyLength + 2U, "verification after delta import incomplete");
    }
    std::cout << "  delta import local reads " << reads[0] << " (50 commits), " << reads[1]
              << " (400 commits); bundle objects " << sizes[0] << ", " << sizes[1] << '\n';
    // The previous importer re-verified the whole local history: ~9,400
    // reads at 50 commits and ~100,000 at 400.
    expect(reads[0] == reads[1], "delta import work depends on history length");
    // Per changed trie level: up to 15 unchanged sibling boundaries plus the
    // parent-snapshot nodes the semantic diff compares (~106 here).
    expect(reads[0] > 0 && reads[0] <= 160, "delta import reads too many local objects");
    // Revision, two model-trie paths and values, reverse-index path.
    expect(sizes[0] == sizes[1] && sizes[0] <= 24, "delta bundle carries unchanged objects");
}

// Insertions that split compressed branches, deletions that collapse them,
// emptying and refilling a tree: the exporter's trie alignment must never
// omit an object the recipient lacks.
void testTrieShapeChanges() {
    TestDirectory directory("shapes");
    ProjectRepository source(directory.path / "source");
    source.createBranch("main");
    std::vector<Change> initial;
    for (std::uint32_t i = 1; i <= 600; ++i) initial.push_back(Change{key(i), payload(i, 0)});
    source.commit("main", initial);
    source.exportBundle("main", directory.path / "full.bundle");
    ProjectRepository recipient(directory.path / "recipient");
    recipient.importBundle(directory.path / "full.bundle", "main");

    // Two keys sharing a long prefix force a deep new branch.
    Key first{};
    first[0] = 0x5a;
    first[1] = 0x5a;
    first[15] = 1;
    Key second = first;
    second[15] = 2;
    const std::vector<std::vector<Change>> steps{
        {Change{first, Bytes{1}}},                                 // new leaf under an existing branch
        {Change{second, Bytes{2}}},                                // leaf becomes a depth-30 branch
        {Change{first, std::nullopt}},                             // branch collapses back to a leaf
        {Change{second, std::nullopt}, Change{key(3), payload(3, 1)}},
        {Change{key(4), payload(4, 0)}},                           // re-store a value the base already holds
    };
    for (const std::vector<Change>& step : steps) {
        source.commit("main", step);
        const Transfer transfer = synchronize(source, recipient, directory.path / "step.bundle");
        expect(transfer.objects <= 12, "shape change carries unchanged objects");
    }
    // A 40-key subtree below prefix abcde0 and one sibling key abcd1...:
    // removing the sibling moves the unchanged subtree up two levels,
    // re-adding it pushes the subtree down again. Neither resends it.
    std::vector<Change> cluster;
    for (std::uint8_t i = 0; i < 40; ++i) {
        Key member = key(9000U + i);
        member[0] = 0xab;
        member[1] = 0xcd;
        member[2] = 0xe0;
        cluster.push_back(Change{member, payload(i, 7)});
    }
    Key sibling = key(9100);
    sibling[0] = 0xab;
    sibling[1] = 0xcd;
    sibling[2] = 0x10;
    cluster.push_back(Change{sibling, Bytes{3}});
    source.commit("main", cluster);
    expect(synchronize(source, recipient, directory.path / "cluster.bundle").objects > 40,
           "cluster not transferred");
    source.commit("main", {Change{sibling, std::nullopt}});
    const Transfer up = synchronize(source, recipient, directory.path / "up.bundle");
    source.commit("main", {Change{sibling, Bytes{4}}});
    const Transfer down = synchronize(source, recipient, directory.path / "down.bundle");
    expect(up.objects <= 8 && down.objects <= 10, "moved subtree was resent (" +
           std::to_string(up.objects) + ", " + std::to_string(down.objects) + " objects)");
    // Delete everything, then restore the original snapshot.
    std::vector<Change> clear;
    for (const std::pair<Key, Bytes>& entry : source.scan("main")) clear.push_back(Change{entry.first, std::nullopt});
    source.commit("main", clear);
    synchronize(source, recipient, directory.path / "clear.bundle");
    expect(source.scan("main").empty(), "tree not emptied");
    // Restoring an older snapshot resends its trie (the delta is taken
    // against the empty parent); the recipient stores none of it again.
    source.commit("main", initial);
    const std::uint64_t objectsBefore = recipient.objectCount();
    const Transfer restore = synchronize(source, recipient, directory.path / "restore.bundle");
    expect(restore.result.objectsImported == 1 && recipient.objectCount() == objectsBefore + 1,
           "restored snapshot stored duplicate objects");

    // Several revisions at once, then an empty delta (base == tip).
    for (std::uint32_t i = 700; i < 710; ++i) source.commit("main", {Change{key(i), payload(i, 1)}});
    const Transfer batch = synchronize(source, recipient, directory.path / "batch.bundle");
    expect(batch.result.status == BundleImportStatus::FastForward, "batch not a fast-forward");
    const Transfer empty = synchronize(source, recipient, directory.path / "empty.bundle");
    expect(empty.objects == 0 && empty.result.status == BundleImportStatus::UpToDate,
           "empty delta not up to date");
    recipient.verifyRepository();
}

// Random model histories with merges and lagging recipients. Every delta
// must import without a missing object and reproduce the source exactly.
void testRandomModelHistories() {
    for (unsigned seed = 1; seed <= 6; ++seed) {
        TestDirectory directory("random-" + std::to_string(seed));
        std::mt19937 random(seed);
        ProjectRepository source(directory.path / "source");
        source.createBranch("main");
        // Anchors are never deleted; other records may refer to them.
        std::vector<ModelChange> anchors;
        for (std::uint32_t i = 1; i <= 40; ++i) anchors.push_back(put(i, 0));
        source.commitModel("main", anchors);
        source.exportBundle("main", directory.path / "full.bundle");
        ProjectRepository eager(directory.path / "eager");
        ProjectRepository lagging(directory.path / "lagging");
        eager.importBundle(directory.path / "full.bundle", "main");
        lagging.importBundle(directory.path / "full.bundle", "main");

        std::set<std::uint32_t> live;
        std::uint32_t version = 0;
        const std::function<std::vector<ModelChange>(std::uint32_t, std::uint32_t)> randomChanges =
            [&](std::uint32_t low, std::uint32_t high) {
                std::vector<ModelChange> changes;
                std::set<std::uint32_t> touched;
                const unsigned count = 1U + random() % 6U;
                for (unsigned j = 0; j < count; ++j) {
                    const std::uint32_t number = low + random() % (high - low);
                    if (!touched.insert(number).second) continue;
                    if (live.contains(number) && random() % 3U == 0U) {
                        changes.push_back(ModelChange{key(number), std::nullopt});
                        live.erase(number);
                    } else {
                        std::vector<Key> references;
                        for (unsigned r = random() % 3U; r > 0; --r) references.push_back(key(1U + random() % 40U));
                        std::sort(references.begin(), references.end());
                        references.erase(std::unique(references.begin(), references.end()), references.end());
                        changes.push_back(put(number, ++version, references));
                        live.insert(number);
                    }
                }
                return changes;
            };
        for (unsigned step = 1; step <= 60; ++step) {
            if (step % 9 == 0) {
                // A feature branch with disjoint keys, merged back as a
                // two-parent revision whose second parent is also new.
                const std::string feature = "feature-" + std::to_string(step);
                source.createBranch(feature, std::string_view("main"));
                for (unsigned k = 0; k < 3; ++k) {
                    std::vector<ModelChange> changes = randomChanges(5000, 5400);
                    if (!changes.empty()) source.commitModel(feature, changes);
                    changes = randomChanges(100, 500);
                    if (!changes.empty()) source.commitModel("main", changes);
                }
                const MergeResult merged = source.merge("main", feature);
                expect(merged.status == MergeStatus::Merged, "random merge failed");
            } else {
                const std::vector<ModelChange> changes = randomChanges(100, 500);
                if (!changes.empty()) source.commitModel("main", changes);
            }
            synchronize(source, eager, directory.path / "eager.bundle");
            if (step % 7 == 0) synchronize(source, lagging, directory.path / "lagging.bundle");
        }
        synchronize(source, lagging, directory.path / "lagging.bundle");
        eager.verifyRepository();
        lagging.verifyRepository();
    }
}

// Recipient: main = A -> B with model records (anchor 1, record 2 -> 1),
// branch "other" = A -> C. All forged objects below graft LOCAL objects.
struct ForgeFixture {
    explicit ForgeFixture(const std::filesystem::path& root) : directory(root / "recipient") {
        ProjectRepository repository(this->directory);
        repository.createBranch("main");
        this->a = repository.revision(repository.commitModel("main", {put(1, 0)}));
        repository.createBranch("other", std::string_view("main"));
        this->b = repository.revision(repository.commitModel("main", {put(2, 0, {key(1)})}));
        this->c = repository.revision(repository.commitModel("other", {put(3, 0)}));
        this->project = repository.project();
    }
    std::filesystem::path directory;
    CommittedRevision a{RevisionId::generate()};
    CommittedRevision b{RevisionId::generate()};
    CommittedRevision c{RevisionId::generate()};
    ProjectId project = ProjectId::generate();
};

void expectForgedRejected(const ForgeFixture& fixture, const std::filesystem::path& bundle,
                          const std::string& label, const CommittedRevision& base,
                          const ForgedRevision& tipRevision, std::vector<Bytes> objects,
                          StorageErrorKind expected = StorageErrorKind::InvalidInput) {
    objects.push_back(tipRevision.bytes);
    ProjectRepository recipient(fixture.directory);
    writeBundle(bundle, fixture.project, tipRevision, base, objects, *recipient.branchId("main"));
    const std::uint64_t before = recipient.objectCount();
    expectKind(expected, label, [&] { (void)recipient.importBundle(bundle, "main"); });
    expect(recipient.objectCount() == before && recipient.head("main") == fixture.b.id,
           label + ": rejected import changed the repository");
}

void testForgedBoundaryGrafts() {
    TestDirectory directory("forged");
    const ForgeFixture fixture(directory.path);
    const std::filesystem::path bundle = directory.path / "forged.bundle";
    const CommittedRevision& b = fixture.b;
    const ProjectId& project = fixture.project;
    const Hash referrers = aowis::dvmdb::sha256(referrersValue({key(2)}));
    expect(storedObject(fixture.directory, referrers) == referrersValue({key(2)}),
           "fixture reverse index differs from the expected encoding");
    const Bytes recordTwoLeaf = leafObject(key(2), aowis::dvmdb::sha256(modelValue(
        ModelRecord{1, {key(1)}, payload(2, 0)})));
    (void)storedObject(fixture.directory, aowis::dvmdb::sha256(recordTwoLeaf));

    // A local reverse-index trie (INR1 values) used as a raw content tree.
    expectForgedRejected(fixture, bundle, "local reference trie as raw tree", b,
                         revisionObject(project, b.referencesRoot, Hash{}, false, {parentOf(b)}), {});
    // ... as a model tree: the semantic phase decodes every changed leaf.
    expectForgedRejected(fixture, bundle, "local reference trie as model tree", b,
                         revisionObject(project, b.referencesRoot, Hash{}, true, {parentOf(b)}), {});
    // A local model trie used as the reverse-reference index.
    expectForgedRejected(fixture, bundle, "local model trie as reference index", b,
                         revisionObject(project, b.root, b.root, true, {parentOf(b)}), {});
    // A new bundled leaf whose model value is a local INR1 object.
    {
        const Bytes leaf = leafObject(key(7), referrers);
        expectForgedRejected(fixture, bundle, "bundled leaf with local reference value", b,
                             revisionObject(project, aowis::dvmdb::sha256(leaf), Hash{}, true, {parentOf(b)}), {leaf});
    }
    // A local leaf placed in a slot that does not match its key.
    {
        const Hash localLeaf = aowis::dvmdb::sha256(recordTwoLeaf);
        const unsigned correct = nibble(key(2), 0);
        const ModelRecord record{1, {}, payload(9, 0)};
        Key other = key(9);
        other[0] = static_cast<std::uint8_t>((((correct + 2U) % 16U) << 4U) | (other[0] & 0x0fU));
        const Bytes otherValue = modelValue(record);
        const Bytes otherLeaf = leafObject(other, aowis::dvmdb::sha256(otherValue));
        const Bytes branch = branchObject(0, Key{}, {{(correct + 1U) % 16U, localLeaf},
                                                     {(correct + 2U) % 16U, aowis::dvmdb::sha256(otherLeaf)}});
        expectForgedRejected(fixture, bundle, "local leaf in the wrong slot", b,
                             revisionObject(project, aowis::dvmdb::sha256(branch), Hash{}, false, {parentOf(b)}),
                             {branch, otherLeaf, otherValue});
    }
    // A bundled branch whose local child branch is not deeper than itself.
    {
        const Bytes root = storedObject(fixture.directory, b.root);
        expect(root.size() == 530 && root[0] == 'B', "fixture model root is not a branch");
        const std::uint8_t depth = root[1];
        Key prefix{};
        std::copy_n(root.begin() + 2, 16, prefix.begin());
        Key sibling = key(11);
        for (unsigned position = 0; position < depth; ++position) {
            const unsigned shift = (position % 2U) == 0U ? 4U : 0U;
            sibling[position / 2U] = static_cast<std::uint8_t>(
                (sibling[position / 2U] & ~(0x0fU << shift)) | (nibble(prefix, position) << shift));
        }
        const unsigned siblingSlot = nibble(sibling, depth) == 0U ? 1U : nibble(sibling, depth);
        const unsigned shift = (depth % 2U) == 0U ? 4U : 0U;
        sibling[depth / 2U] = static_cast<std::uint8_t>(
            (sibling[depth / 2U] & ~(0x0fU << shift)) | (siblingSlot << shift));
        const Bytes value = modelValue(ModelRecord{1, {}, payload(11, 0)});
        const Bytes leaf = leafObject(sibling, aowis::dvmdb::sha256(value));
        const Bytes branch = branchObject(depth, prefix, {{0U, b.root}, {siblingSlot, aowis::dvmdb::sha256(leaf)}});
        expectForgedRejected(fixture, bundle, "local branch not deeper than its parent", b,
                             revisionObject(project, aowis::dvmdb::sha256(branch), Hash{}, false, {parentOf(b)}),
                             {branch, leaf, value});
    }
    // A local tree node or value used as a parent revision.
    expectForgedRejected(fixture, bundle, "local tree node as parent revision", b,
                         revisionObject(project, b.root, b.referencesRoot, true,
                                        {parentOf(b), {RevisionId::generate(), b.root}}), {});
    // The header base must be an ancestor: this tip descends from C only.
    expectForgedRejected(fixture, bundle, "base not an ancestor", b,
                         revisionObject(project, fixture.c.root, fixture.c.referencesRoot, true,
                                        {parentOf(fixture.c)}), {});

    // Positive control: grafting a local model trie as a raw tree is valid
    // (raw values are any 'V' objects), and the import is complete.
    {
        const ForgedRevision revision = revisionObject(project, b.root, Hash{}, false, {parentOf(b)});
        ProjectRepository recipient(fixture.directory);
        writeBundle(bundle, project, revision, b, {revision.bytes}, *recipient.branchId("main"));
        const RevisionBundleImport result = recipient.importBundle(bundle, "main");
        expect(result.status == BundleImportStatus::FastForward && result.objectsImported == 1,
               "valid graft of a local raw-compatible trie rejected");
        recipient.verifyRepository();
    }
}

void testBranchRelations() {
    TestDirectory directory("branches");
    ProjectRepository source(directory.path / "source");
    source.createBranch("main");
    const RevisionId a = source.commitModel("main", {put(1, 0)});
    const RevisionId b = source.commitModel("main", {put(2, 0, {key(1)})});
    source.exportBundle("main", directory.path / "b.bundle");
    const RevisionId c = source.commitModel("main", {put(3, 0)});
    source.exportIncrementalBundle("main", b, directory.path / "c.bundle");
    const RevisionId d = source.commitModel("main", {put(4, 0, {key(3)})});
    source.exportIncrementalBundle("main", c, directory.path / "d.bundle");

    // A delta applies to exactly the HEAD it was cut against. Recipient main
    // at B also holds C (on "side"), yet the delta C -> D is refused for main.
    ProjectRepository recipient(directory.path / "recipient");
    recipient.importBundle(directory.path / "b.bundle", "main");
    // The same UUIDv7 cannot be imported as an alias under another name.
    expectKind(StorageErrorKind::Conflict, "duplicate branch UUID alias", [&] {
        (void)recipient.importBundle(directory.path / "b.bundle", "other");
    });
    // Import C through its own independently named source branch, so the
    // recipient knows that immutable revision without advancing main from B.
    source.createBranchAt("side", "main", c);
    source.exportBundle("side", directory.path / "side.bundle");
    expect(recipient.importBundle(directory.path / "side.bundle", "side").head == c,
           "side branch did not transfer boundary C");
    expectKind(StorageErrorKind::Conflict, "delta cut against a held non-HEAD base", [&] {
        (void)recipient.importBundle(directory.path / "d.bundle", "main");
    });
    expect(recipient.head("main") == b, "refused delta moved main");
    // B -> C, then C -> D, applied in order, fast-forward main to D.
    expect(recipient.importBundle(directory.path / "c.bundle", "main").head == c &&
           recipient.importBundle(directory.path / "d.bundle", "main").status ==
               BundleImportStatus::FastForward && recipient.head("main") == d,
           "chained deltas did not fast-forward");
    expect(recipient.importBundle(directory.path / "d.bundle", "main").status ==
           BundleImportStatus::UpToDate, "repeated import not up to date");
    expectKind(StorageErrorKind::Conflict, "older delta against a moved HEAD", [&] {
        (void)recipient.importBundle(directory.path / "c.bundle", "main");
    });
    recipient.verifyRepository();

    // Local work after B diverges from the delta B -> C.
    ProjectRepository diverged(directory.path / "diverged");
    diverged.importBundle(directory.path / "b.bundle", "main");
    diverged.commitModel("main", {put(9, 0)});
    expectKind(StorageErrorKind::Conflict, "divergent incremental import",
               [&] { (void)diverged.importBundle(directory.path / "c.bundle", "main"); });
    // A delta from an older revision the recipient holds is not its HEAD.
    ProjectRepository behind(directory.path / "behind");
    source.exportIncrementalBundle("main", a, directory.path / "from-a.bundle");
    behind.importBundle(directory.path / "b.bundle", "main");
    expectKind(StorageErrorKind::Conflict, "delta from an older held base",
               [&] { (void)behind.importBundle(directory.path / "from-a.bundle", "main"); });
    expectKind(StorageErrorKind::NotFound, "export from an unknown base",
               [&] { source.exportIncrementalBundle("main", RevisionId::generate(),
                                                    directory.path / "bad.bundle"); });
    source.createBranchAt("unrelated", "main", a);
    const RevisionId unrelated = source.commitModel("unrelated", {put(8, 0)});
    expectKind(StorageErrorKind::Conflict, "export base not an ancestor",
               [&] { source.exportIncrementalBundle("main", unrelated,
                                                    directory.path / "bad.bundle"); });
}

} // namespace

int main() {
    try {
        testWorkIndependentOfHistory();
        testTrieShapeChanges();
        testRandomModelHistories();
        testForgedBoundaryGrafts();
        testBranchRelations();
        std::cout << "Incremental synchronization tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Incremental synchronization test failed: " << error.what() << '\n';
        return 1;
    }
}
