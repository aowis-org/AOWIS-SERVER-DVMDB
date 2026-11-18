#include "aowis/dvmdb/native_store.h"

#ifdef AOWIS_DVMDB_SYNC_WITH_ANNOTATIONS
#include "aowis/dvmdb/map_annotation_snapshot_validation.h"
#endif

#include <cerrno>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using aowis::dvmdb::BranchId;
using aowis::dvmdb::BundleImportStatus;
using aowis::dvmdb::Repository;
using aowis::dvmdb::ProjectId;
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::RevisionBundleImport;

// Canonical CLI accepts only project-scoped UUIDv7 transfer operations.
void usage() {
    std::cerr <<
        "Usage: aowis-dvmdb-sync head REPOSITORY PROJECT_UUID BRANCH\n"
        "       aowis-dvmdb-sync branch-id REPOSITORY PROJECT_UUID BRANCH\n"
        "       aowis-dvmdb-sync export REPOSITORY PROJECT_UUID BRANCH BUNDLE\n"
        "       aowis-dvmdb-sync export-incremental REPOSITORY PROJECT_UUID BRANCH BASE_UUID BUNDLE\n"
        "       aowis-dvmdb-sync import REPOSITORY PROJECT_UUID BUNDLE LOCAL_BRANCH\n";
}

ProjectId requiredProjectId(std::string_view value) {
    const std::optional<ProjectId> parsed = ProjectId::parse(value);
    if (!parsed) throw std::runtime_error("project must be a canonical UUIDv7");
    return *parsed;
}

RevisionId requiredRevisionId(std::string_view value) {
    const std::optional<RevisionId> parsed = RevisionId::parse(value);
    if (!parsed) throw std::runtime_error("revision must be a canonical UUIDv7");
    return *parsed;
}

// Read-only regular-file descriptor for import.
class BundleDescriptor {
public:
    explicit BundleDescriptor(const std::filesystem::path& path) {
        this->fd_ = ::open(path.c_str(), O_CLOEXEC | O_NOFOLLOW | O_RDONLY);
        if (this->fd_ < 0) throw std::system_error(errno, std::generic_category(), "open revision bundle");
        struct stat metadata{};
        if (::fstat(this->fd_, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
            ::close(this->fd_);
            this->fd_ = -1;
            throw std::runtime_error("revision bundle must be a regular file");
        }
    }
    ~BundleDescriptor() {
        if (this->fd_ >= 0) (void)::close(this->fd_);
    }
    BundleDescriptor(const BundleDescriptor&) = delete;
    BundleDescriptor& operator=(const BundleDescriptor&) = delete;
    int fd() const noexcept { return this->fd_; }
private:
    int fd_ = -1;
};

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 4) { usage(); return 2; }
        const std::string command = argv[1];
        if (!((command == "head" && argc == 5) ||
              (command == "branch-id" && argc == 5) ||
              (command == "export" && argc == 6) ||
              (command == "export-incremental" && argc == 7) ||
              (command == "import" && argc == 6))) {
            usage();
            return 2;
        }
        const ProjectId project = requiredProjectId(argv[3]);
        Repository repository(argv[2]);
        if (command == "head") {
            std::cout << repository.head(project, argv[4]).toString() << '\n';
        } else if (command == "branch-id") {
            // Ensure the requested branch belongs to this project's revision DAG.
            (void)repository.head(project, argv[4]);
            const std::optional<BranchId> id = repository.branchId(argv[4]);
            if (!id) throw std::runtime_error("branch has no UUIDv7 identity");
            std::cout << id->toString() << '\n';
        } else if (command == "export" || command == "export-incremental") {
            const bool incremental = command == "export-incremental";
            const std::filesystem::path output = incremental ? argv[6] : argv[5];
            const std::optional<RevisionId> base = incremental
                ? std::optional<RevisionId>(requiredRevisionId(argv[5])) : std::nullopt;
            // Published atomically: a crash never leaves a partial bundle at `output`.
            if (base) repository.exportIncrementalRevisionBundle(project, argv[4], *base, output);
            else repository.exportRevisionBundle(project, argv[4], output);
            std::cout << "exported revision bundle for " << argv[4] << " to " << output.string() << '\n';
        } else {
            BundleDescriptor bundle(argv[4]);
            const RevisionBundleImport result = repository.importRevisionBundle(
                project, bundle.fd(), argv[5], {}, nullptr
#ifdef AOWIS_DVMDB_SYNC_WITH_ANNOTATIONS
                , aowis::dvmdb::annotations::validateAnnotationSnapshot
#endif
            );
            const char* outcome = result.status == BundleImportStatus::Created ? "created" :
                result.status == BundleImportStatus::FastForward ? "fast-forward" : "up-to-date";
            std::cout << outcome << " " << argv[5] << " at " << result.head.toString()
                      << ", imported " << result.objectsImported << " objects\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native synchronization failed: " << error.what() << '\n';
        return 1;
    }
}
