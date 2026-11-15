// Win32 implementation of the DVMDB platform file-system layer (dvmdb_sys.h).
//
// Every descriptor is a CRT descriptor wrapping a HANDLE created by this file
// with FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE. Sharing delete
// access is what allows the POSIX protocol of the engine on Windows: renaming
// over and unlinking files other descriptors still have open.
//
// Contract mapping and the deliberate differences from POSIX:
//  * rename():  FileRenameInfoEx with REPLACE_IF_EXISTS | POSIX_SEMANTICS
//               (atomic replace, even of open targets; Windows 10 1809+ on
//               NTFS). Older systems / other filesystems: MoveFileExW with
//               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH.
//  * unlink():  FileDispositionInfoEx with POSIX_SEMANTICS: the name vanishes
//               immediately while open descriptors remain valid. Fallback:
//               classic delete-on-last-close (name stays until then).
//  * link():    CreateHardLinkW. DVMDB's no-replace publication needs hard
//               links, i.e. an NTFS/ReFS data directory (not FAT/exFAT).
//  * fsync():   FlushFileBuffers for files. Directory fsync is a successful
//               no-op: NTFS journals namespace operations itself and Win32 has
//               no directory flush for read-only directory handles.
//  * flock():   LockFileEx on one byte at offset 0x7FFFFFFF00000000. Windows
//               byte-range locks are mandatory, so the locked byte lies far
//               beyond any lock file's content; reading or copying the file is
//               never blocked. Conversions unlock first, as flock(2) permits.
//  * mmap():    file mappings of existing file content (never extending).
//  * O_NOFOLLOW: symbolic links and junctions are rejected with ELOOP; other
//               reparse points (OneDrive placeholders, dedup) open normally.
//  * openat() and the other *at() calls resolve names against the absolute
//               path recorded for the directory descriptor (Win32 has no
//               descriptor-relative namespace API). The data directory of an
//               embedded desktop database is owned by the logged-on user, so
//               the symlink-race protection that POSIX *at() calls provide is
//               not needed there.
//  * O_TMPFILE does not exist (dvmdb_sys.h reports EOPNOTSUPP); callers use
//               their named mkostemp() + unlink() fallback.

#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "dvmdb_sys.h"
#include "dvmdb_crypto.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK 0xA000000CUL
#endif
#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003UL
#endif

