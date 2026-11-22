/* MIT License. Copyright (c) 2026 ACoward. See LICENSE. */
#ifndef ACESMS_FM_H
#define ACESMS_FM_H

#include <stdint.h>

#if defined(_MSC_VER)
#define ACESMS_FM_CALL __cdecl
#else
#define ACESMS_FM_CALL
#endif
#if defined(_WIN32) && defined(ACESMS_FM_BUILD_PLUGIN)
#define ACESMS_FM_EXPORT __declspec(dllexport)
#else
#define ACESMS_FM_EXPORT
#endif
#define ACESMS_FM_ABI_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push,8)

typedef struct acesms_fm_instance acesms_fm_instance;
typedef int32_t acesms_fm_result;
enum {
    ACESMS_FM_OK=0,
    ACESMS_FM_INVALID_ARGUMENT=1,
    ACESMS_FM_UNSUPPORTED_CONFIG=2,
    ACESMS_FM_BUFFER_TOO_SMALL=3,
    ACESMS_FM_INVALID_STATE=4,
    ACESMS_FM_OUT_OF_MEMORY=5
};
typedef struct acesms_fm_config {
    uint32_t struct_size;
    uint32_t master_clock_hz;
    uint32_t native_divider;
} acesms_fm_config;
typedef struct acesms_fm_load_context {
    uint32_t struct_size;
    uint32_t master_clock_hz;
    uint64_t master_ticks;
    uint32_t native_divider;
} acesms_fm_load_context;
typedef struct acesms_fm_descriptor {
    uint8_t backend_id[16];
    char name[64];
    char revision[48];
    uint32_t state_version;
    uint8_t compatibility_id[16];
    uint32_t master_clock_hz;
    uint32_t native_divider;
} acesms_fm_descriptor;
typedef struct acesms_fm_api {
    uint32_t abi_version;
    uint32_t struct_size;
    acesms_fm_descriptor descriptor;
    acesms_fm_result (ACESMS_FM_CALL *create)(const acesms_fm_config*,acesms_fm_instance**);
    void (ACESMS_FM_CALL *destroy)(acesms_fm_instance*);
    acesms_fm_result (ACESMS_FM_CALL *reset)(acesms_fm_instance*);
    acesms_fm_result (ACESMS_FM_CALL *advance_to)(acesms_fm_instance*,uint64_t,int32_t*,uint32_t,uint32_t*);
    acesms_fm_result (ACESMS_FM_CALL *write_io)(acesms_fm_instance*,uint32_t,uint8_t);
    acesms_fm_result (ACESMS_FM_CALL *state_size)(acesms_fm_instance*,uint32_t*);
    acesms_fm_result (ACESMS_FM_CALL *save_state)(acesms_fm_instance*,uint8_t*,uint32_t,uint32_t*);
    acesms_fm_result (ACESMS_FM_CALL *load_state)(acesms_fm_instance*,const uint8_t*,uint32_t,const acesms_fm_load_context*);
} acesms_fm_api;

#pragma pack(pop)
typedef const acesms_fm_api* (ACESMS_FM_CALL *acesms_fm_get_api_fn)(uint32_t requested_abi);
ACESMS_FM_EXPORT const acesms_fm_api* ACESMS_FM_CALL acesms_fm_get_api(uint32_t requested_abi);
#ifdef __cplusplus
}
#endif
#endif
