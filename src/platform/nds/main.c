#include <nds.h>
#include <fat.h>
#include <dswifi9.h>
#include <netinet/in.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include <ftpsrv.h>
#include <minIni.h>
#include "log/log.h"

#define TEXT_NORMAL "\033[37;1m"
#define TEXT_RED "\033[31;1m"
#define TEXT_GREEN "\033[32;1m"
#define TEXT_YELLOW "\033[33;1m"
#define TEXT_BLUE "\033[34;1m"

static const char* INI_PATH = "/config/ftpsrv/config.ini";
static const char* LOG_PATH = "/config/ftpsrv/log.txt";
static struct FtpSrvConfig g_ftpsrv_config = {0};
static bool g_ftp_init = false;

static PrintConsole topScreen;
static PrintConsole bottomScreen;

static void ftp_log_callback(enum FTP_API_LOG_TYPE type, const char* msg) {
    log_file_write(msg);
    consoleSelect(&bottomScreen);
    switch (type) {
        case FTP_API_LOG_TYPE_COMMAND:
            iprintf(TEXT_BLUE "Command:  %s" TEXT_NORMAL "\n", msg);
            break;
        case FTP_API_LOG_TYPE_RESPONSE:
            iprintf(TEXT_GREEN "Response: %s" TEXT_NORMAL "\n", msg);
            break;
        case FTP_API_LOG_TYPE_ERROR:
            iprintf(TEXT_RED "Error:    %s" TEXT_NORMAL "\n", msg);
            break;
    }
}

static void poll_ftp(void) {
    if (!g_ftp_init) {
        if (FTP_API_LOOP_ERROR_OK == ftpsrv_init(&g_ftpsrv_config)) {
            g_ftp_init = true;
        }
    }

    if (g_ftp_init) {
        if (ftpsrv_loop(500) != FTP_API_LOOP_ERROR_OK) {
            ftpsrv_exit();
            g_ftp_init = false;
        }
    }
}

static void consolePrint(const char* fmt, ...) {
    consoleSelect(&topScreen);
    va_list va;
    va_start(va, fmt);
    vprintf(fmt, va);
    va_end(va);
    swiWaitForVBlank();
}

static int error_loop(const char* msg) {
    consoleSelect(&topScreen);
    iprintf("Error: %s\n\n", msg);
    iprintf("Modify the config at: %s\n\n", INI_PATH);
    iprintf("\tPress (+) to exit...\n");

    while (pmMainLoop()) {
        scanKeys();
        const u32 kdown = keysDown();
        if (kdown & KEY_START) {
            break;
        }
        swiWaitForVBlank();
    }

    return EXIT_FAILURE;
}

int main(void) {
    // for faster transfers.
    // note: found no speed difference.
    // setCpuClock(true);

    // we don't use sound (yet).
    powerOff(PM_SOUND_AMP);
	powerOn(PM_SOUND_MUTE);
    // we don't use the 3d engine.
    powerOff(POWER_MATRIX | POWER_3D_CORE);

	videoSetMode(MODE_0_2D);
	videoSetModeSub(MODE_0_2D);

	vramSetBankA(VRAM_A_MAIN_BG);
	vramSetBankC(VRAM_C_SUB_BG);

    // init console for drawing.
	consoleInit(&topScreen, 3,BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
	consoleInit(&bottomScreen, 3,BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);

    consolePrint("\n[ftpsrv " FTPSRV_VERSION_STR " By TotalJustice]\n\n");

    // init sd card.
    if (!fatInitDefault()) {
        return error_loop("failed to init fat device\n");
    }

    g_ftpsrv_config.log_callback = ftp_log_callback;
    g_ftpsrv_config.anon = ini_getbool("Login", "anon", 0, INI_PATH);
    const int user_len = ini_gets("Login", "user", "", g_ftpsrv_config.user, sizeof(g_ftpsrv_config.user), INI_PATH);
    const int pass_len = ini_gets("Login", "pass", "", g_ftpsrv_config.pass, sizeof(g_ftpsrv_config.pass), INI_PATH);
    g_ftpsrv_config.port = ini_getl("Network", "port", 21, INI_PATH);
    g_ftpsrv_config.timeout = ini_getl("Network", "timeout", 0, INI_PATH);
    g_ftpsrv_config.use_localtime = ini_getbool("Misc", "use_localtime", 0, INI_PATH);
    const bool log_enabled = ini_getbool("Log", "log", 0, INI_PATH);

    if (log_enabled) {
        log_file_init(LOG_PATH, "ftpsrv - " FTPSRV_VERSION_HASH " - NDS");
    }

    if (!user_len && !pass_len) {
        g_ftpsrv_config.anon = true;
    }

    if (!g_ftpsrv_config.port) {
        return error_loop("Network: Invalid port");
    }

    // init wifi so that it connects in the background.
    if (!Wifi_InitDefault(INIT_ONLY)) {
        return error_loop("Network: Wifi_InitDefault()");
    }
    Wifi_AutoConnect();

    consolePrint("Waiting for Network...\n\n");
    bool has_net = false;

	while (pmMainLoop()) {
        const int status = Wifi_AssocStatus();
        if (!has_net && status == ASSOCSTATUS_ASSOCIATED) {
            consoleSelect(&topScreen);
            const struct in_addr addr = {Wifi_GetIP()};
            iprintf(TEXT_YELLOW "ip: %s\n", inet_ntoa(addr));
            iprintf(TEXT_YELLOW "port: %d" TEXT_NORMAL "\n", g_ftpsrv_config.port);
            if (g_ftpsrv_config.anon) {
                iprintf(TEXT_YELLOW "anon: %d" TEXT_NORMAL "\n", 1);
            } else {
                iprintf(TEXT_YELLOW "user: %s" TEXT_NORMAL "\n", g_ftpsrv_config.user);
                iprintf(TEXT_YELLOW "pass: %s" TEXT_NORMAL "\n", g_ftpsrv_config.pass);
            }
            iprintf(TEXT_YELLOW "timeout: %us" TEXT_NORMAL "\n", g_ftpsrv_config.timeout);
            iprintf(TEXT_YELLOW "use_localtime: %u" TEXT_NORMAL "\n", g_ftpsrv_config.use_localtime);
            iprintf(TEXT_YELLOW "log: %d" TEXT_NORMAL "\n", log_enabled);
            iprintf(TEXT_YELLOW "\nconfig: %s" TEXT_NORMAL "\n", INI_PATH);
            iprintf("\n");
            has_net = true;
        }
        else if (has_net && status == ASSOCSTATUS_DISCONNECTED) {
            ftpsrv_exit();
            has_net = false;
        }

        scanKeys();
        const u32 kdown = keysDown();
        if (kdown & KEY_START) {
            break;
        }

        if (has_net) {
            poll_ftp();
        }
        else {
           swiWaitForVBlank();
        }
	}

    ftpsrv_exit();
    log_file_exit();
    return EXIT_SUCCESS;
}
