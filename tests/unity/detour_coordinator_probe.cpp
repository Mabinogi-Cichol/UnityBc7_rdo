// Development-only owned-code test inside the real Editor. Never selects or
// patches a Unity entry; failed cleanup retains generated code until exit.
#include "detour_coordinator.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace {
using Function = int (*)();
Function original;
uint8_t* code;
volatile LONG stopped, paused, active, calls, hook_calls, bad_calls, running;
int hook() { InterlockedIncrement(&hook_calls); return original() + 1; }
DWORD WINAPI worker(void*) {
    while (!InterlockedCompareExchange(&stopped,0,0)) {
        if (InterlockedCompareExchange(&paused,0,0)) { SwitchToThread(); continue; }
        InterlockedIncrement(&active);
        if (!InterlockedCompareExchange(&paused,0,0)) {
            int value = reinterpret_cast<Function>(code)();
            if (value != 17 && value != 18) InterlockedIncrement(&bad_calls);
            InterlockedIncrement(&calls);
        }
        InterlockedDecrement(&active);
    }
    return 0;
}
}
extern "C" __declspec(dllexport) void RgbmTestCoordinatorOwnedCode() {
    if (InterlockedCompareExchange(&running,1,0)) return;
    wchar_t report[32768]{};
    DWORD length = GetEnvironmentVariableW(L"BC7_RGBM_P2_REPORT",report,_countof(report));
    if (!length || length >= _countof(report)) { InterlockedExchange(&running,0); return; }
    HMODULE keepalive = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&RgbmTestCoordinatorOwnedCode),&keepalive)) { InterlockedExchange(&running,0); return; }
    stopped = paused = active = calls = hook_calls = bad_calls = 0;
    bool clean = true, attached = false;
    LONG error = 0, recovery_error = 0;
    unsigned transactions = 0, enlisted = 0, gate_entries = 0;
    HANDLE threads[4]{};
    RgbmDetourTarget target{};
    code = static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if (!code) error = GetLastError();
    if (!error) {
        memset(code,0x90,4096);
        const uint8_t rip[] = {0x8b,0x05,0,0,0,0,0xc3}; memcpy(code,rip,sizeof(rip));
        int32_t displacement = 0x300-6; memcpy(code+2,&displacement,4); int value = 17; memcpy(code+0x300,&value,4);
        DWORD old = 0;
        if (!VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old) || !FlushInstructionCache(GetCurrentProcess(),code,4096)) error = GetLastError();
        original = reinterpret_cast<Function>(code);
        target.original = reinterpret_cast<void**>(&original); target.entry = code; target.hook = reinterpret_cast<void*>(&hook); memcpy(target.expected,code,32);
    }
    for (unsigned i=0;!error && i<4;++i) {
        threads[i] = CreateThread(nullptr,0,worker,nullptr,0,nullptr);
        if (!threads[i]) error = GetLastError();
    }
    if (!error) {
        auto deadline = GetTickCount64()+1000;
        while (InterlockedCompareExchange(&calls,0,0)<1000 && GetTickCount64()<deadline) SwitchToThread();
        if (InterlockedCompareExchange(&calls,0,0)<1000) error = ERROR_TIMEOUT;
    }
    for (unsigned repeat=0;!error && repeat<8;++repeat) {
        auto result = RgbmInstallDetours(&target,1);
        enlisted += result.threads_enlisted; gate_entries += result.loader_gate_entered;
        if (result.state == RgbmDetourState::Installed) attached = true;
        if (result.state == RgbmDetourState::Faulted) { clean = false; attached = true; }
        error = result.error; recovery_error = result.recovery_error;
        if (result.state != RgbmDetourState::Installed || error || recovery_error) { if (!error) error = ERROR_INVALID_STATE; break; }
        if (reinterpret_cast<Function>(code)() != 18) { error = ERROR_INVALID_DATA; break; }
        // Give the already-running native callers an opportunity to enter hooks.
        Sleep(1);
        InterlockedExchange(&paused,1); while (InterlockedCompareExchange(&active,0,0)) SwitchToThread();
        result = RgbmRemoveDetoursForStoppedHost(&target,1,TRUE);
        if (result.state == RgbmDetourState::Removed) attached = false;
        if (result.state == RgbmDetourState::Faulted) clean = false;
        error = result.error; recovery_error = result.recovery_error;
        if (result.state != RgbmDetourState::Removed || error || recovery_error) { if (!error) error = ERROR_INVALID_STATE; break; }
        ++transactions; InterlockedExchange(&paused,0);
    }
    InterlockedExchange(&stopped,1);
    for (auto thread : threads) if (thread) {
        if (WaitForSingleObject(thread,5000) != WAIT_OBJECT_0) { clean = false; if (!error) error = ERROR_TIMEOUT; }
        CloseHandle(thread);
    }
    if (attached && clean) {
        auto result = RgbmRemoveDetoursForStoppedHost(&target,1,TRUE);
        attached = result.state != RgbmDetourState::Removed;
        if (result.recovery_error) recovery_error = result.recovery_error;
        clean = !attached && !result.error && !result.recovery_error;
    }
    bool restored = code && !attached && !memcmp(code,target.expected,32) && reinterpret_cast<void*>(original)==code;
    bool passed = !error && !recovery_error && clean && restored && transactions==8 && hook_calls>0 && bad_calls==0;
    if (code && clean && !attached) { VirtualFree(code,0,MEM_RELEASE); code = nullptr; }
    char text[1024]{};
    _snprintf_s(text,sizeof(text),_TRUNCATE,
        "{\"artifact_kind\":\"unity_owned_code_detour_probe\",\"pid\":%lu,\"passed\":%s,\"unity_entries_patched\":false,\"transactions\":%u,\"install_gate_entries\":%u,\"threads_enlisted_total\":%u,\"entry_calls\":%ld,\"hook_calls\":%ld,\"bad_calls\":%ld,\"code_restored\":%s,\"hooks_remaining\":%s,\"error\":%ld,\"recovery_error\":%ld}\n",
        GetCurrentProcessId(),passed?"true":"false",transactions,gate_entries,enlisted,calls,hook_calls,bad_calls,
        restored?"true":"false",attached?"true":"false",error,recovery_error);
    std::wstring temporary = std::wstring(report)+L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file!=INVALID_HANDLE_VALUE) {
        DWORD written=0; BOOL ok=WriteFile(file,text,DWORD(strlen(text)),&written,nullptr);
        ok = ok && written==strlen(text) && FlushFileBuffers(file); CloseHandle(file);
        if (ok) MoveFileExW(temporary.c_str(),report,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    }
    if (clean) { InterlockedExchange(&running,0); FreeLibrary(keepalive); }
}