namespace aowis::dvmdb::sys {
namespace {

// FILE_INFO_BY_HANDLE_CLASS values and their payloads are declared here
// rather than taken from the MinGW headers, whose coverage of the Windows 10
// "Ex" classes varies between releases. Layouts match the Windows SDK.
constexpr int fileDispositionInfoClass = 4;
constexpr int fileAllocationInfoClass = 5;
constexpr int fileEndOfFileInfoClass = 6;
constexpr int fileAttributeTagInfoClass = 9;
constexpr int fileDispositionInfoExClass = 21;
constexpr int fileRenameInfoExClass = 22;

constexpr DWORD dispositionDelete = 0x00000001;
constexpr DWORD dispositionPosixSemantics = 0x00000002;
constexpr DWORD dispositionIgnoreReadonly = 0x00000010;
constexpr DWORD renameReplaceIfExists = 0x00000001;
constexpr DWORD renamePosixSemantics = 0x00000002;

struct DispositionInfo {
    BOOLEAN deleteFile;
};
struct DispositionInfoEx {
    DWORD flags;
};
struct AllocationInfo {
    LARGE_INTEGER allocationSize;
};
struct EndOfFileInfo {
    LARGE_INTEGER endOfFile;
};
struct AttributeTagInfo {
    DWORD fileAttributes;
    DWORD reparseTag;
};
// FILE_RENAME_INFO: the leading union { BOOLEAN ReplaceIfExists; DWORD Flags; }
// is a DWORD for FileRenameInfoEx.
struct RenameInfo {
    DWORD flags;
    HANDLE rootDirectory;
    DWORD fileNameLength;
    WCHAR fileName[1];
};

FILE_INFO_BY_HANDLE_CLASS infoClass(int value) {
    return static_cast<FILE_INFO_BY_HANDLE_CLASS>(value);
}

constexpr DWORD shareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

int errnoFromWin32(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
        return ENOENT;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return EEXIST;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_WRITE_PROTECT:
        return EACCES;
    case ERROR_SHARING_VIOLATION:
    case ERROR_BUSY:
    case ERROR_USER_MAPPED_FILE:
        return EBUSY;
    case ERROR_LOCK_VIOLATION:
        return EWOULDBLOCK;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_DISK_QUOTA_EXCEEDED:
        return ENOSPC;
    case ERROR_DIR_NOT_EMPTY:
        return ENOTEMPTY;
    case ERROR_NOT_SAME_DEVICE:
        return EXDEV;
    case ERROR_DIRECTORY:
        return ENOTDIR;
    case ERROR_FILENAME_EXCED_RANGE:
        return ENAMETOOLONG;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_INVALID_PARAMETER:
    case ERROR_NEGATIVE_SEEK:
        return EINVAL;
    case ERROR_TOO_MANY_OPEN_FILES:
        return EMFILE;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    case ERROR_INVALID_HANDLE:
        return EBADF;
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:
    case ERROR_CALL_NOT_IMPLEMENTED:
        return EOPNOTSUPP;
    case ERROR_CANT_RESOLVE_FILENAME:
        return ELOOP;
    default:
        return EIO;
    }
}

int failWith(DWORD error) {
    errno = errnoFromWin32(error);
    return -1;
}

int failWithErrno(int value) {
    errno = value;
    return -1;
}

// Book-keeping per descriptor: the absolute path (for the *at() functions),
// its kind, and the flock() state that Windows locks do not expose.
struct Descriptor {
    std::wstring path;
    bool directory = false;
    bool writable = false;
    std::mutex lockMutex;
    int lockMode = 0;
};

std::mutex& registryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<int, std::shared_ptr<Descriptor>>& registry() {
    static std::unordered_map<int, std::shared_ptr<Descriptor>> descriptors;
    return descriptors;
}

std::shared_ptr<Descriptor> descriptorOf(int fd) {
    const std::lock_guard<std::mutex> guard(registryMutex());
    const std::unordered_map<int, std::shared_ptr<Descriptor>>::const_iterator found = registry().find(fd);
    return found == registry().end() ? nullptr : found->second;
}

HANDLE handleOf(int fd) {
    if (fd < 0) return INVALID_HANDLE_VALUE;
    return reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
}

bool startsWith(const std::wstring& value, const wchar_t* prefix) {
    const std::size_t length = std::wcslen(prefix);
    return value.size() >= length && value.compare(0, length, prefix) == 0;
}

bool isAbsolute(const std::wstring& path) {
    if (path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/'))
        return true; // UNC or \\?\ verbatim
    return path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/') &&
           ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'));
}

// Absolute, normalized path (slashes, "." and ".." resolved). Empty on error,
// with errno set.
std::wstring fullPath(const std::wstring& input) {
    if (input.empty()) {
        errno = ENOENT;
        return {};
    }
    if (startsWith(input, L"\\\\?\\")) return input;
    const DWORD needed = ::GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        errno = errnoFromWin32(::GetLastError());
        return {};
    }
    std::wstring output(needed, L'\0');
    const DWORD written = ::GetFullPathNameW(input.c_str(), needed, output.data(), nullptr);
    if (written == 0 || written >= needed) {
        errno = written == 0 ? errnoFromWin32(::GetLastError()) : ENAMETOOLONG;
        return {};
    }
    output.resize(written);
    return output;
}

// The form handed to Win32: \\?\-prefixed when the path would hit MAX_PATH
// (CreateDirectoryW's limit is MAX_PATH - 12) and the process is not
// long-path aware.
std::wstring win32Path(const std::wstring& full) {
    if (full.size() < 240 || startsWith(full, L"\\\\?\\")) return full;
    if (startsWith(full, L"\\\\")) return L"\\\\?\\UNC\\" + full.substr(2);
    return L"\\\\?\\" + full;
}

// Resolves `path` against a directory descriptor (or the working directory
// for AT_FDCWD / absolute paths). Empty on error, with errno set.
std::wstring resolveAt(int directoryFd, const PathArg& path) {
    const std::wstring& raw = path.raw();
    if (directoryFd == AT_FDCWD || isAbsolute(raw)) return fullPath(raw);
    const std::shared_ptr<Descriptor> directory = descriptorOf(directoryFd);
    if (!directory) {
        errno = EBADF;
        return {};
    }
    if (!directory->directory) {
        errno = ENOTDIR;
        return {};
    }
    return fullPath(directory->path + L"\\" + raw);
}

std::wstring widen(const char* text, std::size_t size) {
    if (size == 0) return {};
    const int length = static_cast<int>(std::min<std::size_t>(size, 0x7fffffff));
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text, length, nullptr, 0);
    if (needed <= 0) return {};
    std::wstring output(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text, length, output.data(), needed);
    return output;
}

