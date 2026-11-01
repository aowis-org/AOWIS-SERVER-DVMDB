#include "aowis/dvmdb/native_store.h"
#include "project_repository.h"
#include "uuid_branch_fixture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::test::ProjectRepository;
using aowis::dvmdb::Bytes;
using aowis::dvmdb::Hash;
using aowis::dvmdb::Key;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::Repository;
using aowis::dvmdb::VerificationReport;

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TemporaryDirectory {
    explicit TemporaryDirectory(std::string_view name) {
        static std::uint64_t sequence = 0;
        this->path = std::filesystem::temp_directory_path() /
            ("aowis-full-verify-" + std::string(name) + "-" +
             std::to_string(::getpid()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++sequence));
        std::filesystem::create_directories(this->path);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(this->path, ignored);
    }
    std::filesystem::path path;
};

Key key(std::uint8_t value) {
    Key result{};
    result.back() = value;
    return result;
}
void appendHash(Bytes& bytes, const Hash& value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
}
void appendU64(Bytes& bytes, std::uint64_t value) {
    for (unsigned position = 0; position < 8; ++position) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (56U - position * 8U)));
    }
}
// A forged revision: its UUID and the digest its binding names.
struct ForgedRevision {
    RevisionId id;
    Hash hash{};
};

struct ForgedObjects {
    explicit ForgedObjects(aowis::dvmdb::ProjectId owner) : project(owner) {}
    aowis::dvmdb::ProjectId project;
    std::map<Hash, Bytes> entries;
    std::vector<ForgedRevision> bindings;
    Hash add(const Bytes& bytes) {
        const Hash hash = aowis::dvmdb::sha256(bytes);
        this->entries.emplace(hash, bytes);
        return hash;
    }
    Hash modelValue(const std::vector<Key>& refs) {
        Bytes raw{'V'};
        const Bytes encoded = aowis::dvmdb::encodeModelRecord(
            ModelRecord{1, refs, Bytes{'p', 'a', 'y', 'l', 'o', 'a', 'd'}});
        raw.insert(raw.end(), encoded.begin(), encoded.end());
        return add(raw);
    }
    Hash rawValue() {
        return add(Bytes{'V', 'r', 'a', 'w'});
    }
    Hash inboundValue(const Key& source) {
        Bytes bytes{'I', 'N', 'R', '1', 0, 0, 0, 1};
        bytes.insert(bytes.end(), source.begin(), source.end());
        return add(bytes);
    }
    Hash leaf(const Key& uuid, const Hash& value) {
        Bytes bytes{'L'};
        bytes.insert(bytes.end(), uuid.begin(), uuid.end());
        appendHash(bytes, value);
        return add(bytes);
    }
    Hash branch(const Hash& first, const Hash& second, unsigned secondSlot) {
        expect(secondSlot < 16, "bad test fixture slot");
        Bytes bytes{'B', 31};
        bytes.insert(bytes.end(), 16, 0);
        for (unsigned index = 0; index < 16; ++index) {
            if (index == 1) appendHash(bytes, first);
            else if (index == secondSlot) appendHash(bytes, second);
            else appendHash(bytes, Hash{});
        }
        return add(bytes);
    }
    ForgedRevision revision(const Hash& root, const Hash& reverse, bool model,
                            const std::optional<ForgedRevision>& parent = std::nullopt,
                            const std::optional<ForgedRevision>& secondParent = std::nullopt,
                            std::uint8_t modelFlag = 0xff) {
        std::vector<std::pair<RevisionId, Hash>> parents;
        if (parent) parents.emplace_back(parent->id, parent->hash);
        if (secondParent) parents.emplace_back(secondParent->id, secondParent->hash);
        const RevisionId id = RevisionId::generate();
        const Hash hash = add(aowis::dvmdb::test::forgedRevisionRecord(
            id, this->project, root, reverse, modelFlag == 0xff ? (model ? 1 : 0) : modelFlag,
            parents));
        return bind(id, hash);
    }
    // A published UUID binding to an arbitrary digest (possibly not a record).
    ForgedRevision bind(const RevisionId& id, const Hash& hash) {
        this->bindings.push_back(ForgedRevision{id, hash});
        return this->bindings.back();
    }
};

