#include "rgbm_hook_runtime.h"
#include "rgbm_hook_contract.h"
#include "rgbm_hook_bootstrap.h"
#include "detour_coordinator.h"
#include <mutex>
#include <cstring>

namespace rgbm::runtime {
namespace {
std::mutex initialization_lock;
bool attempted = false;
HMODULE coordinator = nullptr;
void* real_compress = nullptr;
void* real_process = nullptr;
void compress_hook(void* image, uint32_t format, uint32_t usage, int quality,
                   bool f0, bool f1, bool f2, uint8_t* out, const void* texture) {
    invoke_compress(reinterpret_cast<Compress>(real_compress), image, format, usage, quality, f0, f1, f2, out, texture);
}
void process_hook(int rows, const void* job, Callback callback) {
    invoke_process(reinterpret_cast<Process>(real_process), rows, job, callback);
}
}
State initialize(bool enabled, const wchar_t* cache, const wchar_t* directory, std::string& error) {
    std::lock_guard<std::mutex> lock(initialization_lock);
    if (attempted) { error = "initialization_already_attempted"; return state(); }
    attempted = true;
    if (!enabled) { transition(State::Disabled); return state(); }
    transition(State::Resolving);
    std::string image_hash;
    auto unavailable = [&]() { transition(State::Unavailable); report_bootstrap(cache, state(), error, image_hash); return state(); };
    if (!cache || !*cache || !directory || !*directory) { error = "binding_cache_or_coordinator_directory_missing"; return unavailable(); }
    std::wstring exe(32768, L'\0'); DWORD size = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
    if (!size || size >= exe.size()) { error = "main_image_path_unavailable"; return unavailable(); }
    exe.resize(size);
    PeImage image; ContractBindings bindings;
    if (!image.load(exe, error)) return unavailable();
    image_hash = image.identity().sha256;
    if (!read_binding_cache(cache, image, bindings, error)) return unavailable();
    transition(State::Validating);
    HMODULE unity = GetModuleHandleW(nullptr);
    if (!validate_loaded_entries(unity, image, bindings, error)) return unavailable();
    // Absolute dependency paths, outside DllMain. Missing dependencies preserve
    // native BC7 instead of making LoadLibrary of the main hook fail.
    std::wstring dependency = std::wstring(directory) + L"\\bc7_rgbm_coordinator.dll";
    coordinator = LoadLibraryExW(dependency.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!coordinator) { error = "coordinator_load_failed:" + std::to_string(GetLastError()); return unavailable(); }
    using Install = RgbmDetourOutcome (WINAPI*)(const RgbmDetourTarget*, uint32_t);
    auto install = reinterpret_cast<Install>(GetProcAddress(coordinator, "RgbmInstallDetours"));
    if (!install) { error = "coordinator_export_missing"; FreeLibrary(coordinator); coordinator = nullptr; return unavailable(); }
    RgbmDetourTarget targets[2]{};
    targets[0].original = &real_compress; targets[0].hook = reinterpret_cast<void*>(&compress_hook);
    targets[1].original = &real_process; targets[1].hook = reinterpret_cast<void*>(&process_hook);
    for (unsigned i = 0; i < 2; ++i) {
        const auto& entry = bindings.entries[static_cast<size_t>(i ? Role::Process : Role::Compress)];
        targets[i].entry = reinterpret_cast<uint8_t*>(unity) + entry.rva;
        memcpy(targets[i].expected, image.at(entry.rva, 32, true), 32);
        // Detours receives an original-function slot containing the ENTRY; the
        // coordinator atomically replaces it with the prepared trampoline.
        *targets[i].original = targets[i].entry;
    }
    transition(State::Installing);
    const auto result = install(targets, 2);
    if (result.state == RgbmDetourState::Faulted || result.recovery_error) {
        // Recovery may have left executable memory or suspended callers damaged.
        fail_closed(0xE0427006u);
    }
    if (result.state != RgbmDetourState::Installed || result.error) {
        error = "detour_transaction_rejected:" + std::to_string(result.error) + ":index=" +
            std::to_string(result.failed_index) + ":threads=" + std::to_string(result.threads_enlisted);
        FreeLibrary(coordinator); coordinator = nullptr; return unavailable();
    }
    // A process call during installation may already have exhausted the pool.
    // Do not turn sticky Unavailable back into Ready.
    if (!publish_ready()) error = "runtime_degraded_during_installation";
    report_bootstrap(cache, state(), error, image_hash);
    return state();
}
} // namespace rgbm::runtime