bool isLinkTag(DWORD tag) {
    return tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT;
}

// True if the handle (opened with FILE_FLAG_OPEN_REPARSE_POINT) is a
// symbolic link or junction rather than a regular reparse-point file.
bool handleIsLink(HANDLE handle, DWORD attributes) {
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) return false;
    AttributeTagInfo tag{};
    if (!::GetFileInformationByHandleEx(handle, infoClass(fileAttributeTagInfoClass), &tag, sizeof(tag)))
        return true; // unknown reparse point: be conservative, treat it as a link
    return isLinkTag(tag.reparseTag);
}

void fillStat(const BY_HANDLE_FILE_INFORMATION& information, bool link, Stat* state) {
    if (link) state->st_mode = S_IFLNK | 0777U;
    else if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) state->st_mode = S_IFDIR | 0700U;
    else state->st_mode = S_IFREG | 0600U;
    state->st_size = static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) | information.nFileSizeLow);
    state->st_nlink = static_cast<unsigned int>(information.nNumberOfLinks);
}

int openResolved(const std::wstring& full, int flags) {
    const int accessMode = flags & (O_RDONLY | O_WRONLY | O_RDWR);
    DWORD access = GENERIC_READ;
    if (accessMode == O_WRONLY) access = GENERIC_WRITE | FILE_READ_ATTRIBUTES;
    else if (accessMode == O_RDWR) access = GENERIC_READ | GENERIC_WRITE;
    if ((flags & O_APPEND) != 0) return failWithErrno(EINVAL); // not used by the engine

    DWORD disposition = OPEN_EXISTING;
    if ((flags & O_CREAT) != 0) {
        if ((flags & O_EXCL) != 0) disposition = CREATE_NEW;
        else if ((flags & O_TRUNC) != 0) disposition = CREATE_ALWAYS;
        else disposition = OPEN_ALWAYS;
    } else if ((flags & O_TRUNC) != 0) {
        disposition = TRUNCATE_EXISTING;
    }

    // BACKUP_SEMANTICS lets the same call open directories (O_DIRECTORY and
    // plain O_RDONLY opens of a directory, as POSIX permits).
    const DWORD baseAttributes = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS;
    const bool noFollow = (flags & O_NOFOLLOW) != 0;
    const std::wstring target = win32Path(full);
    HANDLE handle = ::CreateFileW(target.c_str(), access, shareAll, nullptr, disposition,
                                  baseAttributes | (noFollow ? FILE_FLAG_OPEN_REPARSE_POINT : 0), nullptr);
    if (handle == INVALID_HANDLE_VALUE) return failWith(::GetLastError());

    BY_HANDLE_FILE_INFORMATION information{};
    if (!::GetFileInformationByHandle(handle, &information)) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(handle);
        return failWith(error);
    }
    if (noFollow && (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (handleIsLink(handle, information.dwFileAttributes)) {
            ::CloseHandle(handle);
            return failWithErrno(ELOOP);
        }
        // A non-link reparse point (cloud placeholder, dedup): reopen it
        // normally so its content is served by its filesystem filter.
        ::CloseHandle(handle);
        handle = ::CreateFileW(target.c_str(), access, shareAll, nullptr, OPEN_EXISTING, baseAttributes, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return failWith(::GetLastError());
        if (!::GetFileInformationByHandle(handle, &information)) {
            const DWORD error = ::GetLastError();
            ::CloseHandle(handle);
            return failWith(error);
        }
    }
    const bool directory = (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if ((flags & O_DIRECTORY) != 0 && !directory) {
        ::CloseHandle(handle);
        return failWithErrno(ENOTDIR);
    }

    const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle),
                                     _O_BINARY | (accessMode == O_RDONLY ? _O_RDONLY : 0));
    if (fd < 0) {
        ::CloseHandle(handle);
        return failWithErrno(EMFILE);
    }
    std::shared_ptr<Descriptor> descriptor = std::make_shared<Descriptor>();
    descriptor->path = full;
    descriptor->directory = directory;
    descriptor->writable = accessMode != O_RDONLY;
    const std::lock_guard<std::mutex> guard(registryMutex());
    registry()[fd] = std::move(descriptor);
    return fd;
}

