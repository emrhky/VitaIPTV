#pragma once
#include <psp2/io/stat.h>
typedef struct { SceIoStat d_stat; char d_name[256]; void *d_private; int dummy; } SceIoDirent;
SceUID sceIoDopen(const char *p); int sceIoDread(SceUID fd, SceIoDirent *d); int sceIoDclose(SceUID fd);
