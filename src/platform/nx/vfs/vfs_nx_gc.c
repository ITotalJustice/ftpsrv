/**
 * Copyright 2024 TotalJustice.
 * SPDX-License-Identifier: MIT
 */

#include "ftpsrv_vfs.h"
#include "log/log.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static FsDeviceOperator g_dev;
static FsGameCardHandle g_handle;
static FsFileSystem g_fs;
static NcmContentMetaDatabase g_db;
static NcmContentStorage g_cs;
static NcmApplicationContentMetaKey g_app_key;
static u8 g_cert[0x200];

static bool g_mounted = false;

#define min(x, y) ((x) < (y) ? (x) : (y))

static bool gc_is_mounted(void) {
    Result rc;
    bool out;
    if (R_FAILED(rc = fsDeviceOperatorIsGameCardInserted(&g_dev, &out))) {
        return false;
    }
    return out;
}

static void gc_unmount(void) {
    if (g_mounted) {
        ncmContentMetaDatabaseClose(&g_db);
        ncmContentStorageClose(&g_cs);
        fsFsClose(&g_fs);
        g_mounted = false;
    }
}

static bool gc_mount(void) {
    gc_unmount();

    if (!gc_is_mounted()) {
        log_file_fwrite("no gc mounted\n");
        return false;
    }

    Result rc;
    if (R_FAILED(rc = fsDeviceOperatorGetGameCardHandle(&g_dev,&g_handle))) {
        log_file_fwrite("failed fsDeviceOperatorGetGameCardHandle(): 0x%X\n", rc);
        return false;
    }

    if (R_FAILED(rc = fsOpenGameCardFileSystem(&g_fs, &g_handle, FsGameCardPartition_Secure))) {
        log_file_fwrite("failed fsOpenGameCardFileSystem(): 0x%X\n", rc);
        return false;
    }

    s64 out_size;
    if (R_FAILED(rc = fsDeviceOperatorGetGameCardDeviceCertificate(&g_dev, &g_handle, g_cert, sizeof(g_cert), &out_size, sizeof(g_cert)))) {
        log_file_fwrite("failed fsDeviceOperatorGetGameCardDeviceCertificate(): 0x%X\n", rc);
        goto fail_close_fs;
    }

    if (R_FAILED(rc = ncmOpenContentMetaDatabase(&g_db, NcmStorageId_GameCard))) {
        log_file_fwrite("failed ncmOpenContentMetaDatabase(NcmStorageId_GameCard): 0x%X\n", rc);
        goto fail_close_fs;
    }

    if (R_FAILED(rc = ncmOpenContentStorage(&g_cs, NcmStorageId_GameCard))) {
        log_file_fwrite("failed ncmOpenContentStorage(NcmStorageId_GameCard): 0x%X\n", rc);
        goto fail_close_ncm_d;
    }

    s32 entries_total;
    s32 entries_written;
    if (R_FAILED(rc = ncmContentMetaDatabaseListApplication(&g_db, &entries_total, &entries_written, &g_app_key, 1, NcmContentMetaType_Application))) {
        log_file_fwrite("failed ncmContentMetaDatabaseListApplication(NcmStorageId_GameCard): 0x%X\n", rc);
        goto fail_close_ncm_c;
    }

    if (entries_written <= 0) {
        log_file_fwrite("failed entries_written <= 0 (NcmStorageId_GameCard): 0x%X\n", rc);
        goto fail_close_ncm_c;
    }

    log_file_fwrite("gamecard is mounted\n");
    return g_mounted = true;

fail_close_ncm_c:
    ncmContentStorageClose(&g_cs);
fail_close_ncm_d:
    ncmContentMetaDatabaseClose(&g_db);
fail_close_fs:
    fsFsClose(&g_fs);
    return false;
}

static enum GcDirType get_type(const char* path) {
    if (!strcmp(path, "gc:")) {
        return GcDirType_Root;
    } else if (!strncmp(path, "gc:/", strlen("gc:/"))) {
        const char* dilem = strrchr(path, '[');
        if (!dilem) {
            return GcDirType_Invalid;
        }
        dilem++;

        char* end;
        const u64 app_id = strtoull(dilem, &end, 0x10);
        if (end == dilem || app_id != g_app_key.application_id) {
            return GcDirType_Invalid;
        }

        if (end[0] != ']') {
            return GcDirType_Invalid;
        }

        if (!strcmp(end, "].certificate")) {
            return GcDirType_Cert;
        } else {
            return GcDirType_App;
        }
    }

