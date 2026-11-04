#include "branch_name_validation.h"
#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/object_id.h"
#include "aowis/dvmdb/revision_record.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <limits>
#include <map>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>
#include <set>
#include <tuple>
#include <memory>
#include <optional>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace aowis::dvmdb {
namespace {

// Catalogue metadata on disk violating its format is storage corruption.
void check(bool ok, const std::string& message,
           StorageErrorKind kind = StorageErrorKind::Corruption) {
    if (!ok) throw StorageError(kind, message);
}

void syscallChecked(bool ok, const char* action) {
    if (!ok) throwSystemError(action);
}

bool validName(std::string_view name) {
    if (name.empty() || name.size() > 200) return false;
    // Reject malformed UTF-8, overlong sequences, surrogates and scalar values
    // above U+10FFFF. A name must round-trip through the JSON HTTP boundary.
    for (std::size_t i = 0; i < name.size();) {
        const unsigned char first = static_cast<unsigned char>(name[i]);
        if (first < 0x80U) {
            if (first < 0x20U || first == 0x7fU) return false;
            ++i;
            continue;
        }
        unsigned count = 0;
        if (first >= 0xc2U && first <= 0xdfU) count = 2;
        else if (first >= 0xe0U && first <= 0xefU) count = 3;
        else if (first >= 0xf0U && first <= 0xf4U) count = 4;
        else return false;
        if (i + count > name.size()) return false;
        for (unsigned step = 1; step < count; ++step) {
            const unsigned char byte = static_cast<unsigned char>(name[i + step]);
            if ((byte & 0xc0U) != 0x80U) return false;
        }
        const unsigned char second = static_cast<unsigned char>(name[i + 1]);
        if ((first == 0xe0U && second < 0xa0U) ||
            (first == 0xedU && second > 0x9fU) ||
            (first == 0xf0U && second < 0x90U) ||
            (first == 0xf4U && second > 0x8fU)) return false;
        i += count;
    }
    return true;
}

std::string encodeName(std::string_view name) {
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    value.reserve(name.size() * 2);
    for (unsigned char ch : name) {
        value.push_back(digits[ch >> 4]);
        value.push_back(digits[ch & 15U]);
    }
    return value;
}

std::string decodeName(const std::string& encoded) {
    check(encoded.size() % 2 == 0 && encoded.size() <= 400,
          "invalid project metadata name encoding");
    std::string output;
    for (std::size_t i = 0; i < encoded.size(); i += 2) {
        const std::string pair = encoded.substr(i, 2);
        check(std::all_of(pair.begin(), pair.end(), [](char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        }), "invalid project metadata name encoding");
        output.push_back(static_cast<char>(std::stoi(pair, nullptr, 16)));
    }
    check(validName(output), "invalid project metadata name");
    return output;
}

void syncDirectory(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    syscallChecked(fd >= 0, "open directory for sync");
    const int status = ::fsync(fd);
    const int savedErrno = errno;
    ::close(fd);
    errno = savedErrno;
    syscallChecked(status == 0, "sync project directory");
}

// Removes entries with one of `prefixes` from `directory`: files and
// symlinks by unlinking (never following), staged directories recursively
// when `directories` is set. Returns whether anything was removed.
bool removePrefixed(const std::filesystem::path& directory,
                    const std::vector<std::string_view>& prefixes, bool directories) {
    if (std::filesystem::is_symlink(directory) || !std::filesystem::is_directory(directory))
        return false;
    bool removed = false;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (std::none_of(prefixes.begin(), prefixes.end(),
                         [&name](std::string_view prefix) { return name.starts_with(prefix); }))
            continue;
        if (!entry.is_symlink() && entry.is_directory()) {
            if (!directories) continue;
            std::filesystem::remove_all(entry.path());
        } else {
            syscallChecked(::unlink(entry.path().c_str()) == 0 || errno == ENOENT,
                           "remove staged catalogue entry");
        }
        removed = true;
    }
    return removed;
}

// Crash remnants of the no-replace publication protocol: staged repository
// identity, local Commit records and unpublished project directories (drafts:
// removeDraftRemnants). None was ever visible, so removing them loses
// nothing; it also lets a retry of an interrupted caller-chosen project
// creation proceed.
void removeCatalogueRemnants(const std::filesystem::path& root) {
    if (removePrefixed(root, {".repository-id-creating-"}, false)) syncDirectory(root);
    if (removePrefixed(root / "local-commits", {".creating-", ".updating-"}, false))
        syncDirectory(root / "local-commits");
    if (removePrefixed(root / "local-commit-revisions", {".creating-"}, false))
        syncDirectory(root / "local-commit-revisions");
    if (removePrefixed(root / "projects", {".creating-"}, true)) syncDirectory(root / "projects");
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(root / "projects")) {
        if (entry.is_directory() && !entry.is_symlink() &&
            removePrefixed(entry.path(), {".project-meta-updating-"}, false))
            syncDirectory(entry.path());
    }
}

void writeManifest(const std::filesystem::path& path, const ProjectInfo& info) {
    const std::string body = "AOWIS-PROJECT-1\n" + info.id + "\n" +
        "pack\n" +
        encodeName(info.name) + "\n";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    syscallChecked(fd >= 0, "create project manifest");
    try {
        std::size_t written = 0;
        while (written < body.size()) {
            const ssize_t amount = ::write(fd, body.data() + written, body.size() - written);
            if (amount < 0 && errno == EINTR) continue;
            syscallChecked(amount > 0, "write project manifest");
            written += static_cast<std::size_t>(amount);
        }
        syscallChecked(::fsync(fd) == 0, "sync project manifest");
    } catch (...) {
        ::close(fd);
        throw;
    }
    syscallChecked(::close(fd) == 0, "close project manifest");
}

// Persistent service-root identity is independent of project and branch IDs.
// Its first publication is exclusive and durable. Older catalogues without
// this file receive an ID on their first open; an existing malformed file is
// corruption and must never be silently replaced with a new identity.
std::string loadRepositoryIdentity(const std::filesystem::path& root) {
    const std::filesystem::path path = root / "repository.id";
    const std::function<std::string()> readExisting = [&]() -> std::string {
        check(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path),
              "unsafe DVMDB repository identity file: " + path.string());
        check(std::filesystem::file_size(path) <= 80,
              "oversized DVMDB repository identity: " + path.string());
        std::ifstream stream(path, std::ios::binary);
        check(stream.good(), "cannot read DVMDB repository identity");
        const std::string data((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
        constexpr std::string_view marker = "AOWIS-REPOSITORY-1\n";
        check(data.starts_with(marker) && data.size() == marker.size() + 37
              && data.back() == '\n', "invalid DVMDB repository identity format");
        const std::string id = data.substr(marker.size(), 36);
        check(UuidV7::parse(id).has_value(), "invalid DVMDB repository UUIDv7");
        return id;
    };
    if (std::filesystem::exists(path) || std::filesystem::is_symlink(path))
        return readExisting();

    const std::string id = UuidV7::generate().toString();
    const std::string body = "AOWIS-REPOSITORY-1\n" + id + "\n";
    // Prepare on the same filesystem and publish only fully synchronized bytes.
    // A crash before link() cannot expose a partial authoritative identity.
    const std::filesystem::path staging = root / (".repository-id-creating-" + id);
    const int fd = ::open(staging.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    syscallChecked(fd >= 0, "stage DVMDB repository identity");
    try {
        std::size_t written = 0;
        while (written < body.size()) {
            const ssize_t amount = ::write(fd, body.data() + written, body.size() - written);
            if (amount < 0 && errno == EINTR) continue;
            syscallChecked(amount > 0, "write DVMDB repository identity");
            written += static_cast<std::size_t>(amount);
        }
        syscallChecked(::fsync(fd) == 0, "sync DVMDB repository identity");
    } catch (...) {
        ::close(fd);
        throw;
    }
    syscallChecked(::close(fd) == 0, "close DVMDB repository identity");
    syscallChecked(::link(staging.c_str(), path.c_str()) == 0,
                   "publish DVMDB repository identity");
    syncDirectory(root);
    syscallChecked(::unlink(staging.c_str()) == 0, "remove DVMDB identity staging file");
    syncDirectory(root);
    return id;
}

// VC-03 root-level tracking schema. These are metadata observations, not
// authoritative refs. The only writable branch HEAD lives in this root's
// native Repository. Each remote reference is qualified by remote repository,
// logical project and logical branch UUIDv7, never by a mutable name.
using TrackingKeyOnDisk = std::tuple<std::string, std::string, std::string>;
struct TrackingStateOnDisk {
    RepositoryRole role = RepositoryRole::Unconfigured;
    std::map<std::string, RemoteRepository> remotes;
    std::map<TrackingKeyOnDisk, RemoteTrackingRef> refs;
};

std::string roleToken(RepositoryRole role) {
    switch (role) {
    case RepositoryRole::Unconfigured: return "unconfigured";
    case RepositoryRole::Private: return "private";
    case RepositoryRole::Shared: return "shared";
    }
    throw ProjectError(ProjectErrorKind::InvalidInput, "invalid repository role");
}

bool validRemoteEndpoint(std::string_view endpoint) {
    if (endpoint.empty() || endpoint.size() > 2048) return false;
    if (!endpoint.starts_with("http://") && !endpoint.starts_with("https://") &&
        !endpoint.starts_with("file:///")) return false;
    const std::size_t prefix = endpoint.starts_with("https://") ? 8 :
        endpoint.starts_with("http://") ? 7 : 7;
    if (endpoint.size() <= prefix) return false;
    if (!endpoint.starts_with("file:///")) {
        const std::size_t hostnameEnd = endpoint.find_first_of("/:?", prefix);
        if (hostnameEnd == prefix) return false;
    }
    for (unsigned char ch : endpoint) {
        if (ch <= 0x20U || ch >= 0x7fU || ch == '@' || ch == '#' || ch == '\\')
            return false;
    }
    return true;
}

std::string decodeRemoteEndpoint(std::string_view encoded) {
    check(encoded.size() % 2 == 0 && encoded.size() <= 4096,
          "invalid remote endpoint encoding");
    std::string result;
    result.reserve(encoded.size() / 2);
    const std::function<int(char)> digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return 10 + value - 'a';
        return -1;
    };
    for (std::size_t offset = 0; offset < encoded.size(); offset += 2) {
        const int first = digit(encoded[offset]);
        const int second = digit(encoded[offset + 1]);
        check(first >= 0 && second >= 0, "invalid remote endpoint encoding");
        result.push_back(static_cast<char>((first << 4) | second));
    }
    check(validRemoteEndpoint(result), "invalid stored remote endpoint");
    return result;
}

std::vector<std::string> trackingFields(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = line.find('\t', start);
        fields.push_back(line.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return fields;
}

TrackingStateOnDisk loadTrackingState(const std::filesystem::path& root,
                                      std::string_view localRepositoryId) {
    const std::filesystem::path stage = root / "repository.tracking.tmp";
    if (std::filesystem::exists(stage) || std::filesystem::is_symlink(stage)) {
        check(!std::filesystem::is_symlink(stage) && std::filesystem::is_regular_file(stage),
              "unsafe stale tracking staging file");
        syscallChecked(::unlink(stage.c_str()) == 0, "discard unpublished tracking staging file");
        syncDirectory(root);
    }
    TrackingStateOnDisk result;
    const std::filesystem::path path = root / "repository.tracking";
    if (!std::filesystem::exists(path) && !std::filesystem::is_symlink(path)) return result;
    check(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path) &&
          std::filesystem::file_size(path) <= 8 * 1024 * 1024,
          "invalid repository tracking metadata file");
    std::ifstream stream(path, std::ios::binary);
    check(stream.good(), "cannot read repository tracking metadata");
    const std::string data((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    check(!data.empty() && data.back() == '\n', "truncated repository tracking metadata");
    std::istringstream input(data);
    std::string line;
    check(static_cast<bool>(std::getline(input, line)) && line == "AOWIS-TRACKING-1",
          "invalid repository tracking metadata marker");
    check(static_cast<bool>(std::getline(input, line)), "missing repository tracking role");
    if (line == "role\tunconfigured") result.role = RepositoryRole::Unconfigured;
    else if (line == "role\tprivate") result.role = RepositoryRole::Private;
    else if (line == "role\tshared") result.role = RepositoryRole::Shared;
    else check(false, "invalid repository tracking role");
    while (std::getline(input, line)) {
        const std::vector<std::string> fields = trackingFields(line);
        if (fields.size() == 3 && fields[0] == "remote") {
            check(UuidV7::parse(fields[1]).has_value() && fields[1] != localRepositoryId,
                  "invalid remote repository UUID in tracking registry");
            const RemoteRepository remote{fields[1], decodeRemoteEndpoint(fields[2])};
            check(result.remotes.emplace(remote.repositoryId, remote).second &&
                  result.remotes.size() <= 64, "duplicate or excessive remote registry entries");
        } else if (fields.size() == 5 && fields[0] == "ref") {
            check(UuidV7::parse(fields[1]).has_value() &&
                  UuidV7::parse(fields[2]).has_value() &&
                  UuidV7::parse(fields[3]).has_value() &&
                  (fields[4] == "-" || RevisionId::parse(fields[4]).has_value()),
                  "invalid qualified remote tracking ref");
            RemoteTrackingRef ref{fields[1], fields[2], fields[3], std::nullopt};
            if (fields[4] != "-") ref.observedHead = *RevisionId::parse(fields[4]);
            check(result.refs.emplace(TrackingKeyOnDisk{ref.remoteRepositoryId, ref.projectId,
                  ref.branchId}, ref).second && result.refs.size() <= 10000,
                  "duplicate or excessive remote tracking refs");
        } else check(false, "unknown repository tracking record");
    }
    check(input.eof() && !input.bad(), "cannot read full repository tracking metadata");
    std::set<std::string> endpoints;
    for (const std::pair<const std::string, RemoteRepository>& entry : result.remotes)
        check(endpoints.insert(entry.second.endpoint).second,
              "remote endpoint assigned to multiple repository UUIDs");
    for (const std::pair<const TrackingKeyOnDisk, RemoteTrackingRef>& record : result.refs)
        check(result.remotes.contains(record.second.remoteRepositoryId),
              "tracking ref references an unregistered repository");
    return result;
}

void storeTrackingState(const std::filesystem::path& root, const TrackingStateOnDisk& state) {
    std::string body = "AOWIS-TRACKING-1\nrole\t" + roleToken(state.role) + "\n";
    for (const std::pair<const std::string, RemoteRepository>& entry : state.remotes)
        body += "remote\t" + entry.first + "\t" + encodeName(entry.second.endpoint) + "\n";
    for (const std::pair<const TrackingKeyOnDisk, RemoteTrackingRef>& entry : state.refs) {
        const RemoteTrackingRef& ref = entry.second;
        body += "ref\t" + ref.remoteRepositoryId + "\t" + ref.projectId + "\t" +
            ref.branchId + "\t" + (ref.observedHead ? ref.observedHead->toString() : "-") + "\n";
    }
    check(body.size() <= 8 * 1024 * 1024, "repository tracking metadata exceeds size limit",
          StorageErrorKind::InvalidInput);
    const std::filesystem::path stage = root / "repository.tracking.tmp";
    const std::filesystem::path path = root / "repository.tracking";
    const int fd = ::open(stage.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    syscallChecked(fd >= 0, "stage repository tracking metadata");
    try {
        std::size_t offset = 0;
        while (offset < body.size()) {
            const ssize_t written = ::write(fd, body.data() + offset, body.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            syscallChecked(written > 0, "write repository tracking metadata");
            offset += static_cast<std::size_t>(written);
        }
        syscallChecked(::fsync(fd) == 0, "sync repository tracking metadata");
    } catch (...) {
        ::close(fd);
        std::error_code ignored;
        std::filesystem::remove(stage, ignored);
        throw;
    }
    syscallChecked(::close(fd) == 0, "close repository tracking metadata");
    if (::rename(stage.c_str(), path.c_str()) != 0) {
        std::error_code ignored;
        std::filesystem::remove(stage, ignored);
        throwSystemError("publish repository tracking metadata");
    }
    syncDirectory(root);
}

ProjectInfo readManifest(const std::filesystem::path& projectDirectory) {
    const std::filesystem::path path = projectDirectory / "project.meta";
    check(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path),
          "missing or unsafe project manifest: " + path.string());
    check(std::filesystem::file_size(path) <= 512,
          "oversized project manifest: " + path.string());
    std::ifstream stream(path, std::ios::binary);
    check(stream.good(), "cannot read project manifest");
    const std::string body((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const std::size_t first = body.find('\n');
    const std::size_t second = first == std::string::npos ? first : body.find('\n', first + 1);
    const std::size_t third = second == std::string::npos ? second : body.find('\n', second + 1);
    check(first != std::string::npos && second != std::string::npos &&
          third != std::string::npos && body.back() == '\n' &&
          body.find('\n', third + 1) == body.size() - 1 &&
          body.substr(0, first) == "AOWIS-PROJECT-1", "invalid project manifest format");
    ProjectInfo info;
    info.id = body.substr(first + 1, second - first - 1);
    check(ProjectService::validProjectId(info.id) &&
          projectDirectory.filename().string() == info.id, "project manifest UUID mismatch");
    const std::string format = body.substr(second + 1, third - second - 1);
    check(format == "pack", "unsupported project storage format");
    info.name = decodeName(body.substr(third + 1, body.size() - third - 2));
    return info;
}

// ProjectService methods report client mistakes as ProjectError (their
// documented contract). Server-side StorageError kinds (corruption, I/O,
// exhausted storage, internal) propagate unchanged.
template <typename Result, typename Function>
Result translated(Function&& function) {
    try {
        return function();
    } catch (const StorageError& error) {
        switch (error.kind()) {
        case StorageErrorKind::InvalidInput:
            throw ProjectError(ProjectErrorKind::InvalidInput, error.what());
        case StorageErrorKind::NotFound:
            throw ProjectError(ProjectErrorKind::NotFound, error.what());
        case StorageErrorKind::Conflict:
            throw ProjectError(ProjectErrorKind::Conflict, error.what());
        case StorageErrorKind::Unavailable:
            throw ProjectError(ProjectErrorKind::Unavailable, error.what());
        default:
            throw;
        }
    }
}

} // namespace

ProjectError::ProjectError(ProjectErrorKind kind, std::string message)
    : std::runtime_error(std::move(message)), kind_(kind) {}
ProjectErrorKind ProjectError::kind() const noexcept { return this->kind_; }

bool ProjectService::validProjectId(std::string_view id) {
    // Canonical lowercase UUIDv7 only; other UUID versions are not projects.
    const std::optional<ProjectId> parsed = ProjectId::parse(id);
    return parsed.has_value() && parsed->toString() == id;
}

bool ProjectService::validBranchName(std::string_view name) {
    return detail::validBranchLabel(name);
}

ProjectService::Project::Project(ProjectInfo value) : info(std::move(value)) {}

namespace {
// Private draft format (see docs/storage-formats.md). Earlier
// pre-release formats are rejected, never migrated.
constexpr std::string_view workingCopyMarker = "AOWIS-WORKING-COPY-7\n";
constexpr std::size_t workingCopyMaxBytes = 8 * 1024 * 1024;
constexpr std::size_t workingCopyMaxChanges = 20000;
constexpr std::size_t workingCopyMaxEvents = 4096;

std::filesystem::path workingCopyDirectory(const std::filesystem::path& root) {
    return root / "working-copies";
}

// Published draft UUIDs in ascending order. Unpublished crash remnants
// (.creating-*, .deleting-* directories) are checked and skipped, never
// advertised.
std::vector<std::string> publishedDraftIds(const std::filesystem::path& directory) {
    std::vector<std::string> ids;
    if (!std::filesystem::exists(directory) && !std::filesystem::is_symlink(directory))
        return ids;
    check(!std::filesystem::is_symlink(directory) && std::filesystem::is_directory(directory),
          "invalid working-copy directory");
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory)) {
        const std::string id = entry.path().filename().string();
        if (id.starts_with(".creating-") || id.starts_with(".deleting-")) {
            check(UuidV7::parse(std::string_view(id).substr(10)).has_value() &&
                  !entry.is_symlink() && entry.is_directory(),
                  "unsafe staged working-copy directory");
            continue;
        }
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::uint64_t systemEpochMs() {
    const std::int64_t millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    check(millis > 0, "invalid system clock", StorageErrorKind::Unavailable);
    return static_cast<std::uint64_t>(millis);
}

std::uint64_t decimalField(std::string_view value) {
    check(!value.empty() && (value.size() == 1 || value.front() != '0'),
          "invalid draft decimal field");
    std::uint64_t result = 0;
    const std::from_chars_result parsed = std::from_chars(
        value.data(), value.data() + value.size(), result);
    check(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(),
          "invalid draft decimal field");
    return result;
}

std::string hexBytes(const std::uint8_t *data, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    value.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        value.push_back(digits[data[i] >> 4]);
        value.push_back(digits[data[i] & 15U]);
    }
    return value;
}

Bytes unhexBytes(std::string_view encoded) {
    check(encoded.size() % 2 == 0 && encoded.size() <= workingCopyMaxBytes * 2,
          "invalid draft model record encoding");
    Bytes result;
    result.reserve(encoded.size() / 2);
    const std::function<unsigned(char)> digit = [](char ch) -> unsigned {
        if (ch >= '0' && ch <= '9') return static_cast<unsigned>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<unsigned>(ch - 'a' + 10);
        check(false, "invalid draft hexadecimal digit");
        return 0;
    };
    for (std::size_t i = 0; i < encoded.size(); i += 2)
        result.push_back(static_cast<std::uint8_t>(digit(encoded[i]) * 16 + digit(encoded[i + 1])));
    return result;
}

// Basic transport/integrity verification for MODEL's AO WE frame. The full
// entity schema belongs to MODEL; the HTTP endpoint additionally decodes it
// with ModelEditTransactionCodec. This Qt-free service verifies framing,
// length, SHA-256, and the event's intrinsic UUIDv7 before persisting it.
WorkingCopyEvent verifiedEditEvent(const Bytes& encoded) {
    constexpr std::size_t header = 9;
    constexpr std::size_t digest = 32;
    if (encoded.size() < header + 16 + digest || encoded.size() > 8 * 1024 * 1024 + header + digest ||
        encoded[0] != 'A' || encoded[1] != 'O' || encoded[2] != 'W' ||
        encoded[3] != 'E' || encoded[4] != 1)
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid edit event frame");
    const std::uint32_t size = (static_cast<std::uint32_t>(encoded[5]) << 24) |
        (static_cast<std::uint32_t>(encoded[6]) << 16) |
        (static_cast<std::uint32_t>(encoded[7]) << 8) | encoded[8];
    if (size < 16 || encoded.size() != header + static_cast<std::size_t>(size) + digest)
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid edit event length");
    Bytes payload(encoded.begin() + header, encoded.begin() + header + size);
    const Hash hash = sha256(payload);
    if (!std::equal(hash.begin(), hash.end(), encoded.begin() + header + size))
        throw ProjectError(ProjectErrorKind::InvalidInput, "edit event digest mismatch");
    UuidV7 uuid;
    std::copy_n(payload.begin(), uuid.bytes.size(), uuid.bytes.begin());
    const std::string id = uuid.toString();
    if (!UuidV7::parse(id).has_value())
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid edit event UUIDv7");
    return WorkingCopyEvent{id, encoded};
}

std::string saveBatchFingerprint(const std::vector<ModelChange>& changes,
                                 const std::vector<WorkingCopyEvent>& events) {
    // Canonical, unambiguous length-delimited digest of the complete request.
    // Sort changes by key because candidate overlay order is entity identity.
    std::map<Key, std::optional<ModelRecord>> ordered;
    for (const ModelChange& change : changes)
        if (!ordered.emplace(change.key, change.record).second)
            throw ProjectError(ProjectErrorKind::InvalidInput, "duplicate key in event save");
    Bytes bytes;
    const std::function<void(std::size_t)> appendSize = [&](std::size_t size) {
        const std::uint64_t count = static_cast<std::uint64_t>(size);
        for (unsigned shift = 0; shift < 8; ++shift)
            bytes.push_back(static_cast<std::uint8_t>(count >> (56 - shift * 8)));
    };
    appendSize(ordered.size());
    for (const std::pair<const Key, std::optional<ModelRecord>>& entry : ordered) {
        bytes.insert(bytes.end(), entry.first.begin(), entry.first.end());
        const Bytes record = entry.second ? encodeModelRecord(*entry.second) : Bytes{};
        appendSize(record.size());
        bytes.insert(bytes.end(), record.begin(), record.end());
    }
    appendSize(events.size());
    for (const WorkingCopyEvent& event : events) {
        appendSize(event.encoded.size());
        bytes.insert(bytes.end(), event.encoded.begin(), event.encoded.end());
    }
    const Hash hash = sha256(bytes);
    return hexBytes(hash.data(), hash.size());
}

// ---------------------------------------------------------------------------
// Draft storage (AOWIS-WORKING-COPY-7). One directory per draft:
//   meta                  small text record, atomically replaced; authoritative
//   candidate-<uuidv7>    immutable binary candidate overlay (sorted changes)
//   journal-<uuidv7>      append-only binary MODEL event journal
// `meta` names the current candidate and journal files and records their
// counts, lengths and digests. Leases, releases and listings touch only
// `meta`; a Save writes a new candidate file only when the candidate changes
// and appends only its own events. Readers take no lock: a file named by a
// meta they read stays valid until a newer meta replaces it, and they retry
// once the name disappears.
// ---------------------------------------------------------------------------

constexpr std::string_view draftMetaName = "meta";
constexpr std::uint32_t absentRecord = std::numeric_limits<std::uint32_t>::max();
constexpr std::string_view candidateMagic = "AOWC\x01";

// Files referenced by a draft's meta.
struct DraftFiles {
    std::string candidate = "-";          // file name or "-" for an empty overlay
    std::uint64_t candidateCount = 0;
    std::uint64_t candidateBytes = 0;
    std::string candidateDigest = "-";    // SHA-256 of the file, hex
    std::string journal = "-";            // file name or "-" for an empty journal
    std::uint64_t journalBytes = 0;       // acknowledged prefix length
    std::string journalDigest = "-";      // chained SHA-256 over the frames, hex
};

struct StoredDraft {
    WorkingCopyInfo info;
    DraftFiles files;
};

enum class DraftLoad { Meta, Candidate, Full };

bool fileReference(std::string_view value, std::string_view prefix) {
    return value == "-" ||
           (value.starts_with(prefix) && UuidV7::parse(value.substr(prefix.size())).has_value());
}

bool hexDigest(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

void appendBig32(Bytes& out, std::uint32_t value) {
    for (unsigned step = 0; step < 4; ++step)
        out.push_back(static_cast<std::uint8_t>(value >> (24 - step * 8)));
}

std::uint32_t big32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | data[3];
}

std::string metaBody(const WorkingCopyInfo& draft, const DraftFiles& files) {
    return std::string(workingCopyMarker)
        + "project\t" + draft.projectId + "\n"
        + "branch\t" + draft.branchId + "\n"
        + "base\t" + (draft.baseRevision ? draft.baseRevision->toString() : "-") + "\n"
        + "session\t" + draft.sessionId + "\n"
        + "generation\t" + std::to_string(draft.generation) + "\n"
        + "dirty\t" + (draft.dirty ? "1" : "0") + "\n"
        + "validation\tunvalidated\n"
        + "lease-session\t" + draft.leaseSessionId.value_or("-") + "\n"
        + "lease-token\t" + draft.leaseToken.value_or("-") + "\n"
        + "lease-expires\t" + std::to_string(draft.leaseExpiresAtMs) + "\n"
        + "candidate\t" + files.candidate + "\t" + std::to_string(files.candidateCount) + "\t"
            + std::to_string(files.candidateBytes) + "\t" + files.candidateDigest + "\n"
        + "journal\t" + files.journal + "\t" + std::to_string(draft.journalCount) + "\t"
            + std::to_string(files.journalBytes) + "\t" + files.journalDigest + "\n"
        + "last-save\t" + draft.lastSaveFingerprint.value_or("-") + "\n"
        + "history-revision\t" + draft.historyCheckpointRevisionId.value_or("-") + "\n"
        + "history-operation\t" + draft.lastHistoryNavigationOperationId.value_or("-") + "\n"
        + "history-source-generation\t" + (draft.lastHistoryNavigationSourceGeneration
            ? std::to_string(*draft.lastHistoryNavigationSourceGeneration) : "-") + "\n"
        + "history-action-position\t" + (draft.historyActionPosition
            ? std::to_string(*draft.historyActionPosition) : "-") + "\n"
        + "history-archive-operation\t" + draft.historyArchiveCommitOperationId.value_or("-") + "\n"
        + "draft-action-position\t" + (draft.draftActionPosition
            ? std::to_string(*draft.draftActionPosition) : "-") + "\n";
}

Bytes encodeCandidate(const std::vector<ModelChange>& changes) {
    Bytes bytes(candidateMagic.begin(), candidateMagic.end());
    for (const ModelChange& change : changes) {
        bytes.insert(bytes.end(), change.key.begin(), change.key.end());
        if (!change.record) {
            appendBig32(bytes, absentRecord);
        } else {
            const Bytes record = encodeModelRecord(*change.record);
            appendBig32(bytes, static_cast<std::uint32_t>(record.size()));
            bytes.insert(bytes.end(), record.begin(), record.end());
        }
        check(bytes.size() <= workingCopyMaxBytes, "draft candidate exceeds the 8 MiB storage limit",
              StorageErrorKind::LimitExceeded);
    }
    return bytes;
}

std::vector<ModelChange> decodeCandidate(const Bytes& bytes, std::uint64_t count) {
    check(bytes.size() >= candidateMagic.size() &&
          std::equal(candidateMagic.begin(), candidateMagic.end(), bytes.begin()),
          "invalid draft candidate file header");
    std::vector<ModelChange> changes;
    std::size_t offset = candidateMagic.size();
    std::optional<Key> last;
    for (std::uint64_t i = 0; i < count; ++i) {
        check(bytes.size() - offset >= 20, "truncated draft candidate record");
        ModelChange change;
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), change.key.size(),
                    change.key.begin());
        check(!last || *last < change.key, "unsorted or duplicate draft changes");
        last = change.key;
        const std::uint32_t size = big32(bytes.data() + offset + 16);
        offset += 20;
        if (size != absentRecord) {
            check(bytes.size() - offset >= size, "truncated draft candidate record");
            change.record = decodeModelRecord(Bytes(
                bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)));
            offset += size;
        }
        changes.push_back(std::move(change));
    }
    check(offset == bytes.size(), "trailing draft candidate content");
    return changes;
}

// Journal frame: event UUID (16) + frame length (4) + MODEL frame bytes.
Bytes encodeJournalFrames(const std::vector<WorkingCopyEvent>& events) {
    Bytes bytes;
    for (const WorkingCopyEvent& event : events) {
        const std::optional<UuidV7> id = UuidV7::parse(event.id);
        check(id.has_value(), "invalid journal event UUIDv7", StorageErrorKind::Internal);
        bytes.insert(bytes.end(), id->bytes.begin(), id->bytes.end());
        appendBig32(bytes, static_cast<std::uint32_t>(event.encoded.size()));
        bytes.insert(bytes.end(), event.encoded.begin(), event.encoded.end());
    }
    return bytes;
}

// digest_0 = 32 zero bytes; digest_i = SHA-256(digest_{i-1} || frame_i).
Hash chainJournal(const Hash& previous, const std::vector<WorkingCopyEvent>& events) {
    Hash digest = previous;
    for (const WorkingCopyEvent& event : events) {
        Bytes input(digest.begin(), digest.end());
        input.insert(input.end(), event.encoded.begin(), event.encoded.end());
        digest = sha256(input);
    }
    return digest;
}

Hash digestFromHex(std::string_view value) {
    Hash digest{};
    if (value == "-") return digest;
    const Bytes bytes = unhexBytes(value);
    check(bytes.size() == digest.size(), "invalid draft digest");
    std::copy(bytes.begin(), bytes.end(), digest.begin());
    return digest;
}

// Reads exactly `size` bytes from the start of `path`; nullopt if the file
// vanished (a concurrent writer replaced it).
std::optional<Bytes> readPrefix(const std::filesystem::path& path, std::uint64_t size) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) return std::nullopt;
    syscallChecked(fd >= 0, "open draft file");
    Bytes bytes(static_cast<std::size_t>(size));
    std::size_t done = 0;
    try {
        struct stat st{};
        syscallChecked(::fstat(fd, &st) == 0, "stat draft file");
        check(S_ISREG(st.st_mode) && static_cast<std::uint64_t>(st.st_size) >= size,
              "draft file is shorter than its meta record");
        while (done < bytes.size()) {
            const ssize_t amount = ::pread(fd, bytes.data() + done, bytes.size() - done,
                                           static_cast<off_t>(done));
            if (amount < 0 && errno == EINTR) continue;
            syscallChecked(amount >= 0, "read draft file");
            check(amount > 0, "truncated draft file");
            done += static_cast<std::size_t>(amount);
        }
    } catch (...) { ::close(fd); throw; }
    ::close(fd);
    return bytes;
}