// Inject deliberately hash-correct objects without going through the normal
// import validator. This simulates a previously corrupted on-disk repository.
// The pack must still pass its independent checksum verification.
void inject(const std::filesystem::path& directory,
            const ForgedObjects& forged) {
    for (const ForgedRevision& revision : forged.bindings)
        aowis::dvmdb::test::forgeBinding(directory, forged.project, revision.id, revision.hash);
    std::ofstream output(directory / "objects.pack", std::ios::binary | std::ios::app);
    expect(output.good(), "cannot append forged pack objects");
    for (const std::pair<const Hash, Bytes>& entry : forged.entries) {
        Bytes header{'A', 'O', 'B', 'J', 1};
        appendHash(header, entry.first);
        appendU64(header, entry.second.size());
        output.write(reinterpret_cast<const char*>(header.data()),
                     static_cast<std::streamsize>(header.size()));
        output.write(reinterpret_cast<const char*>(entry.second.data()),
                     static_cast<std::streamsize>(entry.second.size()));
        expect(output.good(), "cannot write forged pack entry");
    }
}
void repoint(const std::filesystem::path& directory, const ForgedRevision& tip) {
    ProjectRepository repository(directory);
    aowis::dvmdb::test::repointBranch(repository.native(), directory, "stable", tip.id, tip.hash);
}

enum class Fault {
    InvalidRevision, InvalidRootTag, WrongChildSlot, MissingRawValue,
    InconsistentReferenceIndex, DanglingModelReference, WrongReferenceValue,
    InvalidSecondParent, InvalidFirstParent, MissingRevision,
    InvalidRevisionMetadata, InvalidBranchRef, RawIndexPresent,
    ZeroModelUuid, InvalidModelValue, ZeroIncomingTarget
};

