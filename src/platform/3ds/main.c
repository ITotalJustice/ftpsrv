#include <3ds.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <ftpsrv.h>
#include <minIni.h>
#include "log/log.h"

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  (1024*1024*64)
static u32 *SOC_buffer = NULL;

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

static Handle g_mutex;
static struct CallbackData* g_callback_data = NULL;
static u32 g_num_events = 0;
static volatile bool g_should_exit = false;

static PrintConsole topScreen;
static PrintConsole bottomScreen;

static void ftp_log_callback(enum FTP_API_LOG_TYPE type, const char* msg) {
    svcWaitSynchronization(g_mutex, UINT64_MAX);
        g_num_events++;
        g_callback_data = realloc(g_callback_data, g_num_events * sizeof(*g_callback_data));
        g_callback_data[g_num_events-1].type = type;
        strcpy(g_callback_data[g_num_events-1].msg, msg);
    svcReleaseMutex(g_mutex);
}

static void consoleUpdate(void* c) {
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static void processEvents(void) {
    svcWaitSynchronization(g_mutex, UINT64_MAX);
    consoleSelect(&bottomScreen);
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
        consoleUpdate(NULL);
    }
    svcReleaseMutex(g_mutex);
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
    consoleSelect(&topScreen);
    va_list va;
    va_start(va, fmt);
    vprintf(fmt, va);
    va_end(va);
    consoleUpdate(NULL);
}

static int error_loop(const char* msg) {
    consoleSelect(&topScreen);
    log_file_write(msg);
    iprintf("Error: %s\n\n", msg);
    iprintf("Modify the config at: %s\n\n", INI_PATH);
    iprintf("\tPress (+) to exit...\n");
    consoleUpdate(NULL);

    while (aptMainLoop()) {
        hidScanInput();
        const u32 kdown = hidKeysDown();
        if (kdown & KEY_START) {
            break;
        }
        svcSleepThread(1e+9 / 60);
    }

    return EXIT_FAILURE;
}

int main(void) {
    // faster processing.
    osSetSpeedupEnable(true);

    gfxInitDefault();
	consoleInit(GFX_TOP, &topScreen);
	consoleInit(GFX_BOTTOM, &bottomScreen);
    consolePrint("\n[ftpsrv " FTPSRV_VERSION_STR " By TotalJustice]\n\n");

    g_ftpsrv_config.log_callback = ftp_log_callback;
    g_ftpsrv_config.anon = ini_getbool("Login", "anon", 0, INI_PATH);
    const int user_len = ini_gets("Login", "user", "", g_ftpsrv_config.user, sizeof(g_ftpsrv_config.user), INI_PATH);
    const int pass_len = ini_gets("Login", "pass", "", g_ftpsrv_config.pass, sizeof(g_ftpsrv_config.pass), INI_PATH);
    g_ftpsrv_config.port = ini_getl("Network", "port", 21, INI_PATH);
    g_ftpsrv_config.timeout = ini_getl("Network", "timeout", 0, INI_PATH);
    g_ftpsrv_config.use_localtime = ini_getbool("Misc", "use_localtime", 0, INI_PATH);
    const bool log_enabled = ini_getbool("Log", "log", 0, INI_PATH);

    if (log_enabled) {
        log_file_init(LOG_PATH, "ftpsrv - " FTPSRV_VERSION_HASH " - 3DS");
    }

    if (!user_len && !pass_len) {
        g_ftpsrv_config.anon = true;
    }

    if (!g_ftpsrv_config.port) {
        return error_loop("Network: Invalid port");
    }

    SOC_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
	if (!SOC_buffer) {
		return error_loop("memalign: failed to allocate");
	}

	// Now intialise soc:u service
	if (R_FAILED(socInit(SOC_buffer, SOC_BUFFERSIZE))) {
    	return error_loop("socInit");
	}

    const struct in_addr addr = {gethostid()};
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
    consoleUpdate(NULL);

    if (R_FAILED(svcCreateMutex(&g_mutex, false))) {
        return error_loop("failed svcCreateMutex()");
    }

    s32 cpu_id = 0;
	if (R_FAILED(svcGetThreadIdealProcessor(&cpu_id, CUR_THREAD_HANDLE))) {
        return error_loop("failed svcGetThreadIdealProcessor()");
    }

    Thread thread = threadCreate(ftp_thread, NULL, 1024*16, 0x2F, (cpu_id + 1) % 2, false);

	while (aptMainLoop()) {
        hidScanInput();
        const u32 kdown = hidKeysDown();
        if (kdown & KEY_START) {
            break;
        }
        processEvents();
        svcSleepThread(1e+9 / 60);
	}

    g_should_exit = true;
    threadJoin(thread, U64_MAX);
    threadFree(thread);
    svcCloseHandle(g_mutex);
    socExit();
    gfxExit();

    if (g_callback_data) {
        free(g_callback_data);
    }

    log_file_exit();
    return EXIT_SUCCESS;
}
