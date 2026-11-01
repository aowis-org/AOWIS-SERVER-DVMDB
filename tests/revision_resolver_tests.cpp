#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/revision_resolver.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace aowis::dvmdb;
namespace fs = std::filesystem;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Fn>
void expectError(Fn&& fn, StorageErrorKind kind, const char* message) {
    try { fn(); }
    catch (const StorageError& error) {
        if (error.kind() == kind) return;
        std::cerr << message << ": actual error kind " << StorageError::kindName(error.kind()) << '\n';
        throw;
    }
    throw std::runtime_error(message);
}

struct Sandbox final {
    fs::path path;
    Sandbox() {
        this->path = fs::temp_directory_path() / ("aowis-v2-resolver-" + UuidV7::generate().toString());
        fs::create_directories(this->path);
    }
    ~Sandbox() { std::error_code ec; fs::remove_all(this->path, ec); }
};

struct ObjectPack final {
    std::map<Hash, Bytes> objects;
    mutable std::size_t reads = 0;
    void add(const Bytes& bytes) { this->objects.emplace(sha256(bytes), bytes); }
    std::optional<Bytes> read(const Hash& hash) const {
        ++this->reads;
        const std::map<Hash, Bytes>::const_iterator i = this->objects.find(hash);
        if (i == this->objects.end()) return std::nullopt;
        return i->second;
    }
    RevisionObjectReader reader() const {
        return [this](const Hash& hash) { return read(hash); };
    }
};

RevisionRecord makeRecord(ProjectId project, RevisionId revision) {
    RevisionRecord record{revision, project, Hash{}, Hash{}, false, {},
                          aowis::dvmdb::test::commitMetadata(), 1793491823009ULL};
    record.root.fill(0x12);
    return record;
}