// Parses meta. Returns nullopt if the draft directory or meta vanished.
std::optional<StoredDraft> readDraftMeta(const std::filesystem::path& directory) {
    const std::string id = directory.filename().string();
    check(UuidV7::parse(id).has_value(), "invalid working-copy UUIDv7");
    if (!std::filesystem::exists(directory) && !std::filesystem::is_symlink(directory))
        return std::nullopt;
    check(!std::filesystem::is_symlink(directory) && std::filesystem::is_directory(directory),
          "unsupported working-copy record (expected a draft directory): " + directory.string());
    const std::filesystem::path path = directory / draftMetaName;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) return std::nullopt;
    syscallChecked(fd >= 0, "open working-copy meta");
    std::string body;
    try {
        struct stat st{};
        syscallChecked(::fstat(fd, &st) == 0, "stat working-copy meta");
        check(S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 4096,
              "invalid working-copy meta file");
        body.resize(static_cast<std::size_t>(st.st_size));
        std::size_t done = 0;
        while (done < body.size()) {
            const ssize_t amount = ::pread(fd, body.data() + done, body.size() - done,
                                           static_cast<off_t>(done));
            if (amount < 0 && errno == EINTR) continue;
            syscallChecked(amount >= 0, "read working-copy meta");
            check(amount > 0, "truncated working-copy meta");
            done += static_cast<std::size_t>(amount);
        }
    } catch (...) { ::close(fd); throw; }
    ::close(fd);
    check(body.starts_with(workingCopyMarker) && body.back() == '\n',
          "invalid working-copy record marker");
    std::istringstream lines(body.substr(workingCopyMarker.size()));
    const std::function<std::string(std::string_view)> nextField =
        [&](std::string_view name) -> std::string {
        std::string line;
        check(static_cast<bool>(std::getline(lines, line)) &&
              line.starts_with(std::string(name) + "\t"),
              "invalid working-copy record field: " + std::string(name));
        return line.substr(name.size() + 1);
    };
    const std::function<std::vector<std::string>(const std::string&)> split =
        [](const std::string& value) {
        std::vector<std::string> parts;
        std::size_t start = 0;
        for (;;) {
            const std::size_t tab = value.find('\t', start);
            parts.push_back(value.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
            if (tab == std::string::npos) return parts;
            start = tab + 1;
        }
    };
    StoredDraft stored;
    WorkingCopyInfo& draft = stored.info;
    draft.id = id;
    draft.projectId = nextField("project");
    draft.branchId = nextField("branch");
    const std::string base = nextField("base");
    draft.sessionId = nextField("session");
    draft.generation = decimalField(nextField("generation"));
    const std::string dirty = nextField("dirty");
    check(dirty == "0" || dirty == "1", "invalid draft dirty state");
    draft.dirty = dirty == "1";
    check(nextField("validation") == "unvalidated", "invalid draft validation state");
    const std::string owner = nextField("lease-session");
    const std::string token = nextField("lease-token");
    draft.leaseExpiresAtMs = decimalField(nextField("lease-expires"));
    check((owner == "-" && token == "-" && draft.leaseExpiresAtMs == 0) ||
          (UuidV7::parse(owner).has_value() && UuidV7::parse(token).has_value() &&
           draft.leaseExpiresAtMs != 0), "invalid draft lease record");
    if (owner != "-") {
        draft.leaseSessionId = owner;
        draft.leaseToken = token;
    }
    const std::vector<std::string> candidate = split(nextField("candidate"));
    check(candidate.size() == 4 && fileReference(candidate[0], "candidate-"),
          "invalid draft candidate reference");
    stored.files.candidate = candidate[0];
    stored.files.candidateCount = decimalField(candidate[1]);
    stored.files.candidateBytes = decimalField(candidate[2]);
    stored.files.candidateDigest = candidate[3];
    check(stored.files.candidateCount <= workingCopyMaxChanges &&
          stored.files.candidateBytes <= workingCopyMaxBytes &&
          ((candidate[0] == "-" && stored.files.candidateCount == 0 &&
            stored.files.candidateBytes == 0 && candidate[3] == "-") ||
           (candidate[0] != "-" && stored.files.candidateCount > 0 && hexDigest(candidate[3]))),
          "invalid draft candidate reference");
    const std::vector<std::string> journal = split(nextField("journal"));
    check(journal.size() == 4 && fileReference(journal[0], "journal-"),
          "invalid draft journal reference");
    stored.files.journal = journal[0];
    draft.journalCount = decimalField(journal[1]);
    stored.files.journalBytes = decimalField(journal[2]);
    stored.files.journalDigest = journal[3];
    check(draft.journalCount <= workingCopyMaxEvents &&
          stored.files.journalBytes <= workingCopyMaxBytes &&
          ((journal[0] == "-" && draft.journalCount == 0 && stored.files.journalBytes == 0 &&
            journal[3] == "-") ||
           (journal[0] != "-" && draft.journalCount > 0 && hexDigest(journal[3]))),
          "invalid draft journal reference");
    const std::string fingerprint = nextField("last-save");
    if (fingerprint != "-") {
        check(hexDigest(fingerprint), "invalid last-save fingerprint");
        draft.lastSaveFingerprint = fingerprint;
    }
    const std::string historyRevision = nextField("history-revision");
    const std::string historyOperation = nextField("history-operation");
    const std::string historyGeneration = nextField("history-source-generation");
    const std::string historyPosition = nextField("history-action-position");
    const std::string historyArchive = nextField("history-archive-operation");
    const std::string draftPosition = nextField("draft-action-position");
    if (draftPosition != "-") {
        draft.draftActionPosition = decimalField(draftPosition);
        check(draft.journalCount > 0 &&
              *draft.draftActionPosition <= draft.journalCount &&
              historyRevision == "-" && historyPosition == "-" && historyArchive == "-",
              "invalid active draft history cursor");
    }
    check((historyPosition == "-" && historyArchive == "-") ||
          (historyPosition != "-" && UuidV7::parse(historyArchive).has_value() &&
           historyRevision != "-"),
          "incomplete action-level history cursor in draft meta");
    if (historyPosition != "-") {
        draft.historyActionPosition = decimalField(historyPosition);
        check(*draft.historyActionPosition <= workingCopyMaxEvents,
              "history action position is out of range");
        draft.historyArchiveCommitOperationId = historyArchive;
    }
    check((historyRevision == "-" && historyOperation == "-" && historyGeneration == "-" &&
           !draft.draftActionPosition) ||
          ((RevisionId::parse(historyRevision).has_value() || draft.draftActionPosition) &&
           UuidV7::parse(historyOperation).has_value() && historyGeneration != "-"),
          "incomplete history navigation cursor in draft meta");
    if (historyRevision != "-" || draft.draftActionPosition) {
        if (historyRevision != "-") draft.historyCheckpointRevisionId = historyRevision;
        draft.lastHistoryNavigationOperationId = historyOperation;
        draft.lastHistoryNavigationSourceGeneration = decimalField(historyGeneration);
        check(*draft.lastHistoryNavigationSourceGeneration < draft.generation &&
              *draft.lastHistoryNavigationSourceGeneration + 1 == draft.generation &&
              (draft.draftActionPosition ? draft.journalCount > 0 :
                  (draft.journalCount == 0 && !draft.lastSaveFingerprint)),
              "invalid history navigation generation or journal state");
    }
    std::string extra;
    check(!std::getline(lines, extra) && lines.eof(), "trailing working-copy record content");
    check((draft.journalCount == 0 && !draft.lastSaveFingerprint) ||
          (draft.journalCount > 0 && draft.lastSaveFingerprint && draft.generation > 0),
          "inconsistent draft journal metadata");
    check(ProjectId::parse(draft.projectId).has_value() &&
          BranchId::parse(draft.branchId).has_value() &&
          UuidV7::parse(draft.sessionId).has_value(),
          "invalid working-copy project, branch or session identity");
    if (base != "-") {
        draft.baseRevision = RevisionId::parse(base);
        check(draft.baseRevision.has_value(), "invalid working-copy base revision");
    }
    check(!draft.historyCheckpointRevisionId || draft.baseRevision.has_value(),
          "history cursor requires a committed draft base");
    check(draft.dirty == (stored.files.candidateCount != 0), "invalid draft dirty/change mismatch");
    return stored;
}

