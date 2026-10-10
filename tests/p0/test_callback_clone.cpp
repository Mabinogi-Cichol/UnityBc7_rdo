// Executes a checked, isolated copy of the actual Unity ThreadedProcessImage
// and dispatcher. ONLY external schedule/complete/clear calls are substituted.
// This is a native contract experiment, not a Unity integration test. No Unity
// executable/process is mapped as executable or patched; extracted bytes remain
// in an isolated allocation and are never included in a release.
#include "rgbm_hook_bindings.h"
#ifdef RGBM_RUNTIME_TESTING
#include "rgbm_hook_runtime.h"
namespace p0 = rgbm::runtime;
#else
#include "callback_scope.h"
#endif
#include <detours.h>
#include <bcrypt.h>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#pragma comment(lib, "bcrypt.lib")

void require(bool condition, const char* what) { if (!condition) throw std::runtime_error(what); }
using Process = void (*)(int, const void*, p0::Callback);
using Dispatcher = void (*)(void*, unsigned);
struct Group { std::vector<std::thread> workers; std::atomic<bool> wait_entered{false}; };
struct Counters {
    std::atomic<unsigned> jobs{0}, callbacks{0}, schedule{0}, complete{0}, clear{0};
    std::atomic<unsigned> copy_matches{0}, context_matches{0}, tls_restores{0}, failed{0};
} counters;
Process process = nullptr;
struct Observation;
struct JobInput { Observation* owner; unsigned char opaque[0xd0]; };
static_assert(sizeof(JobInput) == 0xd8);
struct Observation {
    JobInput* source;
    p0::Context expected;
    int rows;
    bool nested;
    std::atomic<int> rows_seen{0};
    std::atomic<unsigned> chunks{0};
};
void run_job(p0::Context state, int rows, bool nested = false);
unsigned unknown_on_abandon = 0;
void abandoned_callback(void*, int, int) { if (p0::current_context() == p0::Context::Unknown) ++unknown_on_abandon; }
void throwing_callback(void*, int, int) { throw std::runtime_error("expected callback failure"); }
void callback(void* data, int begin, int end) {
    auto& observation = *static_cast<JobInput*>(data)->owner;
    const auto state = p0::current_context();
    if (state == observation.expected) ++counters.context_matches; else ++counters.failed;
    if (data != observation.source && !memcmp(data, observation.source, sizeof(JobInput))) ++counters.copy_matches;
    else ++counters.failed;
    if (begin < 0 || end <= begin || end > observation.rows || end - begin > 8 || begin % 8) ++counters.failed;
    observation.rows_seen.fetch_add(end - begin); ++observation.chunks; ++counters.callbacks;
    if (observation.nested && begin == 0) {
        run_job(p0::Context::Ordinary, 9);
        if (p0::current_context() != state) ++counters.failed;
    }
}
void schedule(void* fence, Dispatcher dispatcher, void* data, int count,
              void (*cleanup)(void*), int priority, int batch, void* range) {
    require(count > 0 && !cleanup && priority == 0 && batch == 1 && !range, "scheduler argument contract");
    auto* group = new Group;
    *static_cast<Group**>(fence) = group;
    for (int i = 0; i < count; ++i) group->workers.emplace_back([=]() {
        // Jobs cannot start until CompleteFence is actually called by the copied
        // Unity body. Return-before-wait would fail the experiment.
        while (!group->wait_entered.load()) SwitchToThread();
        p0::ContextScope unrelated_worker_state(i % 2 ? p0::Context::RGBM : p0::Context::Ordinary);
        const auto before = p0::current_context();
        dispatcher(data, unsigned(i));
        if (p0::current_context() == before) ++counters.tls_restores; else ++counters.failed;
    });
    ++counters.schedule;
}
void complete(void* fence, int work_steal_mode) {
    require(work_steal_mode == 0, "wait mode contract");
    auto* group = *static_cast<Group**>(fence);
    require(group != nullptr, "fence absent");
    group->wait_entered.store(true);
    for (auto& worker : group->workers) worker.join();
    delete group; *static_cast<Group**>(fence) = nullptr; ++counters.complete;
}
void clear(void* fence) { require(!*static_cast<Group**>(fence), "clear before join"); ++counters.clear; }
std::string hash(const std::vector<uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA256 provider");
    unsigned char digest[32]; auto status = BCryptHash(algorithm, nullptr, 0,
        const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0); require(status >= 0, "SHA256");
    std::string result; for (auto byte : digest) { char hex[3]; sprintf_s(hex, "%02x", byte); result += hex; } return result;
}
struct Patch { unsigned offset; uint32_t original_target; bool call; };
std::vector<Patch> patches(std::vector<uint8_t>& bytes, uint32_t rva) {
    const size_t size = bytes.size(); bytes.resize(size + 32, 0);
    std::vector<Patch> result;
    for (unsigned offset = 0; offset < size;) {
        auto* source = bytes.data() + offset;
        auto* next = static_cast<uint8_t*>(DetourCopyInstruction(nullptr, nullptr, source, nullptr, nullptr));
        require(next && next > source && next - source <= 15 && size_t(next - source) <= size - offset, "decode boundary");
        const unsigned length = unsigned(next - source);
        unsigned displacement_offset = 0; bool call = false;
        if (length == 5 && source[0] == 0xe8) { displacement_offset = 1; call = true; }
        else if (length == 7 && source[0] == 0x48 && source[1] == 0x8d && source[2] == 0x15) displacement_offset = 3;
        if (displacement_offset) {
            int32_t delta = 0; memcpy(&delta, source + displacement_offset, 4);
            const int64_t target = int64_t(rva) + offset + length + delta;
            require(target >= 0 && target <= UINT32_MAX, "edge target overflow");
            result.push_back({offset + displacement_offset, uint32_t(target), call});
            memset(source + displacement_offset, 0, 4);
        }
        offset += length;
    }
    bytes.resize(size); return result;
}
void run_job(p0::Context state, int rows, bool nested) {
    p0::ContextScope semantic(state);
    JobInput input{}; memset(input.opaque, 0xa5, sizeof(input.opaque));
    Observation observation{&input, state, rows, nested}; input.owner = &observation;
#ifdef RGBM_RUNTIME_TESTING
    p0::invoke_process(process, rows, &input, &callback);
#else
    p0::CallbackLease lease(&callback);
    require(lease.callback() != nullptr, "callback pool exhausted");
    process(rows, &input, lease.callback());
    require(lease.complete_after_join(), "callbacks still active on process return");
#endif
    require(observation.rows_seen == rows, "rows not completed");
    require(observation.chunks == unsigned((rows + 7) / 8), "chunk count");
    require(p0::current_context() == state, "caller TLS changed"); ++counters.jobs;
}
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 9) {
        fwprintf(stderr, L"Usage: test_callback_clone <exe> <sha256> <process-rva> <dispatcher-rva> <schedule-rva> <complete-rva> <clear-rva> <report>\n"); return 2;
    }
    uint8_t* clone = nullptr;
    try {
#ifdef RGBM_RUNTIME_TESTING
        p0::test_ready();
#endif
        std::wstring exe_full(32768, L'\0'), report_full(32768, L'\0');
        auto full_path = [](const wchar_t* source, std::wstring& destination) {
            DWORD count = GetFullPathNameW(source, static_cast<DWORD>(destination.size()), destination.data(), nullptr);
            if (!count || count >= destination.size()) return false;
            destination.resize(count); return true;
        };
        require(full_path(argv[1], exe_full) && full_path(argv[8], report_full) &&
            _wcsicmp(exe_full.c_str(), report_full.c_str()), "unsafe output path");
        rgbm::PeImage image; std::string error; require(image.load(argv[1], error), error.c_str());
        std::wstring expected(argv[2]); std::string expected_hash(expected.begin(), expected.end());
        require(image.identity().sha256 == expected_hash, "exe identity mismatch");
        uint32_t rvas[5];
        for (unsigned i = 0; i < 5; ++i) {
            wchar_t* end = nullptr; auto value = wcstoull(argv[i + 3], &end, 16);
            require(end != argv[i + 3] && !*end && value <= UINT32_MAX, "invalid RVA"); rvas[i] = uint32_t(value);
        }
        const auto range = image.function(rvas[0]); require(range.begin && range.end - range.begin == 0x117, "unknown process shape");
        const auto* original = image.at(rvas[0], 0x117, true); const auto* dispatcher = image.at(rvas[1], 31, true);
        require(original && dispatcher, "entry not in code");
        std::vector<uint8_t> body(original, original + 0x117), worker(dispatcher, dispatcher + 31);
        auto edges = patches(body, rvas[0]);
        require(hash(body) == "67114899dd5f1104c61693995f0864bb93c1f8681bee3963db8dd765fde7c273", "unknown normalized process body");
        require(hash(worker) == "486a2e9c09d4a126d35ac51e15ef9b7474b4a522435da5c1867dcd012dfefd0b", "unknown dispatcher body");
        require(edges.size() == 4 && !edges[0].call && edges[0].original_target == rvas[1], "dispatcher edge");
        for (unsigned i = 1; i < 4; ++i) require(edges[i].call && edges[i].original_target == rvas[i + 1], "helper edge");
        clone = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)); require(clone, "allocation");
        memcpy(clone, original, 0x117); memcpy(clone + 0x200, dispatcher, 31);
        void* helpers[] = {reinterpret_cast<void*>(&schedule), reinterpret_cast<void*>(&complete), reinterpret_cast<void*>(&clear)};
        for (unsigned i = 0; i < 3; ++i) {
            auto* relay = clone + 0x300 + i * 16;
            const uint8_t jump[] = {0xff, 0x25, 0, 0, 0, 0}; memcpy(relay, jump, 6); memcpy(relay + 6, &helpers[i], 8);
        }
        for (unsigned i = 0; i < edges.size(); ++i) {
            auto& edge = edges[i]; unsigned destination = i ? 0x300 + (i - 1) * 16 : 0x200;
            int32_t delta = int32_t(destination) - int32_t(edge.offset + 4); memcpy(clone + edge.offset, &delta, 4);
        }
        DWORD previous; require(VirtualProtect(clone, 4096, PAGE_EXECUTE_READ, &previous), "protect");
        FlushInstructionCache(GetCurrentProcess(), clone, 4096); process = reinterpret_cast<Process>(clone);
        run_job(p0::Context::RGBM, 0); require(counters.schedule == 0, "zero rows scheduled");
        run_job(p0::Context::RGBM, 17, true); run_job(p0::Context::Ordinary, 17); run_job(p0::Context::Unknown, 17);
        std::vector<std::thread> callers;
        for (unsigned i = 0; i < 8; ++i) callers.emplace_back([i]() {
            try { for (unsigned j = 0; j < 16; ++j) run_job(static_cast<p0::Context>((i + j) % 3), 9, j % 8 == 0); }
            catch (...) { ++counters.failed; }
        });
        for (auto& caller : callers) caller.join();
        require(counters.failed == 0, "callback mismatch"); require(p0::occupied_slots() == 0, "normal slot not cleaned");
        require(counters.schedule == counters.complete && counters.complete == counters.clear, "wait/clear order or count");
        require(counters.callbacks == counters.copy_matches && counters.copy_matches == counters.context_matches &&
            counters.context_matches == counters.tls_restores, "copy/context/TLS evidence");
        require(p0::current_context() == p0::Context::Unknown, "main TLS leak");
        const size_t slots_after_normal = p0::occupied_slots();
