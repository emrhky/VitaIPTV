#pragma once
#include <stdint.h>
typedef struct { unsigned workBufSize; uint8_t reserved[60]; } SceAppUtilInitParam;
typedef struct { unsigned attr, appVersion; uint8_t reserved[32]; } SceAppUtilBootParam;
int sceAppUtilInit(SceAppUtilInitParam *i, SceAppUtilBootParam *b);
int sceAppUtilSystemParamGetInt(unsigned int id, int *v);