int statResolved(const std::wstring& full, Stat* state, bool follow) {
    if (state == nullptr) return failWithErrno(EFAULT);
    // FILE_READ_ATTRIBUTES never conflicts with other handles' share modes.
    HANDLE handle = ::CreateFileW(win32Path(full).c_str(), FILE_READ_ATTRIBUTES, shareAll, nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT),
                                  nullptr);
    if (handle == INVALID_HANDLE_VALUE) return failWith(::GetLastError());
    BY_HANDLE_FILE_INFORMATION information{};
    const BOOL ok = ::GetFileInformationByHandle(handle, &information);
    const DWORD error = ok ? 0 : ::GetLastError();
    const bool link = ok && !follow && handleIsLink(handle, information.dwFileAttributes);
    ::CloseHandle(handle);
    if (!ok) return failWith(error);
    *state = Stat{};
    fillStat(information, link, state);
    return 0;
}

// Third-party processes (virus scanners, indexers, backup tools) briefly open
// files without FILE_SHARE_DELETE. Namespace changes retry such transient
// sharing failures for up to ~100 ms before reporting them.
bool transientNamespaceError(DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED;
}

constexpr int transientRetries = 20;
constexpr DWORD transientRetryMilliseconds = 5;

// Returns 0 or the Win32 error; a real directory sets isDirectory instead.
DWORD unlinkOnce(const std::wstring& target, bool& isDirectory) {
    isDirectory = false;
    HANDLE handle = ::CreateFileW(target.c_str(), DELETE | FILE_READ_ATTRIBUTES, shareAll, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                  nullptr);
    if (handle == INVALID_HANDLE_VALUE) return ::GetLastError();
    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileInformationByHandle(handle, &information) &&
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        !handleIsLink(handle, information.dwFileAttributes)) {
        ::CloseHandle(handle);
        isDirectory = true; // unlink(2) never removes a real directory
        return ERROR_ACCESS_DENIED;
    }
    DispositionInfoEx posix{dispositionDelete | dispositionPosixSemantics | dispositionIgnoreReadonly};
    BOOL ok = ::SetFileInformationByHandle(handle, infoClass(fileDispositionInfoExClass), &posix, sizeof(posix));
    DWORD error = ok ? 0 : ::GetLastError();
    if (!ok && (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED ||
                error == ERROR_INVALID_FUNCTION)) {
        // Pre-1709 Windows or a filesystem without POSIX deletes (FAT/exFAT).
        DispositionInfo legacy{TRUE};
        ok = ::SetFileInformationByHandle(handle, infoClass(fileDispositionInfoClass), &legacy, sizeof(legacy));
        error = ok ? 0 : ::GetLastError();
    }
    ::CloseHandle(handle);
    return error;
}

