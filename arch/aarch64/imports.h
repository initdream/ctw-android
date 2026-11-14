#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include <AL/al.h>
#include <AL/alc.h>
#include "so_util.h"

extern DynLibFunction dynlib_functions[];
extern const int dynlib_functions_count;

void deinit_openal(void);
void ctw_audio_prewarm(void);

extern ALCcontext *al_hook_ctx;
extern ALCdevice  *al_hook_dev;

#endif