#pragma once
typedef struct { void *memory; int size; int flags; } SceNetInitParam;
int sceNetInit(SceNetInitParam *p); int sceNetTerm(void);