// Event UUIDs of the acknowledged journal prefix (frame headers only).
std::vector<std::string> journalEventIds(const std::filesystem::path& directory,
                                         const StoredDraft& stored, std::optional<Bytes>& bytes) {
    std::vector<std::string> ids;
    if (stored.files.journal == "-") return ids;
    bytes = readPrefix(directory / stored.files.journal, stored.files.journalBytes);
    if (!bytes) return ids;
    std::size_t offset = 0;
    std::set<std::string> seen;
    while (offset < bytes->size()) {
        check(bytes->size() - offset >= 20, "truncated draft journal frame");
        UuidV7 uuid;
        std::copy_n(bytes->begin() + static_cast<std::ptrdiff_t>(offset), uuid.bytes.size(),
                    uuid.bytes.begin());
        const std::uint32_t size = big32(bytes->data() + offset + 16);
        check(bytes->size() - offset - 20 >= size, "truncated draft journal frame");
        ids.push_back(uuid.toString());
        check(seen.insert(ids.back()).second, "duplicate draft journal event");
        offset += 20 + static_cast<std::size_t>(size);
    }
    check(ids.size() == stored.info.journalCount, "draft journal event count mismatch");
    return ids;
}

// Reads a draft at the requested depth. Full verifies every journal frame and
// the chained digest. Retries if a concurrent writer replaced a file between
// reading meta and opening it.
std::optional<StoredDraft> readDraft(const std::filesystem::path& directory, DraftLoad load,
                                     std::vector<std::string>* eventIds = nullptr) {
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        std::optional<StoredDraft> stored = readDraftMeta(directory);
        if (!stored || load == DraftLoad::Meta) return stored;
        if (stored->files.candidate != "-") {
            const std::optional<Bytes> bytes =
                readPrefix(directory / stored->files.candidate, stored->files.candidateBytes);
            if (!bytes) continue;
            const Hash digest = sha256(*bytes);
            check(hexBytes(digest.data(), digest.size()) == stored->files.candidateDigest,
                  "draft candidate file digest mismatch");
            stored->info.candidateChanges = decodeCandidate(*bytes, stored->files.candidateCount);
        }
        if (eventIds || load == DraftLoad::Full) {
            std::optional<Bytes> journal;
            const std::vector<std::string> ids = journalEventIds(directory, *stored, journal);
            if (stored->files.journal != "-" && !journal) continue;
            if (eventIds) *eventIds = ids;
            if (load == DraftLoad::Full && journal) {
                std::size_t offset = 0;
                for (const std::string& id : ids) {
                    const std::uint32_t size = big32(journal->data() + offset + 16);
                    const WorkingCopyEvent event = verifiedEditEvent(Bytes(
                        journal->begin() + static_cast<std::ptrdiff_t>(offset + 20),
                        journal->begin() + static_cast<std::ptrdiff_t>(offset + 20 + size)));
                    check(event.id == id, "draft journal frame UUID mismatch");
                    stored->info.journal.push_back(event);
                    offset += 20 + static_cast<std::size_t>(size);
                }
                const Hash chain = chainJournal(Hash{}, stored->info.journal);
                check(hexBytes(chain.data(), chain.size()) == stored->files.journalDigest,
                      "draft journal digest mismatch");
            }
        }
        return stored;
    }
    throw StorageError(StorageErrorKind::Busy, "draft changed repeatedly while reading");
}

WorkingCopyInfo readWorkingCopy(const std::filesystem::path& directory,
                                DraftLoad load = DraftLoad::Full) {
    std::optional<StoredDraft> stored = readDraft(directory, load);
    if (!stored) throw ProjectError(ProjectErrorKind::NotFound, "working copy not found");
    return std::move(stored->info);
}

// Writes `bytes` to a new file and fsyncs it.
void writeNewFile(const std::filesystem::path& path, const Bytes& bytes) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    syscallChecked(fd >= 0, "create draft file");
    try {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const ssize_t amount = ::write(fd, bytes.data() + done, bytes.size() - done);
            if (amount < 0 && errno == EINTR) continue;
            syscallChecked(amount > 0, "write draft file");
            done += static_cast<std::size_t>(amount);
        }
        syscallChecked(::fsync(fd) == 0, "sync draft file");
    } catch (...) {
        ::close(fd);
        ::unlink(path.c_str());
        throw;
    }
    syscallChecked(::close(fd) == 0, "close draft file");
}

void publishMeta(const std::filesystem::path& directory, const WorkingCopyInfo& draft,
                 const DraftFiles& files) {
    const std::string body = metaBody(draft, files);
    const std::filesystem::path stage = directory / (".meta-" + UuidV7::generate().toString());
    writeNewFile(stage, Bytes(body.begin(), body.end()));
    try {
        syscallChecked(::rename(stage.c_str(), (directory / draftMetaName).c_str()) == 0,
                       "publish draft meta");
        syncDirectory(directory);
    } catch (...) {
        // The rename may have happened even if the directory fsync failed.
        // Callers mark publication uncertain and require a restart.
        (void)::unlink(stage.c_str());
        throw;
    }
}

// What a draft update changes besides its meta fields. prepareDraftUpdate
// encodes and checks every limit before anything is written, so a rejected
// Save never reaches the disk.
struct DraftUpdate {
    std::optional<std::vector<ModelChange>> candidate; // replaces the overlay when set
    std::vector<WorkingCopyEvent> appended;            // appended to the journal
    bool resetJournal = false;                         // starts an empty journal
    Bytes candidateBytes;                              // filled by prepareDraftUpdate
    Bytes frames;                                      // filled by prepareDraftUpdate
};

void prepareDraftUpdate(const StoredDraft& stored, DraftUpdate& update) {
    if (update.candidate && !update.candidate->empty())
        update.candidateBytes = encodeCandidate(*update.candidate);
    if (!update.appended.empty()) {
        update.frames = encodeJournalFrames(update.appended);
        const std::uint64_t kept = update.resetJournal ? 0 : stored.files.journalBytes;
        check(kept + update.frames.size() <= workingCopyMaxBytes,
              "draft event journal exceeds the 8 MiB storage limit",
              StorageErrorKind::LimitExceeded);
    }
}

void appendJournal(const std::filesystem::path& path, std::uint64_t committed, const Bytes& frames) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    syscallChecked(fd >= 0, "open draft journal");
    try {
        // Bytes past the acknowledged length belong to an interrupted, never
        // acknowledged append; they are overwritten.
        syscallChecked(::ftruncate(fd, static_cast<off_t>(committed)) == 0,
                       "truncate draft journal tail");
        std::size_t done = 0;
        while (done < frames.size()) {
            const ssize_t amount = ::pwrite(fd, frames.data() + done, frames.size() - done,
                                            static_cast<off_t>(committed + done));
            if (amount < 0 && errno == EINTR) continue;
            syscallChecked(amount > 0, "append draft journal");
            done += static_cast<std::size_t>(amount);
        }
        syscallChecked(::fsync(fd) == 0, "sync draft journal");
    } catch (...) { ::close(fd); throw; }
    syscallChecked(::close(fd) == 0, "close draft journal");
}

// Publishes `next` (whose journalCount the caller has set) after
// prepareDraftUpdate. Data files are durable before the meta that references
// them; superseded files are removed only after the new meta is durable.
void updateDraft(const std::filesystem::path& directory, StoredDraft& stored,
                 const WorkingCopyInfo& next, const DraftUpdate& update) {
    DraftFiles files = stored.files;
    std::vector<std::filesystem::path> superseded;
    if (update.candidate) {
        if (stored.files.candidate != "-") superseded.push_back(directory / stored.files.candidate);
        files = DraftFiles{"-", 0, 0, "-", files.journal, files.journalBytes, files.journalDigest};
        if (!update.candidate->empty()) {
            files.candidate = "candidate-" + UuidV7::generate().toString();
            writeNewFile(directory / files.candidate, update.candidateBytes);
            const Hash digest = sha256(update.candidateBytes);
            files.candidateCount = update.candidate->size();
            files.candidateBytes = update.candidateBytes.size();
            files.candidateDigest = hexBytes(digest.data(), digest.size());
        }
    }
    if (update.resetJournal) {
        if (stored.files.journal != "-") superseded.push_back(directory / stored.files.journal);
        files.journal = "-";
        files.journalBytes = 0;
        files.journalDigest = "-";
    }
    if (!update.appended.empty()) {
        const Hash chain = chainJournal(digestFromHex(files.journalDigest), update.appended);
        if (files.journal == "-") {
            files.journal = "journal-" + UuidV7::generate().toString();
            writeNewFile(directory / files.journal, update.frames);
        } else {
            appendJournal(directory / files.journal, files.journalBytes, update.frames);
        }
        files.journalBytes += update.frames.size();
        files.journalDigest = hexBytes(chain.data(), chain.size());
    }
    publishMeta(directory, next, files);
    stored.info = next;
    stored.files = files;
    for (const std::filesystem::path& path : superseded) (void)::unlink(path.c_str());
}

// Creates a new draft directory with an empty overlay and journal. The
// directory is assembled under a staged name and renamed into place, so a
// crash never leaves a published draft without its meta.
void createDraft(const std::filesystem::path& root, const WorkingCopyInfo& draft) {
    const std::filesystem::path directory = workingCopyDirectory(root);
    if (!std::filesystem::exists(directory)) {
        syscallChecked(std::filesystem::create_directory(directory), "create working-copy directory");
        syncDirectory(root);
    }
    check(!std::filesystem::is_symlink(directory) && std::filesystem::is_directory(directory),
          "invalid working-copy directory");
    const std::filesystem::path published = directory / draft.id;
    const std::filesystem::path staging = directory / (".creating-" + draft.id);
    std::error_code ignored;
    std::filesystem::remove_all(staging, ignored);
    syscallChecked(::mkdir(staging.c_str(), 0700) == 0, "stage working-copy directory");
    try {
        const std::string body = metaBody(draft, DraftFiles{});
        writeNewFile(staging / draftMetaName, Bytes(body.begin(), body.end()));
        syncDirectory(staging);
        // A published draft directory always contains meta, so renaming onto
        // an existing draft fails (ENOTEMPTY/EEXIST) instead of replacing it.
        syscallChecked(::rename(staging.c_str(), published.c_str()) == 0,
                       "publish working-copy record");
        syncDirectory(directory);
    } catch (...) {
        std::filesystem::remove_all(staging, ignored);
        throw;
    }
}

// Removes a published draft: an atomic rename out of the published namespace,
// then best-effort deletion (a crash leaves a .deleting-* remnant).
void deleteDraft(const std::filesystem::path& root, std::string_view draftId) {
    const std::filesystem::path directory = workingCopyDirectory(root);
    const std::filesystem::path doomed = directory / (".deleting-" + UuidV7::generate().toString());
    syscallChecked(::rename((directory / std::string(draftId)).c_str(), doomed.c_str()) == 0,
                   "discard working-copy record");
    syncDirectory(directory);
    std::error_code ignored;
    std::filesystem::remove_all(doomed, ignored);
}

// Removes files a crash left inside a published draft: staged metas and
// candidate or journal files the current meta does not reference.
void tidyDraftDirectory(const std::filesystem::path& directory) {
    const std::optional<StoredDraft> stored = readDraftMeta(directory);
    if (!stored) return;
    bool removed = false;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        const bool unreferenced =
            (name.starts_with("candidate-") && name != stored->files.candidate) ||
            (name.starts_with("journal-") && name != stored->files.journal);
        if (!(name.starts_with(".meta-") || unreferenced) || entry.is_symlink() ||
            !entry.is_regular_file())
            continue;
        syscallChecked(::unlink(entry.path().c_str()) == 0 || errno == ENOENT,
                       "remove stale draft file");
        removed = true;
    }
    if (removed) syncDirectory(directory);
}

// Draft crash remnants: unpublished (.creating-*) and half-deleted
// (.deleting-*) draft directories, and unreferenced files inside published
// drafts. Runs once at open, under the exclusive catalogue lock.
void removeDraftRemnants(const std::filesystem::path& root) {
    const std::filesystem::path directory = workingCopyDirectory(root);
    if (removePrefixed(directory, {".creating-", ".deleting-"}, true)) syncDirectory(directory);
    for (const std::string& id : publishedDraftIds(directory)) {
        // A damaged draft must not keep the whole root from opening; it
        // reports its corruption when it is accessed.
        try { tidyDraftDirectory(directory / id); }
        catch (const StorageError&) {}
    }
}

StoredDraft loadDraft(const std::filesystem::path& root, std::string_view draftId, DraftLoad load,
                      std::vector<std::string>* eventIds = nullptr) {
    std::optional<StoredDraft> stored =
        readDraft(workingCopyDirectory(root) / std::string(draftId), load, eventIds);
    if (!stored) throw ProjectError(ProjectErrorKind::NotFound, "working copy not found");
    return std::move(*stored);
}

std::vector<ModelChange> mergedCandidate(const std::vector<ModelChange>& current,
                                         const std::vector<ModelChange>& changes) {
    std::map<Key, std::optional<ModelRecord>> merged;
    for (const ModelChange& change : current) merged.emplace(change.key, change.record);
    for (const ModelChange& change : changes) merged[change.key] = change.record;
    if (merged.size() > workingCopyMaxChanges)
        throw StorageError(StorageErrorKind::LimitExceeded, "draft candidate has too many changes");
    std::vector<ModelChange> result;
    result.reserve(merged.size());
    for (const std::pair<const Key, std::optional<ModelRecord>>& entry : merged)
        result.push_back(ModelChange{entry.first, entry.second});
    return result;
}

// One durable private-local operation record owns its UUIDv7, request
// fingerprint, snapshot of candidate changes and reserved revision UUID.
// The pending record is fsynced before any immutable object or HEAD is written.
constexpr std::string_view localCommitMarker = "AOWIS-LOCAL-COMMIT-3\n";
// Candidate and journal frames are hex-encoded in a single, durable intent.
// Candidate up to 8 MiB + journal up to 8 MiB => 32 MiB of hex, plus rows.
constexpr std::size_t localCommitMaxBytes =
    5 * workingCopyMaxBytes + workingCopyMaxEvents * 90 + 2 * maxCommitMessageBytes + 4096;

// The reserved commit time and the metadata make the revision record of a
// retried publication byte-identical to the first attempt.
struct PersistedLocalCommit {
    LocalCommitInfo info;
    std::string requestDigest;
    std::vector<ModelChange> changes;
    std::vector<WorkingCopyEvent> actions; // Frozen before immutable HEAD publication.
    CommitMetadata metadata;
    std::uint64_t committedAtMs = 0;
};

std::filesystem::path localCommitDirectory(const std::filesystem::path& root) {
    return root / "local-commits";
}

std::string localDigest(std::string_view input) {
    const Bytes bytes(input.begin(), input.end());
    const Hash hash = sha256(bytes);
    return hexBytes(hash.data(), hash.size());
}