int unlinkResolved(const std::wstring& full) {
    const std::wstring target = win32Path(full);
    for (int attempt = 0;; ++attempt) {
        bool isDirectory = false;
        const DWORD error = unlinkOnce(target, isDirectory);
        if (error == 0) return 0;
        if (isDirectory) return failWithErrno(EISDIR);
        if (!transientNamespaceError(error) || attempt >= transientRetries) return failWith(error);
        ::Sleep(transientRetryMilliseconds);
    }
}

int linkResolved(const std::wstring& existing, const std::wstring& created) {
    if (existing.empty() || created.empty()) return -1;
    if (!::CreateHardLinkW(win32Path(created).c_str(), win32Path(existing).c_str(), nullptr))
        return failWith(::GetLastError());
    return 0;
}

int mkdirResolved(const std::wstring& full) {
    if (full.empty()) return -1;
    if (!::CreateDirectoryW(win32Path(full).c_str(), nullptr)) return failWith(::GetLastError());
    return 0;
}

OVERLAPPED lockRange() {
    OVERLAPPED range{};
    range.Offset = 0;
    range.OffsetHigh = 0x7FFFFFFFU;
    return range;
}

} // namespace

PathArg::PathArg(const char* utf8) : raw_(widen(utf8, utf8 == nullptr ? 0 : std::strlen(utf8))) {}
PathArg::PathArg(const wchar_t* wide) : raw_(wide == nullptr ? L"" : wide) {}
PathArg::PathArg(const std::string& utf8) : raw_(widen(utf8.data(), utf8.size())) {}
PathArg::PathArg(const std::wstring& wide) : raw_(wide) {}
PathArg::PathArg(const std::filesystem::path& path) : raw_(path.native()) {}

int open(const PathArg& path, int flags, int) {
    const std::wstring full = fullPath(path.raw());
    if (full.empty()) return -1;
    return openResolved(full, flags);
}

int openat(int directoryFd, const PathArg& path, int flags, int) {
    const std::wstring full = resolveAt(directoryFd, path);
    if (full.empty()) return -1;
    return openResolved(full, flags);
}

int close(int fd) {
    {
        const std::lock_guard<std::mutex> guard(registryMutex());
        registry().erase(fd);
    }
    // Closing the handle also releases its LockFileEx lock, like flock(2).
    return ::_close(fd);
}

ssize_t read(int fd, void* buffer, std::size_t size) {
    HANDLE handle = handleOf(fd);
    if (handle == INVALID_HANDLE_VALUE) return failWithErrno(EBADF);
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1U << 30U));
    DWORD done = 0;
    if (!::ReadFile(handle, buffer, chunk, &done, nullptr)) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_HANDLE_EOF || error == ERROR_BROKEN_PIPE) return 0;
        return failWith(error);
    }
    return static_cast<ssize_t>(done);
}

ssize_t write(int fd, const void* buffer, std::size_t size) {
    HANDLE handle = handleOf(fd);
    if (handle == INVALID_HANDLE_VALUE) return failWithErrno(EBADF);
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1U << 30U));
    DWORD done = 0;
    if (!::WriteFile(handle, buffer, chunk, &done, nullptr)) return failWith(::GetLastError());
    return static_cast<ssize_t>(done);
}

// Positional I/O on a synchronous handle also moves its file pointer, which
// pread/pwrite must not do; the pointer is restored afterwards.
ssize_t pread(int fd, void* buffer, std::size_t size, off_t offset) {
    HANDLE handle = handleOf(fd);
    if (handle == INVALID_HANDLE_VALUE) return failWithErrno(EBADF);
    if (offset < 0) return failWithErrno(EINVAL);
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1U << 30U));
    LARGE_INTEGER zero{};
    LARGE_INTEGER saved{};
    const bool restore = ::SetFilePointerEx(handle, zero, &saved, FILE_CURRENT) != 0;
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(static_cast<std::uint64_t>(offset) & 0xFFFFFFFFULL);
    position.OffsetHigh = static_cast<DWORD>(static_cast<std::uint64_t>(offset) >> 32U);
    DWORD done = 0;
    const BOOL ok = ::ReadFile(handle, buffer, chunk, &done, &position);
    const DWORD error = ok ? 0 : ::GetLastError();
    if (restore) ::SetFilePointerEx(handle, saved, nullptr, FILE_BEGIN);
    if (!ok) {
        if (error == ERROR_HANDLE_EOF) return 0;
        return failWith(error);
    }
    return static_cast<ssize_t>(done);
}

