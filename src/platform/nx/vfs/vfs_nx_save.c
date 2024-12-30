/**
 * Copyright 2024 TotalJustice.
 * SPDX-License-Identifier: MIT
 */

#include "ftpsrv_vfs.h"
#include "../utils.h"
#include "log/log.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define min(x, y) ((x) < (y) ? (x) : (y))

#define LOCAL_HEADER_SIG 0x4034B50
#define FILE_HEADER_SIG 0x2014B50
#define DATA_DESCRIPTOR_SIG 0x8074B50
#define END_RECORD_SIG 0x6054B50

#pragma pack(push,1)
typedef struct mmz_LocalHeader {
    uint32_t sig;
    uint16_t version;
    uint16_t flags;
    uint16_t compression;
    uint16_t modtime;
    uint16_t moddate;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_len;
    uint16_t extrafield_len;
} mmz_LocalHeader;
#pragma pack(pop)

#pragma pack(push,1)
typedef struct mmz_DataDescriptor {
    uint32_t sig;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
} mmz_DataDescriptor;
#pragma pack(pop)

#pragma pack(push,1)
typedef struct mmz_FileHeader {
    uint32_t sig;
    uint16_t version;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t compression;
    uint16_t modtime;
    uint16_t moddate;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_len;
    uint16_t extrafield_len;
    uint16_t filecomment_len;
    uint16_t disk_start;
    uint16_t internal_attr;
    uint32_t external_attr;
    uint32_t local_hdr_off;
} mmz_FileHeader;
#pragma pack(pop)

#pragma pack(push,1)
typedef struct mmz_EndRecord {
    uint32_t sig;
    uint16_t disk_number;
    uint16_t disk_wcd;
    uint16_t disk_entries;
    uint16_t total_entries;
    uint32_t central_directory_size;
    uint32_t file_hdr_off;
    uint16_t comment_len;
} mmz_EndRecord;
#pragma pack(pop)

struct mmz_FileInfoBuffer {
    struct mmz_FileInfoMeta meta;
    char path[FS_MAX_PATH];
};

// struct mm
struct mmz_DataBuf {
    s64 fbuf_size; // internal
    FsDirectoryEntry entry;
    char path[FS_MAX_PATH];
    char path_temp[FS_MAX_PATH];
};

static u32 mmz_build_local_header(const struct mmz_Data* mz, struct mmz_LocalHeader* local) {
    memset(local, 0, sizeof(*local));
    local->sig = LOCAL_HEADER_SIG;
    local->flags = 1 << 3; // data descriptor
    local->filename_len = mz->meta.string_len;
    struct tm tm = {0};
    if (localtime_r(&mz->time, &tm)) {
        local->modtime = (tm.tm_sec) | ((tm.tm_min) << 5) | (tm.tm_hour << 11);
        local->moddate = (tm.tm_mday) | ((tm.tm_mon + 1) << 5) | ((tm.tm_year > 80 ? tm.tm_year - 80 : 0) << 9);
    }
    return sizeof(*local) + mz->meta.string_len;
}

static u32 mmz_build_file_header(const struct mmz_Data* mz, struct mmz_FileHeader* file) {
    memset(file, 0, sizeof(*file));
    file->sig = FILE_HEADER_SIG;
    file->version = 3 << 8; // UNIX
    file->flags = 1 << 3; // data descriptor
    file->crc32 = mz->meta.crc32;
    file->compressed_size = mz->meta.size;
    file->uncompressed_size = mz->meta.size;
    file->filename_len = mz->meta.string_len;
    file->local_hdr_off = mz->local_hdr_off;
    struct tm tm = {0};
    if (localtime_r(&mz->time, &tm)) {
        file->modtime = (tm.tm_sec) | ((tm.tm_min) << 5) | (tm.tm_hour << 11);
        file->moddate = (tm.tm_mday) | ((tm.tm_mon + 1) << 5) | ((tm.tm_year > 80 ? tm.tm_year - 80 : 0) << 9);
    }
    return sizeof(*file) + mz->meta.string_len;
}

static u32 mmz_build_data_descriptor(const struct mmz_Data* mz, struct mmz_DataDescriptor* desc) {
    memset(desc, 0, sizeof(*desc));
    desc->sig = DATA_DESCRIPTOR_SIG;
    desc->crc32 = mz->meta.crc32;
    desc->compressed_size = mz->meta.size;
    desc->uncompressed_size = mz->meta.size;
    return sizeof(*desc);
}

