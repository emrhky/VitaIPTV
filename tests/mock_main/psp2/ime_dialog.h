#pragma once
#include <psp2/types.h>
#include <psp2/common_dialog.h>
#include <string.h>
#define SCE_IME_LANGUAGE_ENGLISH 0x00000004ULL
enum { SCE_IME_TYPE_DEFAULT = 0 };
enum { SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT = 0, SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD = 1, SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR = 2 };
enum { SCE_IME_DIALOG_BUTTON_NONE = 0, SCE_IME_DIALOG_BUTTON_CLOSE = 1, SCE_IME_DIALOG_BUTTON_ENTER = 2 };
typedef int (*SceImeTextFilter)(SceWChar16 *, unsigned *, const SceWChar16 *, unsigned);
typedef struct { unsigned sdkVersion, inputMethod; unsigned long long supportedLanguages; SceBool languagesForced; unsigned type, option;
                 SceImeTextFilter filter; unsigned dialogMode, textBoxMode; const SceWChar16 *title; unsigned maxTextLength;
                 SceWChar16 *initialText, *inputTextBuffer; SceCommonDialogParam commonParam; unsigned char enterLabel; char reserved[35]; } SceImeDialogParam;
typedef struct { int result, button; char reserved[28]; } SceImeDialogResult;
static inline void sceImeDialogParamInit(SceImeDialogParam *p) { memset(p, 0, sizeof *p); }
int sceImeDialogInit(const SceImeDialogParam *p); SceCommonDialogStatus sceImeDialogGetStatus(void); int sceImeDialogGetResult(SceImeDialogResult *r); int sceImeDialogTerm(void);
