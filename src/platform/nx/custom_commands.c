#include "custom_commands.h"
#include "reboot_to_payload/reboot_to_payload.h"
#include "rtc/max77620-rtc.h"
#include "utils.h"
#include "minIni.h"
#include "ftpsrv_vfs.h"

#include <switch.h>
#include <switch/services/bsd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// large enough size as to not explode the stack.
#define HEKATE_INI_NAME_MAX 128
// max supported by rtc.
#define HEKATE_CONFIG_MAX 16
// reasonable max config name max.
#define HEKATE_SECTION_NAME_MAX 128
// default error code for custom commands.
#define FTP_DEFAULT_ERROR_CODE 500
// title id for qlaunch.
#define QLAUNCH_TID 0x0100000000001000ULL

struct HekateListIni {
    char* msg_buf;
    unsigned msg_buf_len;
    unsigned off;
    char current_section[HEKATE_SECTION_NAME_MAX];
};

struct HekateConfigIni {
    const char* wanted;
    char current_section[HEKATE_SECTION_NAME_MAX];
    int count;
    bool found;
};

// common paths for hekate, sorted in priority.
static const char* HEKATE_PATHS[] = {
    "/bootloader/update.bin",
    "/atmosphere/reboot_payload.bin",
    "/payload.bin",
};

// messages will no longer be sent, but will be logged.
static u8* close_sockets(u32* size) {
    extern u8 SOCKET_TRANSFER_MEM[];
    extern const u32 SOCKET_TRANSFER_MEM_SIZE;

    socketExit();
    bsdExit();

    *size = SOCKET_TRANSFER_MEM_SIZE;
    return memset(SOCKET_TRANSFER_MEM, 0xFF, SOCKET_TRANSFER_MEM_SIZE);
}

static int ini_browse_callback(const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData) {
    struct HekateConfigIni* h = UserData;

    // may be NULL
    if (!Section || !strcmp(Section, "config")) {
        return 1;
    }

    // check if its the same as before.
    if (!strcmp(Section, h->current_section)) {
        return 1;
    }

    // check if we found the config.
    if (!strcmp(Section, h->wanted)) {
        h->found = true;
        return 0;
    }

    // add new entry.
    h->count++;
    snprintf(h->current_section, sizeof(h->current_section), "%s", Section);
    return 1;

}

static int ini_list_callback(const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData) {
    struct HekateListIni* h = UserData;

    // may be NULL
    if (!Section || !strcmp(Section, "config")) {
        return 1;
    }

    // check if its the same as before.
    if (!strcmp(Section, h->current_section)) {
        return 1;
    }

    // add new entry.
    snprintf(h->current_section, sizeof(h->current_section), "%s", Section);
    h->off += snprintf(h->msg_buf + h->off, h->msg_buf_len - h->off, " %s\r\n", Section);
    if (h->off >= h->msg_buf_len) {
        return 0;
    }

    return 1;

}

static void hekate_list_config(char* msg_buf, unsigned msg_buf_len, unsigned off) {
    struct HekateListIni h = {0};
    h.msg_buf = msg_buf;
    h.msg_buf_len = msg_buf_len;
    h.off = off;

    // list configs found in ipl.ini
    ini_browse(ini_list_callback, &h, "/bootloader/hekate_ipl.ini");

    // list configs found in the ini folder
    FsFileSystem* fs;
    char nxpath[FS_MAX_PATH];
    if (!fsdev_wrapTranslatePath("/bootloader/ini", &fs, nxpath)) {
        FsDir dir;
        if (R_SUCCEEDED(fsFsOpenDirectory(fs, nxpath, FsDirOpenMode_ReadFiles|FsDirOpenMode_NoFileSize, &dir))) {
            s64 total;
            FsDirectoryEntry buf;
            char fullpath[FS_MAX_PATH];
            while (R_SUCCEEDED(fsDirRead(&dir, &total, 1, &buf)) && total == 1) {
                const int len = strlen(buf.name) - 4;
                if (len > 0 && len < HEKATE_INI_NAME_MAX && !strcasecmp(".ini", buf.name + len)) {
                    h.current_section[0] = '\0';
                    snprintf(fullpath, sizeof(fullpath), "%s/%s", nxpath, buf.name);
                    ini_browse(ini_list_callback, &h, fullpath);
                    if (h.off >= h.msg_buf_len) {
                        break;
                    }
                }
            }
            fsDirClose(&dir);
        }
    }
}