static u32 mmz_build_end_record(const struct mmz_Data* mz, struct mmz_EndRecord* rec) {
    memset(rec, 0, sizeof(*rec));
    rec->sig = END_RECORD_SIG;
    rec->disk_entries = mz->file_count;
    rec->total_entries = mz->file_count;
    rec->central_directory_size = mz->central_directory_size;
    rec->file_hdr_off = mz->local_hdr_off;
    return sizeof(*rec);
}

static Result mmz_add_file(struct mmz_Data* mz, struct mmz_DataBuf* db, const char* path) {
    // skip leading root path, zip paths are relative.
    if (path[0] == '/') {
        path++;
    }

    Result rc;
    struct mmz_FileInfoBuffer buf = {0};
    buf.meta.string_len = strlen(path);
    memcpy(buf.path, path, buf.meta.string_len);
    const size_t buf_size = sizeof(buf.meta) + buf.meta.string_len;

    if (db->fbuf_size - mz->fbuf_off < buf_size) {
        db->fbuf_size += 1024 * 64;
        if (R_FAILED(rc = fsFileSetSize(&mz->fbuf_out, db->fbuf_size))) {
            return rc;
        }
    }

    if (R_FAILED(rc = fsFileWrite(&mz->fbuf_out, mz->fbuf_off, &buf, buf_size, FsWriteOption_None))) {
        return rc;
    }

    mz->file_count++;
    mz->fbuf_off += buf_size;
    return rc;
}

static Result mmz_add_dir(struct mmz_Data* mz, struct mmz_DataBuf* db, const char* path) {
    Result rc;
    FsDir dir;
    snprintf(db->path, sizeof(db->path), path);

    if (R_FAILED(rc = fsFsOpenDirectory(mz->fs, db->path, FsDirOpenMode_ReadDirs|FsDirOpenMode_ReadFiles|FsDirOpenMode_NoFileSize, &dir))) {
        return rc;
    }

    if (!strcmp(db->path, "/")) {
        db->path[0] = '\0';
    }

    s64 total_entries;
    while ((R_SUCCEEDED(rc = fsDirRead(&dir, &total_entries, 1, &db->entry))) && total_entries == 1) {
        snprintf(db->path_temp, sizeof(db->path_temp), "%s/%s", db->path, db->entry.name);
        if (db->entry.type == FsDirEntryType_Dir) {
            if (R_FAILED(rc = mmz_add_dir(mz, db, db->path_temp))) {
                break;
            }
            strrchr(db->path, '/')[0] = '\0';
        } else {
            if (R_FAILED(rc = mmz_add_file(mz, db, db->path_temp))) {
                break;
            }
        }
    }

    fsDirClose(&dir);
    return rc;
}

static void mzz_build_temp_path(char* out, u64 app_id, AccountUid uid, u8 type) {
    snprintf(out, FS_MAX_PATH, "/~ftpsrv_mmzip_temp_%016lX_%016lX%016lX_%d", app_id, uid.uid[0], uid.uid[1], type);
}

static Result mmz_build_zip(struct mmz_Data* mz, FsFileSystem* save_fs, u64 app_id, AccountUid uid, u8 type) {
    memset(mz, 0, sizeof(*mz));
    struct mmz_DataBuf db = {0};
    mz->fs = save_fs;
    db.fbuf_size = 1024 * 64;

    FsFileSystem* sdmc_fs = fsdev_wrapGetDeviceFileSystem("sdmc");
    mzz_build_temp_path(db.path_temp, app_id, uid, type);
    fsFsDeleteFile(sdmc_fs, db.path_temp);

    Result rc;
    if (R_FAILED(rc = fsFsCreateFile(sdmc_fs, db.path_temp, db.fbuf_size, 0))) {
        return rc;
    }

    if (R_FAILED(rc = fsFsOpenFile(sdmc_fs, db.path_temp, FsOpenMode_Read|FsOpenMode_Write|FsOpenMode_Append, &mz->fbuf_out))) {
        goto end;
    }

    if (R_FAILED(rc = mmz_add_dir(mz, &db, "/"))) {
        goto end;
    }

    if (!mz->file_count) {
        rc = 0x339602;
        goto end;
    }

end:
    if (R_FAILED(rc)) {
        fsFileClose(&mz->fbuf_out);
        mzz_build_temp_path(db.path_temp, app_id, uid, type);
        fsFsDeleteFile(sdmc_fs, db.path_temp);
    } else {
        mz->fbuf_off = 0;
        mz->time = time(NULL);
    }

    return rc;
}

static Result mmz_read_buffer_info(struct mmz_Data* mz, struct mmz_FileInfoBuffer* buf) {
    Result rc;
    u64 bytes_read;
    if (R_FAILED(rc = fsFileRead(&mz->fbuf_out, mz->fbuf_off, &buf->meta, sizeof(buf->meta), 0, &bytes_read))) {
        return rc;
    }

    if (R_FAILED(rc = fsFileRead(&mz->fbuf_out, mz->fbuf_off + sizeof(buf->meta), buf->path, buf->meta.string_len, 0, &bytes_read))) {
        return rc;
    }

    buf->path[buf->meta.string_len] = 0;
    return rc;
}

