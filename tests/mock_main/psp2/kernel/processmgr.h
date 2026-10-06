#pragma once
#include <psp2/types.h>
typedef enum { SCE_KERNEL_POWER_TICK_DEFAULT = 0, SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND = 1, SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF = 4, SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING = 6 } SceKernelPowerTickType;
unsigned long long sceKernelGetProcessTimeWide(void); int sceKernelExitProcess(int r); int sceKernelPowerTick(SceKernelPowerTickType t);