std::string hexText(std::string_view text) {
    return hexBytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

std::string localRequestDigest(std::string_view repositoryId,
                               std::string_view draftId, std::string_view sessionId,
                               std::string_view token, std::uint64_t generation,
                               const std::optional<RevisionId>& expected,
                               const CommitMetadata& metadata) {
    // Explicit null and UUIDv7 HEAD have distinct canonical encodings; text
    // fields are hex, so no value can contain the separator.
    return localDigest(std::string("AOWIS-LOCAL-COMMIT-REQUEST-2\n") +
        std::string(repositoryId) + "\n" + std::string(draftId) + "\n" +
        std::string(sessionId) + "\n" + std::string(token) + "\n" +
        std::to_string(generation) + "\n" +
        (expected ? expected->toString() : "-") + "\n" +
        metadata.authorId.toString() + "\n" + hexText(metadata.authorName) + "\n" +
        hexText(metadata.message) + "\n");
}

std::string localStateToken(LocalCommitState state) {
    switch (state) {
    case LocalCommitState::Pending: return "pending";
    case LocalCommitState::Succeeded: return "succeeded";
    case LocalCommitState::Rejected: return "rejected";
    }
    throw ProjectError(ProjectErrorKind::InvalidInput, "invalid local commit state");
}

std::string localCommitBody(const PersistedLocalCommit& operation) {
    const LocalCommitInfo& info = operation.info;
    std::string body = std::string(localCommitMarker) +
        "repository\t" + info.repositoryId + "\n" +
        "project\t" + info.projectId + "\n" +
        "branch\t" + info.branchId + "\n" +
        "draft\t" + info.draftId + "\n" +
        "generation\t" + std::to_string(info.expectedGeneration) + "\n" +
        "expected\t" + (info.expectedHead ? info.expectedHead->toString() : "-") + "\n" +
        "reserved\t" + info.reservedRevisionId + "\n" +
        "committed-at\t" + std::to_string(operation.committedAtMs) + "\n" +
        "author-id\t" + operation.metadata.authorId.toString() + "\n" +
        "author-name\t" + hexText(operation.metadata.authorName) + "\n" +
        "message\t" + hexText(operation.metadata.message) + "\n" +
        "request-digest\t" + operation.requestDigest + "\n" +
        "payload-digest\t" + info.payloadDigest + "\n" +
        "state\t" + localStateToken(info.state) + "\n" +
        "rejection\t" + (info.rejection.empty() ? "-" : info.rejection) + "\n" +
        "changes\t" + std::to_string(operation.changes.size()) + "\n";
    check(operation.changes.size() <= workingCopyMaxChanges,
          "local commit has too many changes", StorageErrorKind::InvalidInput);
    std::optional<Key> lastKey;
    for (const ModelChange& change : operation.changes) {
        check(!lastKey || *lastKey < change.key, "unordered local commit changes",
              StorageErrorKind::InvalidInput);
        lastKey = change.key;
        const Bytes record = change.record ? encodeModelRecord(*change.record) : Bytes{};
        body += "change\t" + hexBytes(change.key.data(), change.key.size()) + "\t" +
            (change.record ? hexBytes(record.data(), record.size()) : "-") + "\n";
        check(body.size() <= localCommitMaxBytes, "local commit record exceeds size limit",
              StorageErrorKind::InvalidInput);
    }
    check(operation.actions.size() <= workingCopyMaxEvents,
          "local Commit action archive has too many events", StorageErrorKind::InvalidInput);
    std::size_t totalEventBytes = 0;
    std::set<std::string> used;
    body += "actions\t" + std::to_string(operation.actions.size()) + "\n";
    const Hash chained = chainJournal(Hash{}, operation.actions);
    body += "action-chain\t" + hexBytes(chained.data(), chained.size()) + "\n";
    for (const WorkingCopyEvent& action : operation.actions) {
        const WorkingCopyEvent verified = verifiedEditEvent(action.encoded);
        check(action.id == verified.id && used.insert(action.id).second,
              "invalid or duplicate archived Commit action ID", StorageErrorKind::InvalidInput);
        totalEventBytes += action.encoded.size() + 20;
        check(totalEventBytes <= workingCopyMaxBytes,
              "local Commit action archive exceeds journal budget", StorageErrorKind::InvalidInput);
        body += "action\t" + action.id + "\t" +
            hexBytes(action.encoded.data(), action.encoded.size()) + "\n";
        check(body.size() <= localCommitMaxBytes, "local commit record exceeds size limit",
              StorageErrorKind::InvalidInput);
    }
    return body;
}

PersistedLocalCommit readLocalCommit(const std::filesystem::path& path) {
    check(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path) &&
          std::filesystem::file_size(path) <= localCommitMaxBytes,
          "unsafe or oversized local commit record");
    const std::string id = path.filename().string();
    check(UuidV7::parse(id).has_value(), "invalid local commit operation ID");
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "cannot read local commit record");
    const std::string body((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    check(body.starts_with(localCommitMarker) && body.ends_with('\n'),
          "invalid local commit record marker");
    std::istringstream lines(body.substr(localCommitMarker.size()));
    const std::function<std::string(std::string_view)> nextField =
        [&](std::string_view key) -> std::string {
            std::string line;
            check(static_cast<bool>(std::getline(lines, line)) &&
                  line.starts_with(std::string(key) + "\t"),
                  "invalid local commit field: " + std::string(key));
            return line.substr(key.size() + 1);
        };
    PersistedLocalCommit result;
    LocalCommitInfo& info = result.info;
    info.id = id;
    info.repositoryId = nextField("repository");
    info.projectId = nextField("project");
    info.branchId = nextField("branch");
    info.draftId = nextField("draft");
    info.expectedGeneration = decimalField(nextField("generation"));
    const std::string expected = nextField("expected");
    info.reservedRevisionId = nextField("reserved");
    result.committedAtMs = decimalField(nextField("committed-at"));
    {
        const std::optional<UuidV7> author = UuidV7::parse(nextField("author-id"));
        check(author.has_value(), "invalid local commit author UUIDv7");
        result.metadata.authorId = *author;
        const Bytes name = unhexBytes(nextField("author-name"));
        const Bytes message = unhexBytes(nextField("message"));
        result.metadata.authorName.assign(name.begin(), name.end());
        result.metadata.message.assign(message.begin(), message.end());
        try { validateCommitMetadata(result.metadata); }
        catch (const StorageError&) { check(false, "invalid local commit author or message"); }
        check(result.committedAtMs != 0, "local commit has no reserved commit time");
    }
    result.requestDigest = nextField("request-digest");
    info.payloadDigest = nextField("payload-digest");
    const std::string state = nextField("state");
    if (state == "pending") info.state = LocalCommitState::Pending;
    else if (state == "succeeded") info.state = LocalCommitState::Succeeded;
    else if (state == "rejected") info.state = LocalCommitState::Rejected;
    else check(false, "invalid local commit status");
    const std::string rejection = nextField("rejection");
    if (rejection != "-") info.rejection = rejection;
    const std::uint64_t count = decimalField(nextField("changes"));
    check(count > 0 && count <= workingCopyMaxChanges,
          "invalid local commit candidate count");
    std::optional<Key> lastKey;
    for (std::uint64_t index = 0; index < count; ++index) {
        const std::string row = nextField("change");
        const std::size_t split = row.find('\t');
        check(split == 32 && row.find('\t', split + 1) == std::string::npos,
              "invalid local commit candidate row");
        const Bytes keyBytes = unhexBytes(std::string_view(row).substr(0, split));
        check(keyBytes.size() == Key{}.size(), "invalid local commit model UUID");
        ModelChange change;
        std::copy(keyBytes.begin(), keyBytes.end(), change.key.begin());
        check(!lastKey || *lastKey < change.key, "unsorted local commit candidate");
        lastKey = change.key;
        const std::string_view value = std::string_view(row).substr(split + 1);
        if (value != "-") change.record = decodeModelRecord(unhexBytes(value));
        result.changes.push_back(std::move(change));
    }
    const std::uint64_t actionCount = decimalField(nextField("actions"));
    check(actionCount <= workingCopyMaxEvents, "invalid local Commit action count");
    const std::string actionChain = nextField("action-chain");
    check(hexDigest(actionChain), "invalid local Commit action chain digest");
    std::set<std::string> actionIds;
    std::size_t totalActionBytes = 0;
    for (std::uint64_t index = 0; index < actionCount; ++index) {
        const std::string row = nextField("action");
        const std::size_t split = row.find('\t');
        check(split == 36 && row.find('\t', split + 1) == std::string::npos,
              "invalid local Commit action row");
        const std::string id = row.substr(0, split);
        check(UuidV7::parse(id).has_value() && actionIds.insert(id).second,
              "invalid or duplicate archived Commit action identity");
        const Bytes encoded = unhexBytes(std::string_view(row).substr(split + 1));
        totalActionBytes += encoded.size() + 20;
        check(totalActionBytes <= workingCopyMaxBytes, "oversized local Commit action archive");
        try {
            const WorkingCopyEvent event = verifiedEditEvent(encoded);
            check(event.id == id, "archived Commit event identity mismatches frame");
            result.actions.push_back(event);
        } catch (const ProjectError&) {
            check(false, "invalid archived Commit MODEL event frame");
        }
    }
    const Hash archiveHash = chainJournal(Hash{}, result.actions);
    check(actionChain == hexBytes(archiveHash.data(), archiveHash.size()),
          "archived Commit action chain integrity failure");
    std::string extra;
    check(!std::getline(lines, extra) && lines.eof(), "trailing local commit fields");
    check(UuidV7::parse(info.repositoryId).has_value() &&
          ProjectId::parse(info.projectId).has_value() &&
          BranchId::parse(info.branchId).has_value() &&
          UuidV7::parse(info.draftId).has_value() &&
          RevisionId::parse(info.reservedRevisionId).has_value(),
          "invalid local commit UUID identity");
    if (expected != "-") {
        info.expectedHead = RevisionId::parse(expected);
        check(info.expectedHead.has_value(), "invalid expected local HEAD");
    }
    const std::function<bool(const std::string&)> validDigest = [](const std::string& value) {
        return value.size() == 64 &&
            std::all_of(value.begin(), value.end(), [](char ch) {
                return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
            });
    };
    check(validDigest(result.requestDigest) && validDigest(info.payloadDigest) &&
          info.payloadDigest == localDigest(result.requestDigest + "\n" +
              saveBatchFingerprint(result.changes, {}) + "\n"),
          "local commit payload digest mismatch");
    check((info.state == LocalCommitState::Rejected) == !info.rejection.empty() &&
          (info.rejection.empty() || info.rejection == "conflict" ||
           info.rejection == "invalid-input" || info.rejection == "not-found"),
          "invalid local commit rejection");
    return result;
}

// HIST-02: one immutable revision -> local Commit operation sidecar. Publish
// before the Commit intent, hence before the reserved revision can reach HEAD.
// A crash may leave an unreferenced sidecar, never a published revision without
// its mapping. A retry uses a fresh reserved revision unless the original
// durable operation exists. Orphan sidecars are harmless.
std::filesystem::path localRevisionIndexDirectory(const std::filesystem::path& root) {
    return root / "local-commit-revisions";
}

struct LocalRevisionIndex {
    std::string repository;
    std::string project;
    std::string revision;
    std::string operation;
};

std::string localRevisionIndexBody(const LocalRevisionIndex& index) {
    check(UuidV7::parse(index.repository).has_value() &&
          UuidV7::parse(index.project).has_value() &&
          RevisionId::parse(index.revision).has_value() &&
          UuidV7::parse(index.operation).has_value(),
          "invalid local revision index identity");
    const std::string payload = "AOWIS-LOCAL-REVISION-INDEX-1\n" +
        index.repository + "\n" + index.project + "\n" +
        index.revision + "\n" + index.operation + "\n";
    return payload + localDigest(payload) + "\n";
}

LocalRevisionIndex readLocalRevisionIndex(const std::filesystem::path& path) {
    check(!std::filesystem::is_symlink(path) &&
          std::filesystem::is_regular_file(path) &&
          std::filesystem::file_size(path) <= 320,
          "unsafe local revision index file");
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "cannot read local revision index");
    std::string magic, repository, project, revision, operation, checksum, extra;
    check(static_cast<bool>(std::getline(input, magic)) &&
          std::getline(input, repository) && std::getline(input, project) &&
          std::getline(input, revision) && std::getline(input, operation) &&
          std::getline(input, checksum) && !std::getline(input, extra) &&
          magic == "AOWIS-LOCAL-REVISION-INDEX-1" &&
          path.filename() == revision,
          "invalid local revision index record");
    const LocalRevisionIndex index{repository, project, revision, operation};
    const std::string expected = localRevisionIndexBody(index);
    check(expected == magic + "\n" + repository + "\n" + project + "\n" +
          revision + "\n" + operation + "\n" + checksum + "\n",
          "local revision index checksum mismatch");
    return index;
}

void persistLocalRevisionIndex(const std::filesystem::path& root,
                               const PersistedLocalCommit& operation) {
    const std::filesystem::path directory = localRevisionIndexDirectory(root);
    if (!std::filesystem::exists(directory)) {
        syscallChecked(std::filesystem::create_directory(directory),
                       "create revision index directory");
        syncDirectory(root);
    }
    check(!std::filesystem::is_symlink(directory) &&
          std::filesystem::is_directory(directory),
          "unsafe revision index directory");
    const std::string body = localRevisionIndexBody(LocalRevisionIndex{
        operation.info.repositoryId, operation.info.projectId,
        operation.info.reservedRevisionId, operation.info.id});
    const std::filesystem::path published =
        directory / operation.info.reservedRevisionId;
    if (std::filesystem::exists(published) || std::filesystem::is_symlink(published)) {
        const LocalRevisionIndex current = readLocalRevisionIndex(published);
        check(current.operation == operation.info.id &&
              current.repository == operation.info.repositoryId &&
              current.project == operation.info.projectId,
              "revision index UUID collision");
        return;
    }
    const std::filesystem::path staging = directory /
        (".creating-" + UuidV7::generate().toString());
    const int fd = ::open(staging.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    syscallChecked(fd >= 0, "stage local revision index");
    try {
        std::size_t written = 0;
        while (written < body.size()) {
            const ssize_t amount = ::write(fd, body.data() + written, body.size() - written);
            if (amount < 0 && errno == EINTR) continue;
            syscallChecked(amount > 0, "write local revision index");
            written += static_cast<std::size_t>(amount);
        }
        syscallChecked(::fsync(fd) == 0, "sync local revision index");
    } catch (...) {
        ::close(fd);
        (void)::unlink(staging.c_str());
        throw;
    }
    syscallChecked(::close(fd) == 0, "close local revision index");
    try {
        syscallChecked(::link(staging.c_str(), published.c_str()) == 0,
                       "publish local revision index");
        syncDirectory(directory);
        syscallChecked(::unlink(staging.c_str()) == 0,
                       "remove local revision index staging");
        syncDirectory(directory);
    } catch (...) {
        (void)::unlink(staging.c_str());
        throw;
    }
}

// A missing sidecar means a checkpoint with no indexed local Commit archive.
// A present one must match the full authenticated Commit record exactly.
std::optional<PersistedLocalCommit> indexedLocalCommit(
        const std::filesystem::path& root, std::string_view repositoryId,
        std::string_view projectId, const RevisionId& revision) {
    const std::filesystem::path index =
        localRevisionIndexDirectory(root) / revision.toString();
    if (!std::filesystem::exists(index) && !std::filesystem::is_symlink(index))
        return std::nullopt;
    const LocalRevisionIndex record = readLocalRevisionIndex(index);
    check(record.repository == repositoryId && record.project == projectId &&
          record.revision == revision.toString(),
          "indexed local revision belongs to another repository or project");
    const PersistedLocalCommit operation =
        readLocalCommit(localCommitDirectory(root) / record.operation);
    check(operation.info.repositoryId == repositoryId &&
          operation.info.projectId == projectId &&
          operation.info.reservedRevisionId == record.revision &&
          operation.info.id == record.operation,
          "revision index disagrees with local Commit archive");
    return operation;
}

void persistLocalCommit(const std::filesystem::path& root,
                        const PersistedLocalCommit& operation, bool initial) {
    const std::filesystem::path directory = localCommitDirectory(root);
    if (!std::filesystem::exists(directory)) {
        syscallChecked(std::filesystem::create_directory(directory),
                       "create local commit operation directory");
        syncDirectory(root);
    }
    check(!std::filesystem::is_symlink(directory) && std::filesystem::is_directory(directory),
          "unsafe local commit directory");
    const std::filesystem::path published = directory / operation.info.id;
    const std::filesystem::path staging = directory /
        (std::string(initial ? ".creating-" : ".updating-") +
         UuidV7::generate().toString());
    const std::string body = localCommitBody(operation);
    const int fd = ::open(staging.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    syscallChecked(fd >= 0, "stage local commit operation");
    try {
        std::size_t written = 0;
        while (written < body.size()) {
            const ssize_t amount = ::write(fd, body.data() + written, body.size() - written);
            if (amount < 0 && errno == EINTR) continue;
            syscallChecked(amount > 0, "write local commit operation");
            written += static_cast<std::size_t>(amount);
        }
        syscallChecked(::fsync(fd) == 0, "sync local commit operation");
    } catch (...) {
        ::close(fd);
        (void)::unlink(staging.c_str());
        throw;
    }
    syscallChecked(::close(fd) == 0, "close local commit operation");
    try {
        if (initial) {
            syscallChecked(::link(staging.c_str(), published.c_str()) == 0,
                           "publish new local commit operation");
            syncDirectory(directory);
            syscallChecked(::unlink(staging.c_str()) == 0,
                           "remove local commit staging record");
            syncDirectory(directory);
        } else {
            syscallChecked(::rename(staging.c_str(), published.c_str()) == 0,
                           "publish local commit operation outcome");
            syncDirectory(directory);
        }
    } catch (...) {
        (void)::unlink(staging.c_str());
        throw;
    }
}

// Reconcile an ambiguous crash after the immutable revision and/or HEAD was
// written. Never treat the presence of an orphan revision as proof of Commit:
// the reserved UUID must be reachable from this branch's current HEAD.
bool recoverLocalCommit(Repository& repository, const PersistedLocalCommit& operation) {
    if (operation.info.state != LocalCommitState::Pending) return false;
    const std::optional<ProjectId> project = ProjectId::parse(operation.info.projectId);
    const std::optional<BranchId> branch = BranchId::parse(operation.info.branchId);
    const std::optional<RevisionId> revision = RevisionId::parse(operation.info.reservedRevisionId);
    const std::optional<RevisionGraphHead> head = repository.branchById(*project, *branch);
    if (!head || !head->revision) return false;
    return *head->revision == *revision ||
           repository.revisionReachableFrom(*project, *head->revision, *revision);
}


} // namespace

ProjectService::Operation::Operation(std::shared_ptr<Project> project) noexcept
    : project_(std::move(project)) {}

ProjectService::Operation::Operation(Operation&& other) noexcept
    : project_(std::move(other.project_)) {}

ProjectService::Operation::~Operation() {
    if (this->project_) this->project_->pending.fetch_sub(1);
}

ProjectService::ProjectService(std::filesystem::path root, std::size_t maxPendingOperations,
                               LeaseClock leaseClock)
    : root_(std::filesystem::absolute(std::move(root)).lexically_normal()),
      projects_(this->root_ / "projects"), maxPending_(maxPendingOperations),
      leaseClock_(std::move(leaseClock)) {
    if (!this->leaseClock_.wallMs) this->leaseClock_.wallMs = systemEpochMs;
    if (!this->leaseClock_.steady) this->leaseClock_.steady = []() { return std::chrono::steady_clock::now(); };
    if (this->maxPending_ == 0) throw ProjectError(ProjectErrorKind::InvalidInput,
                                                    "at least one pending operation per project is required");
    std::filesystem::create_directories(this->projects_);
    // Persist the newly created projects/ entry before project publication.
    syncDirectory(this->root_);
    this->lockFd_ = ::open((this->root_ / "catalogue.lock").c_str(),
                           O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    syscallChecked(this->lockFd_ >= 0, "open project catalogue lock");
    if (::flock(this->lockFd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(this->lockFd_);
        this->lockFd_ = -1;
        throw ProjectError(ProjectErrorKind::Conflict,
                           "another database server owns this project root");
    }
    try {
        // This process owns catalogue.lock exclusively for its lifetime, so
        // no staged catalogue, draft, ledger or project entry can be in use.
        removeCatalogueRemnants(this->root_);
        removeDraftRemnants(this->root_);
        this->repository_id_ = loadRepositoryIdentity(this->root_);
        const TrackingStateOnDisk tracking = loadTrackingState(this->root_, this->repository_id_);
        this->repository_role_ = tracking.role;
        this->remotes_ = tracking.remotes;
        this->tracking_ = tracking.refs;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(this->projects_)) {
            const std::string id = entry.path().filename().string();
            check(validProjectId(id) && !entry.is_symlink() && entry.is_directory(),
                  "unexpected entry in project catalogue: " + entry.path().string());
            this->entries_.emplace(id, std::make_shared<Project>(readManifest(entry.path())));
        }
    } catch (...) {
        ::flock(this->lockFd_, LOCK_UN);
        ::close(this->lockFd_);
        this->lockFd_ = -1;
        throw;
    }
}

ProjectService::~ProjectService() {
    this->entries_.clear();
    if (this->lockFd_ >= 0) {
        ::flock(this->lockFd_, LOCK_UN);
        ::close(this->lockFd_);
    }
}

// Called with the catalogue mutex held.
void ProjectService::ensureHealthy() const {
    if (this->uncertain_) throw ProjectError(ProjectErrorKind::Unavailable,
        "catalogue publication status is uncertain; restart the database server");
}

std::uint64_t ProjectService::wallClockMs() const {
    const std::uint64_t now = this->leaseClock_.wallMs();
    check(now > 0, "invalid system clock", StorageErrorKind::Unavailable);
    return now;
}

bool ProjectService::workingCopyLeaseLive(const WorkingCopyInfo& draft) const {
    if (!draft.leaseSessionId || !draft.leaseToken) return false;
    {
        std::lock_guard<std::mutex> guard(this->leaseDeadlinesMutex_);
        const std::map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>>::const_iterator
            granted = this->leaseDeadlines_.find(draft.id);
        if (granted != this->leaseDeadlines_.end() && granted->second.first == *draft.leaseToken)
            return this->leaseClock_.steady() < granted->second.second;
    }
    // Granted by an earlier process (or another handle): only the persisted
    // wall-clock expiry is known.
    return draft.leaseExpiresAtMs > wallClockMs();
}

void ProjectService::rememberLease(const WorkingCopyInfo& draft, std::uint32_t seconds) {
    std::lock_guard<std::mutex> guard(this->leaseDeadlinesMutex_);
    this->leaseDeadlines_[draft.id] = {*draft.leaseToken,
        this->leaseClock_.steady() + std::chrono::seconds(seconds)};
}

void ProjectService::forgetLease(std::string_view draftId) {
    std::lock_guard<std::mutex> guard(this->leaseDeadlinesMutex_);
    this->leaseDeadlines_.erase(std::string(draftId));
}

void ProjectService::checkHealthy() const {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
}

// The role is configured once and never changes afterwards, so a check
// under the catalogue mutex stays valid for the rest of the operation.
void ProjectService::requirePrivateRepository(const char* message) const {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    if (this->repository_role_ != RepositoryRole::Private)
        throw ProjectError(ProjectErrorKind::Conflict, message);
}

void ProjectService::markUncertain() {
    std::lock_guard<std::mutex> guard(this->mutex_);
    this->uncertain_ = true;
}

std::shared_ptr<std::mutex> ProjectService::draftLock(std::string_view draftId) const {
    std::lock_guard<std::mutex> guard(this->draftLocksMutex_);
    for (std::map<std::string, std::weak_ptr<std::mutex>>::iterator entry = this->draftLocks_.begin();
         entry != this->draftLocks_.end();) {
        if (entry->second.expired()) entry = this->draftLocks_.erase(entry);
        else ++entry;
    }
    std::weak_ptr<std::mutex>& slot = this->draftLocks_[std::string(draftId)];
    std::shared_ptr<std::mutex> lock = slot.lock();
    if (!lock) {
        lock = std::make_shared<std::mutex>();
        slot = lock;
    }
    return lock;
}

std::string ProjectService::repositoryId() const {
    return this->repository_id_;
}

RepositoryRole ProjectService::repositoryRole() const {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    return this->repository_role_;
}

void ProjectService::persistTrackingMetadata() {
    TrackingStateOnDisk current;
    current.role = this->repository_role_;
    current.remotes = this->remotes_;
    current.refs = this->tracking_;
    // Never continue accepting metadata writes after an uncertain durable
    // publication. The authoritative state must be reloaded on restart.
    try { storeTrackingState(this->root_, current); }
    catch (...) { this->uncertain_ = true; throw; }
}

void ProjectService::configureRepositoryRole(RepositoryRole role) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    if (role == RepositoryRole::Unconfigured)
        throw ProjectError(ProjectErrorKind::InvalidInput, "choose private or shared role explicitly");
    if (this->repository_role_ == role) return;
    if (this->repository_role_ != RepositoryRole::Unconfigured)
        throw ProjectError(ProjectErrorKind::Conflict, "repository role already configured");
    this->repository_role_ = role;
    persistTrackingMetadata();
}

std::vector<RemoteRepository> ProjectService::remoteRepositories() const {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    std::vector<RemoteRepository> output;
    for (const std::pair<const std::string, RemoteRepository>& entry : this->remotes_)
        output.push_back(entry.second);
    return output;
}

void ProjectService::registerRemoteRepository(std::string_view remoteRepositoryId,
                                                 std::string_view endpoint) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    if (!UuidV7::parse(remoteRepositoryId).has_value() ||
        remoteRepositoryId == this->repository_id_ || !validRemoteEndpoint(endpoint))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid remote identity or endpoint");
    const std::string id(remoteRepositoryId);
    const std::string url(endpoint);
    const std::map<std::string, RemoteRepository>::const_iterator existing =
        this->remotes_.find(id);
    if (existing != this->remotes_.end()) {
        if (existing->second.endpoint == url) return;
        throw ProjectError(ProjectErrorKind::Conflict,
                           "remote repository UUID already registered at another endpoint");
    }
    for (const std::pair<const std::string, RemoteRepository>& entry : this->remotes_) {
        if (entry.second.endpoint == url)
            throw ProjectError(ProjectErrorKind::Conflict,
                               "remote endpoint already registered under another UUID");
    }
    if (this->remotes_.size() >= 64)
        throw ProjectError(ProjectErrorKind::InvalidInput, "remote registry limit exceeded");
    this->remotes_.emplace(id, RemoteRepository{id, url});
    persistTrackingMetadata();
}

