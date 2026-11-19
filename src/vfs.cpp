#include "vfs.h"
#include <climits>
#include <memory>

struct retro_vfs_file_handle {
    HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr;
    const uint8_t *view = nullptr;
    int64_t position = 0, mapped_size = 0;
    bool readable = false, writable = false;
    std::string path;
    ~retro_vfs_file_handle() {
        if (view)
            UnmapViewOfFile(view);
        if (mapping)
            CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }
};
struct retro_vfs_dir_handle {
    HANDLE search = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW entry{};
    bool first = true, include_hidden = false;
    std::string name;
    ~retro_vfs_dir_handle() {
        if (search != INVALID_HANDLE_VALUE)
            FindClose(search);
    }
};
static const char *RETRO_CALLCONV path(retro_vfs_file_handle *f) { return f ? f->path.c_str() : nullptr; }
static retro_vfs_file_handle *RETRO_CALLCONV open(const char *name, unsigned mode, unsigned hints) {
    try {
        if (!name || !(mode & 3) || (mode & ~7u))
            return nullptr;
        auto f = std::make_unique<retro_vfs_file_handle>();
        f->path = name;
        f->readable = (mode & RETRO_VFS_FILE_ACCESS_READ) != 0;
        f->writable = (mode & RETRO_VFS_FILE_ACCESS_WRITE) != 0;
        DWORD access = (f->readable ? GENERIC_READ : 0) | (f->writable ? GENERIC_WRITE : 0);
        DWORD create =
            f->writable && !(mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) ? CREATE_ALWAYS : OPEN_EXISTING;
        f->file = CreateFileW(widen(name).c_str(), access,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, create,
                              (hints & RETRO_VFS_FILE_ACCESS_HINT_SEQUENTIAL_BULK) ? FILE_FLAG_SEQUENTIAL_SCAN
                                                                                   : FILE_ATTRIBUTE_NORMAL,
                              nullptr);
        if (f->file == INVALID_HANDLE_VALUE)
            return nullptr;
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(f->file, &length))
            return nullptr;
        if (!f->writable && length.QuadPart > 0) {
            f->mapping = CreateFileMappingW(f->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (f->mapping)
                f->view = static_cast<const uint8_t *>(MapViewOfFile(f->mapping, FILE_MAP_READ, 0, 0, 0));
            if (f->view)
                f->mapped_size = length.QuadPart;
            // If address space or mapping is unavailable, retain native file I/O.
        }
        return f.release();
    } catch (...) {
        return nullptr;
    }
}
static int RETRO_CALLCONV close(retro_vfs_file_handle *f) {
    if (!f)
        return -1;
    delete f;
    return 0;
}
static int64_t RETRO_CALLCONV size(retro_vfs_file_handle *f) {
    if (!f)
        return -1;
    if (f->view)
        return f->mapped_size;
    LARGE_INTEGER length{};
    return GetFileSizeEx(f->file, &length) ? length.QuadPart : -1;
}
static int64_t RETRO_CALLCONV tell(retro_vfs_file_handle *f) { return f ? f->position : -1; }
static int64_t RETRO_CALLCONV seek(retro_vfs_file_handle *f, int64_t offset, int origin) {
    if (!f)
        return -1;
    int64_t base;
    switch (origin) {
    case RETRO_VFS_SEEK_POSITION_START:
        base = 0;
        break;
    case RETRO_VFS_SEEK_POSITION_CURRENT:
        base = f->position;
        break;
    case RETRO_VFS_SEEK_POSITION_END:
        base = size(f);
        break;
    default:
        return -1;
    }
    if (base < 0 || (offset > 0 && base > INT64_MAX - offset) || (offset < 0 && offset < -base))
        return -1;
    return f->position = base + offset;
}
static int64_t transfer(retro_vfs_file_handle *f, void *buffer, uint64_t length, bool writing) {
    if (!f || (!buffer && length) || length > INT64_MAX || (writing ? !f->writable : !f->readable) ||
        length > uint64_t(INT64_MAX - f->position))
        return -1;
    if (!length)
        return 0;
    if (f->view && !writing) {
        uint64_t remaining = f->position >= f->mapped_size ? 0 : uint64_t(f->mapped_size - f->position);
        size_t count = static_cast<size_t>(std::min(length, remaining));
        if (count)
            std::memcpy(buffer, f->view + f->position, count);
        f->position += count;
        return count;
    }
    LARGE_INTEGER offset;
    offset.QuadPart = f->position;
    if (!SetFilePointerEx(f->file, offset, nullptr, FILE_BEGIN))
        return -1;
    uint64_t total = 0;
    auto *bytes = static_cast<uint8_t *>(buffer);
    while (total < length) {
        DWORD count = DWORD(std::min<uint64_t>(length - total, 1u << 24)), done = 0;
        BOOL ok = writing ? WriteFile(f->file, bytes + total, count, &done, nullptr)
                          : ReadFile(f->file, bytes + total, count, &done, nullptr);
        if (!ok)
            return total ? int64_t(total) : -1;
        total += done;
        f->position += done;
        if (done < count)
            break;
    }
    return int64_t(total);
}
static int64_t RETRO_CALLCONV read(retro_vfs_file_handle *f, void *data, uint64_t count) {
    return transfer(f, data, count, false);
}
static int64_t RETRO_CALLCONV write(retro_vfs_file_handle *f, const void *data, uint64_t count) {
    return transfer(f, const_cast<void *>(data), count, true);
}
static int RETRO_CALLCONV flush(retro_vfs_file_handle *f) {
    return !f ? -1 : (!f->writable || FlushFileBuffers(f->file) ? 0 : -1);
}
static int RETRO_CALLCONV vfs_remove(const char *name) {
    try {
        return name && DeleteFileW(widen(name).c_str()) ? 0 : -1;
    } catch (...) {
        return -1;
    }
}
static int RETRO_CALLCONV vfs_rename(const char *from, const char *to) {
    try {
        return from && to && MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING)
                   ? 0
                   : -1;
    } catch (...) {
        return -1;
    }
}
static int64_t RETRO_CALLCONV truncate(retro_vfs_file_handle *f, int64_t length) {
    if (!f || !f->writable || length < 0)
        return -1;
    LARGE_INTEGER offset;
    offset.QuadPart = length;
    if (!SetFilePointerEx(f->file, offset, nullptr, FILE_BEGIN) || !SetEndOfFile(f->file))
        return -1;
    return length;
}
static int RETRO_CALLCONV stat64(const char *name, int64_t *length) {
    try {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (length)
            *length = 0;
        if (!name || !GetFileAttributesExW(widen(name).c_str(), GetFileExInfoStandard, &attributes))
            return 0;
        if (length)
            *length = int64_t((uint64_t(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow);
        return RETRO_VFS_STAT_IS_VALID |
               ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? RETRO_VFS_STAT_IS_DIRECTORY : 0);
    } catch (...) {
        return 0;
    }
}
static int RETRO_CALLCONV vfs_stat(const char *name, int32_t *length) {
    int64_t full = 0;
    int flags = stat64(name, &full);
    if (length)
        *length = int32_t(std::min<int64_t>(full, INT32_MAX));
    return flags;
}
static int RETRO_CALLCONV vfs_mkdir(const char *name) {
    try {
        if (!name)
            return -1;
        if (CreateDirectoryW(widen(name).c_str(), nullptr))
            return 0;
        return GetLastError() == ERROR_ALREADY_EXISTS &&
                       (vfs_stat(name, nullptr) & RETRO_VFS_STAT_IS_DIRECTORY)
                   ? -2
                   : -1;
    } catch (...) {
        return -1;
    }
}
static retro_vfs_dir_handle *RETRO_CALLCONV opendir(const char *name, bool hidden) {
    try {
        if (!name || !(vfs_stat(name, nullptr) & RETRO_VFS_STAT_IS_DIRECTORY))
            return nullptr;
        auto d = std::make_unique<retro_vfs_dir_handle>();
        d->include_hidden = hidden;
        auto wildcard = fs::path(widen(name)) / L"*";
        d->search = FindFirstFileW(wildcard.c_str(), &d->entry);
        // Empty directories are valid directory streams too.
        if (d->search == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND)
            return nullptr;
        return d.release();
    } catch (...) {
        return nullptr;
    }
}
static bool RETRO_CALLCONV readdir(retro_vfs_dir_handle *d) {
    if (!d || d->search == INVALID_HANDLE_VALUE)
        return false;
    do {
        if (!d->first && !FindNextFileW(d->search, &d->entry))
            return false;
        d->first = false;
        if (!wcscmp(d->entry.cFileName, L".") || !wcscmp(d->entry.cFileName, L".."))
            continue;
        if (!d->include_hidden && (d->entry.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN))
            continue;
        d->name = narrow(d->entry.cFileName);
        return true;
    } while (true);
}
static const char *RETRO_CALLCONV dir_name(retro_vfs_dir_handle *d) { return d ? d->name.c_str() : nullptr; }
static bool RETRO_CALLCONV dir_is_dir(retro_vfs_dir_handle *d) {
    return d && (d->entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}
static int RETRO_CALLCONV closedir(retro_vfs_dir_handle *d) {
    if (!d)
        return -1;
    delete d;
    return 0;
}
const retro_vfs_interface *windows_vfs() {
    static retro_vfs_interface api{};
    api.get_path = path;
    api.open = open;
    api.close = close;
    api.size = size;
    api.tell = tell;
    api.seek = seek;
    api.read = read;
    api.write = write;
    api.flush = flush;
    api.remove = vfs_remove;
    api.rename = vfs_rename;
    api.truncate = truncate;
    api.stat = vfs_stat;
    api.mkdir = vfs_mkdir;
    api.opendir = opendir;
    api.readdir = readdir;
    api.dirent_get_name = dir_name;
    api.dirent_is_dir = dir_is_dir;
    api.closedir = closedir;
    api.stat_64 = stat64;
    return &api;
}
bool vfs_environment(void *data) {
    auto *info = static_cast<retro_vfs_interface_info *>(data);
    if (!info || info->required_interface_version > 4)
        return false;
    info->required_interface_version = 4;
    info->iface = const_cast<retro_vfs_interface *>(windows_vfs());
    return true;
}
