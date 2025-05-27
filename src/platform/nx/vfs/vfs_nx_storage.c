#include "ftpsrv_vfs.h"
#include "vfs_nx_storage.h"
#include "log/log.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>

#define min(x, y) ((x) < (y) ? (x) : (y))

struct StorageCacheEntry {
    FsStorage s;
    FsBisPartitionId id;
    u32 ref_count;
};

struct MountEntry {
    const char* name;
    FsBisPartitionId id;
};

static const struct MountEntry BIS_NAMES[] = {
    { "BootPartition1Root.bin", FsBisPartitionId_BootPartition1Root },
    { "BootPartition2Root.bin", FsBisPartitionId_BootPartition2Root },
    { "UserDataRoot.bin", FsBisPartitionId_UserDataRoot },
    { "BootConfigAndPackage2Part1.bin", FsBisPartitionId_BootConfigAndPackage2Part1 },
    { "BootConfigAndPackage2Part2.bin", FsBisPartitionId_BootConfigAndPackage2Part2 },
    { "BootConfigAndPackage2Part3.bin", FsBisPartitionId_BootConfigAndPackage2Part3 },
    { "BootConfigAndPackage2Part4.bin", FsBisPartitionId_BootConfigAndPackage2Part4 },
    { "BootConfigAndPackage2Part5.bin", FsBisPartitionId_BootConfigAndPackage2Part5 },
    { "BootConfigAndPackage2Part6.bin", FsBisPartitionId_BootConfigAndPackage2Part6 },
    // this causes a fatal part way through the transfer.
    // i assume this is due to cal being protected by ams?
    // { "CalibrationBinary.bin", FsBisPartitionId_CalibrationBinary },
    { "CalibrationFile.bin", FsBisPartitionId_CalibrationFile },
    { "SafeMode.bin", FsBisPartitionId_SafeMode },
    { "User.bin", FsBisPartitionId_User },
    { "System.bin", FsBisPartitionId_System },
    { "SystemProperEncryption.bin", FsBisPartitionId_SystemProperEncryption },
    { "SystemProperPartition.bin", FsBisPartitionId_SystemProperPartition },
    { "SignedSystemPartitionOnSafeMode.bin", FsBisPartitionId_SignedSystemPartitionOnSafeMode },
    { "DeviceTreeBlob.bin", FsBisPartitionId_DeviceTreeBlob },
    { "System0.bin", FsBisPartitionId_System0 },
};

static struct StorageCacheEntry g_storage_cache[ARRAY_SIZE(BIS_NAMES)];

static const struct MountEntry* find_mount_entry(const char* path) {
    if (strncmp(path, "bis:/", strlen("bis:/"))) {
        return NULL;
    }

    path += strlen("bis:/");

    for (int i = 0; i < ARRAY_SIZE(BIS_NAMES); i++) {
        if (!strcmp(path, BIS_NAMES[i].name)) {
            return &BIS_NAMES[i];
        }
    }

    return NULL;
}

static FsStorage* mount_storage(FsBisPartitionId id) {
    for (int i = 0; i < ARRAY_SIZE(g_storage_cache); i++) {
        struct StorageCacheEntry* entry = &g_storage_cache[i];
        if (entry->ref_count && entry->id == id) {
            entry->ref_count++;
            return &entry->s;
        }
    }

    // save is not currently mounted, find the next free slot
    for (int i = 0; i < ARRAY_SIZE(g_storage_cache); i++) {
        struct StorageCacheEntry* entry = &g_storage_cache[i];
        if (!entry->ref_count) {
            Result rc;
            if (R_FAILED(rc = fsOpenBisStorage(&entry->s, id))) {
                vfs_fs_set_errno(rc);
                log_file_fwrite("failed: fsOpenBisStorage(%u) 0x%X\n", id, rc);
                return NULL;
            }

            entry->id = id;
            entry->ref_count++;
            return &entry->s;
        }
    }

    return NULL;
}

static void unmount_storage(FsBisPartitionId id) {
    for (int i = 0; i < ARRAY_SIZE(g_storage_cache); i++) {
        struct StorageCacheEntry* entry = &g_storage_cache[i];
        if (entry->ref_count && entry->id == id) {
            entry->ref_count--;
            if (!entry->ref_count) {
                fsStorageClose(&entry->s);
            }
        }
    }
}

static int vfs_storage_open(void* user, const char* path, enum FtpVfsOpenMode mode) {
    struct VfsStorageFile* f = user;

    const struct MountEntry* e = find_mount_entry(path);
    if (!e) {
        return -1;
    }

    FsStorage* s = mount_storage(e->id);
    if (!s) {
        return -1;
    }

    const int rc = vfs_storage_internal_open(s, f, path, mode);
    if (rc) {
        unmount_storage(e->id);
    } else {
        f->id = e->id;
    }

    return rc;
}

static int vfs_storage_read(void* user, void* buf, size_t size) {
    struct VfsStorageFile* f = user;
    return vfs_storage_internal_read(f, buf, size);
}

static int vfs_storage_write(void* user, const void* buf, size_t size) {
    return -1;
}

static int vfs_storage_seek(void* user, const void* buf, size_t size, size_t off) {
    struct VfsStorageFile* f = user;
    return vfs_storage_internal_seek(f, off);
}