std::vector<RemoteTrackingRef> ProjectService::remoteTrackingRefs() const {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    std::vector<RemoteTrackingRef> output;
    for (const std::pair<const TrackingKey, RemoteTrackingRef>& entry : this->tracking_)
        output.push_back(entry.second);
    return output;
}

void ProjectService::recordRemoteTrackingRef(
    const RemoteTrackingRef& observation,
    std::optional<std::optional<RevisionId>> expectedObservedHead) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    if (!this->remotes_.contains(observation.remoteRepositoryId) ||
        !ProjectId::parse(observation.projectId).has_value() ||
        !BranchId::parse(observation.branchId).has_value())
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "tracking ref must target a registered repository and UUIDv7 project/branch");
    const TrackingKey key{observation.remoteRepositoryId, observation.projectId,
                          observation.branchId};
    const std::map<TrackingKey, RemoteTrackingRef>::const_iterator current =
        this->tracking_.find(key);
    if (!expectedObservedHead.has_value()) {
        if (current != this->tracking_.end())
            throw ProjectError(ProjectErrorKind::Conflict, "remote tracking ref already exists");
    } else if (current == this->tracking_.end() ||
               current->second.observedHead != *expectedObservedHead) {
        throw ProjectError(ProjectErrorKind::Conflict, "remote tracking HEAD changed");
    }
    if (current != this->tracking_.end() &&
        current->second.observedHead == observation.observedHead) return;
    if (current == this->tracking_.end() && this->tracking_.size() >= 10000)
        throw ProjectError(ProjectErrorKind::InvalidInput, "remote tracking ref limit exceeded");
    this->tracking_.insert_or_assign(key, observation);
    persistTrackingMetadata();
}

std::size_t ProjectService::maxPendingOperations() const noexcept {
    return this->maxPending_;
}

ProjectInfo ProjectService::createUnbornProject(std::string_view name,
                                                 std::optional<std::string_view> projectId) {
    if (projectId && !ProjectId::parse(*projectId))
        throw ProjectError(ProjectErrorKind::InvalidInput, "requested project ID must be UUIDv7");
    return createProjectImpl(name, true,
                             projectId ? std::optional<std::string>(std::string(*projectId))
                                       : std::nullopt);
}

ProjectInfo ProjectService::renameProject(std::string_view projectId,
                                         std::string_view expectedName,
                                         std::string_view name) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    if (!validProjectId(projectId) || !validName(name))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid project ID or name");
    const std::map<std::string, std::shared_ptr<Project>>::iterator found =
        this->entries_.find(std::string(projectId));
    if (found == this->entries_.end())
        throw ProjectError(ProjectErrorKind::NotFound, "project not found");
    if (found->second->info.name != expectedName)
        throw ProjectError(ProjectErrorKind::Conflict, "project name was changed by another client");
    if (found->second->info.name == name) return found->second->info;

    ProjectInfo updated = found->second->info;
    updated.name = std::string(name);
    const std::filesystem::path directory = this->projects_ / updated.id;
    const std::filesystem::path staged = directory /
        (".project-meta-updating-" + UuidV7::generate().toString());
    bool published = false;
    try {
        writeManifest(staged, updated);
        syscallChecked(::rename(staged.c_str(), (directory / "project.meta").c_str()) == 0,
                       "publish renamed project manifest");
        published = true;
        syncDirectory(directory);
        found->second->info.name = updated.name;
    } catch (...) {
        if (published) this->uncertain_ = true;
        else {
            std::error_code ignored;
            std::filesystem::remove(staged, ignored);
        }
        throw;
    }
    return updated;
}

ProjectInfo ProjectService::provisionCloneTarget(std::string_view projectId,
                                                    std::string_view name) {
    if (!ProjectId::parse(projectId))
        throw ProjectError(ProjectErrorKind::InvalidInput, "clone target project must be UUIDv7");
    return createProjectImpl(name, false, std::string(projectId));
}

void ProjectService::provisionUnbornBranch(std::string_view projectId,
                                             std::string_view name,
                                             std::string_view branchId) {
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    const std::optional<BranchId> identity = BranchId::parse(branchId);
    if (!project || !identity || !validBranchName(name))
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "unborn checkout requires valid project, branch and name");
    requirePrivateRepository("unborn checkout requires a private repository");
    withRepository(projectId, [&](Repository& repository) {
        repository.provisionUnbornBranch(*project, name, *identity);
    });
}

BranchId ProjectService::createUnbornBranch(std::string_view projectId,
                                            std::string_view name,
                                            std::optional<std::string_view> branchId,
                                            const BranchMetadata& metadata) {
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project || !validBranchName(name))
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "unborn branch requires a valid project UUID and name");
    std::optional<BranchId> identity;
    if (branchId) {
        identity = BranchId::parse(*branchId);
        if (!identity)
            throw ProjectError(ProjectErrorKind::InvalidInput,
                               "new branch ID must be UUIDv7");
    }
    std::optional<BranchId> result;
    withRepository(projectId, [&](Repository& repository) {
        result = translated<BranchId>([&]() {
            return repository.createUnbornBranch(*project, name, identity, metadata);
        });
    });
    return *result;
}

ProjectInfo ProjectService::createProject(std::string_view name,
                                          const CommitMetadata& rootMetadata) {
    translated<int>([&]() { validateCommitMetadata(rootMetadata); return 0; });
    return createProjectImpl(name, false, std::nullopt, &rootMetadata);
}

ProjectInfo ProjectService::createProjectImpl(std::string_view name, bool unborn,
                                             std::optional<std::string> requestedProjectId,
                                             const CommitMetadata* rootMetadata) {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    if (!validName(name)) throw ProjectError(ProjectErrorKind::InvalidInput,
                                            "project name must contain 1..200 non-control bytes");
    ProjectInfo info;
    info.name = std::string(name);
    const bool cloneTarget = requestedProjectId.has_value() && !unborn;
    if (requestedProjectId) {
        info.id = *requestedProjectId;
        const std::map<std::string, std::shared_ptr<Project>>::const_iterator existing =
            this->entries_.find(info.id);
        if (existing != this->entries_.end()) {
            // Repeated request: the creation already happened.
            if (existing->second->info.name != info.name)
                throw ProjectError(ProjectErrorKind::Conflict,
                                   "project UUID already has a different name");
            return existing->second->info;
        }
        if (std::filesystem::exists(this->projects_ / (".creating-" + info.id)))
            throw ProjectError(ProjectErrorKind::Busy, "project publication is pending");
    } else do {
        try { info.id = ProjectId::generate().toString(); }
        catch (const std::runtime_error& error) {
            throw StorageError(StorageErrorKind::Internal,
                               std::string("failed to generate secure UUIDv7 project ID: ") + error.what());
        }
    }
    while (std::filesystem::exists(this->projects_ / info.id) ||
           std::filesystem::exists(this->projects_ / (".creating-" + info.id)));
    const std::filesystem::path staging = this->projects_ / (".creating-" + info.id);
    const std::filesystem::path published = this->projects_ / info.id;
    std::filesystem::create_directory(staging);
    bool renamed = false;
    try {
        writeManifest(staging / "project.meta", info);
        {
            Repository repository(staging / "repository");
            if (cloneTarget) {
                // No local branch, revision or model is fabricated: importing
                // the verified bundle establishes the original branch UUID.
            } else {
                const std::optional<ProjectId> project = ProjectId::parse(info.id);
                if (!project) throw StorageError(StorageErrorKind::Internal,
                                                 "new project UUIDv7 did not validate");
                if (unborn) repository.createUnbornBranch(*project, "main");
                else (void)repository.createRootBranch(*project, "main", *rootMetadata);
            }
        }
        syncDirectory(staging / "repository" / "refs" / "heads");
        syncDirectory(staging / "repository" / "refs");
        syncDirectory(staging / "repository");
        syncDirectory(staging);
        syscallChecked(::rename(staging.c_str(), published.c_str()) == 0,
                       "publish project directory");
        renamed = true;
        syncDirectory(this->projects_);
        this->entries_.emplace(info.id, std::make_shared<Project>(info));
    } catch (...) {
        if (renamed) {
            this->uncertain_ = true;
        } else {
            std::error_code ignored; // never replace the original exception
            std::filesystem::remove_all(staging, ignored);
        }
        throw;
    }
    return info;
}

std::shared_ptr<ProjectService::Project> ProjectService::findProject(std::string_view id) const {
    if (!validProjectId(id)) throw ProjectError(ProjectErrorKind::InvalidInput,
                                                "invalid project UUID");
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    const std::map<std::string, std::shared_ptr<Project>>::const_iterator it =
        this->entries_.find(std::string(id));
    if (it == this->entries_.end())
        throw ProjectError(ProjectErrorKind::NotFound, "project not found");
    return it->second;
}

// Opening scans the pack tail and loads the index; only this project waits.
std::shared_ptr<Repository> ProjectService::openRepository(Project& project) const {
    std::lock_guard<std::mutex> guard(project.openMutex);
    if (!project.repository) {
        const std::filesystem::path directory = this->projects_ / project.info.id / "repository";
        check(!std::filesystem::is_symlink(directory) &&
              std::filesystem::is_directory(directory), "project repository directory is unavailable");
        project.repository = std::make_shared<Repository>(directory);
        const std::uint64_t recovered = project.repository->recoveredPackTailBytes();
        if (recovered != 0) {
            std::cerr << "AOWIS database: project " << project.info.id << " discarded "
                      << recovered << " bytes of an unpublished torn pack tail; a copy is in "
                      << (directory / "recovered-pack-tails").string() << '\n';
        }
    }
    return project.repository;
}

