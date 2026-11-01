#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/native_store.h"
#include "aowis/dvmdb/revision_record.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using aowis::dvmdb::test::commitMetadata;

namespace {
namespace fs = std::filesystem;
using namespace aowis::dvmdb;

void check(bool condition, std::string_view explanation) {
    if (!condition) throw std::runtime_error(std::string(explanation));
}
template<class F> void rejects(F&& action, StorageErrorKind kind, std::string_view why) {
    try { action(); }
    catch (const StorageError& failure) {
        if (failure.kind() == kind) return;
        throw std::runtime_error(std::string(why) + ": unexpected storage error");
    }
    throw std::runtime_error(std::string(why) + ": operation was accepted");
}
struct Temporary {
    fs::path path = fs::temp_directory_path() / ("aowis-v2-head-" + UuidV7::generate().toString());
    ~Temporary() { std::error_code error; fs::remove_all(this->path, error); }
};
Key key(unsigned last) {
    Key result{};
    result[15] = static_cast<std::uint8_t>(last);
    return result;
}
std::optional<Bytes> value(std::string_view text) {
    return Bytes(text.begin(), text.end());
}
void testUnbornNativeRefFoundation() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    BranchId branch = BranchId::generate();
    {
        Repository repo(temp.path);
        repo.createUnbornBranch(project, "main");
        branch = *repo.branchId("main");
        check(!repo.optionalHead(project, "main"), "unborn HEAD is not null");
        check(repo.history(project, "main", 10).empty(),
              "unborn branch must have empty history");
        rejects([&] { (void)repo.history(project, "main", 10, RevisionId::generate()); },
                StorageErrorKind::InvalidInput, "unborn history accepted a cursor");
        const BranchMetadata secondMetadata{"Alternative network layout", {"hydraulic", "experiment"}};
        const BranchId second = repo.createUnbornBranch(project, "second", std::nullopt, secondMetadata);
        check(repo.branchId("second") == second && !repo.optionalHead(project, "second") &&
              repo.history(project, "second").empty(),
              "second independent unborn branch missing");
        rejects([&] { (void)repo.createUnbornBranch(project, "second"); },
                StorageErrorKind::Conflict, "duplicate unborn branch name accepted");
        rejects([&] { (void)repo.createUnbornBranch(ProjectId::generate(), "foreign"); },
                StorageErrorKind::Conflict, "unborn repository accepted a foreign project");
        check(repo.createUnbornBranch(project, "second", second, secondMetadata) == second,
              "identical unborn branch creation retry changed UUID");
        rejects([&] { (void)repo.createUnbornBranch(project, "second", second); },
                StorageErrorKind::Conflict, "idempotent retry accepted different metadata");
        rejects([&] { (void)repo.createUnbornBranch(project, "different", second); },
                StorageErrorKind::Conflict, "branch UUID was rebound to a different name");
        check(repo.branchById(project, second)->metadata == secondMetadata,
              "unborn branch metadata not visible in graph HEAD");
        rejects([&] { (void)repo.head(project, "main"); },
                StorageErrorKind::Unavailable, "required HEAD accepted an unborn branch");
    }
    {
        Repository reopened(temp.path);
        check(reopened.branchId("main") == branch &&
              !reopened.optionalHead(project, "main") &&
              reopened.branchId("second").has_value() &&
              !reopened.optionalHead(project, "second"),
              "independent unborn branches not durable across reopen");
        check(reopened.branchById(project, *reopened.branchId("second"))->metadata ==
              BranchMetadata{"Alternative network layout", {"hydraulic", "experiment"}},
              "branch metadata not durable across restart");
        rejects([&] { (void)reopened.commit(project, "main", {}, commitMetadata(), RevisionId::generate()); },
                StorageErrorKind::Conflict, "unborn commit accepted a non-null expected HEAD");
        const RevisionId first = reopened.commit(project, "main", {{key(1), value("first")}}, commitMetadata());
        check(reopened.optionalHead(project, "main") == first,
              "first raw commit did not publish unborn HEAD");
        const auto bytes = reopened.readRevision(project, first);
        check(bytes && decodeRevisionRecord(*bytes).parents.empty(),
              "first raw revision has a parent");
        check(reopened.branchId("main") == branch, "first commit changed branch identity");
        const BranchId late = reopened.createUnbornBranch(project, "late");
        check(reopened.branchId("late") == late && !reopened.optionalHead(project, "late"),
              "cannot create an independent unborn branch after a commit");
        const RevisionId independent = reopened.commit(project, "second",
            {{key(2), value("independent")}}, commitMetadata());
        const std::optional<Bytes> independentRecord = reopened.readRevision(project, independent);
        check(independentRecord && decodeRevisionRecord(*independentRecord).parents.empty(),
              "independent branch inherited another branch's revision");
        check(reopened.optionalHead(project, "second") == independent &&
              reopened.branchById(project, *reopened.branchId("second"))->metadata ==
              BranchMetadata{"Alternative network layout", {"hydraulic", "experiment"}},
              "committing unborn branch discarded metadata");
        rejects([&] { (void)reopened.commit(project, "main", {}, commitMetadata(), RevisionId::generate()); },
                StorageErrorKind::Conflict, "committed branch accepted stale expected HEAD");
    }
    {
        Repository modelRepo(temp.path / "model");
        modelRepo.createUnbornBranch(project, "main");
        const RevisionId first = modelRepo.commitModel(project, "main", {}, commitMetadata());
        const auto bytes = modelRepo.readRevision(project, first);
        check(bytes.has_value(), "first model revision missing");
        const RevisionRecord record = decodeRevisionRecord(*bytes);
        check(record.parents.empty() && record.modelIndexed,
              "first model revision must be parentless and model-indexed");
        check(modelRepo.optionalHead(project, "main") == first,
              "first model commit did not publish unborn HEAD");
    }
}
void testIdentityPreservingUnbornCheckout() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    const BranchId source = BranchId::generate();
    {
        Repository target(temp.path);
        target.provisionUnbornBranch(project, "main", source);
        target.provisionUnbornBranch(project, "main", source);
        check(target.branchId("main") == source &&
              !target.optionalHead(project, "main") &&
              target.history(project, "main").empty(),
              "unborn checkout fabricated a revision or lost source branch UUID");
        rejects([&] { target.provisionUnbornBranch(project, "different", source); },
                StorageErrorKind::Conflict, "unborn checkout renamed existing source branch");
        rejects([&] { target.provisionUnbornBranch(project, "main", BranchId::generate()); },
                StorageErrorKind::Conflict, "unborn checkout overwrote a branch identity");
    }
    {
        Repository reopened(temp.path);
        check(reopened.branchId("main") == source &&
              !reopened.optionalHead(project, "main"),
              "identity-preserving unborn checkout was not durable");
    }
}

