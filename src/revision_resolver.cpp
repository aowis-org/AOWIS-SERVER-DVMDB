#include "aowis/dvmdb/revision_resolver.h"

#include "aowis/dvmdb/storage_error.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include "dvmdb_sys.h"
#include <utility>

namespace aowis::dvmdb {
namespace {

using Err = StorageErrorKind;

void ensure(bool okay, Err kind, const char* reason) {
    if (!okay) throw StorageError(kind, reason);
}

class Fd final {
public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}
    ~Fd() { if (this->fd_ >= 0) (void)sys::close(this->fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const noexcept { return this->fd_; }
    int release() noexcept { const int result = this->fd_; this->fd_ = -1; return result; }
private:
    int fd_;
};

class FileLock final {
public:
    explicit FileLock(int fd) : fd_(fd) {
        if (sys::flock(this->fd_, LOCK_EX) != 0) throwSystemError("lock revision resolver");
    }
    ~FileLock() { (void)sys::flock(this->fd_, LOCK_UN); }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
private:
    int fd_;
};

void syncDir(int fd) {
    if (sys::fsync(fd) != 0) throwSystemError("sync revision resolver directory");
}

void writeAll(int fd, const std::uint8_t* bytes, std::size_t size) {
    std::size_t offset = 0;
    while (offset != size) {
        const ssize_t n = sys::write(fd, bytes + offset, size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throwSystemError("write revision resolver record");
        ensure(n > 0, Err::Io, "short revision resolver write");
        offset += static_cast<std::size_t>(n);
    }
    if (sys::fsync(fd) != 0) throwSystemError("sync revision resolver record");
}

// Reading a fixed-size regular file with O_NOFOLLOW also rules out symlink
// attacks and unbounded/malformed record sizes.
std::optional<Bytes> readFixed(int dirFd, const std::string& name, std::size_t expected) {
    Fd file(sys::openat(dirFd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.get() < 0 && errno == ENOENT) return std::nullopt;
    if (file.get() < 0 && (errno == ELOOP || errno == ENOTDIR))
        throw StorageError(Err::Corruption, "unsafe revision resolver entry");
    if (file.get() < 0) throwSystemError("read revision resolver entry");
    sys::Stat st{};
    if (sys::fstat(file.get(), &st) != 0) throwSystemError("stat revision resolver entry");
    ensure(S_ISREG(st.st_mode) && st.st_size == static_cast<off_t>(expected),
           Err::Corruption, "bad revision resolver entry size or type");
    Bytes bytes(expected);
    std::size_t offset = 0;
    while (offset != bytes.size()) {
        const ssize_t n = sys::read(file.get(), bytes.data() + offset, bytes.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throwSystemError("read revision resolver entry");
        ensure(n > 0, Err::Corruption, "truncated revision resolver entry");
        offset += static_cast<std::size_t>(n);
    }
    return bytes;
}

// Names in this module are *only* fixed internal tokens or canonical UUIDv7
// values. No user-facing project/branch/revision label is ever a path segment.
int openDirectory(int parent, const char* name, bool& created) {
    created = sys::mkdirat(parent, name, 0700) == 0;
    if (!created && errno != EEXIST)
        throwSystemError("create revision resolver directory");
    const int fd = sys::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && (errno == ELOOP || errno == ENOTDIR))
        throw StorageError(Err::Corruption, "unsafe revision resolver directory");
    if (fd < 0) throwSystemError("open revision resolver directory");
    return fd;
}

// Publication is no-replace: data is fsynced *before* it becomes visible;
// after linkat+directory fsync, a returned success is durable. A leftover
// private staging name is inert after crash and may be cleaned independently.
void publishOnce(int dir, const std::string& destination, const Bytes& payload) {
    const std::string temporary = ".stage-" + UuidV7::generate().toString();
    Fd staged(sys::openat(dir, temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY |
                                   O_NOFOLLOW | O_CLOEXEC, 0600));
    if (staged.get() < 0) throwSystemError("stage revision resolver record");
    try {
        writeAll(staged.get(), payload.data(), payload.size());
        if (sys::linkat(dir, temporary.c_str(), dir, destination.c_str(), 0) != 0) {
            if (errno == EEXIST)
                throw StorageError(Err::Conflict, "revision UUID binding already exists");
            throwSystemError("publish revision resolver record");
        }
        syncDir(dir);
    } catch (...) {
        (void)sys::unlinkat(dir, temporary.c_str(), 0);
        throw;
    }
    (void)sys::unlinkat(dir, temporary.c_str(), 0);
}

// Manifest: 'P','R','J',2 + owning project UUIDv7 = 20 bytes.
// Binding:  'R','I','D',2 + revision UUIDv7 + project UUIDv7 + SHA256 = 68 bytes.
Bytes manifestBytes(const ProjectId& project) {
    Bytes bytes(20, 0);
    bytes[0] = 'P'; bytes[1] = 'R'; bytes[2] = 'J'; bytes[3] = 2;
    std::copy(project.uuid().bytes.begin(), project.uuid().bytes.end(), bytes.begin() + 4);
    return bytes;
}

Bytes bindingBytes(const RevisionId& id, const ProjectId& project, const Hash& hash) {
    Bytes bytes(68, 0);
    bytes[0] = 'R'; bytes[1] = 'I'; bytes[2] = 'D'; bytes[3] = 2;
    std::copy(id.uuid().bytes.begin(), id.uuid().bytes.end(), bytes.begin() + 4);
    std::copy(project.uuid().bytes.begin(), project.uuid().bytes.end(), bytes.begin() + 20);
    std::copy(hash.begin(), hash.end(), bytes.begin() + 36);
    return bytes;
}

} // namespace

RevisionResolver::RevisionResolver(const std::filesystem::path& repository, ProjectId project)
    : project_(std::move(project)) {
    // The repository directory must already exist and may not itself be a
    // symbolic link. The caller owns its higher-level repository path.
    if (std::filesystem::is_symlink(repository))
        throw StorageError(Err::Corruption, "symlinked revision repository");
    ensure(ProjectId::parse(this->project_.toString()).has_value(), Err::InvalidInput,
           "invalid project UUIDv7");
    Fd root(sys::open(repository.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (root.get() < 0) throwSystemError("open revision repository");
    bool createdNamespace = false;
    Fd namespaceDir(openDirectory(root.get(), "revision-v2", createdNamespace));
    // Durable directory ancestry: a successful publication must survive loss
    // of the containing directory entries after a sudden power cut. Only a
    // newly created entry needs the parent sync; lookups never fsync.
    if (createdNamespace) syncDir(root.get());
    Fd lock(sys::openat(namespaceDir.get(), "lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (lock.get() < 0) throwSystemError("open revision resolver lock");
    sys::Stat lockStat{};
    if (sys::fstat(lock.get(), &lockStat) != 0) throwSystemError("stat revision resolver lock");
    ensure(S_ISREG(lockStat.st_mode), Err::Corruption, "invalid revision resolver lock file");
    FileLock exclusive(lock.get());
    const Bytes expected = manifestBytes(this->project_);
    const std::optional<Bytes> recorded = readFixed(namespaceDir.get(), "project", expected.size());
    bool publishedManifest = false;
    if (recorded) {
        ensure(*recorded == expected, Err::Conflict, "revision repository project UUID mismatch");
    } else {
        // This namespace can only lack a manifest during first creation.
        // Never silently rebind an existing UUID index after manifest damage.
        sys::Stat orphaned{};
        if (sys::fstatat(namespaceDir.get(), "ids", &orphaned, AT_SYMLINK_NOFOLLOW) == 0)
            throw StorageError(Err::Corruption, "revision project manifest missing from existing index");
        if (errno != ENOENT) throwSystemError("check revision identity namespace");
        publishOnce(namespaceDir.get(), "project", expected);
        publishedManifest = true;
    }
    bool createdIdentities = false;
    Fd identities(openDirectory(namespaceDir.get(), "ids", createdIdentities));
    if (createdIdentities || publishedManifest) syncDir(namespaceDir.get());
    this->identitiesFd_ = identities.release();
    this->dirFd_ = namespaceDir.release();
    this->lockFd_ = lock.release();
}

RevisionResolver::~RevisionResolver() {
    if (this->identitiesFd_ >= 0) (void)sys::close(this->identitiesFd_);
    if (this->dirFd_ >= 0) (void)sys::close(this->dirFd_);
    if (this->lockFd_ >= 0) (void)sys::close(this->lockFd_);
}

const ProjectId& RevisionResolver::project() const noexcept { return this->project_; }

std::optional<Hash> RevisionResolver::readBindingLocked(const RevisionId& id) const {
    const std::string name = id.toString();
    ensure(RevisionId::parse(name).has_value(), Err::InvalidInput,
           "invalid revision UUIDv7 lookup");
    const std::optional<Bytes> bytes = readFixed(this->identitiesFd_, name, 68);
    if (!bytes) return std::nullopt;
    ensure((*bytes)[0] == 'R' && (*bytes)[1] == 'I' && (*bytes)[2] == 'D' && (*bytes)[3] == 2,
           Err::Corruption, "invalid revision resolver binding version");
    const Bytes expected = bindingBytes(id, this->project_, Hash{});
    ensure(std::equal(bytes->begin() + 4, bytes->begin() + 36, expected.begin() + 4),
           Err::Corruption, "wrong UUID or project in revision resolver binding");
    Hash hash{};
    std::copy_n(bytes->begin() + 36, 32, hash.begin());
    ensure(hash != Hash{}, Err::Corruption, "zero revision SHA-256 binding");
    return hash;
}

// The stored record must be the exact SHA-addressed immutable object, and
// every parent must already be published with the parent digest recorded in
// this record. Parents were checked the same way when they were published,
// so the whole DAG is valid by induction without walking the ancestry.
void RevisionResolver::verifyRecordLocked(const RevisionRecord& record, const Hash& digest,
                                            const RevisionObjectReader& readObject) const {
    const std::optional<Bytes> stored = readObject(digest);
    ensure(stored.has_value() && sha256(*stored) == digest,
           Err::Corruption, "missing or altered immutable revision record");
    const RevisionRecord decoded = decodeRevisionRecord(*stored);
    ensure(decoded.id == record.id && decoded.project == this->project_,
           Err::Corruption, "revision record UUID or project mismatch");
    for (const RevisionParent& parent : decoded.parents) {
        const std::optional<Hash> binding = readBindingLocked(parent.id);
        ensure(binding.has_value() && *binding == parent.expectedRecordHash,
               Err::Corruption, "parent UUID/SHA binding mismatch");
    }
}

Hash RevisionResolver::publish(const Bytes& canonicalRecord,
                                  const RevisionObjectReader& readObject) {
    ensure(static_cast<bool>(readObject), Err::InvalidInput,
           "missing immutable revision object reader");
    const RevisionRecord record = decodeRevisionRecord(canonicalRecord);
    ensure(record.project == this->project_, Err::InvalidInput, "cross-project revision publication");
    const Hash digest = sha256(canonicalRecord);
    FileLock exclusive(this->lockFd_);
    const std::optional<Hash> existing = readBindingLocked(record.id);
    if (existing) {
        ensure(*existing == digest, Err::Conflict, "attempt to rebind immutable revision UUID");
        verifyRecordLocked(record, digest, readObject);
        return digest;
    }
    verifyRecordLocked(record, digest, readObject);
    publishOnce(this->identitiesFd_, record.id.toString(),
                bindingBytes(record.id, this->project_, digest));
    return digest;
}

bool RevisionResolver::contains(const RevisionId& id) const {
    FileLock exclusive(this->lockFd_);
    return readBindingLocked(id).has_value();
}

std::optional<ResolvedRevision> RevisionResolver::resolve(
    const RevisionId& id, const RevisionObjectReader& readObject) const {
    ensure(static_cast<bool>(readObject), Err::InvalidInput,
           "missing immutable revision object reader");
    FileLock exclusive(this->lockFd_);
    const std::optional<Hash> digest = readBindingLocked(id);
    if (!digest) return std::nullopt;
    const std::optional<Bytes> stored = readObject(*digest);
    ensure(stored.has_value() && sha256(*stored) == *digest,
           Err::Corruption, "missing or altered immutable revision record");
    const RevisionRecord record = decodeRevisionRecord(*stored);
    ensure(record.id == id && record.project == this->project_, Err::Corruption,
           "resolved revision UUID or project mismatch");
    for (const RevisionParent& parent : record.parents) {
        const std::optional<Hash> binding = readBindingLocked(parent.id);
        ensure(binding.has_value() && *binding == parent.expectedRecordHash,
               Err::Corruption, "parent UUID/SHA binding mismatch");
    }
    return ResolvedRevision{record, *digest};
}

} // namespace aowis::dvmdb
