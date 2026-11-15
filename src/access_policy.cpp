#include "branch_name_validation.h"
#include "aowis/dvmdb/access_policy.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include "dvmdb_sys.h"
#include <limits>
#include <set>
#include <stdexcept>
#include "dvmdb_crypto.h"

namespace aowis::dvmdb {
namespace {

constexpr std::size_t maxPolicyBytes = 8U * 1024U * 1024U;
constexpr std::array<std::uint8_t, 8> signature{'A','O','W','A','C','L','0','1'};
constexpr char hexDigits[] = "0123456789abcdef";

void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

struct Fd {
    explicit Fd(int descriptor) : fd(descriptor) { check(fd >= 0, "access policy file operation failed"); }
    ~Fd() { if (this->fd >= 0) sys::close(this->fd); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int fd;
};

class Writer {
public:
    Bytes bytes;
    void octet(std::uint8_t value) { this->bytes.push_back(value); }
    void number(std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) octet(static_cast<std::uint8_t>(value >> (i * 8U)));
    }
    void key(const Key& value) { this->bytes.insert(this->bytes.end(), value.begin(), value.end()); }
    void text(std::string_view value) {
        check(value.size() <= 4096, "access policy string too long");
        number(static_cast<std::uint32_t>(value.size()));
        this->bytes.insert(this->bytes.end(), value.begin(), value.end());
    }
    void keys(const std::vector<Key>& values) {
        check(values.size() <= 100000, "access policy too many references");
        number(static_cast<std::uint32_t>(values.size()));
        for (const Key& value : values) key(value);
    }
};

class Reader {
public:
    explicit Reader(const Bytes& bytes) : bytes_(bytes) {}
    std::uint8_t octet() { check(this->position_ < this->bytes_.size(), "truncated access policy"); return this->bytes_[this->position_++]; }
    std::uint32_t number() {
        std::uint32_t result = 0;
        for (unsigned i = 0; i < 4; ++i) result |= static_cast<std::uint32_t>(octet()) << (i * 8U);
        return result;
    }
    Key key() {
        Key result{};
        for (std::uint8_t& item : result) item = octet();
        return result;
    }
    std::string text() {
        const std::uint32_t length = number();
        check(length <= 4096 && length <= this->bytes_.size() - this->position_, "invalid access policy string");
        std::string result(reinterpret_cast<const char*>(this->bytes_.data() + this->position_), length);
        this->position_ += length;
        return result;
    }
    std::vector<Key> keys() {
        const std::uint32_t length = number();
        check(length <= 100000 && length <= (this->bytes_.size() - this->position_) / 16,
              "invalid access policy reference count");
        std::vector<Key> result;
        result.reserve(length);
        for (std::uint32_t i = 0; i < length; ++i) result.push_back(key());
        return result;
    }
    bool done() const { return this->position_ == this->bytes_.size(); }
private:
    const Bytes& bytes_;
    std::size_t position_ = 0;
};

std::vector<std::uint8_t> serialize(const AccessPolicy& policy) {
    validateAccessPolicy(policy);
    Writer writer;
    writer.bytes.insert(writer.bytes.end(), signature.begin(), signature.end());
    auto count = [&writer](std::size_t size) {
        check(size <= 100000, "access policy collection too large");
        writer.number(static_cast<std::uint32_t>(size));
    };
    count(policy.users.size());
    for (const AccessUser& user : policy.users) {
        writer.key(user.id); writer.text(user.account); writer.octet(user.disabled ? 1 : 0);
        writer.keys(user.groups); writer.keys(user.roles);
    }
    count(policy.groups.size());
    for (const AccessGroup& group : policy.groups) {
        writer.key(group.id); writer.text(group.name); writer.keys(group.roles);
    }
    count(policy.roles.size());
    for (const AccessRole& role : policy.roles) {
        writer.key(role.id); writer.text(role.name); writer.keys(role.permissions);
    }
    count(policy.permissions.size());
    for (const AccessPermission& permission : policy.permissions) {
        writer.key(permission.id); writer.octet(static_cast<std::uint8_t>(permission.operation));
        writer.text(permission.branch);
    }
    const Hash checksum = sha256(writer.bytes);
    writer.bytes.insert(writer.bytes.end(), checksum.begin(), checksum.end());
    check(writer.bytes.size() <= maxPolicyBytes, "access policy exceeds size limit");
    return std::move(writer.bytes);
}

AccessPolicy deserialize(const Bytes& file) {
    check(file.size() >= signature.size() + 4 * 4 + 32 && file.size() <= maxPolicyBytes,
          "invalid access policy file size");
    const Bytes content(file.begin(), file.end() - 32);
    const Hash hash = sha256(content);
    check(std::equal(hash.begin(), hash.end(), file.end() - 32), "access policy checksum mismatch");
    Reader reader(content);
    for (std::uint8_t character : signature) check(reader.octet() == character, "invalid access policy version");
    AccessPolicy policy;
    const auto nextCount = [&reader]() {
        const std::uint32_t value = reader.number();
        check(value <= 100000, "access policy collection too large");
        return value;
    };
    for (std::uint32_t i = 0, n = nextCount(); i < n; ++i) {
        AccessUser user;
        user.id = reader.key(); user.account = reader.text();
        const std::uint8_t disabled = reader.octet();
        check(disabled <= 1, "invalid disabled flag");
        user.disabled = disabled == 1; user.groups = reader.keys(); user.roles = reader.keys();
        policy.users.push_back(std::move(user));
    }
    for (std::uint32_t i = 0, n = nextCount(); i < n; ++i) {
        AccessGroup group;
        group.id = reader.key(); group.name = reader.text(); group.roles = reader.keys();
        policy.groups.push_back(std::move(group));
    }
    for (std::uint32_t i = 0, n = nextCount(); i < n; ++i) {
        AccessRole role;
        role.id = reader.key(); role.name = reader.text(); role.permissions = reader.keys();
        policy.roles.push_back(std::move(role));
    }
    for (std::uint32_t i = 0, n = nextCount(); i < n; ++i) {
        AccessPermission permission;
        permission.id = reader.key();
        permission.operation = static_cast<AccessOperation>(reader.octet());
        permission.branch = reader.text();
        policy.permissions.push_back(std::move(permission));
    }
    check(reader.done(), "trailing access policy bytes");
    validateAccessPolicy(policy);
    return policy;
}

std::filesystem::path policyFile(const std::filesystem::path& directory) {
    return directory / "access-policy.bin";
}

bool policyExists(const std::filesystem::path& directory) {
    sys::Stat state{};
    if (sys::lstat(policyFile(directory).c_str(), &state) == -1) {
        if (errno == ENOENT) return false;
        throw std::runtime_error("cannot stat access policy");
    }
    check(S_ISREG(state.st_mode), "access policy must be a regular file");
    check((state.st_mode & (S_IWGRP | S_IWOTH)) == 0,
          "access policy is writable by an untrusted group or other user");
    return true;
}

AccessPolicy readPolicy(const std::filesystem::path& directory) {
    Fd file(sys::open(policyFile(directory).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    sys::Stat state{};
    check(sys::fstat(file.fd, &state) == 0 && S_ISREG(state.st_mode) &&
          state.st_size >= 0 && static_cast<std::uint64_t>(state.st_size) <= maxPolicyBytes &&
          (state.st_mode & (S_IWGRP | S_IWOTH)) == 0,
          "invalid or insecure access policy file metadata");
    Bytes bytes;
    bytes.reserve(static_cast<std::size_t>(state.st_size));
    std::array<std::uint8_t, 8192> buffer{};
    for (;;) {
        const ssize_t length = sys::read(file.fd, buffer.data(), buffer.size());
        if (length < 0 && errno == EINTR) continue;
        check(length >= 0, "cannot read access policy");
        if (length == 0) break;
        check(bytes.size() + static_cast<std::size_t>(length) <= maxPolicyBytes,
              "access policy exceeds size limit");
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + length);
    }
    return deserialize(bytes);
}

void writePolicy(const std::filesystem::path& directory, const AccessPolicy& policy) {
    const Bytes bytes = serialize(policy);
    std::string tmp = (directory / "access-policy-tmp-XXXXXXXX").string();
    std::vector<char> filename(tmp.begin(), tmp.end()); filename.push_back('\0');
    Fd file(sys::mkstemp(filename.data()));
    const std::filesystem::path tempPath = filename.data();
    try {
        check(sys::fchmod(file.fd, 0600) == 0, "cannot chmod access policy");
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t written = sys::write(file.fd, bytes.data() + offset, bytes.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            check(written > 0, "cannot write access policy");
            offset += static_cast<std::size_t>(written);
        }
        check(sys::fsync(file.fd) == 0, "cannot sync access policy");
        check(sys::rename(tempPath.c_str(), policyFile(directory).c_str()) == 0,
              "cannot publish access policy");
        Fd dir(sys::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        check(sys::fsync(dir.fd) == 0, "cannot sync access policy directory");
    } catch (...) { sys::unlink(tempPath.c_str()); throw; }
}

class PolicyLock {
public:
    explicit PolicyLock(const std::filesystem::path& directory)
        : file_(sys::open((directory / "access-policy.lock").c_str(),
                       O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600)) {
        check(sys::flock(this->file_.fd, LOCK_EX) == 0, "cannot lock access policy");
    }
private:
    Fd file_;
};

bool validBranch(std::string_view branch) {
    if (branch == "*") return true;
    // Exact ACL scopes use the same human-readable names as branch records.
    // Retain the historical hierarchical scope syntax below.
    if (detail::validBranchLabel(branch)) return true;
    if (branch.empty() || branch.size() > 255 || branch == "." || branch == "..") return false;
    for (char character : branch) {
        const bool ok = (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '_' ||
            character == '-' || character == '.' || character == '/' || character == ' ';
        if (!ok) return false;
    }
    return branch.front() != '/' && branch.back() != '/' &&
        branch.find("..") == std::string_view::npos &&
        branch.find("//") == std::string_view::npos;
}

bool validName(std::string_view text) {
    if (text.empty() || text.size() > 255) return false;
    for (char character : text) {
        if (static_cast<unsigned char>(character) < 33 || character == 127) return false;
    }
    return true;
}

void validateLinks(const std::vector<Key>& links, const std::set<Key>& known) {
    std::set<Key> unique;
    for (const Key& link : links) {
        check(known.contains(link), "access policy references nonexistent entity");
        check(unique.insert(link).second, "access policy has duplicate assignment");
    }
}

} // namespace

void validateAccessPolicy(const AccessPolicy& policy) {
    std::set<Key> all, users, groups, roles, permissions;
    std::set<std::string> accounts, groupNames, roleNames;
    auto uniqueId = [&all](const Key& id) {
        check(id != Key{}, "access policy UUID cannot be nil");
        check(all.insert(id).second, "duplicate UUID in access policy");
    };
    for (const AccessUser& user : policy.users) {
        uniqueId(user.id); users.insert(user.id);
        check(validName(user.account), "invalid access user account");
        check(accounts.insert(user.account).second, "duplicate access user account");
    }
    for (const AccessGroup& group : policy.groups) {
        uniqueId(group.id); groups.insert(group.id);
        check(validName(group.name), "invalid access group name");
        check(groupNames.insert(group.name).second, "duplicate access group name");
    }
    for (const AccessRole& role : policy.roles) {
        uniqueId(role.id); roles.insert(role.id);
        check(validName(role.name), "invalid access role name");
        check(roleNames.insert(role.name).second, "duplicate access role name");
    }
    for (const AccessPermission& permission : policy.permissions) {
        uniqueId(permission.id); permissions.insert(permission.id);
        check(permission.operation == AccessOperation::Read ||
              permission.operation == AccessOperation::Write,
              "invalid access operation");
        check(validBranch(permission.branch), "invalid access permission branch scope");
    }
    for (const AccessUser& user : policy.users) {
        validateLinks(user.groups, groups);
        validateLinks(user.roles, roles);
    }
    for (const AccessGroup& group : policy.groups) validateLinks(group.roles, roles);
    for (const AccessRole& role : policy.roles) validateLinks(role.permissions, permissions);
}

AccessPolicyStore::AccessPolicyStore(std::filesystem::path repositoryDirectory)
    : directory_(std::move(repositoryDirectory)) {}

bool AccessPolicyStore::initialized() const { return policyExists(this->directory_); }

AccessPolicy AccessPolicyStore::read() const {
    check(initialized(), "access policy not initialized");
    return readPolicy(this->directory_);
}

void AccessPolicyStore::initialize(const AccessPolicy& initial) {
    PolicyLock guard(this->directory_);
    check(!initialized(), "access policy already initialized");
    writePolicy(this->directory_, initial);
}

void AccessPolicyStore::modify(const std::function<void(AccessPolicy&)>& change) {
    PolicyLock guard(this->directory_);
    AccessPolicy policy = read();
    change(policy);
    writePolicy(this->directory_, policy);
}

bool AccessPolicyStore::permits(std::string_view accountName, AccessOperation operation,
                                std::string_view branch) const {
    if (!initialized()) return true; // An uninitialized policy is permissive (not enforced by the HTTP service).
    return permitsRequired(accountName, operation, branch);
}

bool AccessPolicyStore::permitsRequired(std::string_view accountName,
                                        AccessOperation operation,
                                        std::string_view branch) const {
    const AccessPolicy policy = read();
    const AccessUser* current = nullptr;
    for (const AccessUser& user : policy.users) {
        if (user.account == accountName) { current = &user; break; }
    }
    if (!current || current->disabled) return false;
    std::set<Key> assigned(current->roles.begin(), current->roles.end());
    for (const AccessGroup& group : policy.groups) {
        if (std::find(current->groups.begin(), current->groups.end(), group.id) != current->groups.end())
            assigned.insert(group.roles.begin(), group.roles.end());
    }
    for (const AccessRole& role : policy.roles) {
        if (!assigned.contains(role.id)) continue;
        for (const AccessPermission& permission : policy.permissions) {
            if (permission.operation == operation &&
                (permission.branch == "*" || permission.branch == branch) &&
                std::find(role.permissions.begin(), role.permissions.end(), permission.id) != role.permissions.end())
                return true;
        }
    }
    return false;
}

Key newAccessUuid() {
    Key value{};
    check(crypto::secureRandomBytes(value.data(), value.size()),
          "cannot generate access UUID");
    value[6] = static_cast<std::uint8_t>((value[6] & 0x0fU) | 0x40U);
    value[8] = static_cast<std::uint8_t>((value[8] & 0x3fU) | 0x80U);
    return value;
}

std::string accessUuidString(const Key& id) {
    std::string result;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result.push_back('-');
        result.push_back(hexDigits[id[i] >> 4U]);
        result.push_back(hexDigits[id[i] & 15U]);
    }
    return result;
}

Key parseAccessUuid(std::string_view value) {
    check(value.size() == 36, "invalid access UUID length");
    Key result{};
    std::size_t pos = 0;
    auto nibble = [](char ch) -> std::uint8_t {
        if (ch >= '0' && ch <= '9') return static_cast<std::uint8_t>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<std::uint8_t>(ch - 'a' + 10);
        if (ch >= 'A' && ch <= 'F') return static_cast<std::uint8_t>(ch - 'A' + 10);
        throw std::runtime_error("invalid access UUID digit");
    };
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            check(value[pos++] == '-', "invalid access UUID separators");
        }
        result[i] = static_cast<std::uint8_t>((nibble(value[pos]) << 4U) | nibble(value[pos + 1]));
        pos += 2;
    }
    return result;
}

} // namespace aowis::dvmdb
