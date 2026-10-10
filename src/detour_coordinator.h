#pragma once
#include <windows.h>
#include <cstdint>

// C ABI: no STL objects or allocator ownership cross the DLL boundary.
struct RgbmDetourTarget {
    // Host retains these slots and both target/hook modules until successful
    // stopped-host removal, or process exit. Installed hooks cannot be unloaded.
    void** original;
    void* entry;
    void* hook;
    uint8_t expected[32];
};
enum class RgbmDetourState : uint32_t { Unavailable, Installed, Removed, Faulted, Rejected };
struct RgbmDetourOutcome {
    RgbmDetourState state;
    LONG error;
    LONG recovery_error;
    uint32_t failed_index;
    uint32_t threads_enlisted;
    uint32_t loader_gate_entered;
};
#ifdef RGBM_COORDINATOR_BUILD
#define RGBM_COORD_API __declspec(dllexport)
#else
#define RGBM_COORD_API __declspec(dllimport)
#endif
extern "C" RGBM_COORD_API RgbmDetourOutcome WINAPI RgbmInstallDetours(
    const RgbmDetourTarget* targets, uint32_t count);
// Only a cooperating host may supply callers_stopped=TRUE: it must have stopped
// ALL entry/hook/trampoline callers and retained the original slots and modules.
// Unity production does not have this lease; its hooks remain resident until exit.
extern "C" RGBM_COORD_API RgbmDetourOutcome WINAPI RgbmRemoveDetoursForStoppedHost(
    const RgbmDetourTarget* targets, uint32_t count, BOOL callers_stopped);

#ifdef RGBM_COORDINATOR_TESTING
struct RgbmDetourFaults {
    LONG fail_allocation_at;
    LONG fail_context_at;
    BOOL fail_restore_protection;
    BOOL report_resume_failure;
    uint32_t thread_capacity;
    LPTHREAD_START_ROUTINE after_snapshot_start;
    void* after_snapshot_parameter;
};
extern "C" RGBM_COORD_API BOOL WINAPI RgbmSetDetourTestFaults(RgbmDetourFaults faults);
#endif
