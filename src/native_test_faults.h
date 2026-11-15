#pragma once

// Private fault checkpoints. Only the dedicated recovery-test library is built
// with AOWIS_DVMDB_FAULT_TEST_BUILD; shipping binaries never examine an
// environment variable or expose crash injection.
#ifdef AOWIS_DVMDB_FAULT_TEST_BUILD
#include "aowis/dvmdb/storage_error.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <cstdint>
#include "dvmdb_sys.h"
#endif

namespace aowis::dvmdb::detail {
inline void faultCheckpoint(const char* name) {
#ifdef AOWIS_DVMDB_FAULT_TEST_BUILD
    const char* selected = ::getenv("AOWIS_DVMDB_TEST_FAULT");
    if (selected == nullptr || std::strcmp(selected, name) != 0) return;
    const char* action = ::getenv("AOWIS_DVMDB_TEST_ACTION");
    if (action != nullptr && std::strcmp(action, "crash") == 0) ::_exit(93);
    // Same classification as a real ENOSPC reported through systemCall().
    errno = ENOSPC;
    throw StorageError(StorageErrorKind::ResourceExhausted,
                       std::string("injected storage failure at ") + name);
#else
    (void)name;
#endif
}
// Simulates torn writes, not merely termination before a complete write.
// The target fd has already been opened by the production write path.
inline void faultPartialWrite(int fd, const std::uint8_t* bytes, std::size_t size,
                              std::uint64_t offset, const char* name) {
#ifdef AOWIS_DVMDB_FAULT_TEST_BUILD
    const char* selected = ::getenv("AOWIS_DVMDB_TEST_FAULT");
    if (selected == nullptr || std::strcmp(selected, name) != 0) return;
    const std::size_t half = size / 2U;
    std::size_t done = 0;
    while (done < half) {
        const ssize_t amount = sys::pwrite(fd, bytes + done, half - done,
                                        static_cast<off_t>(offset + done));
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) throw std::runtime_error("injected partial-write preparation failed");
        done += static_cast<std::size_t>(amount);
    }
    faultCheckpoint(name);
#else
    (void)fd; (void)bytes; (void)size; (void)offset; (void)name;
#endif
}
// Recovery tests force the named mkostemp() fallback that is otherwise only
// used on filesystems without O_TMPFILE support.
inline bool forceNamedScratchFiles() {
#ifdef AOWIS_DVMDB_FAULT_TEST_BUILD
    const char* forced = ::getenv("AOWIS_DVMDB_TEST_NAMED_SCRATCH");
    return forced != nullptr && std::strcmp(forced, "1") == 0;
#else
    return false;
#endif
}
} // namespace aowis::dvmdb::detail