ssize_t pwrite(int fd, const void* buffer, std::size_t size, off_t offset) {
    HANDLE handle = handleOf(fd);
    if (handle == INVALID_HANDLE_VALUE) return failWithErrno(EBADF);
    if (offset < 0) return failWithErrno(EINVAL);
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1U << 30U));
    LARGE_INTEGER zero{};
    LARGE_INTEGER saved{};
    const bool restore = ::SetFilePointerEx(handle, zero, &saved, FILE_CURRENT) != 0;
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(static_cast<std::uint64_t>(offset) & 0xFFFFFFFFULL);
    position.OffsetHigh = static_cast<DWORD>(static_cast<std::uint64_t>(offset) >> 32U);
    DWORD done = 0;
    const BOOL ok = ::WriteFile(handle, buffer, chunk, &done, &position);
    const DWORD error = ok ? 0 : ::GetLastError();
    if (restore) ::SetFilePointerEx(handle, saved, nullptr, FILE_BEGIN);
    if (!ok) return failWith(error);
    return static_cast<ssize_t>(done);
}

int fstat(int fd, Stat* state) {
    if (state == nullptr) return failWithErrno(EFAULT);
    HANDLE handle = handleOf(fd);
    if (handle == INVALID_HANDLE_VALUE) return failWithErrno(EBADF);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!::GetFileInformationByHandle(handle, &information)) return failWith(::GetLastError());
    *state = Stat{};
    fillStat(information, false, state);
    return 0;
}

int lstat(const PathArg& path, Stat* state) {
    const std::wstring full = fullPath(path.raw());
    if (full.empty()) return -1;
    return statResolved(full, state, false);
}

int fstatat(int directoryFd, const PathArg& path, Stat* state, int flags) {
    const std::wstring full = resolveAt(directoryFd, path);
    if (full.empty()) return -1;
    return statResolved(full, state, (flags & AT_SYMLINK_NOFOLLOW) == 0);
}

int fsync(int fd) {
    HANDLE handle = handleOf(fd);
    if (handle == INVALID_HANDLE_VALUE) return failWithErrno(EBADF);
    const std::shared_ptr<Descriptor> descriptor = descriptorOf(fd);
    if (descriptor && descriptor->directory) return 0; // see file header
    if (!::FlushFileBuffers(handle)) {
        const DWORD error = ::GetLastError();
        // A read-only handle has nothing of its own to flush (POSIX allows
        // fsync on it; Win32 requires write access).
        if (error == ERROR_ACCESS_DENIED && descriptor && !descriptor->writable) return 0;
        return failWith(error);
    }
    return 0;
}

int flock(int fd, int operation) {
    HANDLE handle = handleOf(fd);
    const std::shared_ptr<Descriptor> descriptor = descriptorOf(fd);
    if (handle == INVALID_HANDLE_VALUE || !descriptor) return failWithErrno(EBADF);
    const int wanted = operation & (LOCK_SH | LOCK_EX | LOCK_UN);
    // Serializes lock changes of one descriptor; flock(2) treats a repeated
    // request in the same mode on the same open file as a no-op.
    const std::lock_guard<std::mutex> guard(descriptor->lockMutex);
    if (wanted == LOCK_UN) {
        if (descriptor->lockMode != 0) {
            OVERLAPPED range = lockRange();
            ::UnlockFileEx(handle, 0, 1, 0, &range);
            descriptor->lockMode = 0;
        }
        return 0;
    }
    if (wanted != LOCK_SH && wanted != LOCK_EX) return failWithErrno(EINVAL);
    if (descriptor->lockMode == wanted) return 0;
    if (descriptor->lockMode != 0) {
        // Windows cannot convert a lock in place (a second lock on the same
        // handle would conflict with the first). flock(2) does not promise an
        // atomic conversion either; the engine never relies on one.
        OVERLAPPED range = lockRange();
        ::UnlockFileEx(handle, 0, 1, 0, &range);
        descriptor->lockMode = 0;
    }
    DWORD lockFlags = wanted == LOCK_EX ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if ((operation & LOCK_NB) != 0) lockFlags |= LOCKFILE_FAIL_IMMEDIATELY;
    OVERLAPPED range = lockRange();
    if (!::LockFileEx(handle, lockFlags, 0, 1, 0, &range)) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING) return failWithErrno(EWOULDBLOCK);
        return failWith(error);
    }
    descriptor->lockMode = wanted;
    return 0;
}