static int mmz_read(struct mmz_Data* mz, void* buf, size_t size) {
    Result rc;
    if (mz->pending) {
        mz->off = 0;
        mz->pending = false;

        switch (mz->state) {
            case mmz_State_Local: {
                struct mmz_FileInfoBuffer info_buf;
                if (R_FAILED(rc = mmz_read_buffer_info(mz, &info_buf))) {
                    return vfs_fs_set_errno(rc);
                }

                char file_path[FS_MAX_PATH];
                snprintf(file_path, sizeof(file_path), "/%s", info_buf.path);
                if (R_FAILED(rc = fsFsOpenFile(mz->fs, file_path, FsOpenMode_Read, &mz->fin))) {
                    return vfs_fs_set_errno(rc);
                }

                s64 size;
                if (R_FAILED(rc = fsFileGetSize(&mz->fin, &size))) {
                    return vfs_fs_set_errno(rc);
                }

                mz->meta.size = size;
                mz->new_crc32 = 0;
                mz->state = mmz_State_Data;
            }   break;

            case mmz_State_Data: {
                fsFileClose(&mz->fin);

                // store crc32 and size
                mz->meta.crc32 = mz->new_crc32;
                if (R_FAILED(rc = fsFileWrite(&mz->fbuf_out, mz->fbuf_off, &mz->meta, sizeof(mz->meta), 0))) {
                    return vfs_fs_set_errno(rc);
                }

                mz->state = mmz_State_Descriptor;
            }   break;

            case mmz_State_Descriptor: {
                mz->index++;
                if (mz->index == mz->file_count) {
                    mz->fbuf_off = 0;
                    mz->index = 0;
                    mz->state = mmz_State_File;
                } else {
                    mz->fbuf_off += sizeof(mz->meta) + mz->meta.string_len;
                    mz->state = mmz_State_Local;
                }
            }   break;

            case mmz_State_File:
                mz->index++;
                mz->central_directory_size += sizeof(mmz_FileHeader) + mz->meta.string_len;
                mz->local_hdr_off += sizeof(mmz_LocalHeader) + mz->meta.string_len + mz->meta.size + sizeof(mmz_DataDescriptor);
                if (mz->index == mz->file_count) {
                    mz->state = mmz_State_End;
                } else {
                    mz->fbuf_off += sizeof(mz->meta) + mz->meta.string_len;
                }
                break;

            case mmz_State_End:
                return 0;
        }
    }

    u32 total_size = 0;

    switch (mz->state) {
        case mmz_State_Local: {
            struct mmz_FileInfoBuffer info_buf;
            if (R_FAILED(rc = mmz_read_buffer_info(mz, &info_buf))) {
                return vfs_fs_set_errno(rc);
            }

            mz->meta = info_buf.meta;
            struct mmz_LocalHeader local_hdr;
            total_size = mmz_build_local_header(mz, &local_hdr);

            if (mz->off < sizeof(local_hdr)) {
                size = min(size, sizeof(local_hdr) - mz->off);
                memcpy(buf, (u8*)&local_hdr + mz->off, size);
            } else {
                size = min(size, local_hdr.filename_len - (mz->off - sizeof(local_hdr)));
                memcpy(buf, info_buf.path + mz->off - sizeof(local_hdr), size);
            }
        }   break;

        case mmz_State_Data: {
            mz->meta.crc32 = mz->new_crc32;
            u64 bytes_read;
            total_size = mz->meta.size;
            size = min(size, total_size - mz->off);
            if (R_FAILED(rc = fsFileRead(&mz->fin, mz->off, buf, size, 0, &bytes_read))) {
                return vfs_fs_set_errno(rc);
            }

            mz->new_crc32 = crc32CalculateWithSeed(mz->meta.crc32, buf, size);
        }   break;

        case mmz_State_Descriptor: {
            mmz_DataDescriptor data_desc = {0};
            total_size = mmz_build_data_descriptor(mz, &data_desc);
            size = min(size, total_size - mz->off);
            memcpy(buf, (const u8*)&data_desc + mz->off, size);
        }   break;

        case mmz_State_File: {
            struct mmz_FileInfoBuffer info_buf;
            if (R_FAILED(rc = mmz_read_buffer_info(mz, &info_buf))) {
                return vfs_fs_set_errno(rc);
            }

            mz->meta = info_buf.meta;
            struct mmz_FileHeader file_hdr;
            total_size = mmz_build_file_header(mz, &file_hdr);

            if (mz->off < sizeof(file_hdr)) {
                size = min(size, sizeof(file_hdr) - mz->off);
                memcpy(buf, (const u8*)&file_hdr + mz->off, size);
            } else {
                size = min(size, file_hdr.filename_len - (mz->off - sizeof(file_hdr)));
                memcpy(buf, info_buf.path + mz->off - sizeof(file_hdr), size);
            }
        }   break;

        case mmz_State_End: {
            struct mmz_EndRecord end_rec;
            total_size = mmz_build_end_record(mz, &end_rec);
            size = min(size, total_size - mz->off);
            memcpy(buf, (const u8*)&end_rec + mz->off, size);
        }   break;
    }

    mz->off += size;
    mz->zip_off += size;

    if (mz->off == total_size) {
        mz->pending = true;
    }

    return size;
}

