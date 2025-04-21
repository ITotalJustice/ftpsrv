#include <string.h>
#include <stdbool.h>

#include <switch.h>
#include "ams_bpc.h"
#include "reboot_to_payload.h"
#include "../utils.h"

enum {
    SplConfigItem_ExosphereVersion = 65000,
    SplConfigItem_NeedsReboot      = 65001,
    SplConfigItem_NeedsShutdown    = 65002,
    SplConfigItem_ExosphereVerHash = 65003,
    SplConfigItem_HasRcmBugPatch   = 65004,
};

bool validate_payload_from_file(FsFile* file, bool check_hekate) {
    s64 size;
    if (R_FAILED(fsFileGetSize(file, &size))) {
        return false;
    }

    if (size > IRAM_PAYLOAD_MAX_SIZE) {
        return false;
    }

    if (check_hekate) {
        u32 magic;
        u64 bytes_read;
        if (R_FAILED(fsFileRead(file, 0x118, &magic, sizeof(magic), 0, &bytes_read))) {
            return false;
        }

        if (bytes_read != 4 || magic != 0x43544349) {
            return false;
        }
    }

    return true;
}

bool validate_payload_from_path(const char* path, bool check_hekate) {
    FsFileSystem* fs;
    char nxpath[FS_MAX_PATH];
    if (fsdev_wrapTranslatePath(path, &fs, nxpath)) {
        return false;
    }

    FsFile file;
    if (R_FAILED(fsFsOpenFile(fs, nxpath, FsOpenMode_Read, &file))) {
        return false;
    }

    const bool result = validate_payload_from_file(&file, check_hekate);
    fsFileClose(&file);
    return result;
}

bool is_r2p_supported(void) {
    Result rc;
    bool can_reboot;
    if (R_FAILED(rc = setsysInitialize())) {
        can_reboot = false;
    } else {
        SetSysProductModel model;
        rc = setsysGetProductModel(&model);
        setsysExit();
        if (R_FAILED(rc) || (model != SetSysProductModel_Nx && model != SetSysProductModel_Copper)) {
            can_reboot = false;
        } else {
            can_reboot = true;
        }
    }
    return can_reboot;
}

void smcRebootToIramPayload(void)
{
    splInitialize();
    splSetConfig((SplConfigItem)65001, 2);
    splExit();

    // the below doesn't work
    // SecmonArgs args;
    // args.X[0] = 0xC3000401;                /* smcSetConfig */
    // args.X[1] = SplConfigItem_NeedsReboot; /* Exosphere reboot */
    // args.X[3] = 2;                         /* Perform reboot to payload at 0x40010000 in IRAM. */
    // svcCallSecureMonitor(&args);
}

void smcCopyToIram(uintptr_t iram_addr, const void *src_addr, u32 size)
{
    SecmonArgs args;
    args.X[0] = 0xF0000201;     /* smcAmsIramCopy */
    args.X[1] = (u64)src_addr;  /* DRAM address */
    args.X[2] = (u64)iram_addr; /* IRAM address */
    args.X[3] = size;           /* Amount to copy */
    args.X[4] = 1;              /* 1 = Write */
    svcCallSecureMonitor(&args);
}