void testPublication() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    const ProjectId foreign = ProjectId::generate();
    RevisionId root = RevisionId::generate();
    RevisionId main = RevisionId::generate();
    RevisionId feature = RevisionId::generate();
    BranchId mainBranch = BranchId::generate();
    BranchId featureBranch = BranchId::generate();
    {
        Repository repo(temp.path);
        root = repo.createRootBranch(project, "main", commitMetadata());
        check(root == repo.head(project, "main"), "new main HEAD not published");
        const std::optional<Bytes> rootBytes = repo.readRevision(project, root);
        check(rootBytes.has_value(), "initial UUIDv7 revision missing from real pack");
        check(decodeRevisionRecord(*rootBytes).parents.empty(), "initial revision has parents");
        mainBranch = *repo.branchId("main");
        const fs::path recordPath = temp.path / "refs" / "heads" / mainBranch.toString();
        check(fs::exists(recordPath), "UUIDv7 branch filename is missing");
        std::ifstream branchRecord(recordPath);
        std::string magic, name, uuid, digest;
        std::getline(branchRecord, magic);
        std::getline(branchRecord, name);
        std::getline(branchRecord, uuid);
        std::getline(branchRecord, digest);
        check(magic == "AOWIS-BRANCH-2" && name == "main" && uuid == root.toString() &&
              digest == toHex(sha256(*rootBytes)), "branch HEAD is not UUID and checked SHA");
        check(!fs::exists(temp.path / "refs" / "heads" / "main"), "name-keyed ref created");
        rejects([&] { (void)repo.createRootBranch(project, "second-root", commitMetadata()); },
                StorageErrorKind::InvalidInput, "second independent root branch accepted");
        rejects([&] { (void)repo.head(foreign, "main"); }, StorageErrorKind::Conflict,
                "foreign project accessed branch HEAD");
        check(repo.createBranch(project, "feature", "main") == root,
              "fork did not share source revision UUID");
        featureBranch = *repo.branchId("feature");
        check(featureBranch != mainBranch, "two branch references have identical UUID");
        rejects([&] { (void)repo.createBranch(project, "../evil", "main"); },
                StorageErrorKind::InvalidInput, "malicious branch name accepted");
        rejects([&] { (void)repo.createRootBranch(project, "orphan", commitMetadata()); },
                StorageErrorKind::InvalidInput, "second unsourced root allowed");
        main = repo.commit(project, "main", {{key(1), value("one")}}, commitMetadata(), root);
        feature = repo.commit(project, "feature", {{key(2), value("two")}}, commitMetadata(), root);
        check(main != feature && main != root && feature != root, "revision UUID not unique");
        const RevisionRecord mainRecord = decodeRevisionRecord(*repo.readRevision(project, main));
        const RevisionRecord featureRecord = decodeRevisionRecord(*repo.readRevision(project, feature));
        check(mainRecord.parents.size() == 1 && mainRecord.parents[0].id == root &&
              featureRecord.parents.size() == 1 && featureRecord.parents[0].id == root,
              "fork parent UUID not preserved");
        check(mainRecord.root != featureRecord.root, "independent branch snapshots collapsed");
        rejects([&] { (void)repo.commit(project, "main", {}, commitMetadata(), root); },
                StorageErrorKind::Conflict, "stale expected HEAD accepted");
        rejects([&] { (void)repo.commit(foreign, "main", {}, commitMetadata()); },
                StorageErrorKind::Conflict, "foreign commit accepted");
        check(repo.head(project, "main") == main && repo.head(project, "feature") == feature,
              "commit moved wrong HEAD");
        const VerificationReport verified = repo.verifyRepository();
        check(verified.branches == 2 && verified.revisions == 3 &&
              verified.reachableObjects >= 3,
              "repository verification did not follow both branch histories");
        const CompactionStats compacted = repo.compact();
        check(compacted.objectsAfter >= 3,
              "compaction discarded published branch revisions");
        check(repo.head(project, "main") == main && repo.head(project, "feature") == feature,
              "compaction changed branch HEADs");
        check(repo.verifyRepository().revisions == 3,
              "DAG failed verification after compaction");
        repo.renameBranch(project, "feature", "experiment", feature);
        check(!repo.hasBranch("feature") && repo.hasBranch("experiment") &&
              repo.head(project, "experiment") == feature &&
              *repo.branchId("experiment") == featureBranch,
              "rename changed branch UUID or HEAD");
        rejects([&] { repo.renameBranch(project, "experiment", "invalid", root); },
                StorageErrorKind::Conflict, "stale rename accepted");
    }
    {
        Repository repo(temp.path);
        check(repo.head(project, "main") == main && repo.head(project, "experiment") == feature,
              "reopen changed UUIDv7 branch HEADs");
        check(*repo.branchId("main") == mainBranch &&
              *repo.branchId("experiment") == featureBranch,
              "reopen changed branch IDs");
        const std::optional<Bytes> bytes = repo.readRevision(project, feature);
        check(bytes.has_value(), "reopen lost packed revision");
        rejects([&] { (void)repo.commit(project, "experiment", {}, commitMetadata(), main); },
                StorageErrorKind::Conflict, "stale expected HEAD accepted by raw commit");
    }
}
void testCorruptHeadRejected() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    BranchId branch = BranchId::generate();
    {
        Repository repo(temp.path);
        (void)repo.createRootBranch(project, "main", commitMetadata());
        branch = *repo.branchId("main");
    }
    const fs::path path = temp.path / "refs" / "heads" / branch.toString();
    {
        std::ifstream in(path);
        std::string magic, name, uuid, sha;
        std::getline(in, magic); std::getline(in, name);
        std::getline(in, uuid); std::getline(in, sha);
        check(sha.size() == 64, "expected revision checksum missing");
        sha[0] = sha[0] == '0' ? '1' : '0';
        std::ofstream out(path, std::ios::trunc);
        out << magic << '\n' << name << '\n' << uuid << '\n' << sha << '\n';
    }
    {
        Repository repo(temp.path);
        rejects([&] { (void)repo.head(project, "main"); },
                StorageErrorKind::Corruption, "altered HEAD SHA was not rejected");
        rejects([&] { (void)repo.commit(project, "main", {}, commitMetadata()); },
                StorageErrorKind::Corruption, "commit accepted altered HEAD SHA");
    }
}

