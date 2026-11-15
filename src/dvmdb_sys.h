#pragma once

// Platform file-system layer of the native engine.
//
// DVMDB's durability protocol is written against POSIX semantics: atomic
// rename-over, no-replace hard-link publication, unlinking open files,
// directory fsync, advisory whole-file locks and descriptor-relative opens.
// Every engine source calls these primitives through aowis::dvmdb::sys.
//
//  * Linux:   the libc functions themselves (using-declarations, zero cost).
//  * macOS:   libc as well, except fsync() -> F_FULLFSYNC (Darwin's fsync only
//             hands data to the drive, not through its volatile cache) and
//             posix_fallocate() -> F_PREALLOCATE + ftruncate (not in Darwin).
//  * Windows: dvmdb_sys_windows.cpp implements the same contracts on Win32
//             handles wrapped in CRT descriptors (see that file for the exact
//             mapping and the documented semantic differences).
//
// O_TMPFILE is Linux-only; openTmpfile()/openatTmpfile() report EOPNOTSUPP
// elsewhere so callers take their existing named-and-unlinked fallback.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <sys/file.h>
#include <sys/mman.h>
#endif

namespace aowis::dvmdb::sys {

#if !defined(_WIN32)

using Stat = struct ::stat;

using ::close;
using ::fchmod;
using ::fstat;
using ::fstatat;
using ::ftruncate;
using ::getpid;
using ::link;
using ::linkat;
using ::lstat;
using ::mkdir;
using ::mkdirat;
using ::mkostemp;
using ::mkstemp;
using ::mmap;
using ::munmap;
using ::open;
using ::openat;
using ::pread;
using ::pwrite;
using ::read;
using ::rename;
using ::unlink;
using ::unlinkat;
using ::write;
using ::flock;

#if defined(__APPLE__)
// Darwin's fsync() does not flush the drive's write cache; F_FULLFSYNC does
// (the guarantee Linux fsync() gives and DVMDB's power-loss protocol needs).
// Filesystems that reject F_FULLFSYNC (some network/FAT volumes) get fsync().
inline int fsync(int fd) {
    if (::fcntl(fd, F_FULLFSYNC) == 0) return 0;
    return ::fsync(fd);
}

// posix_fallocate() is not available on Darwin. Reserve the missing extent
// with F_PREALLOCATE so a full disk is reported now (ENOSPC), then extend the
// visible size. Like posix_fallocate(), returns an errno value, never -1.
inline int posix_fallocate(int fd, off_t offset, off_t length) {
    struct ::stat state {};
    if (::fstat(fd, &state) != 0) return errno;
    const off_t end = offset + length;
    if (state.st_size >= end) return 0;
    fstore_t store{};
    store.fst_flags = F_ALLOCATEALL;
    store.fst_posmode = F_PEOFPOSMODE;
    store.fst_offset = 0;
    store.fst_length = end - state.st_size;
    // Filesystems without preallocation support still get the size change.
    if (::fcntl(fd, F_PREALLOCATE, &store) == -1 && (errno == ENOSPC || errno == EDQUOT))
        return errno;
    if (::ftruncate(fd, end) != 0) return errno;
    return 0;
}
#else
using ::fsync;
using ::posix_fallocate;
#endif

inline int openTmpfile(const char* directory, int flags) {
#ifdef O_TMPFILE
    return ::open(directory, O_TMPFILE | flags, 0600);
#else
    (void)directory;
    (void)flags;
    errno = EOPNOTSUPP;
    return -1;
#endif
}

inline int openatTmpfile(int directoryFd, int flags) {
#ifdef O_TMPFILE
    return ::openat(directoryFd, ".", O_TMPFILE | flags, 0600);
#else
    (void)directoryFd;
    (void)flags;
    errno = EOPNOTSUPP;
    return -1;
#endif
}

#else // _WIN32

// Packs, journals and bundles may exceed 2 GiB. The DVMDB CMake targets set
// _FILE_OFFSET_BITS=64, which turns MinGW's off_t into off64_t.
static_assert(sizeof(off_t) == 8, "DVMDB requires a 64-bit off_t (_FILE_OFFSET_BITS=64)");

// Path argument accepted in every form the engine passes: std::filesystem
// paths and their wide c_str(), and narrow strings. Narrow strings are UTF-8,
// which is what libstdc++'s std::filesystem::path::string() produces on
// Windows. Only converted to UTF-16 here; each call resolves it (relative to
// a directory descriptor for the *at() functions) into an absolute, normalized
// path, with the \\?\ prefix when it would exceed MAX_PATH.
class PathArg final {
public:
    PathArg(const char* utf8);
    PathArg(const wchar_t* wide);
    PathArg(const std::string& utf8);
    PathArg(const std::wstring& wide);
    PathArg(const std::filesystem::path& path);

