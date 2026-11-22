#include "core/audio/BuiltinFm.h"
extern "C" const acesms_fm_api* sdk_test_builtin(uint32_t version) {return smsgg::builtinFmApi(version);}
