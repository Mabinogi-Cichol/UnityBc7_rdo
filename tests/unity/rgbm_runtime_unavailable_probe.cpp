// P3 negative integration only: validate actual Editor and bindings, then use a
// proven absent coordinator directory. It cannot install or patch Unity hooks.
#include "rgbm_hook_runtime.h"
#include "rgbm_hook_contract.h"
#include <sstream>
#include <cstring>
namespace {
std::wstring env(const wchar_t* name) {
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0); if (!size || size > 32768) return {};
    std::wstring value(size, L'\0'); DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    if (!written || written >= size) return {}; value.resize(written); return value;
}
}
extern "C" __declspec(dllexport) void __cdecl RgbmTestUnavailableRuntime() {
    auto cache = env(L"BC7_RGBM_P3_CACHE"), report = env(L"BC7_RGBM_P3_REPORT");
    if (cache.empty() || report.empty()) return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&RgbmTestUnavailableRuntime), &self);
    std::wstring directory(32768, L'\0'); DWORD size = GetModuleFileNameW(self, directory.data(), DWORD(directory.size()));
    if (!self || !size || size >= directory.size()) return; directory.resize(size);
    directory.resize(directory.find_last_of(L"\\/")); directory += L"\\intentionally-absent-dependencies";
    const auto dependency = directory + L"\\bc7_rgbm_coordinator.dll";
    if (GetFileAttributesW(directory.c_str()) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(dependency.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    std::wstring exe(32768, L'\0'); size = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
    if (!size || size >= exe.size()) return; exe.resize(size);
    rgbm::PeImage image; rgbm::ContractBindings bindings; std::string error;
    bool validated = image.load(exe, error) && rgbm::read_binding_cache(cache, image, bindings, error) &&
        rgbm::validate_loaded_entries(GetModuleHandleW(nullptr), image, bindings, error);
    if (!validated) return;
    auto value = rgbm::runtime::initialize(true, cache.c_str(), directory.c_str(), error);
    bool unchanged = true;
    for (const auto& entry : bindings.entries)
        unchanged &= memcmp(reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + entry.rva, image.at(entry.rva, 32, true), 32) == 0;
    rgbm::runtime::ContextScope dirty(rgbm::runtime::Context::RGBM);
    const auto decision = rgbm::runtime::encoding_decision(true, 30, 10);
    bool passed = value == rgbm::runtime::State::Unavailable && decision.native_only && unchanged &&
        error.find("coordinator_load_failed:") == 0;
    std::ostringstream out;
    out << "{\"artifact_kind\":\"p3_live_unavailable_runtime\",\"pid\":" << GetCurrentProcessId()
        << ",\"passed\":" << (passed ? "true" : "false") << ",\"bindings_validated\":true,\"roles_checked\":" << rgbm::role_count
        << ",\"state\":" << rgbm::json_string(rgbm::runtime::state_name(value))
        << ",\"unity_entries_unchanged\":" << (unchanged ? "true" : "false")
        << ",\"hook_installed\":false,\"native_fallback_decision\":" << (decision.native_only ? "true" : "false")
        << ",\"reason\":" << rgbm::json_string(error) << "}\n";
    rgbm::publish_report(report, out.str(), error);
}
