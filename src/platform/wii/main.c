#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <wiiuse/wpad.h>
#include <network.h>
#include <fat.h>

#include <ogc/mutex.h>
#include <ogc/system.h>

#include <ftpsrv.h>
#include <minIni.h>
#include "log/log.h"

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;
static volatile bool g_net_init = false;

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

static mutex_t g_mutex;
static struct CallbackData* g_callback_data = NULL;
static u32 g_num_events = 0;
static volatile bool g_should_exit = false;

static void ftp_log_callback(enum FTP_API_LOG_TYPE type, const char* msg) {
    LWP_MutexLock(g_mutex);
        g_num_events++;
        g_callback_data = realloc(g_callback_data, g_num_events * sizeof(*g_callback_data));
        g_callback_data[g_num_events-1].type = type;
        strcpy(g_callback_data[g_num_events-1].msg, msg);
    LWP_MutexUnlock(g_mutex);
}

static void processEvents(void) {
    LWP_MutexLock(g_mutex);
    if (g_num_events) {
        for (int i = 0; i < g_num_events; i++) {
            log_file_write(g_callback_data[i].msg);

            switch (g_callback_data[i].type) {
                case FTP_API_LOG_TYPE_COMMAND:
                    iprintf(TEXT_BLUE "Command:  %s" TEXT_NORMAL "\n", g_callback_data[i].msg);
                    break;
                case FTP_API_LOG_TYPE_RESPONSE:
                    iprintf(TEXT_GREEN "Response: %s" TEXT_NORMAL "\n", g_callback_data[i].msg);
                    break;
                case FTP_API_LOG_TYPE_ERROR:
                    iprintf(TEXT_RED "Error:    %s" TEXT_NORMAL "\n", g_callback_data[i].msg);
                    break;
            }
        }

        g_num_events = 0;
        free(g_callback_data);
        g_callback_data = NULL;
    }
    LWP_MutexUnlock(g_mutex);
}

static void* ftp_thread(void* arg) {
    while (!g_should_exit) {
        if (0 > net_get_status()) {
            usleep(100000);
            continue;
        }

        ftpsrv_init(&g_ftpsrv_config);
        while (!g_should_exit) {
            const int result = ftpsrv_loop(500);
            if (result != FTP_API_LOOP_ERROR_OK) {
                usleep(100000);
                break;
            }
        }
        ftpsrv_exit();
    }

    return NULL;
}

static void consolePrint(const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vprintf(fmt, va);
    va_end(va);
    VIDEO_WaitVSync();
}

static int error_loop(const char* msg) {
    log_file_write(msg);
    iprintf("Error: %s\n\n", msg);
    iprintf("Modify the config at: %s\n\n", INI_PATH);
    iprintf("\tPress (+) to exit...\n");

    while (1) {
        WPAD_ScanPads();
        PAD_ScanPads();

		const u32 wii_down = WPAD_ButtonsDown(0);
        const u16 pad_down = PAD_ButtonsDown(0);

		if ((wii_down & WPAD_BUTTON_HOME) || (pad_down & PAD_BUTTON_MENU)) {
			break;
		}

        VIDEO_WaitVSync();
    }

    return EXIT_FAILURE;
}

int main(void) {
    // Initialise the video system
	VIDEO_Init();

    // This function initialises the attached controllers
	PAD_Init();
    WPAD_Init();

	// Obtain the preferred video mode from the system
	// This will correspond to the settings in the Wii menu
	rmode = VIDEO_GetPreferredMode(NULL);

	// Allocate memory for the display in the uncached region
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

	// Initialise the console, required for iprintf
	console_init(xfb,20,20,rmode->fbWidth,rmode->xfbHeight,rmode->fbWidth*VI_DISPLAY_PIX_SZ);

	// Set up the video registers with the chosen mode
	VIDEO_Configure(rmode);

	// Tell the video hardware where our display memory is
	VIDEO_SetNextFramebuffer(xfb);

	// Make the display visible
	VIDEO_SetBlack(false);

	// Flush the video register changes to the hardware
	VIDEO_Flush();

	// Wait for Video setup to complete
	VIDEO_WaitVSync();
	if(rmode->viTVMode&VI_NON_INTERLACE) VIDEO_WaitVSync();

    consolePrint("\n[ftpsrv " FTPSRV_VERSION_STR " By TotalJustice]\n\n");

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
        log_file_init(LOG_PATH, "ftpsrv - " FTPSRV_VERSION_HASH " - WII");
    }

    if (!user_len && !pass_len) {
        g_ftpsrv_config.anon = true;
    }

    if (!g_ftpsrv_config.port) {
        return error_loop("Network: Invalid port");
    }

	if (0 > net_init_async(NULL, NULL)) {
        return error_loop("network configuration failed!");
    }

    if (0 > LWP_MutexInit(&g_mutex, false)) {
        return error_loop("failed to create mutex!");
    }

    lwp_t thread;
    if (0 > LWP_CreateThread(&thread, ftp_thread, NULL, NULL, 1024*16, 48)) {
        return error_loop("LWP_CreateThread() failed!");
    }

    consolePrint("Waiting for Network...\n");
    bool has_net = false;

    while (1) {
        if (!has_net && net_get_status() >= 0) {
            const struct in_addr addr = {net_gethostip()};
            iprintf(TEXT_YELLOW "ip: %s\n", inet_ntoa(addr));
            iprintf(TEXT_YELLOW "port: %d" TEXT_NORMAL "\n", g_ftpsrv_config.port);
            if (g_ftpsrv_config.anon) {
                iprintf(TEXT_YELLOW "anon: %d" TEXT_NORMAL "\n", 1);
            } else {
                iprintf(TEXT_YELLOW "user: %s" TEXT_NORMAL "\n", g_ftpsrv_config.user);
                iprintf(TEXT_YELLOW "pass: %s" TEXT_NORMAL "\n", g_ftpsrv_config.pass);
            }
            iprintf(TEXT_YELLOW "log: %d" TEXT_NORMAL "\n", log_enabled);
            iprintf(TEXT_YELLOW "timeout: %us" TEXT_NORMAL "\n", g_ftpsrv_config.timeout);
            iprintf(TEXT_YELLOW "use_locatime: %us" TEXT_NORMAL "\n", g_ftpsrv_config.use_localtime);
            iprintf(TEXT_YELLOW "\nconfig: %s" TEXT_NORMAL "\n", INI_PATH);
            iprintf("\n");
            has_net = true;
        }

        WPAD_ScanPads();
        PAD_ScanPads();

		const u32 wii_down = WPAD_ButtonsDown(0);
        const u16 pad_down = PAD_ButtonsDown(0);

		if ((wii_down & WPAD_BUTTON_HOME) || (pad_down & PAD_BUTTON_MENU)) {
			break;
		}

        processEvents();
        VIDEO_WaitVSync();
    }

    consolePrint("exiting main loop\n");

    g_should_exit = true;
    void* result;
    consolePrint("joining loop\n");
    LWP_JoinThread(thread, &result);
    consolePrint("joined loop\n");
    LWP_MutexDestroy(g_mutex);
    consolePrint("destroyed mutex\n");

    if (g_callback_data) {
        free(g_callback_data);
    }

    consolePrint("free'd data\n");

    net_deinit();
    consolePrint("closed net\n");
    log_file_exit();

    return EXIT_SUCCESS;
}