#ifndef RGBM_RUNTIME_TESTING
        // Explicit exhaustion must not silently invent Ordinary classification.
        std::vector<std::unique_ptr<p0::CallbackLease>> full;
        for (unsigned i = 0; i < p0::slot_count; ++i) full.emplace_back(new p0::CallbackLease(&callback, p0::Context::RGBM));
        { p0::CallbackLease extra(&callback); require(!extra.callback(), "pool overflow accepted"); }
        for (auto& lease : full) require(lease->complete_after_join(), "unused scope cleanup"); full.clear();
        p0::Callback abandoned;
        { p0::CallbackLease lease(&abandoned_callback, p0::Context::RGBM); abandoned = lease.callback(); }
        {
            p0::ContextScope unrelated(p0::Context::Ordinary);
            abandoned(nullptr, 0, 1);
            require(p0::current_context() == p0::Context::Ordinary && unknown_on_abandon == 1, "abandoned callback leaked classification");
            p0::CallbackLease next(&abandoned_callback, p0::Context::RGBM);
            require(next.callback() != abandoned, "quarantined identity reused"); require(next.complete_after_join(), "next lease cleanup");
        }
        {
            p0::CallbackLease throwing(&throwing_callback, p0::Context::RGBM);
            bool caught = false;
            try { throwing.callback()(nullptr, 0, 1); } catch (const std::runtime_error&) { caught = true; }
            require(caught && p0::current_context() == p0::Context::Unknown, "callback exception TLS restore");
            // Destructor quarantines it because a native synchronous join has
            // not been certified on this abnormal path. No recycle occurs.
        }
 #endif
        std::ostringstream out;