WorkingCopyInfo ProjectService::createWorkingCopy(
        std::string_view projectId, std::string_view branch,
        std::optional<RevisionId> baseRevision, std::string_view sessionId) {
    if (!UuidV7::parse(sessionId).has_value() || !validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid draft session or branch");
    const std::optional<BranchId> localBranch = branchId(projectId, branch);
    if (!localBranch)
        throw ProjectError(ProjectErrorKind::InvalidInput, "draft requires a UUIDv7 branch");
    return createWorkingCopyForBranch(projectId, localBranch->toString(), baseRevision,
                                        sessionId);
}

WorkingCopyInfo ProjectService::createWorkingCopyForBranch(
        std::string_view projectId, std::string_view branchId,
        std::optional<RevisionId> baseRevision, std::string_view sessionId,
        std::optional<std::string_view> draftId) {
    if (!UuidV7::parse(sessionId).has_value() || !BranchId::parse(branchId).has_value() ||
        (draftId && !UuidV7::parse(*draftId).has_value()))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid draft, session or branch UUIDv7");
    requirePrivateRepository("working copies require an explicitly private repository");
    // This first VC-04 slice supports exact HEAD checkout only. A concurrent
    // commit after this read cannot change the immutable pinned draft base.
    const std::optional<RevisionGraphHead> branch = branchById(projectId, branchId);
    if (!branch)
        throw ProjectError(ProjectErrorKind::NotFound, "branch UUIDv7 not found");
    WorkingCopyInfo draft;
    draft.id = draftId ? std::string(*draftId) : UuidV7::generate().toString();
    draft.projectId = std::string(projectId);
    draft.branchId = branch->branchId.toString();
    draft.baseRevision = baseRevision;
    draft.sessionId = std::string(sessionId);
    const std::shared_ptr<std::mutex> lock = draftLock(draft.id);
    std::lock_guard<std::mutex> guard(*lock);
    checkHealthy();
    const std::filesystem::path existing = workingCopyDirectory(this->root_) / draft.id;
    if (draftId && (std::filesystem::exists(existing) || std::filesystem::is_symlink(existing))) {
        // Repeated request (e.g. a transport resent the POST after a lost
        // response): answer with the draft it created, in its current state.
        const WorkingCopyInfo stored = readWorkingCopy(existing, DraftLoad::Candidate);
        if (stored.projectId != draft.projectId || stored.branchId != draft.branchId ||
            stored.baseRevision != draft.baseRevision || stored.sessionId != draft.sessionId)
            throw ProjectError(ProjectErrorKind::Conflict,
                               "draft UUIDv7 already identifies a different working copy");
        return stored;
    }
    if (branch->revision != baseRevision)
        throw ProjectError(ProjectErrorKind::Conflict,
                           "working-copy base must match current local HEAD");
    try { createDraft(this->root_, draft); }
    catch (...) { markUncertain(); throw; }
    return draft;
}

WorkingCopyInfo ProjectService::workingCopy(std::string_view draftId, bool includeJournal) const {
    if (!UuidV7::parse(draftId).has_value())
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid working-copy UUIDv7");
    checkHealthy();
    return loadDraft(this->root_, draftId,
                     includeJournal ? DraftLoad::Full : DraftLoad::Candidate).info;
}

std::vector<WorkingCopyInfo> ProjectService::listWorkingCopies() const {
    checkHealthy();
    std::vector<WorkingCopyInfo> result;
    const std::filesystem::path directory = workingCopyDirectory(this->root_);
    for (const std::string& id : publishedDraftIds(directory))
        result.push_back(readWorkingCopy(directory / id, DraftLoad::Meta));
    return result;
}

WorkingCopyPage ProjectService::listWorkingCopies(std::optional<std::string_view> projectId,
                                                  std::size_t limit,
                                                  std::optional<std::string_view> after) const {
    if (limit == 0 || limit > 1000)
        throw ProjectError(ProjectErrorKind::InvalidInput, "draft page size must be 1..1000");
    if ((projectId && !validProjectId(*projectId)) || (after && !UuidV7::parse(*after)))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid draft list project or cursor");
    checkHealthy();
    WorkingCopyPage page;
    const std::filesystem::path directory = workingCopyDirectory(this->root_);
    for (const std::string& id : publishedDraftIds(directory)) {
        if (after && id <= *after) continue;
        if (page.workingCopies.size() == limit) {
            page.next = page.workingCopies.back().id;
            break;
        }
        // A draft discarded since the directory scan is simply not listed.
        std::optional<StoredDraft> stored = readDraft(directory / id, DraftLoad::Meta);
        if (!stored || (projectId && stored->info.projectId != *projectId)) continue;
        WorkingCopyInfo& draft = stored->info;
        page.workingCopies.push_back(std::move(draft));
    }
    return page;
}

void ProjectService::discardWorkingCopy(std::string_view draftId, std::string_view sessionId,
                                        std::string_view fencingToken,
                                        std::uint64_t expectedGeneration) {
    if (!UuidV7::parse(draftId).has_value() || !UuidV7::parse(sessionId).has_value() ||
        !UuidV7::parse(fencingToken).has_value())
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid draft discard parameters");
    const std::shared_ptr<std::mutex> lock = draftLock(draftId);
    std::lock_guard<std::mutex> guard(*lock);
    requirePrivateRepository("draft requires a private repository");
    const WorkingCopyInfo draft = loadDraft(this->root_, draftId, DraftLoad::Meta).info;
    if (!draft.leaseSessionId || *draft.leaseSessionId != sessionId ||
        !draft.leaseToken || *draft.leaseToken != fencingToken ||
        !workingCopyLeaseLive(draft) || draft.generation != expectedGeneration)
        throw ProjectError(ProjectErrorKind::Conflict, "draft lease, fencing token or generation changed");
    try { deleteDraft(this->root_, draftId); }
    catch (...) { markUncertain(); throw; }
    forgetLease(draftId);
}

WorkingCopyLease ProjectService::acquireWorkingCopyLease(
        std::string_view draftId, std::string_view sessionId, std::uint32_t seconds) {
    if (!UuidV7::parse(draftId).has_value() || !UuidV7::parse(sessionId).has_value() ||
        seconds < 1 || seconds > 120)
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid draft lease parameters");
    const std::shared_ptr<std::mutex> lock = draftLock(draftId);
    std::lock_guard<std::mutex> guard(*lock);
    requirePrivateRepository("draft requires a private repository");
    // A lease touches only the draft's small meta record.
    StoredDraft stored = loadDraft(this->root_, draftId, DraftLoad::Meta);
    WorkingCopyInfo draft = stored.info;
    const std::uint64_t now = wallClockMs();
    if (workingCopyLeaseLive(draft) && *draft.leaseSessionId != sessionId)
        throw ProjectError(ProjectErrorKind::Conflict, "draft held by another writer session");
    draft.leaseSessionId = std::string(sessionId);
    draft.leaseToken = UuidV7::generate().toString();
    draft.leaseExpiresAtMs = now + static_cast<std::uint64_t>(seconds) * 1000;
    try { updateDraft(workingCopyDirectory(this->root_) / draft.id, stored, draft, DraftUpdate{}); }
    catch (...) { markUncertain(); throw; }
    rememberLease(draft, seconds);
    return WorkingCopyLease{*draft.leaseToken, draft.leaseExpiresAtMs, draft.generation};
}

WorkingCopyInfo ProjectService::saveWorkingCopyChanges(
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken, std::uint64_t expectedGeneration,
        const std::vector<ModelChange>& changes) {
    if (!UuidV7::parse(draftId).has_value() || !UuidV7::parse(sessionId).has_value() ||
        !UuidV7::parse(fencingToken).has_value() || changes.size() > workingCopyMaxChanges)
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid draft save parameters");
    const std::shared_ptr<std::mutex> lock = draftLock(draftId);
    std::lock_guard<std::mutex> guard(*lock);
    requirePrivateRepository("draft requires a private repository");
    StoredDraft stored = loadDraft(this->root_, draftId, DraftLoad::Candidate);
    WorkingCopyInfo draft = stored.info;
    if (draft.journalCount != 0)
        throw ProjectError(ProjectErrorKind::Conflict,
                           "journalled draft requires atomic event saves");
    if (!draft.leaseSessionId || *draft.leaseSessionId != sessionId ||
        !draft.leaseToken || *draft.leaseToken != fencingToken ||
        !workingCopyLeaseLive(draft) || draft.generation != expectedGeneration)
        throw ProjectError(ProjectErrorKind::Conflict, "draft lease, fencing token or generation changed");
    if (draft.generation == std::numeric_limits<std::uint64_t>::max())
        throw ProjectError(ProjectErrorKind::Conflict, "draft generation exhausted");
    std::set<Key> seen;
    for (const ModelChange& change : changes)
        if (!seen.insert(change.key).second)
            throw ProjectError(ProjectErrorKind::InvalidInput, "duplicate key in draft save batch");
    draft.candidateChanges = mergedCandidate(draft.candidateChanges, changes);
    draft.dirty = !draft.candidateChanges.empty();
    draft.historyCheckpointRevisionId.reset();
    draft.historyActionPosition.reset();
    draft.historyArchiveCommitOperationId.reset();
    draft.draftActionPosition.reset();
    draft.lastHistoryNavigationOperationId.reset();
    draft.lastHistoryNavigationSourceGeneration.reset();
    ++draft.generation;
    DraftUpdate update;
    update.candidate = draft.candidateChanges;
    prepareDraftUpdate(stored, update); // Limits are checked BEFORE publishing.
    try { updateDraft(workingCopyDirectory(this->root_) / draft.id, stored, draft, update); }
    catch (...) { markUncertain(); throw; }
    return draft;
}

WorkingCopyInfo ProjectService::saveWorkingCopyTransactions(
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken, std::uint64_t expectedGeneration,
        const std::vector<ModelChange>& changes,
        const std::vector<Bytes>& encodedTransactions,
        std::optional<std::string> logicalRequestFingerprint) {
    if (!UuidV7::parse(draftId).has_value() || !UuidV7::parse(sessionId).has_value() ||
        !UuidV7::parse(fencingToken).has_value() || changes.size() > workingCopyMaxChanges ||
        encodedTransactions.empty() || encodedTransactions.size() > 128)
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid draft event-save parameters");
    std::vector<WorkingCopyEvent> events;
    std::set<std::string> incoming;
    for (const Bytes& bytes : encodedTransactions) {
        WorkingCopyEvent event = verifiedEditEvent(bytes);
        if (!incoming.insert(event.id).second)
            throw ProjectError(ProjectErrorKind::InvalidInput, "duplicate edit event UUID");
        events.push_back(std::move(event));
    }
    const std::string fingerprint = logicalRequestFingerprint.has_value()
        ? *logicalRequestFingerprint : saveBatchFingerprint(changes, events);
    if (logicalRequestFingerprint.has_value() &&
        (fingerprint.size() != 64 ||
         !std::all_of(fingerprint.begin(), fingerprint.end(), [](char ch) {
             return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         })))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid logical event request fingerprint");
    const std::shared_ptr<std::mutex> lock = draftLock(draftId);
    std::lock_guard<std::mutex> guard(*lock);
    requirePrivateRepository("draft requires a private repository");
    // Normally reads only the overlay and journal UUIDs (frame headers).
    // Branching after Undo additionally verifies the complete retained journal
    // so the surviving prefix can be rewritten to a fresh durable file.
    std::vector<std::string> journalIds;
    StoredDraft stored = loadDraft(this->root_, draftId, DraftLoad::Candidate, &journalIds);
    WorkingCopyInfo draft = stored.info;
    if (!draft.leaseSessionId || *draft.leaseSessionId != sessionId ||
        !draft.leaseToken || *draft.leaseToken != fencingToken ||
        !workingCopyLeaseLive(draft))
        throw ProjectError(ProjectErrorKind::Conflict, "stale draft writer lease");
    // Lost HTTP acknowledgements are resolved without repeating a Save. The
    // replayed Save's events are the journal's last ones.
    if (expectedGeneration < std::numeric_limits<std::uint64_t>::max() &&
        draft.generation == expectedGeneration + 1 &&
        draft.lastSaveFingerprint == fingerprint) {
        WorkingCopyInfo replayed = loadDraft(this->root_, draftId, DraftLoad::Full).info;
        if (replayed.generation != draft.generation || replayed.journal.size() < events.size())
            throw ProjectError(ProjectErrorKind::Conflict, "draft generation changed");
        replayed.journal.erase(replayed.journal.begin(),
            replayed.journal.end() - static_cast<std::ptrdiff_t>(events.size()));
        return replayed;
    }
    if (draft.generation != expectedGeneration ||
        draft.generation == std::numeric_limits<std::uint64_t>::max())
        throw ProjectError(ProjectErrorKind::Conflict, "draft generation changed");
    const std::set<std::string> applied(journalIds.begin(), journalIds.end());
    for (const WorkingCopyEvent& event : events)
        if (applied.contains(event.id))
            throw ProjectError(ProjectErrorKind::Conflict, "edit event already saved");
    // An edit made after Undo branches from the *selected* journal boundary.
    // Retain only its acknowledged prefix; the forward Redo tail is no longer
    // eligible. Never truncate the old journal in place: the complete new
    // journal is fsynced into a separate file, then published with the new
    // candidate and cursor in one atomic meta replacement. A crash before
    // that replacement leaves the original Redo tail intact.
    const bool invalidateRedo = draft.draftActionPosition &&
        *draft.draftActionPosition < draft.journalCount;
    const std::uint64_t retainedCount = invalidateRedo
        ? *draft.draftActionPosition : draft.journalCount;
    if (retainedCount + events.size() > workingCopyMaxEvents)
        throw StorageError(StorageErrorKind::LimitExceeded,
                           "draft event journal is full; commit or discard the draft");
    std::vector<WorkingCopyEvent> replacementJournal;
    if (invalidateRedo) {
        const StoredDraft complete = loadDraft(this->root_, draftId, DraftLoad::Full);
        if (complete.info.generation != draft.generation ||
            complete.info.journal.size() != draft.journalCount)
            throw ProjectError(ProjectErrorKind::Conflict, "draft journal changed during Save");
        replacementJournal.assign(complete.info.journal.begin(),
            complete.info.journal.begin() + static_cast<std::ptrdiff_t>(retainedCount));
        replacementJournal.insert(replacementJournal.end(), events.begin(), events.end());
    }
    draft.candidateChanges = mergedCandidate(draft.candidateChanges, changes);
    draft.journal = events; // Save acknowledgement reports only the incoming actions.
    draft.journalCount = retainedCount + events.size();
    draft.dirty = !draft.candidateChanges.empty();
    draft.lastSaveFingerprint = fingerprint;
    draft.historyCheckpointRevisionId.reset();
    draft.historyActionPosition.reset();
    draft.historyArchiveCommitOperationId.reset();
    draft.draftActionPosition.reset();
    draft.lastHistoryNavigationOperationId.reset();
    draft.lastHistoryNavigationSourceGeneration.reset();
    ++draft.generation;
    DraftUpdate update;
    if (!changes.empty()) update.candidate = draft.candidateChanges;
    update.resetJournal = invalidateRedo;
    update.appended = invalidateRedo ? std::move(replacementJournal) : events;
    prepareDraftUpdate(stored, update); // Limits are checked BEFORE publishing.
    try { updateDraft(workingCopyDirectory(this->root_) / draft.id, stored, draft, update); }
    catch (...) { markUncertain(); throw; }
    return draft;
}

namespace {
// After a successful local Commit the draft continues from the new HEAD:
// base = committed revision, empty candidate and journal (both are now
// history), generation + 1, lease unchanged. Only a draft still exactly at
// the committed state is rebased, so the step is idempotent and also heals
// a crash between the outcome record and the draft update. Caller holds the
// draft lock.
void rebaseCommittedDraft(const std::filesystem::path& root, const PersistedLocalCommit& operation,
                          const std::function<void()>& markUncertain) {
    if (operation.info.state != LocalCommitState::Succeeded) return;
    const std::filesystem::path path = workingCopyDirectory(root) / operation.info.draftId;
    std::optional<StoredDraft> stored = readDraft(path, DraftLoad::Meta);
    if (!stored) return; // discarded
    WorkingCopyInfo draft = stored->info;
    const std::optional<RevisionId> committed = RevisionId::parse(operation.info.reservedRevisionId);
    if (!committed || draft.projectId != operation.info.projectId ||
        draft.branchId != operation.info.branchId ||
        draft.baseRevision != operation.info.expectedHead ||
        draft.generation != operation.info.expectedGeneration ||
        draft.generation == std::numeric_limits<std::uint64_t>::max())
        return;
    draft.baseRevision = committed;
    draft.candidateChanges.clear();
    draft.journal.clear();
    draft.journalCount = 0;
    draft.lastSaveFingerprint.reset();
    draft.historyCheckpointRevisionId.reset();
    draft.historyActionPosition.reset();
    draft.historyArchiveCommitOperationId.reset();
    draft.draftActionPosition.reset();
    draft.lastHistoryNavigationOperationId.reset();
    draft.lastHistoryNavigationSourceGeneration.reset();
    draft.dirty = false;
    ++draft.generation;
    DraftUpdate update;
    update.candidate = std::vector<ModelChange>{};
    update.resetJournal = true;
    try { updateDraft(path, *stored, draft, update); }
    catch (...) { markUncertain(); throw; }
}

// Decides a pending operation from the branch alone, under the project's
// operation mutex (no concurrent attempt) and the fact that a HEAD never
// returns to an older revision:
// - the reserved revision is HEAD or reachable from it: Succeeded;
// - HEAD differs from the expected base: the reserved revision can never
//   become HEAD (Commit requires exactly that base), so Rejected/conflict;
// - the branch no longer exists: Rejected/not-found.
// Persists a decided outcome and returns true when the operation is no
// longer pending; false means "pending, HEAD still at the expected base".
bool resolvePendingLocalCommit(Repository& repository, PersistedLocalCommit& operation,
                               const std::filesystem::path& root,
                               const std::function<void()>& markUncertain) {
    if (operation.info.state != LocalCommitState::Pending) return true;
    if (translated<bool>([&]() { return recoverLocalCommit(repository, operation); })) {
        operation.info.state = LocalCommitState::Succeeded;
    } else {
        const std::optional<ProjectId> project = ProjectId::parse(operation.info.projectId);
        const std::optional<BranchId> branch = BranchId::parse(operation.info.branchId);
        const std::optional<RevisionGraphHead> current =
            translated<std::optional<RevisionGraphHead>>([&]() {
                return repository.branchById(*project, *branch);
            });
        if (current && current->revision == operation.info.expectedHead) return false;
        operation.info.state = LocalCommitState::Rejected;
        operation.info.rejection = current ? "conflict" : "not-found";
    }
    try { persistLocalCommit(root, operation, false); }
    catch (...) { markUncertain(); throw; }
    return true;
}

} // namespace

LocalCommitInfo ProjectService::commitWorkingCopy(
        std::string_view operationId, std::string_view draftId,
        std::string_view sessionId, std::string_view fencingToken,
        std::uint64_t expectedGeneration,
        std::optional<RevisionId> expectedLocalHead,
        const CommitMetadata& metadata) {
    translated<int>([&]() { validateCommitMetadata(metadata); return 0; });
    if (!UuidV7::parse(operationId).has_value() ||
        !UuidV7::parse(draftId).has_value() ||
        !UuidV7::parse(sessionId).has_value() ||
        !UuidV7::parse(fencingToken).has_value())
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid local commit operation identity");
    const std::filesystem::path operationPath = localCommitDirectory(this->root_) /
        std::string(operationId);
    requirePrivateRepository("local Commit requires a private repository");
    // The project of an operation or draft is immutable, so it can be read
    // before taking any lock.
    std::string projectId;
    if (std::filesystem::exists(operationPath) || std::filesystem::is_symlink(operationPath)) {
        projectId = readLocalCommit(operationPath).info.projectId;
    } else {
        projectId = loadDraft(this->root_, draftId, DraftLoad::Meta).info.projectId;
    }
    const std::string requestDigest = localRequestDigest(
        this->repository_id_, draftId, sessionId, fencingToken,
        expectedGeneration, expectedLocalHead, metadata);
    LocalCommitInfo outcome;
    // The project's operation mutex covers the complete intent/commit/outcome
    // sequence and every ledger record of the project; the draft lock keeps
    // a concurrent Save from replacing the candidate. Neither is the
    // catalogue mutex, so other projects and the catalogue stay available.
    withRepository(projectId, [&](Repository& repository) {
        const std::shared_ptr<std::mutex> lock = draftLock(draftId);
        std::lock_guard<std::mutex> guard(*lock);
        requirePrivateRepository("not a private repository");
        const bool exists = std::filesystem::exists(operationPath) ||
                            std::filesystem::is_symlink(operationPath);
        PersistedLocalCommit operation;
        if (exists) {
            operation = readLocalCommit(operationPath);
            if (operation.info.repositoryId != this->repository_id_ ||
                operation.info.draftId != draftId ||
                operation.requestDigest != requestDigest)
                throw ProjectError(ProjectErrorKind::Conflict,
                                   "local Commit UUIDv7 reused with a different request");
        } else {
            // Freeze the complete acknowledged event stream inside the
            // fsynced Commit intent BEFORE publishing an immutable revision.
            // Draft rebase may then clear its short-lived journal safely.
            const WorkingCopyInfo draft =
                loadDraft(this->root_, draftId, DraftLoad::Full).info;
            // Authorized by the live lease and its fencing token, exactly as
            // a Save is: a session that took over an expired lease may also
            // Commit, and the creating session's stale token may not.
            if (draft.projectId != projectId ||
                draft.baseRevision != expectedLocalHead ||
                !draft.leaseSessionId || *draft.leaseSessionId != sessionId ||
                !draft.leaseToken || *draft.leaseToken != fencingToken ||
                !workingCopyLeaseLive(draft) ||
                draft.generation != expectedGeneration)
                throw ProjectError(ProjectErrorKind::Conflict,
                                   "local Commit draft/base/lease/generation changed");
            if (draft.draftActionPosition && *draft.draftActionPosition < draft.journalCount)
                throw ProjectError(ProjectErrorKind::Conflict,
                                   "redo actions remain: return to journal tip before Commit");
            if (!draft.dirty || draft.candidateChanges.empty())
                throw ProjectError(ProjectErrorKind::InvalidInput,
                                   "local Commit requires a saved dirty candidate");
            const std::optional<ProjectId> project = ProjectId::parse(projectId);
            const std::optional<BranchId> branch = BranchId::parse(draft.branchId);
            if (!project || !branch)
                throw ProjectError(ProjectErrorKind::InvalidInput,
                                   "local Commit requires UUIDv7 project and branch");
            // HEAD is checked below, after durable intent creation, so even a
            // stale valid request gets a permanent rejected outcome.
            operation.info.id = std::string(operationId);
            operation.info.repositoryId = this->repository_id_;
            operation.info.projectId = draft.projectId;
            operation.info.branchId = draft.branchId;
            operation.info.draftId = draft.id;
            operation.info.expectedHead = expectedLocalHead;
            operation.info.expectedGeneration = expectedGeneration;
            operation.info.reservedRevisionId = RevisionId::generate().toString();
            operation.requestDigest = requestDigest;
            operation.metadata = metadata;
            operation.committedAtMs = wallClockMs();
            operation.changes = draft.candidateChanges;
            operation.actions = draft.journal;
            operation.info.payloadDigest = localDigest(requestDigest + "\n" +
                saveBatchFingerprint(operation.changes, {}) + "\n");
            // Publish and fsync the revision -> operation mapping first. A
            // subsequent crash can leave an orphan mapping, but cannot make
            // a committed revision with inaccessible original actions.
            try { persistLocalRevisionIndex(this->root_, operation); }
            catch (...) { markUncertain(); throw; }
            // Persist the complete candidate snapshot and reserved UUID first.
            try { persistLocalCommit(this->root_, operation, true); }
            catch (...) { markUncertain(); throw; }
        }
        if (resolvePendingLocalCommit(repository, operation, this->root_,
                                      [this]() { markUncertain(); })) {
            rebaseCommittedDraft(this->root_, operation, [this]() { markUncertain(); });
            outcome = operation.info;
            return;
        }
        // Still pending and HEAD equals the expected base: the reserved
        // revision was never published, so (re)apply it.
        const std::optional<ProjectId> project = ProjectId::parse(operation.info.projectId);
        const std::optional<BranchId> branch = BranchId::parse(operation.info.branchId);
        const std::optional<RevisionId> reserved =
            RevisionId::parse(operation.info.reservedRevisionId);
        const std::optional<RevisionGraphHead> current =
            translated<std::optional<RevisionGraphHead>>([&]() {
                return repository.branchById(*project, *branch);
            });
        try {
            (void)repository.commitModel(*project, current->name, operation.changes,
                                           operation.metadata, operation.info.expectedHead, true,
                                           ReservedRevision{*reserved, operation.committedAtMs});
            operation.info.state = LocalCommitState::Succeeded;
        } catch (const StorageError& error) {
            if (error.kind() == StorageErrorKind::Conflict ||
                error.kind() == StorageErrorKind::InvalidInput ||
                error.kind() == StorageErrorKind::NotFound) {
                operation.info.state = LocalCommitState::Rejected;
                operation.info.rejection = error.kind() == StorageErrorKind::Conflict ? "conflict" :
                    (error.kind() == StorageErrorKind::InvalidInput ? "invalid-input" : "not-found");
            } else {
                // I/O or integrity failure may occur AFTER HEAD publication.
                // Leave the durable operation pending for authoritative recovery.
                throw;
            }
        }
        try { persistLocalCommit(this->root_, operation, false); }
        catch (...) { markUncertain(); throw; }
        rebaseCommittedDraft(this->root_, operation, [this]() { markUncertain(); });
        outcome = operation.info;
    });
    return outcome;
}

LocalCommitInfo ProjectService::localCommitStatus(std::string_view operationId) {
    if (!UuidV7::parse(operationId).has_value())
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid local Commit operation ID");
    const std::filesystem::path path = localCommitDirectory(this->root_) /
        std::string(operationId);
    checkHealthy();
    if (!std::filesystem::exists(path) && !std::filesystem::is_symlink(path))
        throw ProjectError(ProjectErrorKind::NotFound, "local Commit operation not found");
    const std::string projectId = readLocalCommit(path).info.projectId;
    LocalCommitInfo outcome;
    withRepository(projectId, [&](Repository& repository) {
        checkHealthy();
        PersistedLocalCommit operation = readLocalCommit(path);
        if (operation.info.repositoryId != this->repository_id_)
            throw ProjectError(ProjectErrorKind::Conflict,
                               "local Commit belongs to another repository");
        const std::shared_ptr<std::mutex> lock = draftLock(operation.info.draftId);
        std::lock_guard<std::mutex> guard(*lock);
        (void)resolvePendingLocalCommit(repository, operation, this->root_,
                                        [this]() { markUncertain(); });
        rebaseCommittedDraft(this->root_, operation, [this]() { markUncertain(); });
        outcome = operation.info;
    });
    return outcome;
}

LocalCommitActionHistory ProjectService::localCommitActionHistory(
        std::string_view operationId) {
    // First reconcile an ambiguous pending Commit. Rebase cannot erase the
    // frozen frames: they were published in the original durable intent.
    const LocalCommitInfo outcome = localCommitStatus(operationId);
    if (outcome.state != LocalCommitState::Succeeded)
        throw ProjectError(ProjectErrorKind::Conflict,
                           "actions are available only for a succeeded local Commit");
    const PersistedLocalCommit operation = readLocalCommit(
        localCommitDirectory(this->root_) / std::string(operationId));
    if (operation.info.state != LocalCommitState::Succeeded ||
        operation.info.repositoryId != this->repository_id_ ||
        operation.info.id != outcome.id ||
        operation.info.reservedRevisionId != outcome.reservedRevisionId)
        throw ProjectError(ProjectErrorKind::Conflict,
                           "local Commit history changed during reconciliation");
    LocalCommitActionHistory history;
    history.commit = operation.info;
    history.committedAtMs = operation.committedAtMs;
    history.actions = operation.actions;
    return history;
}


RevisionActionPage ProjectService::revisionActionPage(
        std::string_view projectId, std::string_view branch,
        const RevisionId& revision, std::size_t offset, std::size_t limit) {
    if (limit < 1 || limit > 100 || offset > workingCopyMaxEvents)
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "invalid archived action page size or offset");
    checkHealthy();
    // The same exclusive cursor validation as branchActionHistory enforces
    // first-parent reachability. An arbitrary revision from another branch
    // must never leak an action archive through this endpoint.
    (void)history(projectId, branch, 1, revision);
    RevisionActionPage page;
    page.revisionId = revision.toString();
    const std::optional<PersistedLocalCommit> index = indexedLocalCommit(
        this->root_, this->repository_id_, projectId, revision);
    if (!index) return page; // Checkpoint only; no synthetic actions.
    PersistedLocalCommit operation = *index;
    if (operation.info.state == LocalCommitState::Pending) {
        const LocalCommitInfo outcome = localCommitStatus(operation.info.id);
        check(outcome.state == LocalCommitState::Succeeded,
              "indexed published revision has no succeeded Commit");
        operation = *indexedLocalCommit(this->root_, this->repository_id_,
                                       projectId, revision);
    }
    check(operation.info.state == LocalCommitState::Succeeded,
          "indexed published revision has no succeeded Commit");
    if (offset > operation.actions.size())
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "archive offset exceeds action count");
    page.commitOperationId = operation.info.id;
    page.total = operation.actions.size();
    const std::size_t end = offset + std::min(limit, page.total - offset);
    page.actions.assign(operation.actions.begin() + static_cast<std::ptrdiff_t>(offset),
                        operation.actions.begin() + static_cast<std::ptrdiff_t>(end));
    if (end < page.total) page.nextOffset = end;
    return page;
}