struct SaveAcc {
    AccountUid uid;
    char name[0x20];
};

// hos only allows save fs to be mounted once...
// to work around this, we keep a cache of 16 saves (plenty)
struct SaveCacheEntry {
    FsFileSystem fs;
    u64 app_id;
    AccountUid uid;
    FsSaveDataType type;
    u32 ref_count;
};

static struct SaveCacheEntry g_save_cache[16];
static struct SaveAcc g_acc_profile[12];
static s32 g_acc_count;
static bool g_writable;

// list of all characters that are invalid for fat,
// these are coverted to "_"
static const char INVALID_CHAR_TABLE[] = {
    '<',
    '>',
    ':',
    '"',
    '/',
    '\\',
    '|',
    '?',
    '*',
    '.',
    ',',
    ';',
    '+',
    '=',
    '&',
    '%', // probably invalid
};

static void make_zip_string_valid(char* str) {
    for (int i = 0; str[i]; i++) {
        const unsigned char c = str[i];
        const unsigned char c2 = str[i + 1];
        if (c < 0x20 || c >= 0x80) {
            if (c == 195 && c2 == 169) {
                str[i + 1] = 'e';
                memcpy(str + i, str + i + 1, strlen(str) - i);
            } else if (c == 226 && c2 == 128 && (unsigned char)str[i + 2] == 153) {
                str[i + 2] = '\'';
                memcpy(str + i, str + i + 2, strlen(str) - i);
            } else {
                str[i] = '_';
            }
        } else {
            for (int j = 0; j < ARRAY_SIZE(INVALID_CHAR_TABLE); j++) {
                if (c == INVALID_CHAR_TABLE[j]) {
                    // see what the next character is
                    if (str[i + 1] == '\0') {
                        str[i] = '\0';
                    } else if (str[i + 1] != ' ') {
                        str[i] = '_';
                    } else {
                        memcpy(str + i, str + i + 1, strlen(str) - i);
                    }
                    break;
                }
            }
        }
    }
}

static FsFileSystem* mount_save_fs(const struct SavePathData* d) {
    for (int i = 0; i < ARRAY_SIZE(g_save_cache); i++) {
        struct SaveCacheEntry* entry = &g_save_cache[i];
        if (entry->ref_count && entry->app_id == d->app_id && entry->type == d->data_type && !memcmp(&entry->uid, &d->uid, sizeof(d->uid))) {
            entry->ref_count++;
            return &entry->fs;
        }
    }

    // save is not currently mounted, find the next free slot
    for (int i = 0; i < ARRAY_SIZE(g_save_cache); i++) {
        struct SaveCacheEntry* entry = &g_save_cache[i];
        if (!entry->ref_count) {
            FsSaveDataAttribute attr = {0};
            attr.save_data_type = d->data_type;
            Result rc;

            if (d->data_type == FsSaveDataType_System) {
                attr.system_save_data_id = d->app_id;
                rc = fsOpenSaveDataFileSystemBySystemSaveDataId(&entry->fs, d->space_id, &attr);
            } else {
                attr.application_id = d->app_id;
                attr.uid = d->uid;
                if (g_writable) {
                    rc = fsOpenSaveDataFileSystem(&entry->fs, d->space_id, &attr);
                } else {
                    rc = fsOpenReadOnlySaveDataFileSystem(&entry->fs, d->space_id, &attr);
                }
            }

            if (R_FAILED(rc)) {
                vfs_fs_set_errno(rc);
                log_file_fwrite("failed: fsOpenReadOnlySaveDataFileSystem(%016lX) 0x%X\n", d->app_id, rc);
                return NULL;
            }

            entry->uid = d->uid;
            entry->app_id = d->app_id;
            entry->type = d->data_type;
            entry->ref_count++;
            return &entry->fs;
        }
    }

    return NULL;
}

