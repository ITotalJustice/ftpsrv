/**
 * Copyright 2024 TotalJustice.
 * SPDX-License-Identifier: MIT
 */

#include "ftpsrv_vfs.h"
#include "log/log.h"
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static void build_native_path(char out[FS_MAX_PATH], const char* path) {
    const char* dilem = strchr(path, ':');

    if (dilem && strlen(dilem + 1)) {
        strcpy(out, path);
    } else {
        snprintf(out, FS_MAX_PATH, "%s/", path);
    }
}

static int vfs_stdio_open(void* user, const char* path, enum FtpVfsOpenMode mode) {
    struct VfsStdioFile* f = user;
    char nxpath[FS_MAX_PATH];
    build_native_path(nxpath, path);
    return vfs_stdio_internal_open(f, nxpath, mode);
}

static int vfs_stdio_read(void* user, void* buf, size_t size) {
    struct VfsStdioFile* f = user;
    return vfs_stdio_internal_read(f, buf, size);
}

static int vfs_stdio_write(void* user, const void* buf, size_t size) {
    struct VfsStdioFile* f = user;
    return vfs_stdio_internal_write(f, buf, size);
}

static int vfs_stdio_seek(void* user, const void* buf, size_t size, size_t off) {
    struct VfsStdioFile* f = user;
    return vfs_stdio_internal_seek(f, off);
}

static int vfs_stdio_isfile_open(void* user) {
    struct VfsStdioFile* f = user;
    return vfs_stdio_internal_isfile_open(f);
}

static int vfs_stdio_close(void* user) {
    struct VfsStdioFile* f = user;
    return vfs_stdio_internal_close(f);
}

static int vfs_stdio_opendir(void* user, const char* path) {
    struct VfsStdioDir* f = user;
    char nxpath[FS_MAX_PATH];
    build_native_path(nxpath, path);
    return vfs_stdio_internal_opendir(f, nxpath);
}

static const char* vfs_stdio_readdir(void* user, void* user_entry) {
    struct VfsStdioDir* f = user;
    struct VfsStdioDirEntry* entry = user_entry;
    return vfs_stdio_internal_readdir(f, entry);
}

static int vfs_stdio_dirlstat(void* user, const void* user_entry, const char* path, struct stat* st) {
    struct VfsStdioDir* f = user;
    const struct VfsStdioDirEntry* entry = user_entry;
    char nxpath[FS_MAX_PATH];
    build_native_path(nxpath, path);
    return vfs_stdio_internal_dirlstat(f, entry, nxpath, st);
}

static int vfs_stdio_isdir_open(void* user) {
    struct VfsStdioDir* f = user;
    return vfs_stdio_internal_isdir_open(f);
}

static int vfs_stdio_closedir(void* user) {
    struct VfsStdioDir* f = user;
    return vfs_stdio_internal_closedir(f);
}

static int vfs_stdio_stat(const char* path, struct stat* st) {
    char nxpath[FS_MAX_PATH];
    build_native_path(nxpath, path);
    return vfs_stdio_internal_stat(nxpath, st);
}

static int vfs_stdio_mkdir(const char* path) {
    char nxpath[FS_MAX_PATH];
    build_native_path(nxpath, path);
    return vfs_stdio_internal_mkdir(nxpath);
}

static int vfs_stdio_unlink(const char* path) {
    char nxpath[FS_MAX_PATH];
    build_native_path(nxpath, path);
    return vfs_stdio_internal_unlink(nxpath);
}

static int vfs_stdio_rmdir(const char* path) {
    char nxpath[FS_MAX_PATH];
    build_native_path(nxpath, path);
    return vfs_stdio_internal_rmdir(nxpath);
}

static int vfs_stdio_rename(const char* src, const char* dst) {
    char nxpath_src[FS_MAX_PATH];
    char nxpath_dst[FS_MAX_PATH];
    build_native_path(nxpath_src, src);
    build_native_path(nxpath_dst, dst);
    return vfs_stdio_internal_rename(nxpath_src, nxpath_dst);
}