RevisionNativeActionWitness ProjectService::verifiedRevisionNativeActionWitness(
        std::string_view projectId, std::string_view branch,
        const RevisionId& revision) {
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project || !validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "invalid project or branch for revision history witness");
    // The first-parent reachability check must happen before disclosing
    // archived actions for this revision. Do not use a global ledger scan.
    (void)history(projectId, branch, 1, revision);
    std::optional<PersistedLocalCommit> indexed = indexedLocalCommit(
        this->root_, this->repository_id_, projectId, revision);
    if (!indexed)
        throw ProjectError(ProjectErrorKind::Conflict,
                           "checkpoint has no indexed local action archive");
    if (indexed->info.state == LocalCommitState::Pending) {
        const LocalCommitInfo reconciled = localCommitStatus(indexed->info.id);
        if (reconciled.state != LocalCommitState::Succeeded)
            throw ProjectError(ProjectErrorKind::Conflict,
                               "indexed local Commit has not completed");
        indexed = indexedLocalCommit(this->root_, this->repository_id_,
                                    projectId, revision);
    }
    check(indexed && indexed->info.state == LocalCommitState::Succeeded &&
          indexed->info.projectId == projectId &&
          indexed->info.reservedRevisionId == revision.toString(),
          "published revision does not match its archived local Commit");
    const PersistedLocalCommit commit = *indexed;
    const RevisionGraphRevision immutable = this->revision(projectId, revision);
    check(immutable.modelIndexed && immutable.parents.size() <= 1 &&
          (immutable.parents.empty() == !commit.info.expectedHead) &&
          (immutable.parents.empty() ||
           immutable.parents.front().id == *commit.info.expectedHead),
          "archived Commit parent does not match published first-parent revision");
    RevisionNativeActionWitness result;
    result.revisionId = revision.toString();
    result.commitOperationId = commit.info.id;
    result.parentRevision = commit.info.expectedHead;
    result.actions = commit.actions;

    withRepository(projectId, [&](Repository& repository) {
        // Enumerate committed and parent root manifests, including unchanged
        // roots whose typed members nevertheless changed. This list is
        // authoritative for later MODEL-aware attribution; a caller cannot
        // simply omit an affected hydraulic network.
        constexpr std::uint16_t hydraulicRootKind = 100;
        constexpr std::size_t maxHydraulicSnapshotKeys = 65536;
        std::set<Key> rootKeys;
        const std::function<void(const RevisionId&)> gatherRoots =
            [&repository, &project, &rootKeys](const RevisionId& snapshot) {
                const std::vector<std::pair<Key, Bytes>> records =
                    translated<std::vector<std::pair<Key, Bytes>>>([&]() {
                        return repository.scanAtRevision(*project, snapshot);
                    });
                check(records.size() <= maxHydraulicSnapshotKeys,
                      "history snapshot exceeds bounded root scan");
                for (const std::pair<Key, Bytes>& record : records) {
                    const ModelRecord decoded = decodeModelRecord(record.second);
                    if (decoded.kind == hydraulicRootKind)
                        rootKeys.insert(record.first);
                }
            };
        if (commit.info.expectedHead) gatherRoots(*commit.info.expectedHead);
        gatherRoots(revision);
        result.hydraulicRootKeys.assign(rootKeys.begin(), rootKeys.end());
        // Only a complete native delta is evidence: unlike the candidate
        // overlay, a revision's diff excludes no-op records and includes
        // server-generated chunks and network root records.
        constexpr std::size_t maxWitnessKeys = 65536;
        std::map<Key, std::optional<ModelRecord>> claimed;
        for (const ModelChange& change : commit.changes)
            check(claimed.emplace(change.key, change.record).second &&
                  claimed.size() <= maxWitnessKeys,
                  "duplicate or excessive native keys in archived Commit");
        std::set<Key> actualKeys;
        if (commit.info.expectedHead) {
            const std::vector<Difference> differences =
                translated<std::vector<Difference>>([&]() {
                    return repository.diffRevisions(*project, *commit.info.expectedHead,
                                                    revision, maxWitnessKeys);
                });
            for (const Difference& difference : differences)
                actualKeys.insert(difference.key);
        } else {
            const std::vector<std::pair<Key, Bytes>> records =
                translated<std::vector<std::pair<Key, Bytes>>>([&]() {
                    return repository.scanAtRevision(*project, revision);
                });
            check(records.size() <= maxWitnessKeys,
                  "initial archived native revision exceeds witness bound");
            for (const std::pair<Key, Bytes>& record : records)
                actualKeys.insert(record.first);
        }
        check(actualKeys.size() <= maxWitnessKeys,
              "native revision history delta exceeds witness bound");
        for (const Key& key : actualKeys) {
            const std::map<Key, std::optional<ModelRecord>>::const_iterator expected =
                claimed.find(key);
            check(expected != claimed.end(),
                  "archived Commit omits an actual native revision delta key");
            const std::optional<ModelRecord> published =
                translated<std::optional<ModelRecord>>([&]() {
                    return repository.getModelAtRevision(*project, revision, key);
                });
            const std::function<bool(const std::optional<ModelRecord>&,
                                     const std::optional<ModelRecord>&)> same =
                [](const std::optional<ModelRecord>& first,
                   const std::optional<ModelRecord>& second) -> bool {
                    if (first.has_value() != second.has_value()) return false;
                    return !first || encodeModelRecord(*first) == encodeModelRecord(*second);
                };
            check(same(published, expected->second),
                  "published native revision bytes disagree with archived Commit candidate");
            result.actualNativeChanges.push_back(ModelChange{key, published});
        }
        // No-op candidate keys are legal, but they must really equal their
        // immutable baseline. A removed candidate that was never present is
        // not a hidden delta. This also detects forged/superfluous writes.
        for (const std::pair<const Key, std::optional<ModelRecord>>& candidate : claimed) {
            if (actualKeys.contains(candidate.first)) continue;
            const std::optional<ModelRecord> published =
                translated<std::optional<ModelRecord>>([&]() {
                    return repository.getModelAtRevision(*project, revision, candidate.first);
                });
            check(published.has_value() == candidate.second.has_value() &&
                  (!published || encodeModelRecord(*published) ==
                                 encodeModelRecord(*candidate.second)),
                  "archived no-op candidate does not match immutable revision");
        }
    });
    return result;
}

BranchActionHistoryPage ProjectService::branchActionHistory(
        std::string_view projectId, std::string_view branch, std::size_t limit,
        std::optional<RevisionId> after,
        std::optional<std::string_view> draftId, std::size_t draftOffset,
        std::optional<std::uint64_t> expectedDraftGeneration) {
    if (limit < 1 || limit > 10 || (draftId && !UuidV7::parse(*draftId)) ||
        (!draftId && (draftOffset != 0 || expectedDraftGeneration)) ||
        (draftOffset != 0 && !expectedDraftGeneration) ||
        draftOffset > workingCopyMaxEvents)
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "invalid history page size, draft or draft offset");
    checkHealthy();
    BranchActionHistoryPage page;
    const std::vector<RevisionGraphRevision> revisions =
        history(projectId, branch, limit + 1, after);
    const std::size_t included = std::min(revisions.size(), limit);
    if (revisions.size() > limit && included > 0)
        page.next = revisions[included - 1].id;
    page.commits.reserve(included);
    std::set<std::string> wanted;
    for (std::size_t index = 0; index < included; ++index) {
        page.commits.push_back(BranchActionHistoryEntry{revisions[index], std::nullopt, {}});
        wanted.insert(revisions[index].id.toString());
    }

    // Each reserved revision has a fsynced immutable sidecar published BEFORE
    // its Commit intent and before its immutable HEAD can be advanced. Look
    // up only the revisions in this page; unrelated Commit count is unbounded.
    for (BranchActionHistoryEntry& entry : page.commits) {
        std::optional<PersistedLocalCommit> operation = indexedLocalCommit(
            this->root_, this->repository_id_, projectId, entry.revision.id);
        if (!operation) continue; // Generic/imported/merge checkpoint.
        if (operation->info.state == LocalCommitState::Pending) {
            const LocalCommitInfo outcome = localCommitStatus(operation->info.id);
            check(outcome.state == LocalCommitState::Succeeded,
                  "indexed published revision has no succeeded Commit");
            operation = indexedLocalCommit(this->root_, this->repository_id_,
                                           projectId, entry.revision.id);
        }
        check(operation && operation->info.state == LocalCommitState::Succeeded,
              "indexed published revision has no succeeded Commit");
        entry.commitOperationId = operation->info.id;
        for (const WorkingCopyEvent& event : operation->actions)
            entry.actionIds.push_back(event.id);
    }

    if (draftId) {
        const WorkingCopyInfo draft = workingCopy(*draftId, true);
        const std::optional<BranchId> selectedBranch = branchId(projectId, branch);
        if (!selectedBranch || draft.projectId != projectId ||
            draft.branchId != selectedBranch->toString())
            throw ProjectError(ProjectErrorKind::Conflict,
                               "history draft belongs to another project or branch");
        // The draft can advance or rebase onto a new HEAD between page
        // requests. An offset without a pinned generation could quietly
        // stitch unrelated actions from two different journal versions.
        if (expectedDraftGeneration && draft.generation != *expectedDraftGeneration)
            throw ProjectError(ProjectErrorKind::Conflict,
                               "draft history generation changed; restart pagination");
        if (draftOffset > draft.journal.size())
            throw ProjectError(ProjectErrorKind::InvalidInput,
                               "draft history offset exceeds current journal");
        page.draftId = draft.id;
        page.draftGeneration = draft.generation;
        page.draftEventCount = draft.journalCount;
        const std::size_t count = std::min<std::size_t>(100,
            draft.journal.size() - draftOffset);
        for (std::size_t index = draftOffset; index < draftOffset + count; ++index) {
            page.draftActionIds.push_back(draft.journal[index].id);
            page.draftActionFrames.push_back(draft.journal[index]);
        }
        if (draftOffset + count < draft.journal.size())
            page.nextDraftOffset = draftOffset + count;
    }
    return page;
}

namespace {
// Caller owns the draft mutex and the repository's per-project operation
// mutex. Both preview and execution call this implementation, so navigation
// cannot apply a stale plan that was valid before a concurrent Commit.
HistoryCheckpointPlan buildHistoryCheckpointPlan(Repository& repository,
        const WorkingCopyInfo& draft, const RevisionId& target) {
    const std::optional<ProjectId> project = ProjectId::parse(draft.projectId);
    const std::optional<BranchId> branch = BranchId::parse(draft.branchId);
    if (!project || !branch)
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "history checkpoint requires UUIDv7 project and branch");
    const std::optional<RevisionGraphHead> selected = translated<
        std::optional<RevisionGraphHead>>([&]() {
            return repository.branchById(*project, *branch);
        });
    if (!selected || !selected->revision || draft.baseRevision != selected->revision)
        throw ProjectError(ProjectErrorKind::Conflict,
                           "history draft is not based on the current branch HEAD");
    translated<std::vector<RevisionGraphRevision>>([&]() {
        return repository.history(*project, selected->name, 1, target);
    });
    std::set<Key> wanted;
    if (draft.baseRevision != target) {
        const std::vector<Difference> changed = translated<std::vector<Difference>>([&]() {
            return repository.diffRevisions(*project, *draft.baseRevision,
                                            target, workingCopyMaxChanges);
        });
        for (const Difference& item : changed) wanted.insert(item.key);
    }
    if (wanted.size() > workingCopyMaxChanges)
        throw StorageError(StorageErrorKind::LimitExceeded,
                           "history checkpoint exceeds draft overlay budget");
    HistoryCheckpointPlan plan;
    plan.draftId = draft.id;
    plan.projectId = draft.projectId;
    plan.branchId = draft.branchId;
    plan.targetRevisionId = target.toString();
    plan.expectedHead = selected->revision;
    plan.expectedGeneration = draft.generation;
    plan.candidateChanges.reserve(wanted.size());
    for (const Key& key : wanted) {
        const std::optional<ModelRecord> desired = translated<std::optional<ModelRecord>>([&]() {
            return repository.getModelAtRevision(*project, target, key);
        });
        plan.candidateChanges.push_back(ModelChange{key, desired});
    }
    return plan;
}
} // namespace

HistoryCheckpointPlan ProjectService::planHistoryCheckpoint(
        std::string_view draftId, const RevisionId& target,
        std::uint64_t expectedGeneration) {
    if (!UuidV7::parse(draftId))
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "invalid history checkpoint draft UUIDv7");
    requirePrivateRepository("history checkpoint requires a private repository");
    const std::shared_ptr<std::mutex> lock = draftLock(draftId);
    std::lock_guard<std::mutex> guard(*lock);
    const WorkingCopyInfo draft = loadDraft(this->root_, draftId, DraftLoad::Candidate).info;
    if (draft.generation != expectedGeneration)
        throw ProjectError(ProjectErrorKind::Conflict,
                           "history draft changed; request a fresh checkpoint plan");
    HistoryCheckpointPlan result;
    withRepository(draft.projectId, [&](Repository& repository) {
        result = buildHistoryCheckpointPlan(repository, draft, target);
    });
    return result;
}

HistoryReverseActionWindow ProjectService::readHistoryReverseActionWindow(
        std::string_view draftId, const RevisionId& revision,
        std::size_t fromPosition, std::uint64_t expectedGeneration,
        std::size_t limit) {
    if (!UuidV7::parse(draftId) || limit == 0 || limit > 100 ||
        fromPosition > workingCopyMaxEvents)
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "invalid history action boundary or window size");
    requirePrivateRepository("history action windows require a private repository");
    // Pin the current draft for the *whole* read so Save, Commit or another
    // navigation cannot publish a different candidate while constructing a
    // window. Project HEAD is checked both before and after archive lookup.
    const std::shared_ptr<std::mutex> lock = draftLock(draftId);
    std::lock_guard<std::mutex> guard(*lock);
    const WorkingCopyInfo draft = loadDraft(this->root_, draftId, DraftLoad::Meta).info;
    if (draft.generation != expectedGeneration)
        throw ProjectError(ProjectErrorKind::Conflict,
                           "history draft generation changed; restart action navigation");
    const std::optional<ProjectId> project = ProjectId::parse(draft.projectId);
    const std::optional<BranchId> branch = BranchId::parse(draft.branchId);
    if (!project || !branch)
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "invalid project or branch in history draft");
    std::string branchName;
    const std::function<void(Repository&)> checkHead = [&](Repository& repository) {
        const std::optional<RevisionGraphHead> selected = translated<
            std::optional<RevisionGraphHead>>([&]() {
                return repository.branchById(*project, *branch);
            });
        if (!selected || !selected->revision ||
            selected->revision != draft.baseRevision)
            throw ProjectError(ProjectErrorKind::Conflict,
                               "history draft no longer follows current branch HEAD");
        // Even a readable revision from this project may belong to another
        // branch. Reject it rather than exposing its archived MODEL frames.
        translated<std::vector<RevisionGraphRevision>>([&]() {
            return repository.history(*project, selected->name, 1, revision);
        });
        branchName = selected->name;
    };
    withRepository(draft.projectId, checkHead);
    // An immutable, revision-indexed archive is verified on every page.
    // Its last original action is the first event to *consider* reversing.
    // Do not decode entity images or infer candidate writes in native code.
    const RevisionActionPage probe = revisionActionPage(
        draft.projectId, branchName, revision, 0, 1);
    if (fromPosition > probe.total)
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "history action boundary exceeds archived action count");
    HistoryReverseActionWindow result;
    result.draftId = draft.id;
    result.revisionId = revision.toString();
    result.commitOperationId = probe.commitOperationId;
    result.expectedHead = draft.baseRevision;
    result.expectedGeneration = draft.generation;
    result.actionCount = probe.total;
    result.fromPosition = fromPosition;
    if (fromPosition > 0) {
        const std::size_t begin = fromPosition > limit ? fromPosition - limit : 0;
        const RevisionActionPage sourcePage = revisionActionPage(
            draft.projectId, branchName, revision, begin, fromPosition - begin);
        if (sourcePage.total != probe.total ||
            sourcePage.commitOperationId != probe.commitOperationId ||
            sourcePage.actions.size() != fromPosition - begin)
            throw ProjectError(ProjectErrorKind::Conflict,
                               "history archive changed during action planning");
        result.originalFramesNewestFirst.assign(sourcePage.actions.rbegin(),
                                                 sourcePage.actions.rend());
        if (begin > 0) result.nextPosition = begin;
    }
    // The active HEAD is not immutable; it may advance while archives are
    // being read. A second check prevents returning a plan already obsolete
    // before it leaves the service. Execution must check HEAD/generation again.
    withRepository(draft.projectId, checkHead);
    return result;
}

WorkingCopyInfo ProjectService::navigateHistoryCheckpoint(
        std::string_view operationId, std::string_view draftId,
        std::string_view sessionId, std::string_view fencingToken,
        std::uint64_t expectedGeneration, const RevisionId& expectedHead,
        const RevisionId& target) {
    if (!UuidV7::parse(operationId) || !UuidV7::parse(draftId) ||
        !UuidV7::parse(sessionId) || !UuidV7::parse(fencingToken))
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "invalid history navigation identity");
    requirePrivateRepository("history navigation requires a private repository");
    const std::shared_ptr<std::mutex> lock = draftLock(draftId);
    std::lock_guard<std::mutex> guard(*lock);
    StoredDraft stored = loadDraft(this->root_, draftId, DraftLoad::Candidate);
    WorkingCopyInfo draft = stored.info;
    if (!draft.leaseSessionId || *draft.leaseSessionId != sessionId ||
        !draft.leaseToken || *draft.leaseToken != fencingToken ||
        !workingCopyLeaseLive(draft))
        throw ProjectError(ProjectErrorKind::Conflict,
                           "history navigation requires the current draft lease");
    WorkingCopyInfo result;
    withRepository(draft.projectId, [&](Repository& repository) {
        // Recheck current immutable HEAD even on exact retry: an unrelated
        // writer's Commit must not make an old cursor appear current.
        const std::optional<ProjectId> project = ProjectId::parse(draft.projectId);
        const std::optional<BranchId> branch = BranchId::parse(draft.branchId);
        const std::optional<RevisionGraphHead> selected = translated<
            std::optional<RevisionGraphHead>>([&]() {
                return repository.branchById(*project, *branch);
            });
        if (!selected || selected->revision != expectedHead ||
            draft.baseRevision != expectedHead)
            throw ProjectError(ProjectErrorKind::Conflict,
                               "branch HEAD changed before history navigation");
        if (draft.lastHistoryNavigationOperationId == operationId) {
            if (draft.historyCheckpointRevisionId != target.toString() ||
                draft.historyActionPosition || draft.historyArchiveCommitOperationId ||
                draft.lastHistoryNavigationSourceGeneration != expectedGeneration ||
                expectedGeneration == std::numeric_limits<std::uint64_t>::max() ||
                draft.generation != expectedGeneration + 1)
                throw ProjectError(ProjectErrorKind::Conflict,
                                   "history navigation operation reused with different arguments");
            result = draft; // Exact lost acknowledgement: no second update.
            return;
        }
        if (draft.generation != expectedGeneration ||
            draft.generation == std::numeric_limits<std::uint64_t>::max())
            throw ProjectError(ProjectErrorKind::Conflict,
                               "history navigation draft generation changed");
        if (draft.journalCount != 0)
            throw ProjectError(ProjectErrorKind::Conflict,
                               "checkpoint navigation requires an empty draft MODEL journal");
        const HistoryCheckpointPlan plan = buildHistoryCheckpointPlan(repository, draft, target);
        draft.candidateChanges = plan.candidateChanges;
        draft.dirty = !draft.candidateChanges.empty();
        draft.lastSaveFingerprint.reset();
        draft.historyCheckpointRevisionId = target.toString();
        draft.historyActionPosition.reset();
        draft.historyArchiveCommitOperationId.reset();
        draft.lastHistoryNavigationOperationId = std::string(operationId);
        draft.lastHistoryNavigationSourceGeneration = expectedGeneration;
        ++draft.generation;
        DraftUpdate update;
        update.candidate = draft.candidateChanges; // COMPLETE replacement, not merge
        prepareDraftUpdate(stored, update);
        try { updateDraft(workingCopyDirectory(this->root_) / draft.id,
                          stored, draft, update); }
        catch (...) { markUncertain(); throw; }
        result = draft;
    });
    return result;
}

