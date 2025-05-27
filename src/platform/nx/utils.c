#include "utils.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

#define FsDevWrap_Module 0x505

enum FsDevWrap_Error {
    FsDevWrap_Error_FailedToMount = MAKERESULT(FsDevWrap_Module, 0x1),
};

struct FsDevWrapEntry {
    FsFileSystem fs;
    char path[FsDevWrap_PATH_MAX];
    const char* shortcut;
    bool active;
    bool own;
};

static struct FsDevWrapEntry g_fsdev_entries[FsDevWrap_DEVICES_MAX] = {0};

static struct FsDevWrapEntry* find_entry(const char* path) {
    for (int i = 0; i < FsDevWrap_DEVICES_MAX; i++) {
        const size_t dev_len = strlen(g_fsdev_entries[i].path);
        if (g_fsdev_entries[i].active && !strncmp(g_fsdev_entries[i].path, path, dev_len) && (path[dev_len] == '\0' || path[dev_len] == ':')) {
            return &g_fsdev_entries[i];
        }
    }

    return NULL;
}

static Result mount_helper(const char* path, FsFileSystem fs) {
    if (fsdev_wrapMountDevice(path, NULL, fs, true)) {
        fsFsClose(&fs);
        return FsDevWrap_Error_FailedToMount;
    }
    return 0;
}

Result fsdev_wrapMountSdmc(void) {
    FsFileSystem fs;
    Result rc;
    if (R_SUCCEEDED(rc = fsOpenSdCardFileSystem(&fs))) {
        rc = mount_helper("sdmc", fs);
    }
    return rc;
}

Result fsdev_wrapMountImage(const char* path, FsImageDirectoryId id) {
    FsFileSystem fs;
    Result rc;
    if (R_SUCCEEDED(rc = fsOpenImageDirectoryFileSystem(&fs, id))) {
        rc = mount_helper(path, fs);
    }
    return rc;
}

Result fsdev_wrapMountContent(const char* path, FsContentStorageId id) {
    FsFileSystem fs;
    Result rc;
    if (R_SUCCEEDED(rc = fsOpenContentStorageFileSystem(&fs, id))) {
        rc = mount_helper(path, fs);
    }
    return rc;
}

Result fsdev_wrapMountBis(const char* path, FsBisPartitionId id) {
    FsFileSystem fs;
    Result rc;
    if (R_SUCCEEDED(rc = fsOpenBisFileSystem(&fs, id, ""))) {
        rc = mount_helper(path, fs);
    }
    return rc;
}

Result fsdev_wrapMountSave(const char* path, u64 id, AccountUid uid) {
    FsFileSystem fs;
    Result rc;
    if (R_SUCCEEDED(rc = fsOpen_SaveData(&fs, id, uid))) {
        rc = mount_helper(path, fs);
    }
    return rc;
}

Result fsdev_wrapMountSaveBcat(const char* path, u64 id) {
    FsFileSystem fs;
    Result rc;
    if (R_SUCCEEDED(rc = fsOpen_BcatSaveData(&fs, id))) {
        rc = mount_helper(path, fs);
    }
    return rc;
}

FsFileSystem* fsdev_wrapGetDeviceFileSystem(const char* name) {
    struct FsDevWrapEntry* entry = find_entry(name);
    if (!entry) {
        errno = ENODEV;
        return NULL;
    }
    return &entry->fs;
}

int fsdev_wrapTranslatePath(const char *path, FsFileSystem** device, char *outpath) {
    int rc = 0;
    char nxpath[FS_MAX_PATH];

    if (!path) {
        errno = ENODEV;
        return -1;
    }

    if (path[0] == '/') {
        if (strchr(path, ':')) {
            strcpy(nxpath, path + 1);
        } else {
            rc = snprintf(nxpath, sizeof(nxpath), "%s%s", "sdmc:", path);
            if (rc <= 0 || rc >= sizeof(nxpath)) {
                errno = ENAMETOOLONG;
                return -1;
            }
        }
    } else {
        strcpy(nxpath, path);
    }

    const char* colon = strchr(nxpath, ':');
    if (!colon) {
        errno = ENODEV;
        return -1;
    }

    struct FsDevWrapEntry* entry = find_entry(nxpath);
    if (!entry) {
        errno = ENODEV;
        return -1;
    }

    if (device) {
        *device = &entry->fs;
    }

    if (outpath) {
        if (entry->shortcut) {
            rc = snprintf(outpath, FS_MAX_PATH, "/%s/%s", entry->shortcut, colon + 1);
            if (rc <= 0 || rc >= FS_MAX_PATH) {
                errno = ENAMETOOLONG;
                return -1;
            }
        } else {
            strcpy(outpath, colon + 1);
        }

        if (outpath[0] == '\0') {
            outpath[0] = '/';
            outpath[1] = '\0';
        }
    }

    return 0;
}

int fsdev_wrapMountDevice(const char *name, const char* shortcut, FsFileSystem fs, bool own) {
    const size_t name_len = strlen(name);
    if (name_len <= 1 || name_len + 1 >= FsDevWrap_PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    for (int i = 0; i < FsDevWrap_DEVICES_MAX; i++) {
        if (!g_fsdev_entries[i].active) {
            g_fsdev_entries[i].active = true;
            g_fsdev_entries[i].fs = fs;
            g_fsdev_entries[i].shortcut = shortcut;
            g_fsdev_entries[i].own = own;
            snprintf(g_fsdev_entries[i].path, sizeof(g_fsdev_entries[i].path), name);
            return 0;
        }
    }

    return -1;
}

void fsdev_wrapUnmountAll(void) {
    for (int i = 0; i < FsDevWrap_DEVICES_MAX; i++) {
        if (!g_fsdev_entries[i].active) {
            g_fsdev_entries[i].active = false;
            if (g_fsdev_entries[i].own) {
                fsFsClose(&g_fsdev_entries[i].fs);
            }
        }
    }

    memset(g_fsdev_entries, 0, sizeof(g_fsdev_entries));
}

void led_flash(void) {
    static const HidsysNotificationLedPattern pattern = {
        .baseMiniCycleDuration = 0x1,             // 12.5ms.
        .totalMiniCycles = 0x1,                   // 1 mini cycle(s).
        .totalFullCycles = 0x1,                   // 1 full run(s).
        .startIntensity = 0xF,                    // 100%.
        .miniCycles[0].ledIntensity = 0xF,        // 100%.
        .miniCycles[0].transitionSteps = 0x1,     // 1 step(s). Total 12.5ms.
        .miniCycles[0].finalStepDuration = 0x0,   // Forced 12.5ms.
    };

    Result rc;
    s32 total;
    HidsysUniquePadId unique_pad_id;

    rc = hidsysGetUniquePadsFromNpad(HidNpadIdType_Handheld, &unique_pad_id, 1, &total);
    if (R_SUCCEEDED(rc) && total) {
        rc = hidsysSetNotificationLedPattern(&pattern, unique_pad_id);
    }

    if (R_FAILED(rc) || !total) {
        rc = hidsysGetUniquePadsFromNpad(HidNpadIdType_No1, &unique_pad_id, 1, &total);
        if (R_SUCCEEDED(rc) && total) {
            hidsysSetNotificationLedPattern(&pattern, unique_pad_id);
        }
    }
}
