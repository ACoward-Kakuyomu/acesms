#pragma pack(push,1)
#include "acesms_fm.h"
typedef struct sdk_pack_restored {uint8_t a;uint64_t b;} sdk_pack_restored;
_Static_assert(sizeof(sdk_pack_restored)==9,"SDK must restore caller packing");
#pragma pack(pop)
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(acesms_fm_config)==12,"config ABI");
_Static_assert(sizeof(acesms_fm_load_context)==24,"context ABI");
_Static_assert(sizeof(acesms_fm_descriptor)==156,"descriptor ABI");
_Static_assert(sizeof(acesms_fm_api)==232,"api ABI");
extern const acesms_fm_api* sdk_test_builtin(uint32_t version);

int main(void) {
    const acesms_fm_api* api=sdk_test_builtin(ACESMS_FM_ABI_VERSION);
    const acesms_fm_config config={sizeof(acesms_fm_config),3579545,72};
    const acesms_fm_load_context context={sizeof(acesms_fm_load_context),3579545,73,72};
    acesms_fm_instance* handle=NULL;
    uint8_t* bytes=NULL;
    uint32_t size=0,written=0,count=0;
    int32_t output=12345;
    int result=1;
    if(!api || sdk_test_builtin(0) || api->struct_size!=sizeof(*api) || api->abi_version!=1
        || strcmp(api->descriptor.name,"emu2413 1.6.4")) return 1;
    if(api->create(&config,&handle)!=ACESMS_FM_OK || !handle) goto done;
    if(api->write_io(handle,0,0x30) || api->write_io(handle,1,0x10)
        || api->write_io(handle,0,0x10) || api->write_io(handle,1,0x80)
        || api->write_io(handle,0,0x20) || api->write_io(handle,1,0x19)) goto done;
    if(api->advance_to(handle,73,&output,1,&count) || count!=1 || api->state_size(handle,&size) || size!=3193) goto done;
    bytes=(uint8_t*)malloc(size);if(!bytes) goto done;
    if(api->save_state(handle,bytes,size,&written) || written!=size || api->reset(handle)
        || api->load_state(handle,bytes,size,&context) || api->advance_to(handle,144,&output,1,&count) || count!=1) goto done;
    result=0;
done:
    api->destroy(handle);free(bytes);return result;
}
