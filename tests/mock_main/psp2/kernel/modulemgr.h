#pragma once
#include <psp2/types.h>
typedef struct SceKernelLMOption SceKernelLMOption;
SceUID sceKernelLoadStartModule(const char *path, SceSize args, void *argp, int flags, SceKernelLMOption *opt, int *status);
