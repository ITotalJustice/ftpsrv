// Copyright 2024 TotalJustice.
// SPDX-License-Identifier: MIT
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/stat.h>
#include <switch.h>

struct VfsFsFile {
    FsFile fd;
    s64 off;
    s64 chunk_size;
    bool is_write;
    bool is_valid;
#if VFS_NX_BUFFER_IO
    s64 buf_off;
    s64 buf_size;
    u8 buf[1024*1024*1];
#endif
};

struct VfsFsDir {
    FsDir dir;
    bool is_valid;
};

struct VfsFsDirEntry {
    FsDirectoryEntry buf;
};

int vfs_fs_set_errno(Result rc);
int vfs_fs_internal_open(FsFileSystem* fs, struct VfsFsFile* f, const char nxpath[static FS_MAX_PATH], enum FtpVfsOpenMode mode);
int vfs_fs_internal_read(struct VfsFsFile* f, void* buf, size_t size);
int vfs_fs_internal_write(struct VfsFsFile* f, const void* buf, size_t size);
int vfs_fs_internal_seek(struct VfsFsFile* f, size_t off);
int vfs_fs_internal_close(struct VfsFsFile* f);
int vfs_fs_internal_isfile_open(struct VfsFsFile* f);

int vfs_fs_internal_opendir(FsFileSystem* fs, struct VfsFsDir* f, const char nxpath[static FS_MAX_PATH]);
const char* vfs_fs_internal_readdir(struct VfsFsDir* f, struct VfsFsDirEntry* entry);
int vfs_fs_internal_dirlstat(FsFileSystem* fs, struct VfsFsDir* f, const struct VfsFsDirEntry* entry, const char nxpath[static FS_MAX_PATH], struct stat* st);
int vfs_fs_internal_closedir(struct VfsFsDir* f);
int vfs_fs_internal_isdir_open(struct VfsFsDir* f);

int vfs_fs_internal_stat(FsFileSystem* fs, const char nxpath[static FS_MAX_PATH], struct stat* st);
int vfs_fs_internal_lstat(FsFileSystem* fs, const char nxpath[static FS_MAX_PATH], struct stat* st);
int vfs_fs_internal_mkdir(FsFileSystem* fs, const char nxpath[static FS_MAX_PATH]);
int vfs_fs_internal_unlink(FsFileSystem* fs, const char nxpath[static FS_MAX_PATH]);
int vfs_fs_internal_rmdir(FsFileSystem* fs, const char nxpath[static FS_MAX_PATH]);
int vfs_fs_internal_rename(FsFileSystem* fs, const char nxpath_src[static FS_MAX_PATH], const char nxpath_dst[static FS_MAX_PATH]);

struct FtpVfs;
const extern struct FtpVfs g_vfs_fs;

#ifdef __cplusplus
}
#endif
