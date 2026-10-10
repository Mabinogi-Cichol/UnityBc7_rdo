#pragma once
#include <windows.h>
#include <array>
#include <vector>
#include <cstdint>

namespace rgbm {
struct DetourTarget {
    // Caller owns this slot for the entire lifetime of the installed hook.
    void** original = nullptr;
    void* entry = nullptr;
    void* hook = nullptr;
    std::array<uint8_t, 32> expected{};
};
struct DetourResult {
    LONG error = NO_ERROR;
    LONG rollback_error = NO_ERROR;
    // True only for a successful attach operation. On any error, inspect error
    // and rollback_error; false alone does not prove that hooks are absent.
    bool installed = false;
    size_t failed_index = SIZE_MAX;
};
// Construction backend: explicitly requires a cooperating host that prevents
// new target-calling threads throughout the transaction. Snapshot enumeration
// cannot prove that a thread will not be created immediately after the snapshot.
// Until Unity startup coordination is proven, this must NOT be used in Unity.
DetourResult install_detours(const std::vector<DetourTarget>& targets,
                            bool thread_creation_quiesced);
DetourResult remove_detours(const std::vector<DetourTarget>& targets,
                           bool target_calls_quiesced);
// Removal additionally requires no active callers/hooks/trampolines. Suspending
// threads alone cannot protect a trampoline pointer already loaded in a register.
} // namespace rgbm