static bool hekate_get_config_idx(rtc_reboot_reason_t* rr, const char* config) {
    struct HekateConfigIni h = {0};
    h.wanted = config;

    if (!ini_browse(ini_browse_callback, &h, "/bootloader/hekate_ipl.ini")) {
        return false;
    }

    if (h.found) {
        rr->dec.autoboot_idx = h.count + 1;
        rr->dec.autoboot_list = 0;
        return true;
    }

    FsFileSystem* fs;
    char nxpath[FS_MAX_PATH];
    if (fsdev_wrapTranslatePath("/bootloader/ini", &fs, nxpath)) {
        return false;
    }

    FsDir dir;
    if (R_FAILED(fsFsOpenDirectory(fs, nxpath, FsDirOpenMode_ReadFiles|FsDirOpenMode_NoFileSize, &dir))) {
        return false;
    }

    s32 dir_count = 0;
    char names[HEKATE_CONFIG_MAX][HEKATE_INI_NAME_MAX] = {0};

    s64 total;
    FsDirectoryEntry buf;
    while (R_SUCCEEDED(fsDirRead(&dir, &total, 1, &buf)) && total == 1) {
        const int len = strlen(buf.name) - 4;
        if (len > 0 && len < HEKATE_INI_NAME_MAX && !strcasecmp(".ini", buf.name + len)) {
            snprintf(names[dir_count], len + 1, "%s", buf.name);
            dir_count++;
            if (dir_count == HEKATE_CONFIG_MAX) {
                break;
            }
        }
    }
    fsDirClose(&dir);

    // sort entries, as hekate does.
    bool did_sort = true;
    while (did_sort) {
        did_sort = false;
        for (int i = 0; i < dir_count - 1; i++) {
            if (strcmp(names[i], names[i + 1]) > 0) {
                char temp[HEKATE_INI_NAME_MAX];
                strcpy(temp, names[i]);
                strcpy(names[i], names[i + 1]);
                strcpy(names[i + 1], temp);
                did_sort = true;
            }
        }
    }

    // find entry
    h.count = 0;
    for (int i = 0; i < dir_count; i++) {
        h.current_section[0] = '\0';
        snprintf(nxpath, sizeof(nxpath), "/bootloader/ini/%s.ini", names[i]);
        if (ini_browse(ini_browse_callback, &h, nxpath)) {
            if (h.found) {
                rr->dec.autoboot_idx = h.count + 1;
                rr->dec.autoboot_list = 1;
                return true;
            }
        }
    }

    return false;
}

static int ftp_custom_cmd_REBT(void* userdata, const char* data, char* msg_buf, unsigned msg_buf_len) {
    Result rc;
    int code = FTP_DEFAULT_ERROR_CODE;

    if (R_FAILED(rc = spsmInitialize())) {
        snprintf(msg_buf, msg_buf_len, "Failed: spsmInitialize() 0x%08X", rc);
    } else {
        if (R_FAILED(rc = spsmShutdown(true))) {
            snprintf(msg_buf, msg_buf_len, "Failed: spsmShutdown(true) 0x%08X", rc);
        } else {
            snprintf(msg_buf, msg_buf_len, "success");
            code = 200;
        }
        spsmExit();
    }
    return code;
}