static void unmount_save_fs(const struct SavePathData* d) {
    for (int i = 0; i < ARRAY_SIZE(g_save_cache); i++) {
        struct SaveCacheEntry* entry = &g_save_cache[i];
        if (entry->ref_count && entry->app_id == d->app_id && entry->type == d->data_type && !memcmp(&entry->uid, &d->uid, sizeof(d->uid))) {
            entry->ref_count--;
            if (!entry->ref_count) {
                if (g_writable) {
                    fsFsCommit(&entry->fs);
                }
                fsFsClose(&entry->fs);
            }
        }
    }
}

static struct SavePathData get_type(const char* path) {
    struct SavePathData data = {0};
    if (!strcmp(path, "save:")) {
        data.type = SaveDirType_Root;
    } else {
        const char* dilem = strchr(path, '[');
        data.space_id = FsSaveDataSpaceId_User;
        if (!strncmp(path, "save:/bcat", strlen("save:/bcat"))) {
            data.data_type = FsSaveDataType_Bcat;
            data.space_id = FsSaveDataSpaceId_User;
            data.type = SaveDirType_User1;
        } else if (!strncmp(path, "save:/cache", strlen("save:/cache"))) {
            data.data_type = FsSaveDataType_Cache;
            data.space_id = FsSaveDataSpaceId_SdUser;
            data.type = SaveDirType_User1;
        } else if (!strncmp(path, "save:/device", strlen("save:/device"))) {
            data.data_type = FsSaveDataType_Device;
            data.space_id = FsSaveDataSpaceId_User;
            data.type = SaveDirType_User1;
        } else if (!strncmp(path, "save:/system", strlen("save:/system"))) {
            data.data_type = FsSaveDataType_System;
            data.space_id = FsSaveDataSpaceId_System;
            data.type = SaveDirType_User1;
        } else if (dilem && strlen(dilem) >= 33) {
            dilem++;
            char uid_buf[2][17];
            snprintf(uid_buf[0], sizeof(uid_buf[0]), "%s", dilem);
            snprintf(uid_buf[1], sizeof(uid_buf[1]), "%s", dilem + 0x10);

            data.uid.uid[0] = strtoull(uid_buf[0], NULL, 0x10);
            data.uid.uid[1] = strtoull(uid_buf[1], NULL, 0x10);

            data.data_type = FsSaveDataType_Account;
            data.space_id = FsSaveDataSpaceId_User;
            data.type = SaveDirType_User1;
            dilem = strchr(dilem, '[');
        }

        if (data.type == SaveDirType_User1) {
            if (strstr(path, "/zips")) {
                data.type = SaveDirType_Zip;
            } else if (strstr(path, "/files")) {
                data.type = SaveDirType_File;
            }

            if (data.type == SaveDirType_File || data.type == SaveDirType_Zip) {
                // will need to correctly handle this, its good enough for now.
                if (dilem && strlen(dilem) >= 17) {
                    dilem++;
                    data.app_id = strtoull(dilem, NULL, 0x10);
                    data.type = data.type == SaveDirType_File ? SaveDirType_FileApp : SaveDirType_ZipApp;
                    dilem += 17;
                    data.path_off = dilem - path;
                }
            }
        }
    }

    return data;
}

static void build_native_path(char out[static FS_MAX_PATH], const char* path, const struct SavePathData* data) {
    if (strlen(path + data->path_off)) {
        snprintf(out, FS_MAX_PATH, "%s", path + data->path_off);
    } else {
        strcpy(out, "/");
    }
}

static int vfs_save_open(void* user, const char* path, enum FtpVfsOpenMode mode) {
    struct VfsSaveFile* f = user;
    f->data = get_type(path);
    if (mode != FtpVfsOpenMode_READ && (!g_writable || f->data.type != SaveDirType_ZipApp)) {
        errno = EROFS;
        return -1;
    }

    if (f->data.type != SaveDirType_FileApp && f->data.type != SaveDirType_ZipApp) {
        return -1;
    }

    FsFileSystem* fs = mount_save_fs(&f->data);
    if (!fs) {
        return -1;
    }

    char nxpath[FS_MAX_PATH];
    build_native_path(nxpath, path, &f->data);

    if (f->data.type == SaveDirType_FileApp) {
        if (vfs_fs_internal_open(fs, &f->fs_file, nxpath, mode)) {
            unmount_save_fs(&f->data);
            return -1;
        }

        f->fs = *fs;
        f->is_valid = 1;
        return 0;
    } else {
        Result rc;
        if (R_FAILED(rc = mmz_build_zip(&f->mz, fs, f->data.app_id, f->data.uid, f->data.space_id))) {
            unmount_save_fs(&f->data);
            return vfs_fs_set_errno(rc);
        }

        f->fs = *fs;
        f->is_valid = 1;
        return 0;
    }
}

