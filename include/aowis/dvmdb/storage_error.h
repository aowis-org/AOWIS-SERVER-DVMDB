#pragma once

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace aowis::dvmdb {

// Classified failure of the native engine and its model layers. Callers map
// the kind, never the message text, to their own error model (for example
// HTTP status codes). Messages of Corruption, Io, ResourceExhausted and
// Internal failures may contain server-side details and must not be returned
// to remote clients verbatim.
enum class StorageErrorKind {
    InvalidInput,      // the request or supplied data is malformed or not permitted
    NotFound,          // a requested branch, revision or object does not exist
    Conflict,          // valid request, but incompatible with the current state
    LimitExceeded,     // a configured or format size/count limit was exceeded
    Unavailable,       // handle unusable until reopened (e.g. after interrupted compaction)
    Busy,              // temporary process resource budget saturation; retry later
    Corruption,        // stored repository data violates an integrity invariant
    ResourceExhausted, // ENOSPC / EDQUOT
    Io,                // any other operating-system failure
    Internal           // violated engine invariant or library failure
};

class StorageError : public std::runtime_error {
public:
    StorageError(StorageErrorKind kind, const std::string& message)
        : std::runtime_error(message), kind_(kind) {}
    StorageErrorKind kind() const noexcept { return this->kind_; }

    static const char* kindName(StorageErrorKind kind) noexcept {
        switch (kind) {
        case StorageErrorKind::InvalidInput: return "invalid_input";
        case StorageErrorKind::NotFound: return "not_found";
        case StorageErrorKind::Conflict: return "conflict";
        case StorageErrorKind::LimitExceeded: return "limit_exceeded";
        case StorageErrorKind::Unavailable: return "unavailable";
        case StorageErrorKind::Busy: return "busy";
        case StorageErrorKind::Corruption: return "corruption";
        case StorageErrorKind::ResourceExhausted: return "resource_exhausted";
        case StorageErrorKind::Io: return "io";
        case StorageErrorKind::Internal: return "internal";
        }
        return "internal";
    }

private:
    StorageErrorKind kind_;
};

inline StorageErrorKind storageErrorKindForErrno(int error) noexcept {
#ifdef EDQUOT
    if (error == EDQUOT) return StorageErrorKind::ResourceExhausted;
#endif
    // Windows has no EDQUOT; dvmdb_sys_windows.cpp maps quota errors to ENOSPC.
    return error == ENOSPC ? StorageErrorKind::ResourceExhausted : StorageErrorKind::Io;
}

// Throws for the current errno; captures it before building the message.
[[noreturn]] inline void throwSystemError(const std::string& operation) {
    const int error = errno;
    throw StorageError(storageErrorKindForErrno(error),
                       operation + ": " + std::strerror(error));
}

} // namespace aowis::dvmdb