static int ftp_custom_cmd_RTOP(void* userdata, const char* data, char* msg_buf, unsigned msg_buf_len) {
    Result rc;
    int code = FTP_DEFAULT_ERROR_CODE;
    char pathname[FS_MAX_PATH] = {0};
    const int r = snprintf(pathname, sizeof(pathname), "%s", data);

    if (r <= 0 || r >= sizeof(pathname)) {
        snprintf(msg_buf, msg_buf_len, "Syntax error in parameters or arguments.");
    } else {
        FsFileSystem* fs;
        char nxpath[FS_MAX_PATH] = {0};
        if (fsdev_wrapTranslatePath(pathname, &fs, nxpath)) {
            snprintf(msg_buf, msg_buf_len, "Syntax error in parameters or arguments, invalid path: %s", pathname);
        } else {
            FsFile file;
            if (R_FAILED(rc = fsFsOpenFile(fs, nxpath, FsOpenMode_Read, &file))) {
                snprintf(msg_buf, msg_buf_len, "Syntax error in parameters or arguments, Failed to open file: %s", nxpath);
            } else {
                if (!validate_payload_from_file(&file, false)) {
                    snprintf(msg_buf, msg_buf_len, "Syntax error in parameters or arguments, Not a valid payload: %s", nxpath);
                } else {
                    u32 payload_buf_size;
                    u8* paylod_buf = close_sockets(&payload_buf_size);

                    u64 bytes_read;
                    rc = fsFileRead(&file, 0, paylod_buf, payload_buf_size, 0, &bytes_read);
                    if (R_FAILED(rc)) {
                        snprintf(msg_buf, msg_buf_len, "Failed to read file: %s, please restart ftpsrv", nxpath);
                    } else {
                        if (!reboot_to_payload(paylod_buf, bytes_read)) {
                            snprintf(msg_buf, msg_buf_len, "Failed to reboot to payload: %s, please restart ftpsrv", nxpath);
                        } else {
                            code = 200;
                            snprintf(msg_buf, msg_buf_len, "Bye!");
                        }
                    }
                }
                fsFileClose(&file);
            }
        }
    }
    return code; /* this code / msg is never received, obviously. */
}

