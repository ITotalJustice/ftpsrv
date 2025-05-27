/**
 * Copyright 2024 TotalJustice.
 * SPDX-License-Identifier: MIT
 */

#include "ftpsrv_vfs.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>

static const struct VfsDeviceEntry* g_entries;
static const u32* g_count;

static int vfs_root_open(void* user, const char* path, enum FtpVfsOpenMode mode) {
    return -1;
}

static int vfs_root_read(void* user, void* buf, size_t size) {
    return -1;
}

static int vfs_root_write(void* user, const void* buf, size_t size) {
    return -1;
}

static int vfs_root_seek(void* user, const void* buf, size_t size, size_t off) {
    return -1;
}

static int vfs_root_isfile_open(void* user) {
    return -1;
}

static int vfs_root_close(void* user) {
    return -1;
}

static int vfs_root_opendir(void* user, const char* path) {
    struct VfsRootDir* f = user;
    f->index = 0;
    f->is_valid = 1;
    return 0;
}

static const char* vfs_root_readdir(void* user, void* user_entry) {
    struct VfsRootDir* f = user;
    if (f->index < *g_count) {
        return g_entries[f->index++].name;
    } else {
        return NULL;
    }
}

static int vfs_root_dirlstat(void* user, const void* user_entry, const char* path, struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    st->st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
    return 0;
}

static int vfs_root_isdir_open(void* user) {
    struct VfsRootDir* f = user;
    return f->is_valid;
}

static int vfs_root_closedir(void* user) {
    struct VfsRootDir* f = user;
    if (!vfs_root_isdir_open(f)) {
        return -1;
    }
    memset(f, 0, sizeof(*f));
    return 0;
}

static int vfs_root_stat(const char* path, struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    st->st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
    return 0;
}

static int vfs_root_mkdir(const char* path) {
    return -1;
}

static int vfs_root_unlink(const char* path) {
    return -1;
}

static int vfs_root_rmdir(const char* path) {
    return -1;
}

static int vfs_root_rename(const char* src, const char* dst) {
    return -1;
}

void vfs_root_init(const struct VfsDeviceEntry* entries, const u32* count) {
    g_entries = entries;
    g_count = count;
}

void vfs_root_exit(void) {
    g_entries = NULL;
    g_count = 0;
}

const FtpVfs g_vfs_root = {
    .open = vfs_root_open,
    .read = vfs_root_read,
    .write = vfs_root_write,
    .seek = vfs_root_seek,
    .close = vfs_root_close,
    .isfile_open = vfs_root_isfile_open,
    .opendir = vfs_root_opendir,
    .readdir = vfs_root_readdir,
    .dirlstat = vfs_root_dirlstat,
    .closedir = vfs_root_closedir,
    .isdir_open = vfs_root_isdir_open,
    .stat = vfs_root_stat,
    .lstat = vfs_root_stat,
    .mkdir = vfs_root_mkdir,
    .unlink = vfs_root_unlink,
    .rmdir = vfs_root_rmdir,
    .rename = vfs_root_rename,
};