void testPersistedGraphAndProjectBinding() {
    Sandbox sandbox;
    const ProjectId project = ProjectId::generate();
    const RevisionId rootId = RevisionId::generate();
    const RevisionId childId = RevisionId::generate();
    const RevisionId mergeId = RevisionId::generate();
    const RevisionId siblingId = RevisionId::generate();
    ObjectPack pack;
    const RevisionRecord root = makeRecord(project, rootId);
    const Bytes rootBytes = encodeRevisionRecord(root);
    pack.add(rootBytes);
    RevisionRecord child = makeRecord(project, childId);
    child.parents.push_back({rootId, sha256(rootBytes)});
    const Bytes childBytes = encodeRevisionRecord(child);
    pack.add(childBytes);
    RevisionRecord sibling = makeRecord(project, siblingId);
    sibling.parents.push_back({rootId, sha256(rootBytes)});
    const Bytes siblingBytes = encodeRevisionRecord(sibling);
    pack.add(siblingBytes);
    RevisionRecord merge = makeRecord(project, mergeId);
    merge.parents.push_back({childId, sha256(childBytes)});
    merge.parents.push_back({siblingId, sha256(siblingBytes)});
    const Bytes mergeBytes = encodeRevisionRecord(merge);
    pack.add(mergeBytes);

    {
        RevisionResolver index(sandbox.path, project);
        check(!index.resolve(rootId, pack.reader()).has_value(), "unknown revision resolved");
        check(index.publish(rootBytes, pack.reader()) == sha256(rootBytes), "root digest incorrect");
        check(index.publish(rootBytes, pack.reader()) == sha256(rootBytes), "idempotent root publication failed");
        check(index.publish(childBytes, pack.reader()) == sha256(childBytes), "child digest incorrect");
        check(index.publish(siblingBytes, pack.reader()) == sha256(siblingBytes), "sibling digest incorrect");
        check(index.publish(mergeBytes, pack.reader()) == sha256(mergeBytes), "merge digest incorrect");
        // A lookup is O(1): it reads only the revision's own record, however
        // deep the ancestry is. Publication already proved every parent.
        pack.reads = 0;
        const std::optional<ResolvedRevision> resolved = index.resolve(mergeId, pack.reader());
        check(resolved && resolved->record.parents.size() == 2 &&
              resolved->recordHash == sha256(mergeBytes), "merge ancestry not resolved");
        check(pack.reads == 1, "revision lookup read more than its own record");
        check(index.contains(mergeId) && !index.contains(RevisionId::generate()),
              "binding presence check is wrong");
        RevisionRecord malicious = root;
        malicious.root[0] = 0x15;
        const Bytes otherBytes = encodeRevisionRecord(malicious);
        pack.add(otherBytes);
        expectError([&] { (void)index.publish(otherBytes, pack.reader()); },
                    StorageErrorKind::Conflict, "revision UUID was rebound");
        RevisionRecord foreign = makeRecord(ProjectId::generate(), RevisionId::generate());
        const Bytes foreignBytes = encodeRevisionRecord(foreign);
        pack.add(foreignBytes);
        expectError([&] { (void)index.publish(foreignBytes, pack.reader()); },
                    StorageErrorKind::InvalidInput, "foreign project revision published");
    }
    {
        RevisionResolver reopened(sandbox.path, project);
        check(reopened.resolve(mergeId, pack.reader()).has_value(), "bindings lost on restart");
        check(fs::exists(sandbox.path / "revision-v2" / "ids" / mergeId.toString()),
              "UUIDv7 file not created");
        check(!fs::exists(sandbox.path / "revision-v2" / "ids" / "main"),
              "user-supplied branch name used as filename");
    }
    expectError([&] { RevisionResolver wrong(sandbox.path, ProjectId::generate()); },
                StorageErrorKind::Conflict, "repository project identity was not pinned");
    {
        const fs::path manifest = sandbox.path / "revision-v2" / "project";
        const fs::path missing = sandbox.path / "revision-v2" / "project.missing";
        fs::rename(manifest, missing);
        expectError([&] { RevisionResolver wrong(sandbox.path, ProjectId::generate()); },
                    StorageErrorKind::Corruption, "missing project manifest was silently reinitialized");
        check(!fs::exists(manifest), "damaged project manifest was recreated");
        fs::rename(missing, manifest);
    }

    // Resolution fails closed when the immutable record it serves is missing or
    // altered. Damage to an ancestor is reported where that ancestor is read
    // (and by Repository::verifyRepository), not by every descendant lookup.
    pack.objects.erase(sha256(siblingBytes));
    RevisionResolver reopened(sandbox.path, project);
    check(reopened.resolve(mergeId, pack.reader()).has_value(),
          "descendant lookup walked into damaged ancestry");
    expectError([&] { (void)reopened.resolve(siblingId, pack.reader()); },
                StorageErrorKind::Corruption, "missing revision record accepted");
    pack.objects[sha256(siblingBytes)] = rootBytes;
    expectError([&] { (void)reopened.resolve(siblingId, pack.reader()); },
                StorageErrorKind::Corruption, "altered revision record accepted");
    // A damaged parent binding is detected by the child lookup itself.
    const fs::path siblingBinding = sandbox.path / "revision-v2" / "ids" / siblingId.toString();
    {
        std::fstream altered(siblingBinding, std::ios::binary | std::ios::in | std::ios::out);
        altered.seekp(40);
        altered.put('\x5a');
    }
    expectError([&] { (void)reopened.resolve(mergeId, pack.reader()); },
                StorageErrorKind::Corruption, "altered parent binding accepted");
}