static int ftp_custom_cmd_RTOH(void* userdata, const char* data, char* msg_buf, unsigned msg_buf_len) {
    Result rc;
    int code = FTP_DEFAULT_ERROR_CODE;
    rtc_reboot_reason_t rr = {0};
    char* end;
    const u64 opt = strtoull(data, &end, 10);

    if (opt > 3 || data == end) {
        snprintf(msg_buf, msg_buf_len, "Failed: bad args");
    } else if (opt == 1 && (end[0] == '\0' || !hekate_get_config_idx(&rr, end + 1))) {
        code = 200;
        int rc = snprintf(msg_buf, msg_buf_len, "-Listing configs:\r\n");
        hekate_list_config(msg_buf, msg_buf_len, rc);
    } else {
        rr.dec.reason = opt;

        if (is_r2p_supported()) {
            FsFile file;
            int found_hekate = -1;
            for (int i = 0; i < sizeof(HEKATE_PATHS)/sizeof(HEKATE_PATHS[0]); i++) {
                FsFileSystem* fs;
                char nxpath[FS_MAX_PATH] = {0};
                if (!fsdev_wrapTranslatePath(HEKATE_PATHS[i], &fs, nxpath)) {
                    if (R_SUCCEEDED(rc = fsFsOpenFile(fs, nxpath, FsOpenMode_Read, &file))) {
                        if (validate_payload_from_file(&file, true)) {
                            found_hekate = i;
                            break;
                        }
                        fsFileClose(&file);
                    }
                }
            }

            if (found_hekate < 0) {
                snprintf(msg_buf, msg_buf_len, "Failed: unable to find hekate!");
            } else {
                u32 payload_buf_size;
                u8* paylod_buf = close_sockets(&payload_buf_size);

                u64 bytes_read;
                rc = fsFileRead(&file, 0, paylod_buf, payload_buf_size, 0, &bytes_read);
                fsFileClose(&file);
                if (R_FAILED(rc)) {
                    snprintf(msg_buf, msg_buf_len, "Failed to read file: %s, please restart ftpsrv", HEKATE_PATHS[found_hekate]);
                } else {
                    paylod_buf[0x94] = 1 << 0; // [boot_cfg] Force AutoBoot
                    paylod_buf[0x95] = rr.dec.autoboot_idx; // [autoboot] Load config at this index
                    paylod_buf[0x96] = rr.dec.autoboot_list; // [autoboot_list] Load config from list.

                    if (opt == 3) {
                        paylod_buf[0x97] = 1 << 5; // boot into ums
                        paylod_buf[0x98] = rr.dec.ums_idx; // mount sd card
                    }

                    if (!reboot_to_payload(paylod_buf, bytes_read)) {
                        snprintf(msg_buf, msg_buf_len, "Failed to reboot to payload: %s, please restart ftpsrv", HEKATE_PATHS[found_hekate]);
                    } else {
                        code = 200;
                        snprintf(msg_buf, msg_buf_len, "Bye!");
                    }
                }
            }
        } else {
            if (!validate_payload_from_path("/bootloader/update.bin", true)) {
                snprintf(msg_buf, msg_buf_len, "Failed: hekate not found!");
            } else {
                if (R_FAILED(rc = spsmInitialize())) {
                    snprintf(msg_buf, msg_buf_len, "Failed: spsmInitialize() 0x%08X", rc);
                } else {
                    if (R_FAILED(rc = i2cInitialize())) {
                        snprintf(msg_buf, msg_buf_len, "Failed: i2cInitialize() 0x%08X", rc);
                    } else {
                        I2cSession s;
                        if (R_FAILED(rc = i2cOpenSession(&s, I2cDevice_Max77620Rtc))) {
                            snprintf(msg_buf, msg_buf_len, "Failed: i2cOpenSession(I2cDevice_Max77620Rtc) 0x%08X", rc);
                        } else {
                            rc = max77620_rtc_set_reboot_reason(&s, &rr);
                            i2csessionClose(&s);

                            if (R_FAILED(rc)) {
                                snprintf(msg_buf, msg_buf_len, "Failed: max77620_rtc_set_reboot_reason(opts) 0x%08X", rc);
                            } else if (R_FAILED(rc = spsmShutdown(true))) {
                                snprintf(msg_buf, msg_buf_len, "Failed: spsmShutdown(true) 0x%08X", rc);
                            } else {
                                code = 200;
                                snprintf(msg_buf, msg_buf_len, "Bye!");
                            }
                        }
                        i2cExit();
                    }
                    spsmExit();
                }
            }
        }
    }
    return code;
}

static int ftp_custom_cmd_SHUT(void* userdata, const char* data, char* msg_buf, unsigned msg_buf_len) {
    Result rc;
    int code = FTP_DEFAULT_ERROR_CODE;

    if (R_FAILED(rc = spsmInitialize())) {
        snprintf(msg_buf, msg_buf_len, "Failed: spsmInitialize() 0x%08X", rc);
    } else {
        if (R_FAILED(rc = spsmShutdown(false))) {
            snprintf(msg_buf, msg_buf_len, "Failed: spsmShutdown(false) 0x%08X", rc);
        } else {
            snprintf(msg_buf, msg_buf_len, "success");
            code = 200;
        }
        spsmExit();
    }
    return code;
}

static int ftp_custom_cmd_TID(void* userdata, const char* data, char* msg_buf, unsigned msg_buf_len) {
    Result rc;
    int code = FTP_DEFAULT_ERROR_CODE;

    if (R_FAILED(rc = pmdmntInitialize())) {
        snprintf(msg_buf, msg_buf_len, "Failed: pmdmntInitialize() 0x%08X", rc);
    } else {
        if (R_FAILED(rc = pminfoInitialize())) {
            snprintf(msg_buf, msg_buf_len, "Failed: pminfoInitialize() 0x%08X", rc);
        } else {
            u64 pid, tid;
            if (R_SUCCEEDED(rc = pmdmntGetApplicationProcessId(&pid))) {
                if (0x20f == pminfoGetProgramId(&tid, pid)){
                    tid = QLAUNCH_TID;
                }
            } else if (rc == 0x20f) {
                tid = QLAUNCH_TID;
            } else {
                tid = 0;
            }

            if (!tid) {
                snprintf(msg_buf, msg_buf_len, "Failed: pminfoInitialize() 0x%08X", rc);
            } else {
                code = 200;
                NcmContentId id;
                struct AppName name;
                if (tid == QLAUNCH_TID) {
                    snprintf(msg_buf, msg_buf_len, "%016lX %s", tid, "Qlaunch");
                } else if (R_SUCCEEDED(get_app_name(tid, &id, &name))) {
                    snprintf(msg_buf, msg_buf_len, "%016lX %s", tid, name.str);
                } else {
                    snprintf(msg_buf, msg_buf_len, "%016lX", tid);
                }
            }
            pminfoExit();
        }
        pmdmntExit();
    }
    return code;
}