int vfs_stdio_internal_open(struct VfsStdioFile* f, const char* path, enum FtpVfsOpenMode mode) {
    int flags = 0, args = 0;

    switch (mode) {
        case FtpVfsOpenMode_READ:
            flags = O_RDONLY;
            args = 0;
            break;
        case FtpVfsOpenMode_WRITE:
            flags = O_WRONLY | O_CREAT | O_TRUNC;
            args = 0666;
            break;
        case FtpVfsOpenMode_APPEND:
            flags = O_WRONLY | O_CREAT | O_APPEND;
            args = 0666;
            break;
    }

    f->fd = open(path, flags, args);
    if (f->fd >= 0) {
        f->valid = 1;
    }
    return f->fd;
}

int vfs_stdio_internal_read(struct VfsStdioFile* f, void* buf, size_t size) {
    return read(f->fd, buf, size);
}

int vfs_stdio_internal_write(struct VfsStdioFile* f, const void* buf, size_t size) {
    return write(f->fd, buf, size);
}

int vfs_stdio_internal_seek(struct VfsStdioFile* f, size_t off) {
    return lseek(f->fd, off, SEEK_SET);
}

int vfs_stdio_internal_isfile_open(struct VfsStdioFile* f) {
    return f->valid && f->fd >= 0;
}

int vfs_stdio_internal_close(struct VfsStdioFile* f) {
    if (!vfs_stdio_isfile_open(f)) {
        return -1;
    }
    int rc = close(f->fd);
    f->fd = -1;
    f->valid = 0;
    return rc;
}

int vfs_stdio_internal_opendir(struct VfsStdioDir* f, const char* path) {
    f->fd = opendir(path);
    if (!f->fd) {
        return -1;
    }

    f->is_valid = 1;
    return 0;
}

const char* vfs_stdio_internal_readdir(struct VfsStdioDir* f, struct VfsStdioDirEntry* entry) {
    entry->d = readdir(f->fd);
    if (!entry->d) {
        return NULL;
    }

    return entry->d->d_name;
}

int vfs_stdio_internal_dirlstat(struct VfsStdioDir* f, const struct VfsStdioDirEntry* entry, const char* path, struct stat* st) {
    return lstat(path, st);
}

int vfs_stdio_internal_isdir_open(struct VfsStdioDir* f) {
    return f->is_valid;
}

int vfs_stdio_internal_closedir(struct VfsStdioDir* f) {
    if (!vfs_stdio_isdir_open(f)) {
        return -1;
    }
    if (f->fd) {
        closedir(f->fd);
    }
    memset(f, 0, sizeof(*f));
    return 0;
}

int vfs_stdio_internal_stat(const char* path, struct stat* st) {
    return stat(path, st);
}

int vfs_stdio_internal_mkdir(const char* path) {
    return mkdir(path, 0777);
}

int vfs_stdio_internal_unlink(const char* path) {
    return unlink(path);
}

int vfs_stdio_internal_rmdir(const char* path) {
    return rmdir(path);
}

int vfs_stdio_internal_rename(const char* src, const char* dst) {
    const char* dilem_src = strchr(src, ':');
    const char* dilem_dst = strchr(dst, ':');
    if (!dilem_src || !dilem_dst || dilem_src - src != dilem_dst - dst || strncmp(src, dst, dilem_dst - dst)) {
        return -1;
    }
    return rename(src, dst);
}

const FtpVfs g_vfs_stdio = {
    .open = vfs_stdio_open,
    .read = vfs_stdio_read,
    .write = vfs_stdio_write,
    .seek = vfs_stdio_seek,
    .close = vfs_stdio_close,
    .isfile_open = vfs_stdio_isfile_open,
    .opendir = vfs_stdio_opendir,
    .readdir = vfs_stdio_readdir,
    .dirlstat = vfs_stdio_dirlstat,
    .closedir = vfs_stdio_closedir,
    .isdir_open = vfs_stdio_isdir_open,
    .stat = vfs_stdio_stat,
    .lstat = vfs_stdio_stat,
    .mkdir = vfs_stdio_mkdir,
    .unlink = vfs_stdio_unlink,
    .rmdir = vfs_stdio_rmdir,
    .rename = vfs_stdio_rename,
};
