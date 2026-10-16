#pragma once
int sceNetCtlInit(void); int sceNetCtlTerm(void);
enum { SCE_NETCTL_STATE_DISCONNECTED, SCE_NETCTL_STATE_CONNECTING, SCE_NETCTL_STATE_FINALIZING, SCE_NETCTL_STATE_CONNECTED };
int sceNetCtlInetGetState(int *state);
