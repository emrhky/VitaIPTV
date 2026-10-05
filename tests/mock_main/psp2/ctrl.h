#pragma once
typedef struct { unsigned long long timeStamp; unsigned buttons; unsigned char lx, ly, rx, ry; } SceCtrlData;
enum { SCE_CTRL_SELECT = 1, SCE_CTRL_START = 8, SCE_CTRL_UP = 16, SCE_CTRL_RIGHT = 32, SCE_CTRL_DOWN = 64, SCE_CTRL_LEFT = 128,
       SCE_CTRL_LTRIGGER = 256, SCE_CTRL_RTRIGGER = 512, SCE_CTRL_TRIANGLE = 4096, SCE_CTRL_CIRCLE = 8192, SCE_CTRL_CROSS = 16384, SCE_CTRL_SQUARE = 32768 };
#define SCE_CTRL_MODE_ANALOG 1
int sceCtrlPeekBufferPositive(int port, SceCtrlData *d, int n); int sceCtrlSetSamplingMode(int m);