void testMissingStoreAndInvalidParents() {
    Sandbox sandbox;
    const ProjectId project = ProjectId::generate();
    RevisionResolver index(sandbox.path, project);
    ObjectPack pack;
    const RevisionId rootId = RevisionId::generate();
    const RevisionRecord root = makeRecord(project, rootId);
    const Bytes rootBytes = encodeRevisionRecord(root);
    expectError([&] { (void)index.publish(rootBytes, pack.reader()); },
                StorageErrorKind::Corruption, "unpersisted revision published");
    check(!index.resolve(rootId, pack.reader()).has_value(), "published unpersisted revision");
    expectError([&] { (void)index.resolve(rootId, {}); },
                StorageErrorKind::InvalidInput, "missing reader was not rejected");
    pack.add(rootBytes);
    RevisionRecord child = makeRecord(project, RevisionId::generate());
    child.parents.push_back({rootId, sha256(rootBytes)});
    const Bytes childBytes = encodeRevisionRecord(child);
    pack.add(childBytes);
    expectError([&] { (void)index.publish(childBytes, pack.reader()); },
                StorageErrorKind::Corruption, "unmapped parent accepted");
    (void)index.publish(rootBytes, pack.reader());
    (void)index.publish(childBytes, pack.reader());
    RevisionRecord wrongParent = makeRecord(project, RevisionId::generate());
    Hash bogus{};
    bogus[0] = 1;
    wrongParent.parents.push_back({rootId, bogus});
    const Bytes wrongBytes = encodeRevisionRecord(wrongParent);
    pack.add(wrongBytes);
    expectError([&] { (void)index.publish(wrongBytes, pack.reader()); },
                StorageErrorKind::Corruption, "incorrect parent SHA accepted");
}

void testCorruptAndUnsafeFiles() {
    Sandbox sandbox;
    const ProjectId project = ProjectId::generate();
    const RevisionId revisionId = RevisionId::generate();
    ObjectPack pack;
    const Bytes bytes = encodeRevisionRecord(makeRecord(project, revisionId));
    pack.add(bytes);
    RevisionResolver index(sandbox.path, project);
    const fs::path ids = sandbox.path / "revision-v2" / "ids";
    const fs::path binding = ids / revisionId.toString();
    {
        std::ofstream invalid(binding);
        invalid << "bad";
    }
    expectError([&] { (void)index.resolve(revisionId, pack.reader()); },
                StorageErrorKind::Corruption, "truncated mapping accepted");
    fs::remove(binding);
    fs::create_symlink("nonexistent", binding);
    expectError([&] { (void)index.resolve(revisionId, pack.reader()); },
                StorageErrorKind::Corruption, "symlink mapping accepted");
    fs::remove(binding);
    {
        std::ofstream staged(ids / (".stage-" + RevisionId::generate().toString()));
        staged << "partial";
    }
    check(!index.resolve(revisionId, pack.reader()), "uncommitted staging entry became visible");
    (void)index.publish(bytes, pack.reader());
    check(index.resolve(revisionId, pack.reader()).has_value(), "private staging residue prevented publish");
    {
        std::fstream altered(binding, std::ios::binary | std::ios::in | std::ios::out);
        altered.seekp(20);
        altered.put('x');
    }
    expectError([&] { (void)index.resolve(revisionId, pack.reader()); },
                StorageErrorKind::Corruption, "altered UUID/project binding accepted");
}

void testConcurrentPublish() {
    Sandbox sandbox;
    const ProjectId project = ProjectId::generate();
    const RevisionId revisionId = RevisionId::generate();
    ObjectPack pack;
    const Bytes bytes = encodeRevisionRecord(makeRecord(project, revisionId));
    pack.add(bytes);
    RevisionResolver first(sandbox.path, project);
    RevisionResolver second(sandbox.path, project);
    std::vector<std::exception_ptr> failures(2);
    std::thread a([&] { try { (void)first.publish(bytes, pack.reader()); }
                       catch (...) { failures[0] = std::current_exception(); } });
    std::thread b([&] { try { (void)second.publish(bytes, pack.reader()); }
                       catch (...) { failures[1] = std::current_exception(); } });
    a.join();
    b.join();
    for (const std::exception_ptr& e : failures) if (e) std::rethrow_exception(e);
    check(first.resolve(revisionId, pack.reader()).has_value(), "parallel publication failed");
}

} // namespace

int main() {
    try {
        testPersistedGraphAndProjectBinding();
        testMissingStoreAndInvalidParents();
        testCorruptAndUnsafeFiles();
        testConcurrentPublish();
        std::cout << "UUIDv7 revision resolver persistence tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UUIDv7 revision resolver FAILED:" << error.what() << '\n';
        return 1;
    }
}