void testCorruptPublishedMappingRejected() {
    Temporary tmp;
    const ProjectId project = ProjectId::generate();
    RevisionId head = RevisionId::generate();
    {
        Repository repo(tmp.path);
        head = repo.createRootBranch(project, "main", commitMetadata());
        check(repo.verifyRepository().revisions == 1,
              "initial binding was not verified");
    }
    const fs::path mapping = tmp.path / "revision-v2" / "ids" / head.toString();
    {
        std::fstream file(mapping, std::ios::in | std::ios::out | std::ios::binary);
        check(file.good(), "cannot open published mapping");
        file.seekg(36);
        char original = 0;
        file.read(&original, 1);
        check(file.good(), "cannot read published UUID binding");
        const char damaged = static_cast<char>(static_cast<unsigned char>(original) ^ 1U);
        file.seekp(36);
        file.write(&damaged, 1);
        check(file.good(), "cannot corrupt published UUID binding");
    }
    {
        Repository repo(tmp.path);
        // Altering the immutable UUID->SHA mapping must not produce a false
        // success even when physical pack checksums are perfectly valid.
        repo.verifyPack();
        rejects([&] { (void)repo.verifyRepository(); }, StorageErrorKind::Corruption,
                "corrupted resolver entry passed whole-repository verification");
        rejects([&] { (void)repo.compact(); }, StorageErrorKind::Corruption,
                "compaction accepted corrupt UUID binding");
    }
}

