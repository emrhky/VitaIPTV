/* Minimal taiHEN declarations for compile checks on a PC (same shapes as vitasdk's taihen.h). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <psp2/types.h>
typedef uintptr_t tai_hook_ref_t;
struct _tai_hook_user { uintptr_t next; void *func; void *old; };
#define TAI_ANY_LIBRARY 0xFFFFFFFF
typedef struct _tai_module_info { size_t size; SceUID modid; uint32_t module_nid; char name[27];
  uintptr_t exports_start, exports_end, imports_start, imports_end; } tai_module_info_t;
int taiGetModuleInfo(const char *module, tai_module_info_t *info);
SceUID taiHookFunctionExport(tai_hook_ref_t *p_hook, const char *module, uint32_t library_nid, uint32_t func_nid, const void *hook_func);
int taiHookRelease(SceUID tai_uid, tai_hook_ref_t hook);
#define TAI_CONTINUE(type, h, ...) ({ \
  struct _tai_hook_user *cur, *next; \
  cur = (struct _tai_hook_user *)(h); \
  next = (struct _tai_hook_user *)cur->next; \
  (next == NULL) ? ((type(*)())cur->old)(__VA_ARGS__) : ((type(*)())next->func)(__VA_ARGS__); \
})
