/**
 * Copyright 2024 TotalJustice.
 * SPDX-License-Identifier: MIT
 */
#include <ftpsrv.h>
#include <ftpsrv_vfs.h>
#include "utils.h"
#include "log/log.h"
#include "custom_commands.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <switch/services/bsd.h>
#include <switch.h>
#include <minIni.h>

#define TEXT_NORMAL "\033[37;1m"
#define TEXT_RED "\033[31;1m"
#define TEXT_GREEN "\033[32;1m"
#define TEXT_YELLOW "\033[33;1m"
#define TEXT_BLUE "\033[34;1m"

struct CallbackData {
    enum FTP_API_LOG_TYPE type;
    char msg[1024];
};

static const char* INI_PATH = "/config/ftpsrv/config.ini";
static const char* LOG_PATH = "/config/ftpsrv/log.txt";
static struct FtpSrvConfig g_ftpsrv_config = {0};
static bool g_led_enabled = false;

static PadState g_pad = {0};
static Mutex g_mutex = {0};
static struct CallbackData* g_callback_data = NULL;
static u32 g_num_events = 0;
static volatile bool g_should_exit = false;

static bool IsApplication(void) {
    const AppletType type = appletGetAppletType();
    return type == AppletType_Application || type == AppletType_SystemApplication;
}

static void ftp_log_callback(enum FTP_API_LOG_TYPE type, const char* msg) {
    if (g_led_enabled) {
        led_flash();
    }

    mutexLock(&g_mutex);
        g_num_events++;
        g_callback_data = realloc(g_callback_data, g_num_events * sizeof(*g_callback_data));
        g_callback_data[g_num_events-1].type = type;
        strcpy(g_callback_data[g_num_events-1].msg, msg);
    mutexUnlock(&g_mutex);
}

static void ftp_progress_callback(void) {
    if (g_led_enabled) {
        led_flash();
    }
}

static void processEvents(void) {
    mutexLock(&g_mutex);
    if (g_num_events) {
        for (int i = 0; i < g_num_events; i++) {
            log_file_write(g_callback_data[i].msg);

            switch (g_callback_data[i].type) {
                case FTP_API_LOG_TYPE_COMMAND:
                    printf(TEXT_BLUE "Command:  %s" TEXT_NORMAL "\n", g_callback_data[i].msg);
                    break;
                case FTP_API_LOG_TYPE_RESPONSE:
                    printf(TEXT_GREEN "Response: %s" TEXT_NORMAL "\n", g_callback_data[i].msg);
                    break;
                case FTP_API_LOG_TYPE_ERROR:
                    printf(TEXT_RED "Error:    %s" TEXT_NORMAL "\n", g_callback_data[i].msg);
                    break;
            }
        }

        g_num_events = 0;
        free(g_callback_data);
        g_callback_data = NULL;
        consoleUpdate(NULL);
    }
    mutexUnlock(&g_mutex);
}

static void ftp_thread(void* arg) {
    while (!g_should_exit) {
        ftpsrv_init(&g_ftpsrv_config);
        while (!g_should_exit) {
            if (ftpsrv_loop(500) != FTP_API_LOOP_ERROR_OK) {
                svcSleepThread(1000000000);
                break;
            }
        }
        ftpsrv_exit();
    }
}

static void consolePrint(const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vprintf(fmt, va);
    va_end(va);
    consoleUpdate(NULL);
}

static int error_loop(const char* msg) {
    log_file_write(msg);
    printf("Error: %s\n\n", msg);
    printf("Modify the config at: %s\n\n", INI_PATH);
    printf("\tPress (+) to exit...\n");
    consoleUpdate(NULL);

    while (appletMainLoop()) {
        padUpdate(&g_pad);
        const u64 kDown = padGetButtonsDown(&g_pad);
        if (kDown & HidNpadButton_Plus) {
            break;
        }
        svcSleepThread(1e+9 / 60);
    }
    return EXIT_FAILURE;
}

