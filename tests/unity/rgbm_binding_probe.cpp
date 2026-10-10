// Read-only in-Editor P1 probe. No target patching, no threads, no initializer.
// The caller supplies two temporary process environment variables and restores
// them after the synchronous call. The DLL may be unloaded once the call ends.
#include "rgbm_hook_contract.h"
#include <sstream>
#include <cstdio>
namespace {
std::wstring environment(const wchar_t* name) {
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0); if (!size || size > 32768) return {};
    std::wstring value(size, L'\0'); DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    if (!written || written >= size) return {}; value.resize(written); return value;
}
}
extern "C" __declspec(dllexport) void __cdecl RgbmValidateNoPatch() {
    auto cache = environment(L"BC7_RGBM_PROBE_CACHE"), report_path = environment(L"BC7_RGBM_PROBE_REPORT");
    if (cache.empty() || report_path.empty()) return;
    std::wstring exe(32768, L'\0'); DWORD size = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
    if (!size || size >= exe.size()) return; exe.resize(size);
    // Normalize all paths before publication, including the loaded main image.
    auto full = [](const std::wstring& source) {
        std::wstring result(32768, L'\0'); DWORD count = GetFullPathNameW(source.c_str(), DWORD(result.size()), result.data(), nullptr);
        if (!count || count >= result.size()) return std::wstring{}; result.resize(count); return result;
    };
    cache = full(cache); report_path = full(report_path); exe = full(exe);
    if (cache.empty() || report_path.empty() || exe.empty() || !_wcsicmp(cache.c_str(), report_path.c_str()) ||
        !_wcsicmp(exe.c_str(), report_path.c_str())) return;
    rgbm::PeImage image; rgbm::ContractBindings bindings; std::string error;
    bool valid = image.load(exe, error) && rgbm::read_binding_cache(cache, image, bindings, error) &&
        rgbm::validate_loaded_entries(GetModuleHandleW(nullptr), image, bindings, error);
    std::ostringstream output;
    output << "{\"artifact_kind\":\"unity_live_binding_probe\",\"pid\":" << GetCurrentProcessId()
        << ",\"validated\":" << (valid ? "true" : "false") << ",\"hook_installed\":false,\"patched\":false"
        << ",\"roles_checked\":" << (valid ? rgbm::role_count : 0) << ",\"exe_sha256\":" << rgbm::json_string(image.identity().sha256)
        << ",\"reason\":" << rgbm::json_string(error) << "}\n";
    rgbm::publish_report(report_path, output.str(), error);
}
