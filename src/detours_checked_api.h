#pragma once
#include <windows.h>
namespace rgbm_engine {
void reset(LONG allocation_at, LONG context_at, BOOL restore_failure, BOOL resume_failure);
void* allocate(size_t bytes) noexcept;
LONG recovery_error();
BOOL checked_get_context(HANDLE thread, LPCONTEXT context);
BOOL checked_set_context(HANDLE thread, const CONTEXT* context);
BOOL checked_protect(LPVOID address, SIZE_T bytes, DWORD protection, PDWORD old);
BOOL checked_flush(HANDLE process, LPCVOID address, SIZE_T bytes);
DWORD checked_resume(HANDLE thread);
}
