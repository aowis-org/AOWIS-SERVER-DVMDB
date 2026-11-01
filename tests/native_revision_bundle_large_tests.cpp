#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/native_store.h"
#include "aowis/dvmdb/storage_error.h"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

using aowis::dvmdb::test::commitMetadata;

namespace {
namespace fs = std::filesystem;
using namespace aowis::dvmdb;

void check(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

struct WorkingDirectory {
    fs::path root = fs::temp_directory_path() /
                    ("aowis-revision-disk-stage-" + UuidV7::generate().toString());
    WorkingDirectory() { fs::create_directories(this->root); }
    ~WorkingDirectory() { std::error_code failure; fs::remove_all(this->root, failure); }
};

struct File {
    int fd;
    File(const fs::path& path, int flags)
        : fd(::open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW, 0600)) {
        if (this->fd < 0) throw std::runtime_error("cannot open staging file");
    }
    ~File() { (void)::close(this->fd); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};

void run() {
    WorkingDirectory work;
    const ProjectId project = ProjectId::generate();
    Repository source(work.root / "source");
    const RevisionId initial = source.createRootBranch(project, "main", commitMetadata());
    // Distinct incompressible-looking raw model values. The old fixed 64 MiB
    // *bundle* ceiling must not reject a bounded, disk-backed transfer.
    std::vector<Change> changes;
    for (unsigned index = 0; index < 18; ++index) {
        Key key{};
        key[15] = static_cast<std::uint8_t>(index + 1);
        Bytes value(4U * 1024U * 1024U);
        for (std::size_t position = 0; position < value.size(); ++position)
            value[position] = static_cast<std::uint8_t>((position * 131U + index * 17U) & 255U);
        changes.push_back(Change{key, std::move(value)});
    }
    const RevisionId head = source.commit(project, "main", changes, commitMetadata(), initial);
    changes.clear();
    const fs::path fullPath = work.root / "full.bundle";
    {
        File output(fullPath, O_RDWR | O_CREAT | O_EXCL);
        source.exportRevisionBundle(project, "main", output.fd);
    }
    check(fs::file_size(fullPath) > 64ULL * 1024ULL * 1024ULL,
          "regression fixture did not exceed the old 64 MiB ceiling");
    Repository target(work.root / "target");
    {
        File input(fullPath, O_RDONLY);
        const RevisionBundleImport result = target.importRevisionBundle(project, input.fd, "main");
        check(result.status == BundleImportStatus::Created && result.head == head,
              "disk-backed full import failed");
    }
    check(target.head(project, "main") == head &&
          target.branchId("main") == source.branchId("main"),
          "large bundle changed revision/branch UUIDv7 identities");
    check(target.verifyRepository().revisions >= 2,
          "large disk-backed import failed repository integrity check");
    // Caller-provided smaller budgets still reject the same file cleanly.
    Repository limited(work.root / "limited");
    {
        File input(fullPath, O_RDONLY);
        try {
            (void)limited.importRevisionBundle(project, input.fd, "main",
                  BundleLimits{32ULL * 1024ULL * 1024ULL, 0, 0});
            throw std::runtime_error("configured import byte ceiling ignored");
        } catch (const StorageError& error) {
            check(error.kind() == StorageErrorKind::LimitExceeded,
                  "large import returned wrong budget error");
        }
    }
    check(!limited.hasBranch("main"), "rejected large bundle published a HEAD");
}
} // namespace

int main() {
    try {
        run();
        std::cout << "UUIDv7 disk-backed >64 MiB revision transfer passed\n";
    } catch (const std::exception& error) {
        std::cerr << "UUIDv7 large revision bundle FAILED: " << error.what() << '\n';
        return 1;
    }
}
