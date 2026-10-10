#include "detours_checked_api.h"
#include <new>
namespace {
LONG allocation_count, context_count, allocation_failure, context_failure, recovery;
BOOL protect_failure, resume_failure;
void record() { if (!recovery) recovery = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE; }
}
namespace rgbm_engine {
void reset(LONG a, LONG c, BOOL p, BOOL r) {
    allocation_count = context_count = recovery = 0;
    allocation_failure = a; context_failure = c; protect_failure = p; resume_failure = r;
}
void* allocate(size_t bytes) noexcept {
    if (allocation_failure && InterlockedIncrement(&allocation_count) == allocation_failure) return nullptr;
    return VirtualAlloc(nullptr, bytes ? bytes : 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
LONG recovery_error() { return recovery; }
BOOL checked_get_context(HANDLE thread, LPCONTEXT context) {
    if (context_failure && ++context_count == context_failure) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    BOOL ok = GetThreadContext(thread, context); if (!ok) record(); return ok;
}
BOOL checked_set_context(HANDLE thread, const CONTEXT* context) {
    BOOL ok = SetThreadContext(thread, context); if (!ok) record(); return ok;
}
BOOL checked_protect(LPVOID address, SIZE_T bytes, DWORD protection, PDWORD old) {
    if (protect_failure && protection == PAGE_EXECUTE_READ) {
        protect_failure = FALSE; SetLastError(ERROR_ACCESS_DENIED); record(); return FALSE;
    }
    BOOL ok = VirtualProtect(address, bytes, protection, old); if (!ok) record(); return ok;
}
BOOL checked_flush(HANDLE process, LPCVOID address, SIZE_T bytes) {
    BOOL ok = FlushInstructionCache(process, address, bytes); if (!ok) record(); return ok;
}
DWORD checked_resume(HANDLE thread) {
    DWORD result = ResumeThread(thread); if (result == DWORD(-1)) record();
    // Test-only failure reporting still performs the real resume, so the fault
    // test cannot strand host threads. This is NOT a simulated successful API.
    if (resume_failure) { resume_failure = FALSE; SetLastError(ERROR_ACCESS_DENIED); record(); }
    return result;
}
}
// This override is confined to the dedicated coordinator DLL. All its C++
// allocations use kernel virtual memory, never a CRT/process heap lock.
void* operator new(size_t bytes) { if (void* p = rgbm_engine::allocate(bytes)) return p; throw std::bad_alloc(); }
void* operator new[](size_t bytes) { return operator new(bytes); }
void* operator new(size_t bytes, const std::nothrow_t&) noexcept { return rgbm_engine::allocate(bytes); }
void* operator new[](size_t bytes, const std::nothrow_t&) noexcept { return rgbm_engine::allocate(bytes); }
void operator delete(void* p) noexcept { if (p) VirtualFree(p, 0, MEM_RELEASE); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { operator delete(p); }