int main(int argc, char** argv) {
    consolePrint("\n[ftpsrv " FTPSRV_VERSION_STR " By TotalJustice]\n\n");

    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    g_ftpsrv_config.custom_command = CUSTOM_COMMANDS;
    g_ftpsrv_config.custom_command_count = CUSTOM_COMMANDS_SIZE;
    g_ftpsrv_config.log_callback = ftp_log_callback;
    g_ftpsrv_config.progress_callback = ftp_progress_callback;
    g_ftpsrv_config.anon = ini_getbool("Login", "anon", 0, INI_PATH);
    int user_len = ini_gets("Login", "user", "", g_ftpsrv_config.user, sizeof(g_ftpsrv_config.user), INI_PATH);
    int pass_len = ini_gets("Login", "pass", "", g_ftpsrv_config.pass, sizeof(g_ftpsrv_config.pass), INI_PATH);
    g_ftpsrv_config.port = ini_getl("Network", "port", 21, INI_PATH);
    g_ftpsrv_config.timeout = ini_getl("Network", "timeout", 0, INI_PATH);
    g_ftpsrv_config.use_localtime = ini_getbool("Misc", "use_localtime", 0, INI_PATH);
    bool log_enabled = ini_getbool("Log", "log", 0, INI_PATH);

    // get nx config
    bool mount_devices = ini_getbool("Nx", "mount_devices", 1, INI_PATH);
    bool mount_bis = ini_getbool("Nx", "mount_bis", 0, INI_PATH);
    bool save_writable = ini_getbool("Nx", "save_writable", 0, INI_PATH);
    g_led_enabled = ini_getbool("Nx", "led", 1, INI_PATH);
    g_ftpsrv_config.port = ini_getl("Nx", "app_port", g_ftpsrv_config.port, INI_PATH); // compat

    // get Nx-App overrides
    g_ftpsrv_config.anon = ini_getbool("Nx-App", "anon", g_ftpsrv_config.anon, INI_PATH);
    user_len = ini_gets("Nx-App", "user", g_ftpsrv_config.user, g_ftpsrv_config.user, sizeof(g_ftpsrv_config.user), INI_PATH);
    pass_len = ini_gets("Nx-App", "pass", g_ftpsrv_config.pass, g_ftpsrv_config.pass, sizeof(g_ftpsrv_config.pass), INI_PATH);
    g_ftpsrv_config.port = ini_getl("Nx-App", "port", g_ftpsrv_config.port, INI_PATH);
    g_ftpsrv_config.timeout = ini_getl("Nx-App", "timeout", g_ftpsrv_config.timeout, INI_PATH);
    g_ftpsrv_config.use_localtime = ini_getbool("Nx-App", "use_localtime", g_ftpsrv_config.use_localtime, INI_PATH);
    log_enabled = ini_getbool("Nx-App", "log", log_enabled, INI_PATH);
    mount_devices = ini_getbool("Nx-App", "mount_devices", mount_devices, INI_PATH);
    mount_bis = ini_getbool("Nx-App", "mount_bis", mount_bis, INI_PATH);
    save_writable = ini_getbool("Nx-App", "save_writable", save_writable, INI_PATH);
    g_led_enabled = ini_getbool("Nx-App", "led", g_led_enabled, INI_PATH);

    if (log_enabled) {
        log_file_init(LOG_PATH, "ftpsrv - " FTPSRV_VERSION_HASH " - NX-app");
    }

    if (!user_len && !pass_len && !g_ftpsrv_config.anon) {
        return error_loop("User / Pass / Anon not set in config!");
    }

    if (!g_ftpsrv_config.port) {
        return error_loop("Network: Invalid port");
    }

    u32 ip;
    if (R_FAILED(nifmGetCurrentIpAddress(&ip))) {
        return error_loop("failed to get current ip address");
    }

    vfs_nx_init(NULL, mount_devices, save_writable, mount_bis);

    const struct in_addr addr = {ip};
    printf(TEXT_YELLOW "ip: %s\n", inet_ntoa(addr));
    printf(TEXT_YELLOW "port: %d" TEXT_NORMAL "\n", g_ftpsrv_config.port);
    if (g_ftpsrv_config.anon) {
        printf(TEXT_YELLOW "anon: %d" TEXT_NORMAL "\n", 1);
    } else {
        printf(TEXT_YELLOW "user: %s" TEXT_NORMAL "\n", g_ftpsrv_config.user);
        printf(TEXT_YELLOW "pass: %s" TEXT_NORMAL "\n", g_ftpsrv_config.pass);
    }
    printf("\n");
    printf(TEXT_YELLOW "timeout: %us" TEXT_NORMAL "\n", g_ftpsrv_config.timeout);
    printf(TEXT_YELLOW "use_localtime: %u" TEXT_NORMAL "\n", g_ftpsrv_config.use_localtime);
    printf(TEXT_YELLOW "log: %d" TEXT_NORMAL "\n", log_enabled);
    printf(TEXT_YELLOW "mount_devices: %d" TEXT_NORMAL "\n", mount_devices);
    printf(TEXT_YELLOW "save_writable: %d" TEXT_NORMAL "\n", save_writable);
    printf(TEXT_YELLOW "\nconfig: %s" TEXT_NORMAL "\n", INI_PATH);
    if (appletGetAppletType() == AppletType_LibraryApplet || appletGetAppletType() == AppletType_SystemApplet) {
        printf(TEXT_RED "\napplet_mode: %u" TEXT_NORMAL "\n", 1);
    }
    printf("\n");
    consoleUpdate(NULL);

    mutexInit(&g_mutex);
    Thread thread;
    if (R_FAILED(threadCreate(&thread, ftp_thread, NULL, NULL, 1024*16, 0x31, 1))) {
        error_loop("threadCreate() failed");
    } else {
        if (R_FAILED(threadStart(&thread))) {
            error_loop("threadStart() failed");
        } else {
            while (appletMainLoop()) {
                padUpdate(&g_pad);
                const u64 kDown = padGetButtonsDown(&g_pad);
                if (kDown & HidNpadButton_Plus) {
                    break;
                }

                processEvents();
                svcSleepThread(1e+9 / 60);
            }
            g_should_exit = 1;
            threadWaitForExit(&thread);
        }
        threadClose(&thread);
    }

    if (g_callback_data) {
        free(g_callback_data);
    }
}

