/**
 * Copyright 2024 TotalJustice.
 * SPDX-License-Identifier: MIT
 */
#include "ftpsrv.h"
#include "args/args.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#define TEXT_NORMAL "\033[0m"
#define TEXT_RED "\033[0;31m"
#define TEXT_GREEN "\033[0;32m"
#define TEXT_YELLOW "\033[0;33m"
#define TEXT_BLUE "\033[0;34m"

enum ArgsId {
    ArgsId_help,
    ArgsId_version,
    ArgsId_port,
    ArgsId_user,
    ArgsId_pass,
    ArgsId_anon,
    ArgsId_timeout,
    ArgsId_localtime
};

#define ARGS_ENTRY(_key, _type, _single) \
    { .key = #_key, .id = ArgsId_##_key, .type = _type, .single = _single },

static const struct ArgsMeta ARGS_META[] = {
    ARGS_ENTRY(help, ArgsValueType_NONE, 'h')
    ARGS_ENTRY(version, ArgsValueType_NONE, 'v')
    ARGS_ENTRY(port, ArgsValueType_INT, 'P')
    ARGS_ENTRY(user, ArgsValueType_STR, 'u')
    ARGS_ENTRY(pass, ArgsValueType_STR, 'p')
    ARGS_ENTRY(anon, ArgsValueType_BOOL, 'a')
    ARGS_ENTRY(timeout, ArgsValueType_INT, 't')
    ARGS_ENTRY(localtime, ArgsValueType_BOOL, 0)
};

static void ftp_log_callback(enum FTP_API_LOG_TYPE type, const char* msg) {
    switch (type) {
        case FTP_API_LOG_TYPE_COMMAND:
            printf(TEXT_BLUE "Command:  %s" TEXT_NORMAL "\n", msg);
            break;
        case FTP_API_LOG_TYPE_RESPONSE:
            printf(TEXT_GREEN "Response: %s" TEXT_NORMAL "\n", msg);
            break;
        case FTP_API_LOG_TYPE_ERROR:
            printf(TEXT_RED "Error:    %s" TEXT_NORMAL "\n", msg);
            break;
    }
}

static int print_usage(int code) {
    printf("\
[ftpsrv " FTPSRV_VERSION_STR " By TotalJustice] \n\n\
Usage\n\n\
    -h, --help      = Display help.\n\
    -v, --version   = Display version.\n\
    -P, --port      = Set port.\n\
    -u, --user      = Set username.\n\
    -p, --pass      = Set password.\n\
    -a, --anon      = Enable anonymous login.\n\
    -t, --timeout   = Set session timeout in seconds.\n\
    --localtime     = Use local time over gm time.\n\
    \n");

    return code;
}

int main(int argc, char** argv) {
    struct FtpSrvConfig ftpsrv_config = {
        .log_callback = ftp_log_callback,
    };

    int arg_index = 1;
    struct ArgsData arg_data;
    enum ArgsResult arg_result;
    while (!(arg_result = args_parse(&arg_index, argc, argv, ARGS_META, sizeof(ARGS_META) / sizeof(ARGS_META[0]), &arg_data))) {
        switch (ARGS_META[arg_data.meta_index].id) {
            case ArgsId_help:
            case ArgsId_version:
                return print_usage(EXIT_SUCCESS);
            case ArgsId_port:
                ftpsrv_config.port = arg_data.value.i;
                break;
            case ArgsId_user:
                snprintf(ftpsrv_config.user, sizeof(ftpsrv_config.user), arg_data.value.s);
                break;
            case ArgsId_pass:
                snprintf(ftpsrv_config.pass, sizeof(ftpsrv_config.pass), arg_data.value.s);
                break;
            case ArgsId_anon:
                ftpsrv_config.anon = arg_data.value.b;
                break;
            case ArgsId_timeout:
                ftpsrv_config.timeout = arg_data.value.i;
                break;
            case ArgsId_localtime:
                ftpsrv_config.use_localtime = arg_data.value.b;
                break;
        }
    }

    // handle error.
    if (arg_result < 0) {
        if (arg_result == ArgsResult_UNKNOWN_KEY) {
            fprintf(stderr, "unknown arg [%s]\n", argv[arg_index]);
        }
        else if (arg_result == ArgsResult_BAD_VALUE) {
            fprintf(stderr, "arg [--%s] had bad value type [%s]\n", ARGS_META[arg_data.meta_index].key, arg_data.value.s);
        }
        else if (arg_result == ArgsResult_MISSING_VALUE) {
            fprintf(stderr, "arg [--%s] requires a value\n", ARGS_META[arg_data.meta_index].key);
        }
        else {
            fprintf(stderr, "bad args: %d\n", arg_result);
        }
        return print_usage(EXIT_FAILURE);
    }

    if (!ftpsrv_config.port) {
        fprintf(stderr, "port not set\n");
        return EXIT_FAILURE;
    }

    if (!strlen(ftpsrv_config.user) && !strlen(ftpsrv_config.pass) && !ftpsrv_config.anon) {
        fprintf(stderr, "User / Pass / Anon not set\n");
        return EXIT_FAILURE;
    }

    unsigned ip = gethostid();
    ip = ((ip & 0xFFFF) << 16) | ((ip >> 16) & 0xFFFF);
    struct in_addr addr = {ip};
    printf(TEXT_YELLOW "ip: %s\n", inet_ntoa(addr));
    printf(TEXT_YELLOW "port: %d" TEXT_NORMAL "\n", ftpsrv_config.port);
    if (ftpsrv_config.anon) {
        printf(TEXT_YELLOW "anon: %d" TEXT_NORMAL "\n", 1);
    } else {
        printf(TEXT_YELLOW "user: %s" TEXT_NORMAL "\n", ftpsrv_config.user);
        printf(TEXT_YELLOW "pass: %s" TEXT_NORMAL "\n", ftpsrv_config.pass);
    }
    printf(TEXT_YELLOW "timeout: %us" TEXT_NORMAL "\n", ftpsrv_config.timeout);
    printf(TEXT_YELLOW "use_locatime: %us" TEXT_NORMAL "\n", ftpsrv_config.use_localtime);

    int timeout = -1;
    if (ftpsrv_config.timeout) {
        timeout = 1000 * ftpsrv_config.timeout;
    }

    while (1) {
        ftpsrv_init(&ftpsrv_config);
        while (1) {
            if (ftpsrv_loop(timeout) != FTP_API_LOOP_ERROR_OK) {
                sleep(1);
                break;
            }
        }
        ftpsrv_exit();
    }
}