int ftruncate(int fd, off_t length) {
    HANDLE handle = handleOf(fd);
    if (handle == INVALID_HANDLE_VALUE) return failWithErrno(EBADF);
    if (length < 0) return failWithErrno(EINVAL);
    EndOfFileInfo end{};
    end.endOfFile.QuadPart = static_cast<LONGLONG>(length);
    if (!::SetFileInformationByHandle(handle, infoClass(fileEndOfFileInfoClass), &end, sizeof(end)))
        return failWith(::GetLastError());
    return 0;
}

int posix_fallocate(int fd, off_t offset, off_t length) {
    HANDLE handle = handleOf(fd);
    if (handle == INVALID_HANDLE_VALUE) return EBADF;
    if (offset < 0 || length <= 0) return EINVAL;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle, &size)) return errnoFromWin32(::GetLastError());
    const std::int64_t end = static_cast<std::int64_t>(offset) + static_cast<std::int64_t>(length);
    if (size.QuadPart >= end) return 0;
    // Reserve clusters first so a full disk is reported now (the reason the
    // engine preallocates scratch space before mapping it).
    AllocationInfo allocation{};
    allocation.allocationSize.QuadPart = end;
    if (!::SetFileInformationByHandle(handle, infoClass(fileAllocationInfoClass), &allocation, sizeof(allocation))) {
        const int error = errnoFromWin32(::GetLastError());
        if (error == ENOSPC) return ENOSPC;
    }
    EndOfFileInfo eof{};
    eof.endOfFile.QuadPart = end;
    if (!::SetFileInformationByHandle(handle, infoClass(fileEndOfFileInfoClass), &eof, sizeof(eof)))
        return errnoFromWin32(::GetLastError());
    return 0;
}

int link(const PathArg& existing, const PathArg& created) {
    return linkResolved(fullPath(existing.raw()), fullPath(created.raw()));
}

int linkat(int existingDirectoryFd, const PathArg& existing,
           int createdDirectoryFd, const PathArg& created, int) {
    return linkResolved(resolveAt(existingDirectoryFd, existing), resolveAt(createdDirectoryFd, created));
}

int unlink(const PathArg& path) {
    const std::wstring full = fullPath(path.raw());
    if (full.empty()) return -1;
    return unlinkResolved(full);
}

int unlinkat(int directoryFd, const PathArg& path, int flags) {
    if (flags != 0) return failWithErrno(EINVAL); // AT_REMOVEDIR is not used by the engine
    const std::wstring full = resolveAt(directoryFd, path);
    if (full.empty()) return -1;
    return unlinkResolved(full);
}

