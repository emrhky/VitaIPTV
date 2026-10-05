#pragma once
#include <psp2/types.h>
typedef struct { int st_mode; } SceIoStat;
#define SCE_S_ISDIR(m) (((m) & 0xF000) == 0x1000)
int sceIoMkdir(const char *p, int mode);