static int vfs_save_read(void* user, void* buf, size_t size) {
    struct VfsSaveFile* f = user;
    if (f->data.type == SaveDirType_ZipApp) {
        return mmz_read(&f->mz, buf, size);
    } else {
        return vfs_fs_internal_read(&f->fs_file, buf, size);
    }
}

static int vfs_save_write(void* user, const void* buf, size_t size) {
    struct VfsSaveFile* f = user;
    if (!g_writable || f->data.type == SaveDirType_ZipApp) {
        errno = EROFS;
        return -1;
    }

    return vfs_fs_internal_write(&f->fs_file, buf, size);
}

static int vfs_save_seek(void* user, const void* buf, size_t size, size_t off) {
    struct VfsSaveFile* f = user;

    if (f->data.type == SaveDirType_ZipApp) {
        if (off > f->mz.zip_off) {
            errno = ESPIPE;
            return -1;
        }

        f->mz.new_crc32 = crc32CalculateWithSeed(f->mz.meta.crc32, buf, size);
        f->mz.pending = false;
        f->mz.off -= f->mz.zip_off - off;
        f->mz.zip_off = off;
        return 0;
    } else {
        return vfs_fs_internal_seek(&f->fs_file, off);
    }
}

static int vfs_save_isfile_open(void* user) {
    struct VfsSaveFile* f = user;
    return f->is_valid;
}

static int vfs_save_close(void* user) {
    struct VfsSaveFile* f = user;
    if (!vfs_save_isfile_open(f)) {
        return -1;
    }

    if (f->data.type == SaveDirType_FileApp) {
        vfs_fs_internal_close(&f->fs_file);
    } else {
        fsFileClose(&f->mz.fbuf_out);
        fsFileClose(&f->mz.fin);
        char nxpath[FS_MAX_PATH];
        mzz_build_temp_path(nxpath, f->data.app_id, f->data.uid, f->data.space_id);
        fsFsDeleteFile(fsdev_wrapGetDeviceFileSystem("sdmc"), nxpath);
    }

    unmount_save_fs(&f->data);
    f->is_valid = 0;
    return 0;
}

static int vfs_save_opendir(void* user, const char* path) {
    struct VfsSaveDir* f = user;
    f->data = get_type(path);

    switch (f->data.type) {
        default: return -1;

        case SaveDirType_Root:
        case SaveDirType_User1:
            break;

        case SaveDirType_File:
        case SaveDirType_Zip: {
            FsSaveDataFilter filter = {0};
            filter.filter_by_save_data_type = true;
            filter.attr.save_data_type = f->data.data_type;

            if (f->data.data_type == FsSaveDataType_Account) {
                filter.filter_by_user_id = true;
                filter.attr.uid = f->data.uid;
            }

            Result rc;
            if (R_FAILED(rc = fsOpenSaveDataInfoReaderWithFilter(&f->r, f->data.space_id, &filter))) {
                log_file_fwrite("failed: fsOpenSaveDataInfoReaderWithFilter() 0x%X\n", rc);
                return -1;
            }
        }   break;

        case SaveDirType_FileApp: {
            FsFileSystem* fs = mount_save_fs(&f->data);
            if (!fs) {
                return -1;
            }
            f->fs = *fs;

            char nxpath[FS_MAX_PATH] = {"/"};
            build_native_path(nxpath, path, &f->data);
            if (vfs_fs_internal_opendir(&f->fs, &f->fs_dir, nxpath)) {
                unmount_save_fs(&f->data);
                return -1;
            }
        }   break;
    }

    f->index = 0;
    f->is_valid = 1;
    return 0;
}