// AOWIS-BRANCH-1 stored a SHA-256 revision HEAD. That pre-release format is
// rejected on open, never interpreted or converted.
void testObsoleteShaBranchRecordRejected() {
    Temporary temp;
    {
        Repository repo(temp.path);
        repo.createUnbornBranch(ProjectId::generate(), "main");
    }
    const fs::path record = temp.path / "refs" / "heads" / BranchId::generate().toString();
    {
        std::ofstream obsolete(record, std::ios::binary);
        obsolete << "AOWIS-BRANCH-1\nold\n" << std::string(64, 'a') << "\n";
    }
    rejects([&] { Repository reopened(temp.path); }, StorageErrorKind::Corruption,
            "obsolete SHA-headed branch record was opened");
}

// Lookups at a revision read that revision only. Damage deep in the history
// must not affect reads at the HEAD; full verification still finds it.
void testLookupsDoNotWalkHistory() {
    Temporary temp;
    const ProjectId project = ProjectId::generate();
    Key key{};
    key[15] = 9;
    RevisionId first = RevisionId::generate();
    Bytes firstRecord;
    {
        Repository repo(temp.path);
        repo.createUnbornBranch(project, "main");
        for (std::uint8_t value = 0; value != 40; ++value) {
            const RevisionId id = repo.commit(project, "main", {Change{key, Bytes{value}}}, commitMetadata());
            if (value == 0) first = id;
        }
        firstRecord = *repo.readRevision(project, first);
    }
    // Damage the first revision's record inside the pack.
    fs::path pack;
    for (const fs::directory_entry& entry : fs::directory_iterator(temp.path))
        if (entry.path().extension() == ".pack") pack = entry.path();
    std::string bytes;
    {
        std::ifstream in(pack, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::size_t at = bytes.find(std::string(firstRecord.begin(), firstRecord.end()));
    check(at != std::string::npos, "first revision record not found in pack");
    {
        std::fstream out(pack, std::ios::binary | std::ios::in | std::ios::out);
        out.seekp(static_cast<std::streamoff>(at + 40));
        out.put(static_cast<char>(bytes[at + 40] ^ 0x01));
    }
    Repository repo(temp.path);
    const RevisionId head = repo.head(project, "main");
    try {
        check(repo.getAtRevision(project, head, key) == std::optional<Bytes>(Bytes{39}),
              "HEAD read returned the wrong value");
    } catch (const StorageError& error) {
        throw std::runtime_error(std::string("HEAD read failed because an old revision is damaged: ") +
                                 error.what());
    }
    (void)repo.commit(project, "main", {Change{key, Bytes{40}}}, commitMetadata());
    check(repo.history(project, "main", 5).size() == 5, "recent history unreadable");
    rejects([&] { (void)repo.verifyRepository(); }, StorageErrorKind::Corruption,
            "full verification missed a damaged historical revision");
}
} // namespace

int main() {
    try {
        testUnbornNativeRefFoundation();
        testIdentityPreservingUnbornCheckout();
        testPublication();
        testObsoleteShaBranchRecordRejected();
        testLookupsDoNotWalkHistory();
        testCorruptHeadRejected();
        testCorruptPublishedMappingRejected();
        std::cout << "UUIDv7 HEAD publication, fork, raw commit, rename and recovery passed\n";
    } catch (const std::exception& error) {
        std::cerr << "UUIDv7 HEAD test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
