#pragma once
typedef enum { SCE_COMMON_DIALOG_STATUS_NONE = 0, SCE_COMMON_DIALOG_STATUS_RUNNING = 1, SCE_COMMON_DIALOG_STATUS_FINISHED = 2 } SceCommonDialogStatus;
typedef struct { unsigned char data[0x2C]; } SceCommonDialogParam;