static const char* vfs_save_readdir(void* user, void* user_entry) {
    struct VfsSaveDir* f = user;
    struct VfsSaveDirEntry* entry = user_entry;

    Result rc;
    switch (f->data.type) {
        default: return NULL;

        case SaveDirType_Root: {
            if (f->index >= g_acc_count) {
                return NULL;
            }
            const struct SaveAcc* p = &g_acc_profile[f->index];
            if (!accountUidIsValid(&p->uid)) {
                snprintf(entry->name, sizeof(entry->name), "%s", p->name);
            } else {
                snprintf(entry->name, sizeof(entry->name), "%s [%016lX%016lX]", p->name, p->uid.uid[0], p->uid.uid[1]);
            }
            f->index++;
            return entry->name;
        }

        case SaveDirType_User1: {
            static const char* e[] = { "files","zips" };
            if (f->index >= sizeof(e)/sizeof(e[0])) {
                return NULL;
            }
            return e[f->index++];
        }

        case SaveDirType_File:
        case SaveDirType_Zip: {
            s64 total;
            if (R_FAILED(rc = fsSaveDataInfoReaderRead(&f->r, &entry->info, 1, &total))) {
                log_file_fwrite("failed: fsSaveDataInfoReaderRead() 0x%X\n", rc);
                return NULL;
            }

            if (total <= 0) {
                log_file_fwrite("fsSaveDataInfoReaderRead() no more entries %zd\n", total);
                return NULL;
            }

            // this can fail if the game is no longer installed.
            NcmContentId id;
            struct AppName name;
            const char* ext = f->data.type == SaveDirType_File ? "" : ".zip";
            if (entry->info.save_data_type == FsSaveDataType_System || entry->info.save_data_type == FsSaveDataType_SystemBcat) {
                snprintf(entry->name, sizeof(entry->name), "[%016lX]%s", entry->info.system_save_data_id, ext);
            } else if (R_FAILED(rc = get_app_name(entry->info.application_id, &id, &name))) {
                snprintf(entry->name, sizeof(entry->name), "[%016lX]%s", entry->info.application_id, ext);
            } else {
                if (f->data.type == SaveDirType_Zip) {
                    make_zip_string_valid(name.str);
                }
                snprintf(entry->name, sizeof(entry->name), "%s [%016lX]%s", name.str, entry->info.application_id, ext);
            }

            log_file_fwrite("read entry %s data: %s space: %s %u index: %u rank %u\n", name.str, entry->info.save_data_index, entry->info.save_data_rank);
            return entry->name;
        }

        case SaveDirType_FileApp: {
            return vfs_fs_internal_readdir(&f->fs_dir, &entry->fs_buf);
        }
    }
}

static int vfs_save_dirlstat(void* user, const void* user_entry, const char* path, struct stat* st) {
    struct VfsSaveDir* f = user;
    const struct VfsSaveDirEntry* entry = user_entry;
    memset(st, 0, sizeof(*st));

    switch (f->data.type) {
        default: return -1;

        case SaveDirType_Root:
        case SaveDirType_User1:
        case SaveDirType_File:
            st->st_nlink = 1;
            st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
            return 0;

        case SaveDirType_Zip:
            // random size for the client, hopefully they don't take it seriously ;)
            st->st_nlink = 1;
            st->st_size = entry->info.size;
            st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
            return 0;

        case SaveDirType_FileApp: {
            char nxpath[FS_MAX_PATH];
            build_native_path(nxpath, path, &f->data);
            return vfs_fs_internal_dirlstat(&f->fs, &f->fs_dir, &entry->fs_buf, nxpath, st);
        }
    }
}

static int vfs_save_isdir_open(void* user) {
    struct VfsSaveDir* f = user;
    return f->is_valid;
}

static int vfs_save_closedir(void* user) {
    struct VfsSaveDir* f = user;
    if (!vfs_save_isdir_open(f)) {
        return -1;
    }

    switch (f->data.type) {
        default: break;

        case SaveDirType_File:
        case SaveDirType_Zip:
            fsSaveDataInfoReaderClose(&f->r);
            break;

        case SaveDirType_FileApp:
            vfs_fs_internal_closedir(&f->fs_dir);
            unmount_save_fs(&f->data);
            break;
    }

    memset(f, 0, sizeof(*f));
    return 0;
}

static int vfs_save_stat(const char* path, struct stat* st) {
    const struct SavePathData data = get_type(path);
    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;


    switch (data.type) {
        default: return -1;

        case SaveDirType_Root:
        case SaveDirType_User1:
        case SaveDirType_File:
        case SaveDirType_Zip:
            st->st_nlink = 1;
            st->st_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
            return 0;

        case SaveDirType_FileApp: {
            FsFileSystem* fs = mount_save_fs(&data);
            if (!fs) {
                return -1;
            }

            char nxpath[FS_MAX_PATH];
            build_native_path(nxpath, path, &data);
            int rc = vfs_fs_internal_stat(fs, nxpath, st);
            unmount_save_fs(&data);
            return rc;
        }

        case SaveDirType_ZipApp: {
            // random size for the client, hopefully they don't take it seriously ;)
            st->st_size = 1024*1024*64;
            st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
            return 0;
        }
    }

    return 0;
}

static int vfs_save_mkdir(const char* path) {
    const struct SavePathData data = get_type(path);
    if (!g_writable || data.type != SaveDirType_FileApp) {
        errno = EROFS;
        return -1;
    }

    FsFileSystem* fs = mount_save_fs(&data);
    if (!fs) {
        return -1;
    }

    char nxpath[FS_MAX_PATH];
    build_native_path(nxpath, path, &data);
    int rc = vfs_fs_internal_mkdir(fs, nxpath);
    unmount_save_fs(&data);
    return rc;
}

