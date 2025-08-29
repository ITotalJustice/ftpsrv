/**
 * Copyright 2024 TotalJustice.
 * SPDX-License-Identifier: MIT
 *
 * Improved and modernized public API surface:
 * - clearer typedef names
 * - fixed-width / size types (stdint.h / stddef.h)
 * - explicit return codes enum
 * - named constant for custom command name length
 * - improved callback and custom-command signatures (size_t for buffer lengths)
 * - small documentation comments for API clarity
 *
 * NOTE: These changes are intended to be backward-compatible where possible,
 * but callers should be updated to use the improved types/consts where needed.
 */

#ifndef FTP_SRV_H
#define FTP_SRV_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Maximum length for a custom command name including terminating NUL.
 * Historically name[5] was used; keep the same maximum (4 chars + NUL). */
#define FTP_SRV_CUSTOM_COMMAND_NAME_MAX 5

/* Generic API result codes returned by ftpsrv functions. */
typedef enum FtpSrvResult {
    FTP_SRV_OK = 0,
    FTP_SRV_ERR_INVALID_ARG = -1,
    FTP_SRV_ERR_NO_MEMORY = -2,
    FTP_SRV_ERR_NOT_INITIALIZED = -3,
    FTP_SRV_ERR_ALREADY_INITIALIZED = -4,
    FTP_SRV_ERR_INTERNAL = -5,
} FtpSrvResult;

/* Log message types delivered to the log callback. */
typedef enum FtpSrvLogType {
    FTP_SRV_LOG_COMMAND,
    FTP_SRV_LOG_RESPONSE,
    FTP_SRV_LOG_ERROR,
} FtpSrvLogType;

/* Loop-level errors returned by ftpsrv_loop(). */
typedef enum FtpSrvLoopError {
    FTP_SRV_LOOP_OK = 0,
    FTP_SRV_LOOP_ERROR_INIT, /* call ftpsrv_exit() and ftpsrv_init() again */
} FtpSrvLoopError;

/* Log callback: message points to a NUL-terminated UTF-8 string. */
typedef void (*FtpSrvLogCallback)(FtpSrvLogType type, const char* message);

/* Progress callback called periodically by the server loop (optional). */
typedef void (*FtpSrvProgressCallback)(void);

/* Custom command function signature:
 * - userdata: opaque pointer from FtpSrvCustomCommand
 * - args: NUL-terminated argument string (may be empty) or NULL if none
 * - reply_buf: buffer to write a NUL-terminated reply message into
 * - reply_buf_len: size of reply_buf in bytes (use size_t)
 *
 * Return value semantics: 0 for success (command handled), non-zero for error.
 */
typedef int (*FtpSrvCustomCommandFunc)(
    void* userdata,
    const char* args,
    char* reply_buf,
    size_t reply_buf_len
);

/* Custom command descriptor. `name` must be ASCII, up to 4 characters (RFC-style).
 * The name array includes space for a terminating NUL (FTP_SRV_CUSTOM_COMMAND_NAME_MAX). */
struct FtpSrvCustomCommand {
    char name[FTP_SRV_CUSTOM_COMMAND_NAME_MAX];
    FtpSrvCustomCommandFunc func;
    void* userdata;
    bool auth_required;
    bool args_required;
};

/* Server configuration structure. Keep fields fixed-size to simplify usage
 * in constrained environments. If a field is unused set it to zero / false / "".
 *
 * Notes:
 * - user / pass are fixed-size buffers. They must be NUL-terminated.
 * - port is a 16-bit value (0 for default behavior if implemented that way).
 * - timeout is in seconds (0 means no timeout).
 * - custom_command_count uses size_t for compatibility with pointer-sized counts.
 */
struct FtpSrvConfig {
    char user[128];
    char pass[128];
    uint16_t port;

    /* Allow anonymous access when true. */
    bool anon;
    /* Disallow uploads when true. */
    bool read_only;
    /* Require an account to store files when true. */
    bool write_account_required;
    /* Use localtime instead of gmtime when true. */
    bool use_localtime;
    /* Session timeout in seconds (0 = disabled). */
    uint32_t timeout;

    /* Optional custom commands. The array may be NULL when count is 0. */
    const struct FtpSrvCustomCommand* custom_command;
    size_t custom_command_count;

    /* Optional callbacks (may be NULL). */
    FtpSrvLogCallback log_callback;
    FtpSrvProgressCallback progress_callback;

    /* Internal/compatibility version: set to 1. Reserved for future extension. */
    uint32_t config_version;
};

/* Initialize the FTP server library with the provided configuration.
 * - cfg must point to a valid FtpSrvConfig structure.
 * - The function typically copies or consumes the configuration; callers
 *   should not modify cfg concurrently while the server is running.
 *
 * Returns FTP_SRV_OK on success or a negative FtpSrvResult on failure.
 */
int ftpsrv_init(const struct FtpSrvConfig* cfg);

/* Run the server loop for up to timeout_ms milliseconds. A value of -1 may
 * indicate blocking behavior depending on the implementation.
 *
 * Returns:
 * - non-negative: number of processed events (implementation-defined)
 * - negative: one of the FtpSrvResult codes (cast to int) or a FtpSrvLoopError
 *   value. Consult implementation docs for exact semantics.
 */
int ftpsrv_loop(int timeout_ms);

/* Gracefully shut down the server and free internal resources. Safe to call
 * even if ftpsrv_init() failed or was never successfully called.
 */
void ftpsrv_exit(void);

#ifdef __cplusplus
}
#endif

#endif /* FTP_SRV_H */