    return GcDirType_Invalid;
}

static void build_native_path(char out[FS_MAX_PATH], const char* path) {
    const char* dilem = strchr(path, ']');

    if (dilem && strlen(dilem + 1)) {
        snprintf(out, FS_MAX_PATH, "%s", dilem + 1);
    } else {
        strcpy(out, "/");
    }
}

// returns true if mounted.
static bool gc_poll(void) {
    if (g_mounted) {
        if (!gc_is_mounted()) {
            gc_unmount();
            return false;
        } else {
            FsGameCardHandle h;
            if (R_FAILED(fsDeviceOperatorGetGameCardHandle(&g_dev, &h))) {
                gc_unmount();
                return false;
            }

            if (h.value != g_handle.value) {
                return gc_mount();
            }

            return true;
        }
    } else {
        log_file_fwrite("attempting to mount\n");
        return gc_mount();
    }
}

static int vfs_gc_open(void* user, const char* path, enum FtpVfsOpenMode mode) {
    if (!gc_poll()) {
        return -1;
    }

    if (mode != FtpVfsOpenMode_READ) {
        return -1;
    }

    struct VfsGcFile* f = user;
    f->type = get_type(path);
    switch (f->type) {
        default: return -1;

        case GcDirType_Cert:
            f->raw.ptr = g_cert;
            f->raw.size = sizeof(g_cert);
            f->raw.offset = 0;
            break;

        case GcDirType_App: {
            char nxpath[FS_MAX_PATH];
            build_native_path(nxpath, path);
            if (vfs_fs_internal_open(&g_fs, &f->fs_file, nxpath, mode)) {
                return -1;
            }
        }   break;
    }

    f->is_valid = 1;
    return 0;
}

static int vfs_gc_read(void* user, void* buf, size_t size) {
    struct VfsGcFile* f = user;
    switch (f->type) {
        default: return -1;

        case GcDirType_Cert:
            size = min(size, f->raw.size - f->raw.offset);
            memcpy(buf, g_cert + f->raw.offset, size);
            f->raw.offset += size;
            return size;

        case GcDirType_App:
            return vfs_fs_internal_read(&f->fs_file, buf, size);
    }
}

static int vfs_gc_write(void* user, const void* buf, size_t size) {
    return -1;
}

static int vfs_gc_seek(void* user, const void* buf, size_t size, size_t off) {
    struct VfsGcFile* f = user;
    switch (f->type) {
        default: return -1;

        case GcDirType_Cert:
            if (off >= f->raw.size) {
                return -1;
            }
            f->raw.offset = off;
            return 0;

        case GcDirType_App:
            return vfs_fs_internal_seek(&f->fs_file, off);
    }
}

static int vfs_gc_isfile_open(void* user) {
    struct VfsGcFile* f = user;
    return f->is_valid;
}

static int vfs_gc_close(void* user) {
    struct VfsGcFile* f = user;
    if (!vfs_gc_isfile_open(f)) {
        return -1;
    }

    switch (f->type) {
        default: break;

        case GcDirType_App:
            vfs_fs_internal_close(&f->fs_file);
            break;
    }

    f->is_valid = 0;
    return 0;
}

static int vfs_gc_opendir(void* user, const char* path) {
    if (!gc_poll()) {
        return -1;
    }

    struct VfsGcDir* f = user;
    f->type = get_type(path);
    switch (f->type) {
        default: return -1;

        case GcDirType_Root:
            break;

        case GcDirType_App: {
            char nxpath[FS_MAX_PATH];
            build_native_path(nxpath, path);
            if (vfs_fs_internal_opendir(&g_fs, &f->fs_dir, nxpath)) {
                return -1;
            }
        }   break;
    }

    f->index = 0;
    f->is_valid = 1;
    return 0;
}

