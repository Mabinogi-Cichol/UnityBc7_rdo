#include "detour_coordinator.h"
#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); ExitProcess(1); } } while (0)
using Function = int (*)();
Function originals[3];
int hook0() { return originals[0]() + 1; }
int hook1() { return originals[1]() + 1; }
int hook2() { return originals[2]() + 1; }
struct Birth { void* entry; HANDLE done; std::atomic<unsigned> errors{0}; };
DWORD WINAPI born_after_snapshot(void* parameter) {
    auto& b = *static_cast<Birth*>(parameter);
    if (reinterpret_cast<Function>(b.entry)() != 18) ++b.errors;
    SetEvent(b.done); return 0;
}
bool restored(const RgbmDetourTarget* targets) {
    for (unsigned i = 0; i < 3; ++i) if (memcmp(targets[i].entry, targets[i].expected, 32) || *targets[i].original != targets[i].entry) return false;
    return true;
}
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(code); memset(code, 0x90, 4096);
    const uint8_t rip[] = {0x8b,0x05,0,0,0,0,0xc3}; memcpy(code, rip, sizeof(rip));
    int32_t d = 0x300 - 6; memcpy(code + 2, &d, 4); int value = 17; memcpy(code + 0x300, &value, 4);
    code[0x40] = 0xe8; d = 0x100 - 0x45; memcpy(code + 0x41, &d, 4); code[0x45] = 0xc3;
    const uint8_t helper[] = {0xb8,17,0,0,0,0xc3}; memcpy(code + 0x100, helper, sizeof(helper));
    const uint8_t branch[] = {0x31,0xc0,0x74,0x02,0xff,0xc0,0xb8,17,0,0,0,0xc3}; memcpy(code + 0x80, branch, sizeof(branch));
    code[0xc0] = 0xc3; code[0xc1] = 0x41; code[0xc2] = 0x51;
    DWORD old; CHECK(VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old)); CHECK(FlushInstructionCache(GetCurrentProcess(),code,4096));
    void* hooks[] = {reinterpret_cast<void*>(&hook0),reinterpret_cast<void*>(&hook1),reinterpret_cast<void*>(&hook2)};
    RgbmDetourTarget targets[3]{};
    for (unsigned i = 0; i < 3; ++i) {
        originals[i] = reinterpret_cast<Function>(code + i * 0x40);
        targets[i].original = reinterpret_cast<void**>(&originals[i]); targets[i].entry = code + i * 0x40; targets[i].hook = hooks[i];
        memcpy(targets[i].expected, targets[i].entry, 32);
    }
    if (argc > 1) {
        if (!strcmp(argv[1], "missing-gate")) {
            auto result = RgbmInstallDetours(targets,3);
            CHECK(result.error == ERROR_MOD_NOT_FOUND && result.state == RgbmDetourState::Unavailable && !result.loader_gate_entered);
            CHECK(restored(targets)); CHECK(VirtualFree(code,0,MEM_RELEASE));
            puts("{\"case\":\"missing-gate\",\"passed\":true}"); return 0;
        }
        RgbmDetourFaults faults{};
        if (!strcmp(argv[1], "restore-fault")) faults.fail_restore_protection = TRUE;
        else if (!strcmp(argv[1], "resume-fault")) faults.report_resume_failure = TRUE;
        else return 2;
        std::atomic<bool> stop{false}; std::thread thread([&] { while (!stop) SwitchToThread(); });
        CHECK(RgbmSetDetourTestFaults(faults)); auto result = RgbmInstallDetours(targets,3);
        CHECK(result.state == RgbmDetourState::Faulted && result.recovery_error);
        CHECK(RgbmInstallDetours(targets,3).state == RgbmDetourState::Faulted);
        stop = true; thread.join();
        printf("{\"case\":\"%s\",\"passed\":true,\"state\":\"Faulted\",\"error\":%ld,\"recovery_error\":%ld}\n",argv[1],result.error,result.recovery_error);
        // Intentionally retain code, original slots and modules until process exit.
        return 0;
    }
    auto corrupt = targets[2]; targets[2].expected[0] ^= 1;
    auto result = RgbmInstallDetours(targets,3); CHECK(result.error && !result.loader_gate_entered); targets[2] = corrupt;
    CHECK(restored(targets));
    auto duplicate = targets[1]; targets[1] = targets[0];
    result = RgbmInstallDetours(targets,3); CHECK(result.error && !result.loader_gate_entered); targets[1] = duplicate;
    auto slot = targets[1].original; targets[1].original = reinterpret_cast<void**>(uintptr_t(1));
    result = RgbmInstallDetours(targets,3); CHECK(result.error && !result.loader_gate_entered); targets[1].original = slot;
    HMODULE gate = LoadLibraryW(L"bc7_rgbm_loader_gate.dll"); CHECK(gate);
    result = RgbmInstallDetours(targets,3); CHECK(result.error == ERROR_ALREADY_EXISTS && !result.loader_gate_entered); CHECK(FreeLibrary(gate)); CHECK(restored(targets));
    originals[2] = reinterpret_cast<Function>(code + 0xc0); targets[2].entry = code + 0xc0; memcpy(targets[2].expected,code + 0xc0,32);
    result = RgbmInstallDetours(targets,3); CHECK(result.error && result.failed_index == 2 && !result.recovery_error); CHECK(restored(targets));
    targets[2] = corrupt; originals[2] = reinterpret_cast<Function>(targets[2].entry);
    std::atomic<bool> start_installers{false}; RgbmDetourOutcome contender_results[2]{};
    std::thread contenders[2];
    for (unsigned i=0;i<2;++i) contenders[i] = std::thread([&,i] {
        while (!start_installers) SwitchToThread(); contender_results[i] = RgbmInstallDetours(targets,3);
    });
    start_installers = true; for (auto& thread : contenders) thread.join();
    unsigned winners = 0;
    for (auto& r : contender_results) {
        if (!r.error && r.state == RgbmDetourState::Installed) ++winners;
        else CHECK(r.error == ERROR_BUSY || r.error == ERROR_INVALID_STATE);
        CHECK(!r.recovery_error);
    }
    CHECK(winners == 1); result = RgbmRemoveDetoursForStoppedHost(targets,3,TRUE);
    CHECK(result.state == RgbmDetourState::Removed && !result.error && !result.recovery_error); CHECK(restored(targets));
    std::atomic<bool> stop{false}, pause{false}, failed{false}; std::atomic<unsigned> active{0}, calls{0};
    auto call = [&] {
        while (!stop) {
            if (pause) { SwitchToThread(); continue; }
            ++active; if (pause) { --active; continue; }
            for (auto& t : targets) { int v = reinterpret_cast<Function>(t.entry)(); if (v != 17 && v != 18) failed = true; ++calls; }
            --active;
        }
    };
    std::vector<std::thread> workers; for (unsigned i=0;i<8;++i) workers.emplace_back(call);
    while (calls < 1000) SwitchToThread();
    for (LONG nth : {1L,3L,5L}) {
        RgbmDetourFaults faults{}; faults.fail_allocation_at = nth; CHECK(RgbmSetDetourTestFaults(faults));
        result = RgbmInstallDetours(targets,3); CHECK(result.error && !result.recovery_error && result.state == RgbmDetourState::Unavailable); CHECK(restored(targets));
    }
    for (bool capacity : {false,true}) {
        RgbmDetourFaults faults{}; if (capacity) faults.thread_capacity = 1; else faults.fail_context_at = 2;
        CHECK(RgbmSetDetourTestFaults(faults)); result = RgbmInstallDetours(targets,3);
        CHECK(result.error && !result.recovery_error && result.state == RgbmDetourState::Unavailable); CHECK(restored(targets));
    }
    CHECK(RgbmSetDetourTestFaults({}));
    // Churn creates new native callers during every installation. No host
    // thread-creation barrier is supplied to the coordinator.
    std::thread creator([&] { while (!stop) { std::thread newborn([&] { if (!pause) { ++active; if (!pause) {
        int v = reinterpret_cast<Function>(targets[0].entry)(); if (v != 17 && v != 18) failed = true; ++calls;
    } --active; } }); newborn.join(); } });
    Birth birth{targets[0].entry, CreateEventW(nullptr,TRUE,FALSE,nullptr)}; CHECK(birth.done);
    uint32_t enlisted = 0;
    for (unsigned repeat=0;repeat<32;++repeat) {
        ResetEvent(birth.done); RgbmDetourFaults faults{};
        faults.after_snapshot_start = born_after_snapshot; faults.after_snapshot_parameter = &birth;
        CHECK(RgbmSetDetourTestFaults(faults)); result = RgbmInstallDetours(targets,3);
        CHECK(result.state == RgbmDetourState::Installed && !result.error && !result.recovery_error && result.loader_gate_entered);
        enlisted += result.threads_enlisted;
        CHECK(RgbmInstallDetours(targets,3).state == RgbmDetourState::Installed);
        CHECK(WaitForSingleObject(birth.done,10000) == WAIT_OBJECT_0 && !birth.errors);
        CHECK(RgbmRemoveDetoursForStoppedHost(targets,3,FALSE).error == ERROR_INVALID_STATE);
        // Cooperating lease stops all entry/hook/trampoline callers, including
        // the deterministic newborn, BEFORE the removal transaction.
        pause = true; while (active) SwitchToThread();
        if (!repeat) {
            uint8_t installed_bytes[3][32]; void* installed_originals[3];
            for (unsigned i=0;i<3;++i) { memcpy(installed_bytes[i],targets[i].entry,32); installed_originals[i]=*targets[i].original; }
            RgbmDetourFaults removal_fault{}; removal_fault.fail_allocation_at = 2;
            CHECK(RgbmSetDetourTestFaults(removal_fault)); result = RgbmRemoveDetoursForStoppedHost(targets,3,TRUE);
            CHECK(result.error && !result.recovery_error && result.state == RgbmDetourState::Installed);
            for (unsigned i=0;i<3;++i) CHECK(!memcmp(installed_bytes[i],targets[i].entry,32) && installed_originals[i]==*targets[i].original && reinterpret_cast<Function>(targets[i].entry)()==18);
        }
        CHECK(RgbmSetDetourTestFaults({})); result = RgbmRemoveDetoursForStoppedHost(targets,3,TRUE);
        CHECK(result.state == RgbmDetourState::Removed && !result.error && !result.recovery_error); CHECK(restored(targets)); pause = false;
    }
    stop = true; creator.join(); for (auto& thread : workers) thread.join(); CHECK(!failed);
    CloseHandle(birth.done); CHECK(VirtualFree(code,0,MEM_RELEASE));
    printf("{\"case\":\"matrix\",\"passed\":true,\"transactions\":32,\"competing_installers\":2,\"deterministic_births_after_snapshot\":32,\"calls\":%u,\"threads_enlisted\":%u}\n",calls.load(),enlisted);
    return 0;
}
