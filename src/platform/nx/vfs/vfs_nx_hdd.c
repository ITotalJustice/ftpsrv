/**
 * Copyright 2024 TotalJustice.
 * SPDX-License-Identifier: MIT
 */

#include "ftpsrv_vfs.h"
#include "vfs_nx_hdd.h"
#include "log/log.h"
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <usbhsfs.h>

static UsbHsFsDevice g_device[0x20];
static s32 g_count;

static const char* fix_path(const char* path) {
    return path + strlen("hdd:/");
}

static void poll_usbhsfs(void) {
    g_count = usbHsFsListMountedDevices(g_device, sizeof(g_device)/sizeof(g_device[0]));
}

static int vfs_hdd_open(void* user, const char* path, enum FtpVfsOpenMode mode) {
    poll_usbhsfs();

    struct VfsHddFile* f = user;
    path = fix_path(path);
    if (strncmp(path, "ums", strlen("ums"))) {
        return -1;
    }
    return vfs_stdio_internal_open(&f->stdio_file, path, mode);
}

static int vfs_hdd_read(void* user, void* buf, size_t size) {
    struct VfsHddFile* f = user;
    return vfs_stdio_internal_read(&f->stdio_file, buf, size);
}

static int vfs_hdd_write(void* user, const void* buf, size_t size) {
    struct VfsHddFile* f = user;
    return vfs_stdio_internal_write(&f->stdio_file, buf, size);
}

static int vfs_hdd_seek(void* user, const void* buf, size_t size, size_t off) {
    struct VfsHddFile* f = user;
    return vfs_stdio_internal_seek(&f->stdio_file, off);
}

static int vfs_hdd_isfile_open(void* user) {
    struct VfsHddFile* f = user;
    return vfs_stdio_internal_isfile_open(&f->stdio_file);
}

static int vfs_hdd_close(void* user) {
    struct VfsHddFile* f = user;
    return vfs_stdio_internal_close(&f->stdio_file);
}

static int vfs_hdd_opendir(void* user, const char* path) {
    poll_usbhsfs();
    struct VfsHddDir* f = user;
    path = fix_path(path);
    if (!strncmp(path, "ums", strlen("ums"))) {
        if (vfs_stdio_internal_opendir(&f->stdio_dir, path)) {
            return -1;
        }
    }

    f->index = 0;
    f->is_valid = 1;
    return 0;
}

static const char* vfs_hdd_readdir(void* user, void* user_entry) {
    struct VfsHddDir* f = user;
    struct VfsHddDirEntry* entry = user_entry;

    if (vfs_stdio_internal_isdir_open(&f->stdio_dir)) {
        return vfs_stdio_internal_readdir(&f->stdio_dir, &entry->stdio_dir);
    } else {
        if (f->index >= g_count) {
            return NULL;
        }
        return g_device[f->index++].name;
    }
}

static int vfs_hdd_dirlstat(void* user, const void* user_entry, const char* path, struct stat* st) {
    struct VfsHddDir* f = user;
    const struct VfsHddDirEntry* entry = user_entry;
    path = fix_path(path);
    if (vfs_stdio_internal_isdir_open(&f->stdio_dir)) {
        return vfs_stdio_internal_dirlstat(&f->stdio_dir, &entry->stdio_dir, path, st);
    } else {
        memset(st, 0, sizeof(*st));
        st->st_nlink = 1;
        st->st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
        return 0;
    }
}

static int vfs_hdd_isdir_open(void* user) {
    struct VfsHddDir* f = user;
    return f->is_valid;
}

static int vfs_hdd_closedir(void* user) {
    struct VfsHddDir* f = user;
    if (!vfs_hdd_isdir_open(f)) {
        return -1;
    }
    if (vfs_stdio_internal_isdir_open(&f->stdio_dir)) {
        vfs_stdio_internal_closedir(&f->stdio_dir);
    }
    memset(f, 0, sizeof(*f));
    return 0;
}

static int vfs_hdd_stat(const char* path, struct stat* st) {
    poll_usbhsfs();
    path = fix_path(path);
    if (strncmp(path, "ums", strlen("ums"))) {
        return -1;
    }

    if (strlen(path) == 5) {
        memset(st, 0, sizeof(*st));
        st->st_nlink = 1;
        st->st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
        return 0;
    } else {
        return vfs_stdio_internal_stat(path, st);
    }
}

static int vfs_hdd_mkdir(const char* path) {
    poll_usbhsfs();
    path = fix_path(path);
    if (strlen(path) <= 5 || strncmp(path, "ums", strlen("ums"))) {
        return -1;
    }
    return vfs_stdio_internal_mkdir(path);
}

static int vfs_hdd_unlink(const char* path) {
    poll_usbhsfs();
    path = fix_path(path);
    if (strlen(path) <= 5 || strncmp(path, "ums", strlen("ums"))) {
        return -1;
    }
    return vfs_stdio_internal_unlink(path);
}

static int vfs_hdd_rmdir(const char* path) {
    poll_usbhsfs();
    path = fix_path(path);
    if (strlen(path) <= 5 || strncmp(path, "ums", strlen("ums"))) {
        return -1;
    }
    return vfs_stdio_internal_rmdir(path);
}

static int vfs_hdd_rename(const char* src, const char* dst) {
    poll_usbhsfs();
    const char* path_src = fix_path(src);
    const char* path_dst = fix_path(dst);
    if (strlen(path_src) <= 5 || strncmp(path_src, "ums", strlen("ums"))) {
        return -1;
    }

    if (strncmp(path_src, path_dst, 6)) {
        return -1;
    }

    return vfs_stdio_internal_rename(path_src, path_dst);
}

Result vfs_hdd_init(void) {
    return usbHsFsInitialize(0);
}

void vfs_hdd_exit(void) {
    usbHsFsExit();
}

const FtpVfs g_vfs_hdd = {
    .open = vfs_hdd_open,
    .read = vfs_hdd_read,
    .write = vfs_hdd_write,
    .seek = vfs_hdd_seek,
    .close = vfs_hdd_close,
    .isfile_open = vfs_hdd_isfile_open,
    .opendir = vfs_hdd_opendir,
    .readdir = vfs_hdd_readdir,
    .dirlstat = vfs_hdd_dirlstat,
    .closedir = vfs_hdd_closedir,
    .isdir_open = vfs_hdd_isdir_open,
    .stat = vfs_hdd_stat,
    .lstat = vfs_hdd_stat,
    .mkdir = vfs_hdd_mkdir,
    .unlink = vfs_hdd_unlink,
    .rmdir = vfs_hdd_rmdir,
    .rename = vfs_hdd_rename,
};