ForgedRevision makeBadHistory(ForgedObjects& objects, Fault fault, const ForgedRevision& goodHead) {
    const Hash value = objects.rawValue();
    const Hash leafOne = objects.leaf(key(1), value);
    const Hash leafTwo = objects.leaf(key(2), value);
    switch (fault) {
    case Fault::InvalidRevision:
        return objects.bind(RevisionId::generate(), value); // valid object, not a revision
    case Fault::InvalidRootTag:
        return objects.revision(objects.add(Bytes{'X'}), Hash{}, false, goodHead);
    case Fault::WrongChildSlot:
        return objects.revision(objects.branch(leafOne, leafTwo, 3), Hash{}, false, goodHead);
    case Fault::MissingRawValue: {
        const Hash missing = aowis::dvmdb::sha256(Bytes{'V', 'a', 'b', 's', 'e', 'n', 't'});
        return objects.revision(objects.leaf(key(1), missing), Hash{}, false, goodHead);
    }
    case Fault::InconsistentReferenceIndex: {
        const Hash a = objects.leaf(key(1), objects.modelValue({}));
        const Hash b = objects.leaf(key(2), objects.modelValue({key(1)}));
        return objects.revision(objects.branch(a, b, 2), Hash{}, true, goodHead);
    }
    case Fault::DanglingModelReference: {
        const Hash model = objects.leaf(key(1), objects.modelValue({key(9)}));
        const Hash incoming = objects.leaf(key(9), objects.inboundValue(key(1)));
        return objects.revision(model, incoming, true, goodHead);
    }
    case Fault::WrongReferenceValue: {
        const Hash a = objects.leaf(key(1), objects.modelValue({}));
        const Hash b = objects.leaf(key(2), objects.modelValue({key(1)}));
        const Hash wrongIncoming = objects.leaf(key(1), objects.rawValue());
        return objects.revision(objects.branch(a, b, 2), wrongIncoming, true, goodHead);
    }
    case Fault::InvalidSecondParent: {
        const ForgedRevision badParent = objects.revision(objects.add(Bytes{'X'}), Hash{}, false);
        return objects.revision(leafOne, Hash{}, false, goodHead, badParent);
    }
    case Fault::InvalidFirstParent: {
        const ForgedRevision badParent = objects.revision(objects.add(Bytes{'X'}), Hash{}, false);
        return objects.revision(leafOne, Hash{}, false, badParent);
    }
    case Fault::MissingRevision:
        return objects.bind(RevisionId::generate(),
                            aowis::dvmdb::sha256(Bytes{'T', 'm', 'i', 's', 's', 'i', 'n', 'g'}));
    case Fault::InvalidRevisionMetadata:
        return objects.revision(leafOne, Hash{}, false, goodHead, std::nullopt, 5); // bad flag
    case Fault::InvalidBranchRef:
        return ForgedRevision{RevisionId::generate(), Hash{}};
    case Fault::RawIndexPresent:
        return objects.revision(leafOne,
            objects.leaf(key(1), objects.inboundValue(key(2))), false, goodHead);
    case Fault::ZeroModelUuid:
        return objects.revision(objects.leaf(Key{}, objects.modelValue({})),
                                Hash{}, true, goodHead);
    case Fault::InvalidModelValue:
        return objects.revision(objects.leaf(key(1), objects.rawValue()),
                                Hash{}, true, goodHead);
    case Fault::ZeroIncomingTarget:
        return objects.revision(objects.leaf(key(1), objects.modelValue({})),
                                objects.leaf(Key{}, objects.inboundValue(key(1))),
                                true, goodHead);
    }
    throw std::runtime_error("unhandled forged history type");
}

void testCorruption(Fault fault, const std::string& reason) {
    TemporaryDirectory temp("corrupt");
    const std::filesystem::path directory = temp.path / "repo";
    std::optional<ForgedObjects> forged;
    ForgedRevision original{RevisionId::generate(), Hash{}};
    {
        ProjectRepository repository(directory);
        repository.createBranch("stable");
        original.id = repository.commit("stable", {{key(42), Bytes{'g', 'o', 'o', 'd'}}});
        original.hash = repository.revision(original.id).recordHash;
        expect(repository.verifyRepository().revisions == 1,
               "original repository does not pass full verification");
        forged.emplace(repository.project());
    }
    const ForgedRevision tip = makeBadHistory(*forged, fault, original);
    inject(directory, *forged);
    if (fault == Fault::InvalidBranchRef) {
        ProjectRepository live(directory);
        aowis::dvmdb::test::overwriteBranch(live.native(), directory, "stable", "invalid-ref");
    } else {
        repoint(directory, tip);
    }
    // The new UUID-only store validates the branch record on open. A broken
    // published record is rejected earlier than full graph verification.
    if (fault == Fault::InvalidBranchRef) {
        bool refused = false;
        try { ProjectRepository invalid(directory); }
        catch (const std::exception&) { refused = true; }
        expect(refused, "invalid UUID branch record survived repository open");
        return;
    }
    ProjectRepository repository(directory);
    repository.verifyPack(); // Physically valid even though logically invalid
    const std::uint64_t objects = repository.objectCount();
    const std::uint64_t bytes = repository.packSize();
    bool rejected = false;
    try {
        (void)repository.verifyRepository();
    } catch (const std::exception& error) {
        rejected = true;
        expect(std::string(error.what()).find(reason) != std::string::npos,
               "wrong verification error: " + std::string(error.what()));
    }
    expect(rejected, "full verifier accepted invalid local repository");
    expect(repository.objectCount() == objects && repository.packSize() == bytes,
           "verification changed physical repository contents");
}