    const std::wstring& raw() const noexcept { return this->raw_; }

private:
    std::wstring raw_;
};

// The subset of struct stat the engine reads.
struct Stat {
    unsigned int st_mode = 0;
    std::int64_t st_size = 0;
    unsigned int st_nlink = 0;
};

int open(const PathArg& path, int flags, int mode = 0);
int openat(int directoryFd, const PathArg& path, int flags, int mode = 0);
int close(int fd);
ssize_t read(int fd, void* buffer, std::size_t size);
ssize_t write(int fd, const void* buffer, std::size_t size);
ssize_t pread(int fd, void* buffer, std::size_t size, off_t offset);
ssize_t pwrite(int fd, const void* buffer, std::size_t size, off_t offset);
int fstat(int fd, Stat* state);
int lstat(const PathArg& path, Stat* state);
int fstatat(int directoryFd, const PathArg& path, Stat* state, int flags);
int fsync(int fd);
int flock(int fd, int operation);
int ftruncate(int fd, off_t length);
int posix_fallocate(int fd, off_t offset, off_t length);
int link(const PathArg& existing, const PathArg& created);
int linkat(int existingDirectoryFd, const PathArg& existing,
           int createdDirectoryFd, const PathArg& created, int flags);
int unlink(const PathArg& path);
int unlinkat(int directoryFd, const PathArg& path, int flags);
int rename(const PathArg& from, const PathArg& to);
int mkdir(const PathArg& path, int mode);
int mkdirat(int directoryFd, const PathArg& path, int mode);
int mkostemp(char* utf8Template, int flags);
int mkstemp(char* utf8Template);
void* mmap(void* address, std::size_t length, int protection, int flags, int fd, off_t offset);
int munmap(void* address, std::size_t length);
int fchmod(int fd, int mode);
int getpid();

inline int openTmpfile(const PathArg&, int) {
    errno = EOPNOTSUPP;
    return -1;
}

inline int openatTmpfile(int, int) {
    errno = EOPNOTSUPP;
    return -1;
}

#endif // _WIN32

} // namespace aowis::dvmdb::sys

#if defined(_WIN32)
// POSIX constants MinGW does not provide. The O_ values use bits outside the
// CRT's _O_* range; dvmdb_sys_windows.cpp interprets them.
#ifndef O_CLOEXEC
#define O_CLOEXEC 0x01000000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x02000000
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x04000000
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x08000000
#endif
#ifndef LOCK_SH
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
#endif
#ifndef PROT_READ
#define PROT_READ 1
#define PROT_WRITE 2
#endif
#ifndef MAP_SHARED
#define MAP_SHARED 1
#endif
#ifndef MAP_FAILED
#define MAP_FAILED (reinterpret_cast<void*>(-1))
#endif
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_SYMLINK_FOLLOW
#define AT_SYMLINK_FOLLOW 0x400
#endif
#ifndef S_IFLNK
#define S_IFLNK 0xA000
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_IWGRP
#define S_IWGRP 0020
#endif
#ifndef S_IWOTH
#define S_IWOTH 0002
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#endif