static int vfs_storage_isfile_open(void* user) {
    struct VfsStorageFile* f = user;
    return vfs_storage_internal_isfile_open(f);
}

static int vfs_storage_close(void* user) {
    struct VfsStorageFile* f = user;
    if (!vfs_storage_isfile_open(f)) {
        return -1;
    }

    vfs_storage_internal_close(f);
    unmount_storage(f->id);
    return 0;
}

static int vfs_storage_opendir(void* user, const char* path) {
    if (strcmp(path, "bis:")) {
        return -1;
    }

    struct VfsStorageDir* f = user;
    f->index = 0;
    f->is_valid = 1;
    return 0;
}

static const char* vfs_storage_readdir(void* user, void* user_entry) {
    struct VfsStorageDir* f = user;
    struct VfsStorageDirEntry* entry = user_entry;

    if (f->index < ARRAY_SIZE(BIS_NAMES)) {
        const struct MountEntry e = BIS_NAMES[f->index++];
        entry->id = e.id;
        return e.name;
    } else {
        return NULL;
    }
}

static int vfs_storage_dirlstat(void* user, const void* user_entry, const char* path, struct stat* st) {
    const struct VfsStorageDirEntry* entry = user_entry;

    FsStorage* s = mount_storage(entry->id);
    if (!s) {
        return -1;
    }

    const int rc = vfs_storage_internal_stat(s, path, st);
    unmount_storage(entry->id);
    return rc;
}

static int vfs_storage_isdir_open(void* user) {
    struct VfsStorageDir* f = user;
    return f->is_valid;
}

static int vfs_storage_closedir(void* user) {
    struct VfsStorageDir* f = user;
    if (!vfs_storage_isdir_open(f)) {
        return -1;
    }
    memset(f, 0, sizeof(*f));
    return 0;
}

static int vfs_storage_stat(const char* path, struct stat* st) {
    const struct MountEntry* e = find_mount_entry(path);
    if (!e) {
        return -1;
    }

    FsStorage* s = mount_storage(e->id);
    if (!s) {
        return -1;
    }

    const int rc = vfs_storage_internal_stat(s, path, st);
    unmount_storage(e->id);
    return rc;
}

static int vfs_storage_mkdir(const char* path) {
    return -1;
}

static int vfs_storage_unlink(const char* path) {
    return -1;
}

static int vfs_storage_rmdir(const char* path) {
    return -1;
}

static int vfs_storage_rename(const char* src, const char* dst) {
    return -1;
}

void vfs_storage_init(void) {
}

void vfs_storage_exit(void) {
    for (int i = 0; i < ARRAY_SIZE(g_storage_cache); i++) {
        struct StorageCacheEntry* entry = &g_storage_cache[i];
        if (entry->ref_count) {
            entry->ref_count = 0;
            fsStorageClose(&entry->s);
        }
    }

    memset(&g_storage_cache, 0, sizeof(g_storage_cache));
}

int vfs_storage_internal_open(FsStorage* s, struct VfsStorageFile* f, const char* path, enum FtpVfsOpenMode mode) {
    if (mode != FtpVfsOpenMode_READ) {
        return -1;
    }

    Result rc;
    if (R_FAILED(rc = fsStorageGetSize(s, &f->size))) {
        return -1;
    }

    f->s = *s;
    f->is_valid = true;
    f->off = 0;
    return 0;
}

int vfs_storage_internal_read(struct VfsStorageFile* f, void* buf, size_t size) {
    size = min(size, f->size - f->off);

    Result rc;
    if (R_FAILED(rc = fsStorageRead(&f->s, f->off, buf, size))) {
        return -1;
    }

    f->off += size;
    return size;
}

int vfs_storage_internal_seek(struct VfsStorageFile* f, size_t off) {
    f->off = off;
    return 0;
}

int vfs_storage_internal_close(struct VfsStorageFile* f) {
    f->is_valid = 0;
    return 0;
}

int vfs_storage_internal_isfile_open(struct VfsStorageFile* f) {
    return f->is_valid;
}

int vfs_storage_internal_stat(FsStorage* s, const char* path, struct stat* st) {
    Result rc;
    s64 size;
    if (R_FAILED(rc = fsStorageGetSize(s, &size))) {
        return -1;
    }

    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    st->st_size = size;
    st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    return 0;
}

int vfs_storage_internal_lstat(FsStorage* s, const char* path, struct stat* st) {
    return vfs_storage_internal_stat(s, path, st);
}

const FtpVfs g_vfs_storage = {
    .open = vfs_storage_open,
    .read = vfs_storage_read,
    .write = vfs_storage_write,
    .seek = vfs_storage_seek,
    .close = vfs_storage_close,
    .isfile_open = vfs_storage_isfile_open,
    .opendir = vfs_storage_opendir,
    .readdir = vfs_storage_readdir,
    .dirlstat = vfs_storage_dirlstat,
    .closedir = vfs_storage_closedir,
    .isdir_open = vfs_storage_isdir_open,
    .stat = vfs_storage_stat,
    .lstat = vfs_storage_stat,
    .mkdir = vfs_storage_mkdir,
    .unlink = vfs_storage_unlink,
    .rmdir = vfs_storage_rmdir,
    .rename = vfs_storage_rename,
};