void testValidHistories() {
    TemporaryDirectory temp("valid");
    ProjectRepository repo(temp.path / "repo");
    const VerificationReport empty = repo.verifyRepository();
    expect(empty.branches == 0 && empty.revisions == 0 && empty.physicalObjects == 0,
           "empty repository verification counts are wrong");
    repo.createBranch("main");
    const RevisionId first = repo.commitModel("main", {
        {key(1), ModelRecord{1, {}, Bytes{'a'}}},
        {key(2), ModelRecord{1, {key(1)}, Bytes{'b'}}}});
    repo.createBranch("feature", "main");
    (void)repo.commitModel("feature", {{key(3), ModelRecord{1, {key(1)}, Bytes{'c'}}}});
    (void)repo.commitModel("main", {{key(4), ModelRecord{1, {key(2)}, Bytes{'d'}}}});
    const aowis::dvmdb::MergeResult merge = repo.merge("main", "feature");
    expect(merge.status == aowis::dvmdb::MergeStatus::Merged && merge.resultHead &&
           repo.revision(*merge.resultHead).parents.size() == 2,
           "test fixture did not produce merge");
    const VerificationReport result = repo.verifyRepository();
    expect(result.branches == 2 && result.revisions == 4 &&
           result.modelSnapshots >= 3 && result.treeNodes != 0 &&
           result.reachableObjects >= result.revisions &&
           result.physicalObjects >= result.reachableObjects &&
           repo.head("main") == *merge.resultHead &&
           repo.revision(first).id == first,
           "valid history verification returned incorrect results");
    repo.verifyPack();
    (void)repo.compact();
    const VerificationReport compacted = repo.verifyRepository();
    expect(compacted.branches == result.branches &&
           compacted.revisions == result.revisions &&
           compacted.modelSnapshots == result.modelSnapshots &&
           compacted.reachableObjects == result.reachableObjects,
           "compaction changed the logical verified revision graph");
}

void testCrossBackend() {
    TemporaryDirectory temp("transfer");
    ProjectRepository original(temp.path / "pack");
    original.createBranch("models");
    (void)original.commitModel("models", {
        {key(1), ModelRecord{1, {}, Bytes{'a'}}},
        {key(2), ModelRecord{1, {key(1)}, Bytes{'b'}}}});
    original.exportBundle("models", temp.path / "history.bundle");
    ProjectRepository destination(temp.path / "recipient");
    destination.importBundle(temp.path / "history.bundle", "models");
    expect(destination.verifyRepository().revisions ==
               original.verifyRepository().revisions,
           "full verification differs after independent repository replication");
}

// Physical storage corruption must not be mistaken for a logical failure.
// Full verification runs the physical check first, including unreachable data.
void testPhysicalCorruption() {
    TemporaryDirectory temp("physical");
    const std::filesystem::path directory = temp.path / "repo";
    {
        ProjectRepository repo(directory);
        repo.createBranch("stable");
        (void)repo.commit("stable", {{key(5), Bytes{'x', 'y', 'z'}}});
        (void)repo.verifyRepository();
    }
    const std::filesystem::path pack = directory / "objects.pack";
    expect(std::filesystem::file_size(pack) > 0, "no pack data to corrupt");
    std::fstream stream(pack, std::ios::in | std::ios::out | std::ios::binary);
    expect(stream.good(), "cannot open pack for test corruption");
    stream.seekg(-1, std::ios::end);
    char value = 0;
    stream.read(&value, 1);
    expect(stream.good(), "cannot read pack byte");
    value = static_cast<char>(static_cast<unsigned char>(value) ^ 0xffU);
    stream.seekp(-1, std::ios::end);
    stream.write(&value, 1);
    stream.flush();
    stream.close();
    bool refused = false;
    try {
        ProjectRepository invalid(directory);
        invalid.verifyPack();
        (void)invalid.verifyRepository();
    } catch (const std::exception&) {
        refused = true;
    }
    expect(refused, "physical corruption was accepted");
}