static int vfs_save_unlink(const char* path) {
    const struct SavePathData data = get_type(path);
    if (!g_writable || data.type != SaveDirType_FileApp) {
        errno = EROFS;
        return -1;
    }

    FsFileSystem* fs = mount_save_fs(&data);
    if (!fs) {
        return -1;
    }

    char nxpath[FS_MAX_PATH];
    build_native_path(nxpath, path, &data);
    int rc = vfs_fs_internal_unlink(fs, nxpath);
    unmount_save_fs(&data);
    return rc;
}

static int vfs_save_rmdir(const char* path) {
    const struct SavePathData data = get_type(path);
    if (!g_writable || data.type != SaveDirType_FileApp) {
        errno = EROFS;
        return -1;
    }

    FsFileSystem* fs = mount_save_fs(&data);
    if (!fs) {
        return -1;
    }

    char nxpath[FS_MAX_PATH];
    build_native_path(nxpath, path, &data);
    int rc = vfs_fs_internal_rmdir(fs, nxpath);
    unmount_save_fs(&data);
    return rc;
}

static int vfs_save_rename(const char* src, const char* dst) {
    const struct SavePathData data_src = get_type(src);
    const struct SavePathData data_dst = get_type(dst);
    if (!g_writable || data_src.type != SaveDirType_FileApp || data_dst.type != SaveDirType_FileApp) {
        errno = EROFS;
        return -1;
    }


    if (data_src.app_id != data_dst.app_id || memcmp(&data_src.uid, &data_dst.uid, sizeof(data_src.uid))) {
        return -1;
    }

    FsFileSystem* fs = mount_save_fs(&data_src);
    if (!fs) {
        return -1;
    }

    char nxpath_src[FS_MAX_PATH];
    char nxpath_dst[FS_MAX_PATH];
    build_native_path(nxpath_src, src, &data_src);
    build_native_path(nxpath_dst, dst, &data_dst);
    int rc = vfs_fs_internal_rename(fs, nxpath_src, nxpath_dst);
    unmount_save_fs(&data_src);
    return rc;
}

void vfs_save_init(bool save_writable) {
    g_writable = save_writable;

    AccountUid uids[8];
    s32 count;
    Result rc;
    if (R_FAILED(rc = accountListAllUsers(uids, 8, &count))) {
        log_file_fwrite("failed: accountListAllUsers() 0x%X\n", rc);
    } else {
        for (int i = 0; i < count; i++) {
            AccountProfile profile;
            if (R_FAILED(rc = accountGetProfile(&profile, uids[i]))) {
                log_file_fwrite("failed: accountGetProfile() 0x%X\n", rc);
            } else {
                AccountProfileBase base;
                if (R_FAILED(rc = accountProfileGet(&profile, NULL, &base))) {
                    log_file_fwrite("failed: accountProfileGet() 0x%X\n", rc);
                } else {
                    strcpy(g_acc_profile[g_acc_count].name, base.nickname);
                    g_acc_profile[g_acc_count].uid = base.uid;
                    g_acc_count++;
                }
                accountProfileClose(&profile);
            }
        }
    }

    strcpy(g_acc_profile[g_acc_count++].name, "bcat");
    strcpy(g_acc_profile[g_acc_count++].name, "cache");
    strcpy(g_acc_profile[g_acc_count++].name, "device");
    strcpy(g_acc_profile[g_acc_count++].name, "system");
}

void vfs_save_exit(void) {
    for (int i = 0; i < ARRAY_SIZE(g_save_cache); i++) {
        struct SaveCacheEntry* entry = &g_save_cache[i];
        if (entry->ref_count) {
            if (g_writable) {
                fsFsCommit(&entry->fs);
            }
            fsFsClose(&entry->fs);
        }
    }
}

const FtpVfs g_vfs_save = {
    .open = vfs_save_open,
    .read = vfs_save_read,
    .write = vfs_save_write,
    .seek = vfs_save_seek,
    .close = vfs_save_close,
    .isfile_open = vfs_save_isfile_open,
    .opendir = vfs_save_opendir,
    .readdir = vfs_save_readdir,
    .dirlstat = vfs_save_dirlstat,
    .closedir = vfs_save_closedir,
    .isdir_open = vfs_save_isdir_open,
    .stat = vfs_save_stat,
    .lstat = vfs_save_stat,
    .mkdir = vfs_save_mkdir,
    .unlink = vfs_save_unlink,
    .rmdir = vfs_save_rmdir,
    .rename = vfs_save_rename,
};