static int ftp_custom_cmd_TIDS(void* userdata, const char* data, char* msg_buf, unsigned msg_buf_len) {
    Result rc;
    int code = FTP_DEFAULT_ERROR_CODE;
    u64 pids[0x50];
    s32 process_count;

    if (R_FAILED(rc = svcGetProcessList(&process_count, pids, 0x50))) {
        snprintf(msg_buf, msg_buf_len, "Failed: svcGetProcessList() 0x%08X", rc);
    } else {
        if (R_FAILED(rc = pminfoInitialize())) {
            snprintf(msg_buf, msg_buf_len, "Failed: pminfoInitialize() 0x%08X", rc);
        } else {
            code = 200;
            int off = snprintf(msg_buf, msg_buf_len, "-Listing TIDS\r\n");
            for (int i = 0; i < (process_count - 1); i++) {
                u64 tid;
                if (R_SUCCEEDED(rc = pminfoGetProgramId(&tid, pids[i]))) {
                    off += snprintf(msg_buf + off, msg_buf_len - off, " %016lX\r\n", tid);
                    if (off >= msg_buf_len) {
                        break;
                    }
                }
            }
            pminfoExit();
        }
    }
    return code;
}

static int ftp_custom_cmd_TRMT(void* userdata, const char* data, char* msg_buf, unsigned msg_buf_len) {
    Result rc;
    int code = FTP_DEFAULT_ERROR_CODE;
    char* end;
    u64 tid = strtoull(data, &end, 0x10);

    if (!tid || data == end) {
        snprintf(msg_buf, msg_buf_len, "Failed: bad args");
    } else {
        if (R_FAILED(rc = pmshellInitialize())) {
            snprintf(msg_buf, msg_buf_len, "Failed: pmshellInitialize() 0x%08X", rc);
        } else {
            if (R_FAILED(rc = pmshellTerminateProgram(tid))) {
                snprintf(msg_buf, msg_buf_len, "Failed: pmshellTerminateProgram(0x%016lX) 0x%08X", tid, rc);
            } else {
                code = 200;
                snprintf(msg_buf, msg_buf_len, "success");
            }
            pmshellExit();
        }
    }
    return code;
}

const struct FtpSrvCustomCommand CUSTOM_COMMANDS[] = {
    // reboot console
    { "REBT", ftp_custom_cmd_REBT, NULL, 1, 0 },
    // reboot to payload
    { "RTOP", ftp_custom_cmd_RTOP, NULL, 1, 1 },
    // reboot to hekate
    { "RTOH", ftp_custom_cmd_RTOH, NULL, 1, 1 },
    // shutdown console
    { "SHUT", ftp_custom_cmd_SHUT, NULL, 1, 0 },
    // list current application tid
    { "TID", ftp_custom_cmd_TID, NULL, 1, 0 },
    // list all tids
    { "TIDS", ftp_custom_cmd_TIDS, NULL, 1, 0 },
    // terminate tid
    { "TRMT", ftp_custom_cmd_TRMT, NULL, 1, 1 },
};

const unsigned CUSTOM_COMMANDS_SIZE = sizeof(CUSTOM_COMMANDS) / sizeof(CUSTOM_COMMANDS[0]);
