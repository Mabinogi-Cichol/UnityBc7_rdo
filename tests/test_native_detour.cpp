#include "native_detour_backend.h"
#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); return 1; } } while (0)
using Function = int (*)();
Function originals[3]{};
int hook0() { return originals[0]() + 1; }
int hook1() { return originals[1]() + 1; }
int hook2() { return originals[2]() + 1; }

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(code); memset(code, 0x90, 4096);
    // RIP-relative read right at the entry (7 bytes copied, not a fixed prefix).
    const uint8_t rip[] = {0x8b, 0x05, 0, 0, 0, 0, 0xc3};
    memcpy(code, rip, sizeof(rip)); int32_t displacement = 0x300 - 6; memcpy(code + 2, &displacement, 4);
    int value = 17; memcpy(code + 0x300, &value, 4);
    // Relative call in the first instruction, followed by a return.
    code[0x40] = 0xe8; displacement = 0x100 - 0x45; memcpy(code + 0x41, &displacement, 4); code[0x45] = 0xc3;
    const uint8_t helper[] = {0xb8, 17, 0, 0, 0, 0xc3}; memcpy(code + 0x100, helper, sizeof(helper));
    // Short conditional branch within the copied entry, covering both paths.
    const uint8_t branch[] = {0x31, 0xc0, 0x74, 0x02, 0xff, 0xc0, 0xb8, 17, 0, 0, 0, 0xc3};
    memcpy(code + 0x80, branch, sizeof(branch));
    // Too-small function, with no consumable padding after RET.
    code[0xc0] = 0xc3; code[0xc1] = 0x41; code[0xc2] = 0x51;
    DWORD old = 0; CHECK(VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old));
    FlushInstructionCache(GetCurrentProcess(), code, 4096);
    void* hooks[] = {reinterpret_cast<void*>(&hook0), reinterpret_cast<void*>(&hook1), reinterpret_cast<void*>(&hook2)};
    std::vector<rgbm::DetourTarget> targets;
    for (unsigned i = 0; i < 3; ++i) {
        originals[i] = reinterpret_cast<Function>(code + i * 0x40);
        rgbm::DetourTarget target;
        target.original = reinterpret_cast<void**>(&originals[i]); target.entry = code + i * 0x40; target.hook = hooks[i];
        memcpy(target.expected.data(), target.entry, target.expected.size()); targets.push_back(target);
        CHECK(originals[i]() == 17);
    }
    CHECK(rgbm::install_detours(targets, false).error == ERROR_INVALID_STATE);
    auto corrupt = targets; corrupt[2].expected[0] ^= 1;
    auto result = rgbm::install_detours(corrupt, true);
    CHECK(result.error && result.failed_index == 2 && !result.installed);
    for (const auto& target : targets) CHECK(!memcmp(target.entry, target.expected.data(), 32));
    auto fail_third = targets;
    originals[2] = reinterpret_cast<Function>(code + 0xc0);
    fail_third[2].entry = code + 0xc0; memcpy(fail_third[2].expected.data(), code + 0xc0, 32);
    result = rgbm::install_detours(fail_third, true);
    CHECK(result.error && result.failed_index == 2 && !result.rollback_error && !result.installed);
    for (const auto& target : fail_third) CHECK(!memcmp(target.entry, target.expected.data(), 32) && *target.original == target.entry);
    originals[2] = reinterpret_cast<Function>(targets[2].entry);
    // Cooperating host creates all callers before the transaction; no new
    // threads are created until removal completes. Each calls the ENTRY (not
    // the original slot, which is changed by Detours).
    std::atomic<bool> stop{false}, failed{false}, paused{false};
    std::atomic<unsigned> iterations{0}, active{0};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&]() {
        while (!stop.load()) {
            if (paused.load()) { SwitchToThread(); continue; }
            active.fetch_add(1);
            if (paused.load()) { active.fetch_sub(1); continue; }
            for (const auto& target : targets) {
            int observed = reinterpret_cast<Function>(target.entry)();
            if (observed != 17 && observed != 18) failed.store(true);
            iterations.fetch_add(1);
            }
            active.fetch_sub(1);
        }
    });
    while (iterations.load() < 1000) SwitchToThread();
    unsigned installed = 0;
    for (unsigned repeat = 0; repeat < 32; ++repeat) {
        result = rgbm::install_detours(targets, true);
        if (result.error || result.rollback_error || !result.installed) { failed.store(true); break; }
        ++installed;
        if (rgbm::remove_detours(targets, false).error != ERROR_INVALID_STATE) failed.store(true);
        for (const auto& target : targets) if (reinterpret_cast<Function>(target.entry)() != 18) failed.store(true);
        paused.store(true);
        while (active.load()) SwitchToThread();
        result = rgbm::remove_detours(targets, true);
        if (result.error || result.rollback_error || result.installed) { failed.store(true); break; }
        for (const auto& target : targets) if (memcmp(target.entry, target.expected.data(), 32)) failed.store(true);
        paused.store(false);
    }
    stop.store(true); for (auto& worker : workers) worker.join();
    CHECK(!failed.load()); CHECK(installed == 32);
    for (const auto& target : targets) CHECK(*target.original == target.entry);
    CHECK(VirtualFree(code, 0, MEM_RELEASE));
    printf("RIP-relative, relative call, conditional branch, preflight and third-attach abort passed; concurrent_transactions=%u calls=%u\n", installed, iterations.load());
    return 0;
}