#ifdef RGBM_RUNTIME_TESTING
        out << "{\"artifact_kind\":\"p3_production_runtime_clone_test\",\"production_runtime\":true,\"exe_sha256\":";
#else
        out << "{\"artifact_kind\":\"p0_native_clone_test\",\"exe_sha256\":";
#endif
        out << rgbm::json_string(image.identity().sha256)
            << ",\"unity_integration\":false,\"external_scheduler_substituted\":true,\"passed\":true,"
            << "\"process_rva\":" << rvas[0] << ",\"dispatcher_rva\":" << rvas[1]
            << ",\"jobs\":" << counters.jobs << ",\"callbacks\":" << counters.callbacks
            << ",\"copied_payload_verified\":" << counters.copy_matches << ",\"context_verified\":" << counters.context_matches
            << ",\"worker_tls_restored\":" << counters.tls_restores << ",\"schedule\":" << counters.schedule
            << ",\"complete\":" << counters.complete << ",\"clear\":" << counters.clear
            << ",\"active_slots_after_normal_exit\":" << slots_after_normal
#ifndef RGBM_RUNTIME_TESTING
            << ",\"quarantined_slots\":" << p0::occupied_slots()
            << ",\"abandon_is_unknown\":true,\"exception_tls_restored\":true,\"quarantine_not_reused\":true"
            << ",\"pool_exhaustion_rejected\":true"
#endif
            << "}\n";
        require(rgbm::publish_report(argv[8], out.str(), error), "report publication");
        VirtualFree(clone, 0, MEM_RELEASE); puts("actual process/dispatcher clone: copied payload, callback ABI, wait order, mixed/nested contexts, TLS restore and slot reuse passed"); return 0;
    } catch (const std::exception& ex) {
        if (clone) VirtualFree(clone, 0, MEM_RELEASE);
        fprintf(stderr, "P0 clone refused/failed: %s\n", ex.what()); return 1;
    }
}