#define TCP_TX_BUF_SIZE (1024 * 64)
#define TCP_RX_BUF_SIZE (1024 * 64)
#define TCP_TX_BUF_SIZE_MAX (1024 * 1024 * 4)
#define TCP_RX_BUF_SIZE_MAX (1024 * 1024 * 4)
#define UDP_TX_BUF_SIZE (0)
#define UDP_RX_BUF_SIZE (0)
#define SB_EFFICIENCY (12)

#define ALIGN_MSS(v) ((((v) + 1500 - 1) / 1500) * 1500)

#define SOCKET_TMEM_SIZE \
    ((((( \
      ALIGN_MSS(TCP_TX_BUF_SIZE_MAX ? TCP_TX_BUF_SIZE_MAX : TCP_TX_BUF_SIZE) \
    + ALIGN_MSS(TCP_RX_BUF_SIZE_MAX ? TCP_RX_BUF_SIZE_MAX : TCP_RX_BUF_SIZE)) \
    + (UDP_TX_BUF_SIZE ? ALIGN_MSS(UDP_TX_BUF_SIZE) : 0)) \
    + (UDP_RX_BUF_SIZE ? ALIGN_MSS(UDP_RX_BUF_SIZE) : 0)) \
    + 0xFFF) &~ 0xFFF) \
    * SB_EFFICIENCY

alignas(0x1000) u8 SOCKET_TRANSFER_MEM[SOCKET_TMEM_SIZE];
const u32 SOCKET_TRANSFER_MEM_SIZE = sizeof(SOCKET_TRANSFER_MEM);

static u32 socketSelectVersion(void) {
    if (hosversionBefore(3,0,0)) {
        return 1;
    } else if (hosversionBefore(4,0,0)) {
        return 2;
    } else if (hosversionBefore(5,0,0)) {
        return 3;
    } else if (hosversionBefore(6,0,0)) {
        return 4;
    } else if (hosversionBefore(8,0,0)) {
        return 5;
    } else if (hosversionBefore(9,0,0)) {
        return 6;
    } else if (hosversionBefore(13,0,0)) {
        return 7;
    } else if (hosversionBefore(16,0,0)) {
        return 8;
    } else /* latest known version */ {
        return 9;
    }
}

void userAppInit(void) {
    Result rc;

    // https://github.com/mtheall/ftpd/blob/e27898f0c3101522311f330e82a324861e0e3f7e/source/switch/init.c#L31
    // this allocates 96MiB of memory (4Mib * 2 * 12)
    const SocketInitConfig socket_config = {
        .tcp_tx_buf_size     = TCP_TX_BUF_SIZE,
        .tcp_rx_buf_size     = TCP_RX_BUF_SIZE,
        .tcp_tx_buf_max_size = TCP_TX_BUF_SIZE_MAX,
        .tcp_rx_buf_max_size = TCP_RX_BUF_SIZE_MAX,
        .udp_tx_buf_size     = UDP_TX_BUF_SIZE,
        .udp_rx_buf_size     = UDP_RX_BUF_SIZE,
        .sb_efficiency       = SB_EFFICIENCY,
        .num_bsd_sessions    = 2,
        .bsd_service_type    = BsdServiceType_Auto,
    };

    const BsdInitConfig bsd_config = {
        .version             = socketSelectVersion(),

        .tmem_buffer         = SOCKET_TRANSFER_MEM,
        .tmem_buffer_size    = sizeof(SOCKET_TRANSFER_MEM),

        .tcp_tx_buf_size     = socket_config.tcp_tx_buf_size,
        .tcp_rx_buf_size     = socket_config.tcp_rx_buf_size,
        .tcp_tx_buf_max_size = socket_config.tcp_tx_buf_max_size,
        .tcp_rx_buf_max_size = socket_config.tcp_rx_buf_max_size,

        .udp_tx_buf_size     = socket_config.udp_tx_buf_size,
        .udp_rx_buf_size     = socket_config.udp_rx_buf_size,

        .sb_efficiency       = socket_config.sb_efficiency,
    };

    if (R_FAILED(rc = appletLockExit()))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = bsdInitialize(&bsd_config, socket_config.num_bsd_sessions, socket_config.bsd_service_type)))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = socketInitialize(&socket_config)))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = nifmInitialize(NifmServiceType_User)))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = accountInitialize(IsApplication() ? AccountServiceType_Application : AccountServiceType_System)))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = ncmInitialize()))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = setInitialize()))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = fsdev_wrapMountSdmc()))
        diagAbortWithResult(rc);

    // the below doesnt matter if they fail to init.
    hidsysInitialize();
    appletSetAutoSleepDisabled(true);
    consoleInit(NULL);
}

void userAppExit(void) {
    log_file_exit();
    vfs_nx_exit();
    consoleExit(NULL);

    hidsysExit();
    setExit();
    ncmExit();
    accountExit();
    socketExit();
    bsdExit();
    nifmExit();
    fsdev_wrapUnmountAll();
    appletSetAutoSleepDisabled(false);
    appletUnlockExit();
}