static const char* vfs_gc_readdir(void* user, void* user_entry) {
    struct VfsGcDir* f = user;
    struct VfsGcDirEntry* entry = user_entry;

    switch (f->type) {
        default: return NULL;

        case GcDirType_Root: {
            if (f->index >= 2) {
                return NULL;
            }

            Result rc;
            NcmContentId id;
            struct AppName name;
            const char* ext = f->index ? ".certificate" : "";
            if (R_FAILED(rc = get_app_name2(g_app_key.application_id, &g_db, &g_cs, &id, &name))) {
                snprintf(entry->name, sizeof(entry->name), "[%016lX]%s", g_app_key.application_id, ext);
            } else {
                snprintf(entry->name, sizeof(entry->name), "%s [%016lX]%s", name.str, g_app_key.application_id, ext);
            }
            f->index++;
            return entry->name;
        }

        case GcDirType_App: {
            return vfs_fs_internal_readdir(&f->fs_dir, &entry->fs_entry);
        }
    }
}

static int vfs_gc_dirlstat(void* user, const void* user_entry, const char* path, struct stat* st) {
    struct VfsGcDir* f = user;
    const struct VfsGcDirEntry* entry = user_entry;
    memset(st, 0, sizeof(*st));

    switch (f->type) {
        default: return -1;

        case GcDirType_Root: {
            st->st_nlink = 1;
            if (strstr(path, ".certificate")) {
                st->st_size = sizeof(g_cert);
                st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
            } else {
                st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
            }
            return 0;
        }

        case GcDirType_App: {
            char nxpath[FS_MAX_PATH];
            build_native_path(nxpath, path);
            return vfs_fs_internal_dirlstat(&g_fs, &f->fs_dir, &entry->fs_entry, nxpath, st);
        }
    }
}

static int vfs_gc_isdir_open(void* user) {
    struct VfsGcDir* f = user;
    return f->is_valid;
}

static int vfs_gc_closedir(void* user) {
    struct VfsGcDir* f = user;
    if (!vfs_gc_isdir_open(f)) {
        return -1;
    }

    switch (f->type) {
        default: break;

        case GcDirType_App:
            vfs_fs_internal_closedir(&f->fs_dir);;
            break;
    }

    f->is_valid = 0;
    return 0;
}

static int vfs_gc_stat(const char* path, struct stat* st) {
    if (!gc_poll()) {
        return -1;
    }

    memset(st, 0, sizeof(*st));
    const enum GcDirType type = get_type(path);

    switch (type) {
        default: return -1;

        case GcDirType_App:
            if (strstr(path, "]/")) {
                char nxpath[FS_MAX_PATH];
                build_native_path(nxpath, path);
                return vfs_fs_internal_stat(&g_fs, nxpath, st);
            }
        case GcDirType_Root:
            st->st_nlink = 1;
            st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
            return 0;

        case GcDirType_Cert:
            st->st_nlink = 1;
            st->st_size = sizeof(g_cert);
            st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
            return 0;

    }
}

static int vfs_gc_mkdir(const char* path) {
    return -1;
}

static int vfs_gc_unlink(const char* path) {
    return -1;
}

static int vfs_gc_rmdir(const char* path) {
    return -1;
}

static int vfs_gc_rename(const char* src, const char* dst) {
    return -1;
}

Result vfs_gc_init(void) {
    return fsOpenDeviceOperator(&g_dev);
}

void vfs_gc_exit(void) {
    gc_unmount();
    fsOpenDeviceOperator(&g_dev);
}

const FtpVfs g_vfs_gc = {
    .open = vfs_gc_open,
    .read = vfs_gc_read,
    .write = vfs_gc_write,
    .seek = vfs_gc_seek,
    .close = vfs_gc_close,
    .isfile_open = vfs_gc_isfile_open,
    .opendir = vfs_gc_opendir,
    .readdir = vfs_gc_readdir,
    .dirlstat = vfs_gc_dirlstat,
    .closedir = vfs_gc_closedir,
    .isdir_open = vfs_gc_isdir_open,
    .stat = vfs_gc_stat,
    .lstat = vfs_gc_stat,
    .mkdir = vfs_gc_mkdir,
    .unlink = vfs_gc_unlink,
    .rmdir = vfs_gc_rmdir,
    .rename = vfs_gc_rename,
};
