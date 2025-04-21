#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <switch.h>

#define IRAM_PAYLOAD_MAX_SIZE 0x24000
#define AMS_IWRAM_OFFSET 0x40010000

bool validate_payload_from_path(const char* path, bool check_hekate);
bool validate_payload_from_file(FsFile* file, bool check_hekate);
bool is_r2p_supported(void);

void smcRebootToIramPayload(void);
void smcCopyToIram(uintptr_t iram_addr, const void *src_addr, u32 size);

#ifdef __cplusplus
}
#endif