WorkingCopyInfo ProjectService::publishVerifiedHistoryActionNavigation(
        std::string_view operationId, std::string_view draftId,
        std::string_view sessionId, std::string_view fencingToken,
        std::uint64_t expectedGeneration, const RevisionId& expectedHead,
        const RevisionId& revision, std::string_view archiveCommitOperationId,
        std::size_t actionPosition, const std::vector<ModelChange>& verifiedCandidate) {
    if (!UuidV7::parse(operationId) || !UuidV7::parse(draftId) ||
        !UuidV7::parse(sessionId) || !UuidV7::parse(fencingToken) ||
        !UuidV7::parse(archiveCommitOperationId) ||
        actionPosition > workingCopyMaxEvents ||
        verifiedCandidate.size() > workingCopyMaxChanges)
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "invalid verified history action publication request");
    requirePrivateRepository("history action publication requires a private repository");
    const std::shared_ptr<std::mutex> lock = draftLock(draftId);
    std::lock_guard<std::mutex> guard(*lock);
    StoredDraft stored = loadDraft(this->root_, draftId, DraftLoad::Candidate);
    WorkingCopyInfo draft = stored.info;
    if (!draft.leaseSessionId || *draft.leaseSessionId != sessionId ||
        !draft.leaseToken || *draft.leaseToken != fencingToken ||
        !workingCopyLeaseLive(draft))
        throw ProjectError(ProjectErrorKind::Conflict,
                           "history action navigation requires the current draft lease");
    WorkingCopyInfo result;
    withRepository(draft.projectId, [&](Repository& repository) {
        const std::optional<ProjectId> project = ProjectId::parse(draft.projectId);
        const std::optional<BranchId> branch = BranchId::parse(draft.branchId);
        if (!project || !branch)
            throw ProjectError(ProjectErrorKind::InvalidInput,
                               "invalid history action project or branch");
        const std::optional<RevisionGraphHead> selected = translated<
            std::optional<RevisionGraphHead>>([&] {
                return repository.branchById(*project, *branch);
            });
        if (!selected || selected->revision != expectedHead ||
            draft.baseRevision != expectedHead)
            throw ProjectError(ProjectErrorKind::Conflict,
                               "branch HEAD changed before history action navigation");
        if (draft.lastHistoryNavigationOperationId == operationId) {
            if (draft.historyCheckpointRevisionId != revision.toString() ||
                draft.historyActionPosition != actionPosition ||
                draft.historyArchiveCommitOperationId != archiveCommitOperationId ||
                draft.lastHistoryNavigationSourceGeneration != expectedGeneration ||
                expectedGeneration == std::numeric_limits<std::uint64_t>::max() ||
                draft.generation != expectedGeneration + 1)
                throw ProjectError(ProjectErrorKind::Conflict,
                                   "history action navigation ID reused with different arguments");
            result = draft; // Persisted successful operation: no second publication.
            return;
        }
        if (draft.generation != expectedGeneration ||
            draft.generation == std::numeric_limits<std::uint64_t>::max())
            throw ProjectError(ProjectErrorKind::Conflict,
                               "history action draft generation changed");
        if (draft.journalCount != 0 ||
            (draft.dirty && !draft.historyCheckpointRevisionId))
            throw ProjectError(ProjectErrorKind::Conflict,
                               "history action navigation requires no active MODEL edits");
        // Verify the immutable indexed archive and this action coordinate,
        // independent of the MODEL-aware overlay computation done by the
        // hydraulic adapter. Neither a forged operation ID nor a fabricated
        // cursor may be written into the native draft metadata.
        const std::optional<PersistedLocalCommit> archive = indexedLocalCommit(
            this->root_, this->repository_id_, draft.projectId, revision);
        if (!archive || archive->info.state != LocalCommitState::Succeeded ||
            archive->info.id != archiveCommitOperationId ||
            archive->info.branchId != draft.branchId ||
            archive->actions.empty() || actionPosition > archive->actions.size())
            throw ProjectError(ProjectErrorKind::Conflict,
                               "history action coordinate does not match a succeeded Commit archive");
        // The hydraulic adapter supplies the MODEL-verified candidate. The
        // native writer rechecks ancestry and data budget under the locks.
        (void)buildHistoryCheckpointPlan(repository, draft, revision);
        std::set<Key> seen;
        for (const ModelChange& change : verifiedCandidate)
            if (!seen.insert(change.key).second)
                throw ProjectError(ProjectErrorKind::InvalidInput,
                                   "duplicate key in verified history action overlay");
        draft.candidateChanges = verifiedCandidate; // COMPLETE replacement.
        draft.dirty = !draft.candidateChanges.empty();
        draft.lastSaveFingerprint.reset();
        draft.historyCheckpointRevisionId = revision.toString();
        draft.historyActionPosition = static_cast<std::uint64_t>(actionPosition);
        draft.historyArchiveCommitOperationId = std::string(archiveCommitOperationId);
        draft.lastHistoryNavigationOperationId = std::string(operationId);
        draft.lastHistoryNavigationSourceGeneration = expectedGeneration;
        ++draft.generation;
        DraftUpdate update;
        update.candidate = draft.candidateChanges;
        prepareDraftUpdate(stored, update);
        try { updateDraft(workingCopyDirectory(this->root_) / draft.id,
                          stored, draft, update); }
        catch (...) { markUncertain(); throw; }
        result = draft;
    });
    return result;
}

WorkingCopyInfo ProjectService::publishVerifiedDraftActionNavigation(
        std::string_view operationId, std::string_view draftId,
        std::string_view sessionId, std::string_view fencingToken,
        std::uint64_t expectedGeneration, std::optional<RevisionId> expectedHead,
        std::size_t actionPosition, const std::vector<ModelChange>& verifiedCandidate) {
    if (!UuidV7::parse(operationId) || !UuidV7::parse(draftId) ||
        !UuidV7::parse(sessionId) || !UuidV7::parse(fencingToken) ||
        actionPosition > workingCopyMaxEvents ||
        verifiedCandidate.size() > workingCopyMaxChanges)
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "invalid draft action navigation request");
    requirePrivateRepository("draft action navigation requires a private repository");
    const std::shared_ptr<std::mutex> lock = draftLock(draftId);
    std::lock_guard<std::mutex> guard(*lock);
    StoredDraft stored = loadDraft(this->root_, draftId, DraftLoad::Candidate);
    WorkingCopyInfo draft = stored.info;
    if (!draft.leaseSessionId || *draft.leaseSessionId != sessionId ||
        !draft.leaseToken || *draft.leaseToken != fencingToken ||
        !workingCopyLeaseLive(draft))
        throw ProjectError(ProjectErrorKind::Conflict,
                           "draft action navigation requires the current lease");
    WorkingCopyInfo result;
    withRepository(draft.projectId, [&](Repository& repository) {
        const std::optional<ProjectId> project = ProjectId::parse(draft.projectId);
        const std::optional<BranchId> branch = BranchId::parse(draft.branchId);
        if (!project || !branch)
            throw ProjectError(ProjectErrorKind::InvalidInput,
                               "invalid draft action project or branch");
        const std::optional<RevisionGraphHead> selected = translated<
            std::optional<RevisionGraphHead>>([&] {
                return repository.branchById(*project, *branch);
            });
        if (!selected || selected->revision != expectedHead ||
            draft.baseRevision != expectedHead)
            throw ProjectError(ProjectErrorKind::Conflict,
                               "draft branch HEAD changed during action navigation");
        if (draft.lastHistoryNavigationOperationId == operationId) {
            if (draft.draftActionPosition != actionPosition ||
                draft.lastHistoryNavigationSourceGeneration != expectedGeneration ||
                expectedGeneration == std::numeric_limits<std::uint64_t>::max() ||
                draft.generation != expectedGeneration + 1)
                throw ProjectError(ProjectErrorKind::Conflict,
                                   "draft action operation reused with different arguments");
            result = draft;
            return;
        }
        if (draft.generation != expectedGeneration ||
            draft.generation == std::numeric_limits<std::uint64_t>::max())
            throw ProjectError(ProjectErrorKind::Conflict,
                               "draft action generation changed");
        if (draft.journalCount == 0 || actionPosition > draft.journalCount ||
            draft.historyCheckpointRevisionId || draft.historyActionPosition)
            throw ProjectError(ProjectErrorKind::Conflict,
                               "draft action navigation requires an active uninterrupted journal");
        std::set<Key> seen;
        for (const ModelChange& change : verifiedCandidate)
            if (!seen.insert(change.key).second)
                throw ProjectError(ProjectErrorKind::InvalidInput,
                                   "duplicate verified draft action overlay key");
        // Keep the original action frames, including the forward Redo tail.
        // This position and candidate are one atomic durable publication.
        draft.candidateChanges = verifiedCandidate;
        draft.dirty = !verifiedCandidate.empty();
        draft.draftActionPosition = static_cast<std::uint64_t>(actionPosition);
        draft.lastHistoryNavigationOperationId = std::string(operationId);
        draft.lastHistoryNavigationSourceGeneration = expectedGeneration;
        ++draft.generation;
        DraftUpdate update;
        update.candidate = draft.candidateChanges;
        prepareDraftUpdate(stored, update);
        try { updateDraft(workingCopyDirectory(this->root_) / draft.id,
                          stored, draft, update); }
        catch (...) { markUncertain(); throw; }
        result = draft;
    });
    return result;
}

void ProjectService::releaseWorkingCopyLease(
        std::string_view draftId, std::string_view sessionId,
        std::string_view fencingToken) {
    if (!UuidV7::parse(draftId).has_value() || !UuidV7::parse(sessionId).has_value() ||
        !UuidV7::parse(fencingToken).has_value())
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid draft lease identifiers");
    const std::shared_ptr<std::mutex> lock = draftLock(draftId);
    std::lock_guard<std::mutex> guard(*lock);
    checkHealthy();
    StoredDraft stored = loadDraft(this->root_, draftId, DraftLoad::Meta);
    WorkingCopyInfo draft = stored.info;
    // No holder at all: the lease is already released, e.g. by this same
    // request before a transport resent it. Releasing is idempotent; a
    // lease held by anyone else is still never released with a stale token.
    if (!draft.leaseSessionId && !draft.leaseToken) return;
    if (!draft.leaseSessionId || *draft.leaseSessionId != sessionId ||
        !draft.leaseToken || *draft.leaseToken != fencingToken)
        throw ProjectError(ProjectErrorKind::Conflict, "stale draft lease token");
    draft.leaseSessionId.reset();
    draft.leaseToken.reset();
    draft.leaseExpiresAtMs = 0;
    try { updateDraft(workingCopyDirectory(this->root_) / draft.id, stored, draft, DraftUpdate{}); }
    catch (...) { markUncertain(); throw; }
    forgetLease(draftId);
}

std::vector<ProjectInfo> ProjectService::listProjects() const {
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    std::vector<ProjectInfo> result;
    result.reserve(this->entries_.size());
    for (const std::pair<const std::string, std::shared_ptr<Project>>& entry : this->entries_)
        result.push_back(entry.second->info);
    return result;
}

ProjectInfo ProjectService::project(std::string_view id) const {
    if (!validProjectId(id))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid project UUID");
    std::lock_guard<std::mutex> guard(this->mutex_);
    ensureHealthy();
    const std::map<std::string, std::shared_ptr<Project>>::const_iterator found =
        this->entries_.find(std::string(id));
    if (found == this->entries_.end())
        throw ProjectError(ProjectErrorKind::NotFound, "project not found");
    return found->second->info;
}

std::vector<std::string> ProjectService::listBranches(std::string_view projectId) {
    const std::shared_ptr<Project> found = findProject(projectId);
    return translated<std::vector<std::string>>([&]() {
        return openRepository(*found)->listBranches();
    });
}

std::optional<BranchId> ProjectService::branchId(std::string_view projectId,
                                                  std::string_view branch) {
    if (!validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::shared_ptr<Project> found = findProject(projectId);
    return translated<std::optional<BranchId>>([&] {
        return openRepository(*found)->branchId(branch);
    });
}

RevisionId ProjectService::createBranch(std::string_view projectId,
              std::string_view name, std::optional<std::string_view> source) {
    return createBranchWithIdentity(projectId, name, source).revisionId;
}

BranchCreationResult ProjectService::createBranchWithIdentity(
        std::string_view projectId, std::string_view name,
        std::optional<std::string_view> source) {
    if (!validBranchName(name) || (source && !validBranchName(*source)))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    std::optional<BranchCreationResult> created;
    withRepository(projectId, [&](Repository& repository) {
        created = translated<BranchCreationResult>([&]() {
            return repository.createBranchWithIdentity(*project, name, source);
        });
    });
    return *created;
}

std::optional<RevisionGraphHead> ProjectService::branchById(std::string_view projectId,
                                                                std::string_view branchId) {
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    const std::optional<BranchId> branch = BranchId::parse(branchId);
    if (!project || !branch)
        throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project and branch required");
    std::optional<RevisionGraphHead> found;
    withRepository(projectId, [&](Repository& repository) {
        found = translated<std::optional<RevisionGraphHead>>([&]() {
            return repository.branchById(*project, *branch);
        });
    });
    return found;
}

BranchCreationResult ProjectService::createBranchFromId(
        std::string_view projectId, std::string_view name, std::string_view sourceBranchId,
        std::optional<RevisionId> revision, std::string_view createdBranchId) {
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    const std::optional<BranchId> source = BranchId::parse(sourceBranchId);
    const std::optional<BranchId> created = BranchId::parse(createdBranchId);
    if (!validBranchName(name))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    if (!project || !source || !created)
        throw ProjectError(ProjectErrorKind::InvalidInput,
                           "UUIDv7 project, source branch and new branch required");
    std::optional<BranchCreationResult> result;
    withRepository(projectId, [&](Repository& repository) {
        result = translated<BranchCreationResult>([&]() {
            return repository.createBranchFromId(*project, name, *source, revision, *created);
        });
    });
    return *result;
}

RevisionId ProjectService::createBranchAtRevision(std::string_view projectId,
                std::string_view name, std::string_view sourceBranch, const RevisionId& revision) {
    return createBranchAtRevisionWithIdentity(projectId, name, sourceBranch, revision).revisionId;
}

BranchCreationResult ProjectService::createBranchAtRevisionWithIdentity(
        std::string_view projectId, std::string_view name,
        std::string_view sourceBranch, const RevisionId& revision) {
    if (!validBranchName(name) || !validBranchName(sourceBranch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    std::optional<BranchCreationResult> created;
    withRepository(projectId, [&](Repository& repository) {
        created = translated<BranchCreationResult>([&]() {
            return repository.createBranchAtRevisionWithIdentity(*project, name, sourceBranch, revision);
        });
    });
    return *created;
}

MergeResult ProjectService::mergeBranches(std::string_view projectId,
                std::string_view target, std::string_view source, const CommitMetadata& metadata,
                std::optional<RevisionId> expectedTarget,
                std::optional<RevisionId> expectedSource) {
    if (!validBranchName(target) || !validBranchName(source))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    MergeResult result;
    withRepository(projectId, [&](Repository& repository) {
        result = translated<MergeResult>([&]() {
            return repository.mergeBranches(*project, target, source, metadata,
                                               expectedTarget, expectedSource);
        });
    });
    return result;
}

std::optional<RevisionId> ProjectService::optionalHead(std::string_view projectId, std::string_view branch) {
    if (!validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    const std::shared_ptr<Project> entry = findProject(projectId);
    return translated<std::optional<RevisionId>>([&]() {
        return openRepository(*entry)->optionalHead(*project, branch);
    });
}

RevisionId ProjectService::head(std::string_view projectId, std::string_view branch) {
    if (!validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    const std::shared_ptr<Project> entry = findProject(projectId);
    return translated<RevisionId>([&]() { return openRepository(*entry)->head(*project, branch); });
}

std::vector<RevisionGraphRevision> ProjectService::history(
        std::string_view projectId, std::string_view branch, std::size_t maxCount,
        std::optional<RevisionId> after) {
    if (!validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    const std::shared_ptr<Project> entry = findProject(projectId);
    return translated<std::vector<RevisionGraphRevision>>([&]() {
        return openRepository(*entry)->history(*project, branch, maxCount, after);
    });
}

RevisionGraphPage ProjectService::revisionGraph(
        std::string_view projectId, std::size_t maxCount,
        std::optional<RevisionGraphCursor> cursor) {
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    const std::shared_ptr<Project> entry = findProject(projectId);
    return translated<RevisionGraphPage>([&]() {
        return openRepository(*entry)->revisionGraph(*project, maxCount, std::move(cursor));
    });
}

RevisionId ProjectService::commitModel(std::string_view projectId,
                          std::string_view branch, std::vector<ModelChange> changes,
                          const CommitMetadata& metadata,
                          std::optional<RevisionId> expectedHead) {
    if (!validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    RevisionId committed = RevisionId::generate();
    withRepository(projectId, [&](Repository& repository) {
        committed = translated<RevisionId>([&]() {
            return repository.commitModel(*project, branch, std::move(changes), metadata,
                                            expectedHead);
        });
    });
    return committed;
}

RevisionId ProjectService::commitModelAtExpectedHead(
        std::string_view projectId, std::string_view branch,
        std::vector<ModelChange> changes, const CommitMetadata& metadata,
        std::optional<RevisionId> expectedLocalHead) {
    requirePrivateRepository("local commit requires a private repository");
    if (!validBranchName(branch))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    RevisionId committed = RevisionId::generate();
    withRepository(projectId, [&](Repository& repository) {
        committed = translated<RevisionId>([&]() {
            return repository.commitModel(*project, branch, std::move(changes), metadata,
                                            expectedLocalHead, true);
        });
    });
    return committed;
}

std::optional<ModelRecord> ProjectService::getModelAtRevision(
        std::string_view projectId, const RevisionId& revisionId, const Key& key) {
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    const std::shared_ptr<Project> entry = findProject(projectId);
    return translated<std::optional<ModelRecord>>([&]() {
        return openRepository(*entry)->getModelAtRevision(*project, revisionId, key);
    });
}

RevisionGraphRevision ProjectService::revision(std::string_view projectId, const RevisionId& id) {
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    const std::shared_ptr<Project> found = findProject(projectId);
    return translated<RevisionGraphRevision>([&]() {
        const std::optional<Bytes> bytes = openRepository(*found)->readRevision(*project, id);
        if (!bytes) throw ProjectError(ProjectErrorKind::NotFound, "revision not found");
        const RevisionRecord record = decodeRevisionRecord(*bytes);
        RevisionGraphRevision result{record.id, hashRevisionRecord(record),
                                      record.root, record.referencesRoot, record.modelIndexed, {},
                                      record.metadata, record.committedAtMs, std::nullopt};
        result.parents.reserve(record.parents.size());
        for (const RevisionParent& parent : record.parents)
            result.parents.push_back(RevisionGraphParent{parent.id, parent.expectedRecordHash});
        return result;
    });
}

void ProjectService::updateBranchProperties(std::string_view projectId,
        std::string_view branchId, std::string_view expectedName, std::string_view name,
        const BranchMetadata& metadata) {
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    const std::optional<BranchId> branch = BranchId::parse(branchId);
    if (!project || !branch) throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch identity");
    withRepository(projectId, [&](Repository& repository) {
        repository.updateBranchProperties(*project, *branch, expectedName, name, metadata);
    });
}

void ProjectService::updateRevisionDisplayMessage(std::string_view projectId,
        std::string_view revisionId, std::string_view expected, std::string_view message) {
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    const std::optional<RevisionId> revision = RevisionId::parse(revisionId);
    if (!project || !revision) throw ProjectError(ProjectErrorKind::InvalidInput, "invalid revision identity");
    withRepository(projectId, [&](Repository& repository) {
        repository.updateRevisionDisplayMessage(*project, *revision, expected, message);
    });
}

std::optional<std::string> ProjectService::revisionDisplayMessage(std::string_view projectId,
                                                                   std::string_view revisionId) {
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    const std::optional<RevisionId> revision = RevisionId::parse(revisionId);
    if (!project || !revision) throw ProjectError(ProjectErrorKind::InvalidInput, "invalid revision identity");
    std::optional<std::string> result;
    withRepository(projectId, [&](Repository& repository) {
        result = repository.revisionDisplayMessage(*project, *revision);
    });
    return result;
}

void ProjectService::renameBranch(std::string_view projectId, std::string_view previous,
                                  std::string_view renamed, const RevisionId& expectedHead) {
    if (!validBranchName(previous) || !validBranchName(renamed))
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid branch name");
    const std::optional<ProjectId> project = ProjectId::parse(projectId);
    if (!project) throw ProjectError(ProjectErrorKind::InvalidInput, "UUIDv7 project required");
    withRepository(projectId, [&](Repository& repository) {
        translated<int>([&]() {
            repository.renameBranch(*project, previous, renamed, expectedHead);
            return 0;
        });
    });
}

ScratchFile ProjectService::createScratchFile(std::string_view projectId) {
    const std::shared_ptr<Project> found = findProject(projectId);
    return translated<ScratchFile>([&]() { return openRepository(*found)->createScratchFile(); });
}

ProjectService::Operation ProjectService::reserveOperation(std::string_view projectId) {
    std::shared_ptr<Project> found = findProject(projectId);
    if (found->pending.fetch_add(1) >= this->maxPending_) {
        found->pending.fetch_sub(1);
        throw ProjectError(ProjectErrorKind::Busy,
                           "project has too many pending operations; retry later");
    }
    return Operation(std::move(found));
}

void ProjectService::withRepository(Operation& operation,
                                    const std::function<void(Repository&)>& action) {
    if (!operation.project_) throw ProjectError(ProjectErrorKind::InvalidInput,
                                                "operation reservation was moved from");
    const std::shared_ptr<Project> project = operation.project_;
    std::lock_guard<std::mutex> guard(project->operationMutex);
    const std::shared_ptr<Repository> repository =
        translated<std::shared_ptr<Repository>>([&]() { return openRepository(*project); });
    action(*repository);
}

void ProjectService::withRepository(std::string_view projectId,
                                    const std::function<void(Repository&)>& action) {
    Operation operation = reserveOperation(projectId);
    withRepository(operation, action);
}

std::size_t ProjectService::pendingOperations(std::string_view projectId) const {
    return findProject(projectId)->pending.load();
}

} // namespace aowis::dvmdb
