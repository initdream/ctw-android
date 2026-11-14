#ifndef __CTW_IMPORTS_H__
#define __CTW_IMPORTS_H__

#include <AL/al.h>
#include <AL/alc.h>

#include "so_util.h"

#ifdef __cplusplus
extern "C" {
#endif

extern DynLibFunction dynlib_functions[];
extern const int dynlib_functions_count;

void ctw_imports_init(void);

void deinit_openal(void);

void ctw_audio_prewarm(void);

extern ALCcontext *al_hook_ctx;
extern ALCdevice  *al_hook_dev;

#ifdef __cplusplus
}
#endif

#endif