// CLI exit status is part of the administrative integrity contract.
int runCli(const std::string& executable, const std::filesystem::path& directory,
           std::string_view mode) {
    const pid_t child = ::fork();
    expect(child >= 0, "fork failed");
    if (child == 0) {
        const std::string path = directory.string();
        if (mode.empty()) ::execl(executable.c_str(), executable.c_str(), path.c_str(), nullptr);
        else {
            const std::string option(mode);
            ::execl(executable.c_str(), executable.c_str(), option.c_str(), path.c_str(), nullptr);
        }
        ::_exit(127);
    }
    int status = 0;
    expect(::waitpid(child, &status, 0) == child, "waitpid failed");
    expect(WIFEXITED(status), "verifier died unexpectedly");
    return WEXITSTATUS(status);
}

void testCli(const std::string& executable) {
    TemporaryDirectory temp("cli");
    const std::filesystem::path directory = temp.path / "repo";
    std::optional<ForgedObjects> forged;
    ForgedRevision oldHead{RevisionId::generate(), Hash{}};
    {
        ProjectRepository repo(directory);
        repo.createBranch("main");
        oldHead.id = repo.commit("main", {{key(3), Bytes{'o', 'k'}}});
        oldHead.hash = repo.revision(oldHead.id).recordHash;
        forged.emplace(repo.project());
    }
    expect(runCli(executable, directory, "") == 0 &&
           runCli(executable, directory, "--full") == 0 &&
           runCli(executable, directory, "--physical") == 0,
           "valid repository CLI verification failed");
    const ForgedRevision bad = forged->revision(forged->add(Bytes{'X'}), Hash{}, false, oldHead);
    inject(directory, *forged);
    // Repoint the actual branch to the forged tip; invalid content remains checksum-correct.
    {
        ProjectRepository live(directory);
        aowis::dvmdb::test::repointBranch(live.native(), directory, "main", bad.id, bad.hash);
    }
    expect(runCli(executable, directory, "--physical") == 0 &&
           runCli(executable, directory, "--full") == 1 &&
           runCli(executable, directory, "") == 1,
           "CLI conflates physical and logical verification");
}
} // namespace

int main(int argc, char** argv) {
    try {
        expect(argc == 2, "repository verifier test requires CLI executable");
        {
            testValidHistories();
            for (const std::pair<Fault, std::string>& entry :
                 std::vector<std::pair<Fault, std::string>>{
                    {Fault::InvalidRevision, "unsupported revision record header"},
                    {Fault::InvalidRootTag, "invalid bundled tree tag"},
                    {Fault::WrongChildSlot, "wrong slot"},
                    {Fault::MissingRawValue, "missing"},
                    {Fault::InconsistentReferenceIndex, "missing model references"},
                    {Fault::DanglingModelReference, "target does not exist"},
                    {Fault::WrongReferenceValue, "invalid incoming-reference index object"},
                    {Fault::InvalidSecondParent, "invalid bundled tree tag"},
                    {Fault::InvalidFirstParent, "invalid bundled tree tag"},
                    {Fault::MissingRevision, "missing"},
                    {Fault::InvalidRevisionMetadata, "invalid indexing flag"},
                    {Fault::InvalidBranchRef, "invalid branch"},
                    {Fault::RawIndexPresent, "raw revision cannot contain model references"},
                    {Fault::ZeroModelUuid, "model tree contains zero UUID"},
                    {Fault::InvalidModelValue, "invalid native model record"},
                    {Fault::ZeroIncomingTarget, "incoming-reference target is zero"}}) {
                testCorruption(entry.first, entry.second);
            }
            testCli(argv[1]);
            testPhysicalCorruption();
        }
        testCrossBackend();
        std::cout << "Full repository verification tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Full repository verification tests failed: " << error.what() << '\n';
        return 1;
    }
}
