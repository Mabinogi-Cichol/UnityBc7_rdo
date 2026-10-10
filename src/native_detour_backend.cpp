#include "native_detour_backend.h"
#include <detours.h>
#include <tlhelp32.h>
#include <malloc.h>
#include <mutex>
#include <cstring>

namespace rgbm {
namespace {
std::mutex transaction_mutex;
class ThreadHandles {
public:
    std::vector<HANDLE> handles;
    std::vector<DWORD> ids;
    ~ThreadHandles() { for (auto handle : handles) CloseHandle(handle); }
    LONG collect() {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return GetLastError();
        THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
        BOOL more = Thread32First(snapshot, &entry);
        LONG error = NO_ERROR;
        if (!more && GetLastError() != ERROR_NO_MORE_FILES) error = GetLastError();
        while (more) {
            if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != GetCurrentThreadId()) {
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                    THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ThreadID);
                if (!thread) {
                    // ERROR_INVALID_PARAMETER is a raced exit, never skip access denial.
                    if (GetLastError() != ERROR_INVALID_PARAMETER) { error = GetLastError(); break; }
                } else {
                    // Snapshot IDs can be reused between enumeration and open.
                    if (GetProcessIdOfThread(thread) != GetCurrentProcessId()) CloseHandle(thread);
                    else { handles.push_back(thread); ids.push_back(entry.th32ThreadID); }
                }
            }
            entry.dwSize = sizeof(entry); more = Thread32Next(snapshot, &entry);
        }
        if (!more && GetLastError() != ERROR_NO_MORE_FILES && !error) error = GetLastError();
        CloseHandle(snapshot); return error;
    }
};
bool executable(void* address, size_t count) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    DWORD protection = info.Protect & 0xff;
    if (protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE &&
        protection != PAGE_EXECUTE_WRITECOPY) return false;
    const auto start = reinterpret_cast<uintptr_t>(address);
    const auto base = reinterpret_cast<uintptr_t>(info.BaseAddress);
    return start >= base && start - base <= info.RegionSize && count <= info.RegionSize - (start - base);
}
bool already_detoured(const uint8_t* entry) {
    // Fail closed on common branch/indirect detours, including Detours thunks.
    return entry[0] == 0xe9 || entry[0] == 0xeb ||
        (entry[0] == 0xff && (entry[1] == 0x25 || entry[1] == 0xe0)) ||
        (entry[0] == 0x48 && entry[1] == 0xb8 && entry[10] == 0xff && entry[11] == 0xe0);
}
DetourResult transact(const std::vector<DetourTarget>& targets, bool quiesced, bool attach) {
    std::lock_guard<std::mutex> guard(transaction_mutex);
    DetourResult result;
    if (!quiesced || targets.empty()) { result.error = ERROR_INVALID_STATE; return result; }
    // Validate the entire batch before touching protections or original slots.
    for (size_t i = 0; i < targets.size(); ++i) {
        const auto& target = targets[i];
        if (!target.original || !target.entry || !target.hook || target.entry == target.hook ||
            !executable(target.entry, target.expected.size()) || !executable(target.hook, 1) ||
            (attach && (*target.original != target.entry ||
                memcmp(target.entry, target.expected.data(), target.expected.size()) ||
                already_detoured(static_cast<const uint8_t*>(target.entry)))) ||
            (!attach && (!*target.original || *target.original == target.entry))) {
            result.error = ERROR_INVALID_DATA; result.failed_index = i; return result;
        }
        for (size_t j = 0; j < i; ++j) {
            const auto a = reinterpret_cast<uintptr_t>(targets[j].entry);
            const auto b = reinterpret_cast<uintptr_t>(target.entry);
            if (targets[j].original == target.original || (a <= b ? b - a : a - b) < 32) {
                result.error = ERROR_INVALID_DATA; result.failed_index = i; return result;
            }
        }
    }
    ThreadHandles threads;
    if ((result.error = threads.collect()) != NO_ERROR) return result;
    if ((result.error = DetourTransactionBegin()) != NO_ERROR) return result;
    bool transaction_open = true;
    auto abort = [&]() {
        result.rollback_error = DetourTransactionAbort(); transaction_open = false;
    };
    // Allocate all trampolines before suspending any worker.
    for (size_t i = 0; i < targets.size(); ++i) {
        const auto& target = targets[i];
        result.error = attach ? DetourAttach(target.original, target.hook) : DetourDetach(target.original, target.hook);
        if (result.error) { result.failed_index = i; abort(); return result; }
    }
    // DetourUpdateThread allocates bookkeeping before suspending each thread.
    // Lock the /MT CRT heap before suspension to prevent suspending a thread
    // while it owns the heap needed by Detours' subsequent allocations.
    HANDLE heap = reinterpret_cast<HANDLE>(_get_heap_handle());
    if (!HeapLock(heap)) { result.error = ERROR_LOCK_FAILED; abort(); return result; }
    for (auto thread : threads.handles) {
        if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) continue;
        result.error = DetourUpdateThread(thread);
        if (result.error) { abort(); break; }
    }
    if (transaction_open) {
        PVOID* failed = nullptr;
        result.error = DetourTransactionCommitEx(&failed); transaction_open = false;
        // CommitEx internally aborts on error. Verify restoration below rather
        // than calling Abort on a transaction that has already ended.
        for (size_t i = 0; i < targets.size(); ++i)
            if (failed == targets[i].original) result.failed_index = i;
    }
    if (!HeapUnlock(heap)) result.rollback_error = ERROR_LOCK_FAILED;
    if (result.error || !attach) {
        for (const auto& target : targets)
            if (attach ? (memcmp(target.entry, target.expected.data(), target.expected.size()) ||
                          *target.original != target.entry) :
                         (result.error == NO_ERROR &&
                          (memcmp(target.entry, target.expected.data(), target.expected.size()) || *target.original != target.entry)))
                result.rollback_error = ERROR_INVALID_DATA;
    }
    result.installed = result.error == NO_ERROR && result.rollback_error == NO_ERROR && attach;
    return result;
}
}
DetourResult install_detours(const std::vector<DetourTarget>& targets, bool quiesced) { return transact(targets, quiesced, true); }
DetourResult remove_detours(const std::vector<DetourTarget>& targets, bool quiesced) { return transact(targets, quiesced, false); }
} // namespace rgbm
