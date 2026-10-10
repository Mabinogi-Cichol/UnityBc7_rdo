#define RGBM_COORDINATOR_BUILD
#include "detour_coordinator.h"
#include "detours_checked_api.h"
#include <detours.h>
#include <tlhelp32.h>
#include <cstring>

namespace {
constexpr uint32_t max_targets = 8, max_threads = 2048;
volatile LONG busy;
volatile LONG owner, armed;
bool removing, faulted;
bool installed;
RgbmDetourTarget pending[max_targets];
RgbmDetourTarget installed_targets[max_targets];
uint32_t installed_count;
HMODULE retained_modules[max_targets * 2 + 1];
uint32_t retained_count;
uint32_t pending_count;
HANDLE threads[max_threads];
DWORD protection[max_targets];
uint8_t before_bytes[max_targets][32];
void* before_original[max_targets];
RgbmDetourOutcome outcome;
#ifdef RGBM_COORDINATOR_TESTING
RgbmDetourFaults faults{};
#endif
bool executable(void* address, size_t bytes, DWORD* protect = nullptr) {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(address, &m, sizeof(m)) || m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    DWORD p = m.Protect & 0xff;
    if (p != PAGE_EXECUTE_READ && p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY) return false;
    auto offset = uintptr_t(address) - uintptr_t(m.BaseAddress);
    if (offset > m.RegionSize || bytes > m.RegionSize - offset) return false;
    if (protect) *protect = m.Protect;
    return true;
}
bool branch(const uint8_t* e) {
    return e[0] == 0xe9 || e[0] == 0xeb || (e[0] == 0xff && (e[1] == 0x25 || e[1] == 0xe0)) ||
        (e[0] == 0x48 && e[1] == 0xb8 && e[10] == 0xff && e[11] == 0xe0);
}
bool writable_slot(void** slot) {
    MEMORY_BASIC_INFORMATION m{};
    if (!slot || uintptr_t(slot) % alignof(void*) || !VirtualQuery(slot, &m, sizeof(m)) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    DWORD p = m.Protect & 0xff;
    auto offset = uintptr_t(slot) - uintptr_t(m.BaseAddress);
    return (p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY) &&
        offset <= m.RegionSize && sizeof(*slot) <= m.RegionSize - offset;
}
LONG retain_image(void* address, HMODULE* modules, uint32_t& count) {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(address, &m, sizeof(m))) return GetLastError();
    if (m.Type != MEM_IMAGE) return NO_ERROR; // Host-owned generated code has no module reference.
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(address), &module)) return GetLastError();
    modules[count++] = module; return NO_ERROR;
}
LONG preflight() {
    for (uint32_t i = 0; i < pending_count; ++i) {
        auto& t = pending[i]; outcome.failed_index = i;
        if (!writable_slot(t.original) || !t.entry || !t.hook || t.entry == t.hook || !executable(t.entry, 32, &protection[i]) ||
            !executable(t.hook, 1) || (removing ? (!*t.original || *t.original == t.entry) :
            (*t.original != t.entry || memcmp(t.entry, t.expected, 32) || branch(static_cast<uint8_t*>(t.entry))))) return ERROR_INVALID_DATA;
        for (uint32_t j = 0; j < i; ++j) {
            auto a = uintptr_t(t.entry), b = uintptr_t(pending[j].entry);
            if (t.original == pending[j].original || (a > b ? a - b : b - a) < 32) return ERROR_INVALID_DATA;
        }
    }
    outcome.failed_index = UINT32_MAX; return NO_ERROR;
}
LONG collect(uint32_t& count) {
    count = 0;
    auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return GetLastError();
    THREADENTRY32 e{}; e.dwSize = sizeof(e);
    BOOL more = Thread32First(snapshot, &e); LONG error = NO_ERROR;
    if (!more && GetLastError() != ERROR_NO_MORE_FILES) error = GetLastError();
    uint32_t limit = max_threads;
#ifdef RGBM_COORDINATOR_TESTING
    if (faults.thread_capacity && faults.thread_capacity < max_threads) limit = faults.thread_capacity;
#endif
    while (more) {
        if (e.th32OwnerProcessID == GetCurrentProcessId() && e.th32ThreadID != owner) {
            if (count == limit) { error = ERROR_INSUFFICIENT_BUFFER; break; }
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION | SYNCHRONIZE,
                FALSE, e.th32ThreadID);
            if (!h) { if (GetLastError() != ERROR_INVALID_PARAMETER) { error = GetLastError(); break; } }
            else {
                DWORD pid = GetProcessIdOfThread(h);
                if (!pid) { error = GetLastError(); CloseHandle(h); break; }
                if (pid != GetCurrentProcessId()) CloseHandle(h);
                else threads[count++] = h;
            }
        }
        e.dwSize = sizeof(e); more = Thread32Next(snapshot, &e);
    }
    if (!more && !error && GetLastError() != ERROR_NO_MORE_FILES) error = GetLastError();
    CloseHandle(snapshot); return error;
}
void verify_restored(bool check_code, bool to_original) {
    for (uint32_t i = 0; i < pending_count; ++i) {
        MEMORY_BASIC_INFORMATION m{};
        const auto* bytes = to_original ? pending[i].expected : before_bytes[i];
        void* original = to_original ? pending[i].entry : before_original[i];
        if ((check_code && (memcmp(pending[i].entry,bytes,32) || *pending[i].original != original)) ||
            !VirtualQuery(pending[i].entry, &m, sizeof(m)) || m.Protect != protection[i]) outcome.recovery_error = ERROR_INVALID_DATA;
    }
}
RgbmDetourOutcome run(const RgbmDetourTarget* targets, uint32_t count, bool detach, bool stopped) {
    RgbmDetourOutcome rejected{RgbmDetourState::Rejected, ERROR_INVALID_STATE, 0, UINT32_MAX, 0, 0};
    if (InterlockedCompareExchange(&busy, 1, 0)) { rejected.error = ERROR_BUSY; return rejected; }
    if (faulted || !targets || !count || count > max_targets || (detach && !stopped)) {
        if (faulted) rejected.state = RgbmDetourState::Faulted;
        InterlockedExchange(&busy, 0); return rejected;
    }
    if (installed != detach) {
        if (installed) rejected.state = RgbmDetourState::Installed;
        InterlockedExchange(&busy, 0); return rejected;
    }
    if (detach) {
        bool same = count == installed_count;
        for (uint32_t i = 0; same && i < count; ++i) {
            const auto& a = targets[i]; const auto& b = installed_targets[i];
            same = a.entry == b.entry && a.hook == b.hook && a.original == b.original && !memcmp(a.expected,b.expected,32);
        }
        if (!same) { InterlockedExchange(&busy,0); return rejected; }
    }
    InterlockedExchange(&owner,LONG(GetCurrentThreadId())); pending_count = count; removing = detach;
    memcpy(pending, targets, count * sizeof(*targets)); outcome = rejected; outcome.error = 0; outcome.state = RgbmDetourState::Unavailable;
    HMODULE self = nullptr, already = nullptr;
    wchar_t path[32768]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&RgbmInstallDetours), &self)) outcome.error = GetLastError();
    DWORD length = outcome.error ? 0 : GetModuleFileNameW(self, path, 32768);
    if (!outcome.error && (!length || length >= 32768)) outcome.error = ERROR_BAD_PATHNAME;
    wchar_t* slash = length ? wcsrchr(path, L'\\') : nullptr;
    const wchar_t name[] = L"bc7_rgbm_loader_gate.dll";
    if (!outcome.error && (!slash || size_t(slash - path) + _countof(name) + 1 > _countof(path))) outcome.error = ERROR_BAD_PATHNAME;
    if (!outcome.error) wcscpy(slash + 1, name);
    if (!outcome.error && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, path, &already)) outcome.error = ERROR_ALREADY_EXISTS;
    // All path lookup and heavy caller-side identity validation are outside the gate.
    if (!outcome.error) outcome.error = preflight();
    HMODULE holds[max_targets * 2 + 1]{}; uint32_t holds_count = 0;
    if (!detach && !outcome.error) {
        outcome.error = retain_image(reinterpret_cast<void*>(&RgbmInstallDetours), holds, holds_count);
        for (uint32_t i = 0; !outcome.error && i < count; ++i) {
            outcome.error = retain_image(pending[i].hook, holds, holds_count);
            if (!outcome.error) outcome.error = retain_image(pending[i].entry, holds, holds_count);
        }
    }
    HMODULE gate = nullptr;
    if (!outcome.error) {
        InterlockedExchange(&armed,1);
        gate = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        InterlockedExchange(&armed,0);
        if (!gate) outcome.error = GetLastError();
        else if (!outcome.loader_gate_entered) outcome.error = ERROR_INVALID_STATE;
    }
    if (gate && !FreeLibrary(gate)) { outcome.recovery_error = GetLastError(); outcome.state = RgbmDetourState::Faulted; }
    if (detach && outcome.state == RgbmDetourState::Unavailable) outcome.state = RgbmDetourState::Installed;
    if (outcome.state == RgbmDetourState::Faulted) faulted = true;
    if (!detach && (outcome.state == RgbmDetourState::Installed || faulted)) {
        installed = true; installed_count = count; memcpy(installed_targets,pending,count*sizeof(*pending));
        retained_count = holds_count; memcpy(retained_modules,holds,holds_count*sizeof(*holds)); holds_count = 0;
    } else if (outcome.state == RgbmDetourState::Removed) {
        installed = false; installed_count = 0;
        for (uint32_t i=0;i<retained_count;++i) FreeLibrary(retained_modules[i]); retained_count = 0;
    }
    for (uint32_t i=0;i<holds_count;++i) FreeLibrary(holds[i]);
    auto result = outcome; InterlockedExchange(&owner,0); pending_count = 0; InterlockedExchange(&busy, 0); return result;
}
}
extern "C" __declspec(dllexport) void WINAPI RgbmCommitFromLoaderGate() {
    if (!InterlockedCompareExchange(&armed,0,0) || GetCurrentThreadId() != DWORD(InterlockedCompareExchange(&owner,0,0)) || outcome.loader_gate_entered) return;
    outcome.loader_gate_entered = 1;
    uint32_t count = 0;
    outcome.error = preflight();
    if (!outcome.error) for (uint32_t i=0;i<pending_count;++i) {
        memcpy(before_bytes[i],pending[i].entry,32); before_original[i] = *pending[i].original;
    }
    if (!outcome.error) outcome.error = collect(count);
#ifdef RGBM_COORDINATOR_TESTING
    // A deterministic missed-snapshot birth. Do not wait for it in DllMain.
    if (!outcome.error && faults.after_snapshot_start) {
        HANDLE born = CreateThread(nullptr, 0, faults.after_snapshot_start, faults.after_snapshot_parameter, 0, nullptr);
        if (!born) outcome.error = GetLastError(); else CloseHandle(born);
    }
#endif
    // Allocation and operation records use VirtualAlloc, including after suspend.
#ifdef RGBM_COORDINATOR_TESTING
    rgbm_engine::reset(faults.fail_allocation_at, faults.fail_context_at, faults.fail_restore_protection, faults.report_resume_failure);
#else
    rgbm_engine::reset(0, 0, FALSE, FALSE);
#endif
    bool open = false, committed = false;
    if (!outcome.error) {
        outcome.error = DetourTransactionBegin(); open = !outcome.error;
        if (outcome.error && outcome.error != ERROR_INVALID_OPERATION) outcome.recovery_error = DetourTransactionAbort();
    }
    for (uint32_t i = 0; open && !outcome.error && i < pending_count; ++i) {
        outcome.error = removing ? DetourDetach(pending[i].original, pending[i].hook) : DetourAttach(pending[i].original, pending[i].hook);
        if (outcome.error) outcome.failed_index = i;
    }
    for (uint32_t i = 0; open && !outcome.error && i < count; ++i) {
        DWORD status = WaitForSingleObject(threads[i],0);
        if (status == WAIT_OBJECT_0) continue;
        if (status != WAIT_TIMEOUT) { outcome.error = status == WAIT_FAILED ? GetLastError() : ERROR_INVALID_STATE; break; }
        outcome.error = DetourUpdateThread(threads[i]);
        if (!outcome.error) {
            ++outcome.threads_enlisted;
            CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
            // Upstream ignores GetThreadContext failures during commit: refuse
            // before patching if a suspended thread cannot be inspected.
            if (!rgbm_engine::checked_get_context(threads[i], &context)) outcome.error = GetLastError();
            else if (!SetThreadContext(threads[i], &context)) outcome.error = GetLastError();
        }
    }
    if (open) {
        if (outcome.error) outcome.recovery_error = DetourTransactionAbort();
        else {
            PVOID* failed = nullptr;
            outcome.error = DetourTransactionCommitEx(&failed); committed = !outcome.error;
            for (uint32_t i = 0; i < pending_count; ++i) if (failed == pending[i].original) outcome.failed_index = i;
        }
        if (auto error = rgbm_engine::recovery_error()) outcome.recovery_error = error;
        // A failed removal restores the INSTALLED state, not the unhooked entry.
        // Always check protections, including successful attachment.
        verify_restored(!committed || removing, committed && removing);
    }
    for (uint32_t i = 0; i < count; ++i) CloseHandle(threads[i]);
    if (outcome.recovery_error) outcome.state = RgbmDetourState::Faulted;
    else if (committed) outcome.state = removing ? RgbmDetourState::Removed : RgbmDetourState::Installed;
}
extern "C" RgbmDetourOutcome WINAPI RgbmInstallDetours(const RgbmDetourTarget* t, uint32_t n) { return run(t, n, false, false); }
extern "C" RgbmDetourOutcome WINAPI RgbmRemoveDetoursForStoppedHost(const RgbmDetourTarget* t, uint32_t n, BOOL stopped) { return run(t, n, true, stopped != FALSE); }
#ifdef RGBM_COORDINATOR_TESTING
extern "C" BOOL WINAPI RgbmSetDetourTestFaults(RgbmDetourFaults value) {
    if (InterlockedCompareExchange(&busy, 1, 0)) return FALSE;
    faults = value; InterlockedExchange(&busy, 0); return TRUE;
}
#endif