namespace {

// Returns 0 or the Win32 error of one atomic replace-rename attempt.
DWORD renameOnce(const std::wstring& sourcePath, const std::wstring& destinationPath) {
    HANDLE handle = ::CreateFileW(sourcePath.c_str(), DELETE | SYNCHRONIZE, shareAll, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return ::GetLastError();
    const std::size_t nameBytes = destinationPath.size() * sizeof(wchar_t);
    std::vector<std::uint8_t> buffer(sizeof(RenameInfo) + nameBytes, 0);
    RenameInfo* information = reinterpret_cast<RenameInfo*>(buffer.data());
    information->flags = renameReplaceIfExists | renamePosixSemantics;
    information->rootDirectory = nullptr;
    information->fileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(information->fileName, destinationPath.data(), nameBytes);
    const BOOL ok = ::SetFileInformationByHandle(handle, infoClass(fileRenameInfoExClass), information,
                                                 static_cast<DWORD>(buffer.size()));
    DWORD error = ok ? 0 : ::GetLastError();
    ::CloseHandle(handle);
    if (ok) return 0;
    if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION) {
        // No POSIX rename on this system/filesystem (or a directory source).
        // MoveFileExW never replaces a directory, matching rename(2) onto a
        // non-empty directory failing.
        if (::MoveFileExW(sourcePath.c_str(), destinationPath.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return 0;
        error = ::GetLastError();
    }
    return error;
}

} // namespace

int rename(const PathArg& from, const PathArg& to) {
    const std::wstring source = fullPath(from.raw());
    if (source.empty()) return -1;
    const std::wstring destination = fullPath(to.raw());
    if (destination.empty()) return -1;
    const std::wstring sourcePath = win32Path(source);
    const std::wstring destinationPath = win32Path(destination);
    for (int attempt = 0;; ++attempt) {
        const DWORD error = renameOnce(sourcePath, destinationPath);
        if (error == 0) return 0;
        if (!transientNamespaceError(error) || attempt >= transientRetries) return failWith(error);
        ::Sleep(transientRetryMilliseconds);
    }
}

int mkdir(const PathArg& path, int) {
    return mkdirResolved(fullPath(path.raw()));
}

int mkdirat(int directoryFd, const PathArg& path, int) {
    return mkdirResolved(resolveAt(directoryFd, path));
}

int mkostemp(char* utf8Template, int flags) {
    if (utf8Template == nullptr) return failWithErrno(EINVAL);
    const std::size_t length = std::strlen(utf8Template);
    if (length < 6 || std::strcmp(utf8Template + length - 6, "XXXXXX") != 0) return failWithErrno(EINVAL);
    static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int attempt = 0; attempt < 128; ++attempt) {
        std::uint8_t random[6]{};
        if (!crypto::secureRandomBytes(random, sizeof(random))) return failWithErrno(EIO);
        for (std::size_t index = 0; index < sizeof(random); ++index)
            utf8Template[length - 6 + index] = alphabet[random[index] % (sizeof(alphabet) - 1)];
        const int fd = sys::open(utf8Template, O_RDWR | O_CREAT | O_EXCL | (flags & O_CLOEXEC), 0600);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    return failWithErrno(EEXIST);
}

int mkstemp(char* utf8Template) {
    return sys::mkostemp(utf8Template, 0);
}

void* mmap(void*, std::size_t length, int protection, int, int fd, off_t offset) {
    HANDLE handle = handleOf(fd);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return MAP_FAILED;
    }
    if (length == 0 || offset < 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    const bool writable = (protection & PROT_WRITE) != 0;
    // Maximum size 0 = the current file size: like mmap(2), never extend.
    HANDLE mapping = ::CreateFileMappingW(handle, nullptr, writable ? PAGE_READWRITE : PAGE_READONLY,
                                          0, 0, nullptr);
    if (mapping == nullptr) {
        errno = errnoFromWin32(::GetLastError());
        return MAP_FAILED;
    }
    const std::uint64_t start = static_cast<std::uint64_t>(offset);
    void* view = ::MapViewOfFile(mapping, writable ? FILE_MAP_WRITE : FILE_MAP_READ,
                                 static_cast<DWORD>(start >> 32U),
                                 static_cast<DWORD>(start & 0xFFFFFFFFULL), length);
    const DWORD error = view == nullptr ? ::GetLastError() : 0;
    ::CloseHandle(mapping); // the view keeps the section alive
    if (view == nullptr) {
        errno = errnoFromWin32(error);
        return MAP_FAILED;
    }
    return view;
}

int munmap(void* address, std::size_t) {
    if (!::UnmapViewOfFile(address)) return failWith(::GetLastError());
    return 0;
}

int fchmod(int, int) {
    return 0; // POSIX permission bits do not exist; the data directory ACL applies
}

int getpid() {
    return static_cast<int>(::GetCurrentProcessId());
}

} // namespace aowis::dvmdb::sys
