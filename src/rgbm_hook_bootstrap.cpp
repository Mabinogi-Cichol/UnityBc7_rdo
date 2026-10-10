#include "rgbm_hook_bootstrap.h"
#include "rgbm_hook_bindings.h"
#include <shellapi.h>
#include <sstream>
#pragma comment(lib, "shell32.lib")
namespace rgbm::runtime {
std::wstring cache_from_command_line(std::string& error) {
    int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) { error = "command_line_unavailable"; return {}; }
    std::wstring project;
    for (int i = 1; i + 1 < count; ++i) if (!_wcsicmp(args[i], L"-projectPath")) { project = args[i + 1]; break; }
    LocalFree(args);
    if (project.empty()) { error = "awaiting_explicit_editor_initialization"; return {}; }
    wchar_t path[32768]; DWORD size = GetFullPathNameW(project.c_str(), _countof(path), path, nullptr);
    if (!size || size >= _countof(path)) { error = "project_path_invalid"; return {}; }
    project.assign(path, size);
    size = GetModuleFileNameW(nullptr, path, _countof(path));
    if (!size || size >= _countof(path)) { error = "main_image_path_unavailable"; return {}; }
    PeImage image; if (!image.load(std::wstring(path, size), error)) return {};
    return project + L"\\Library\\Bc7Rdo\\HookBindings\\" +
        std::wstring(image.identity().sha256.begin(), image.identity().sha256.end()) + L".v1.bindings";
}
void report_bootstrap(const wchar_t* cache, State value, const std::string& error, const std::string& hash) {
    if (!cache || !*cache) return;
    std::wstring path(cache); auto slash = path.find_last_of(L"\\/"); if (slash == std::wstring::npos) return;
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return;
    ULARGE_INTEGER stamp{}; stamp.LowPart = creation.dwLowDateTime; stamp.HighPart = creation.dwHighDateTime;
    path.resize(slash + 1); path += std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(stamp.QuadPart) + L".runtime.json";
    bool worker = false, adb2 = false, worker_name = false, server = false;
    int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (args) {
        for (int i = 1; i < count; ++i) {
            worker |= !_wcsicmp(args[i], L"-assetImportWorker");
            adb2 |= !_wcsicmp(args[i], L"-adb2");
            if (!_wcsicmp(args[i], L"-name") && i + 1 < count) {
                std::wstring name(args[++i]), prefix(L"AssetImportWorker");
                worker_name = name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0;
                if (worker_name) for (size_t j = prefix.size(); j < name.size(); ++j) worker_name &= name[j] >= L'0' && name[j] <= L'9';
            }
            if (!_wcsicmp(args[i], L"-srvPort") && i + 1 < count) {
                wchar_t* end = nullptr; long port = wcstol(args[++i], &end, 10);
                server = end && end != args[i] && !*end && port > 0 && port <= 65535;
            }
        }
        worker |= adb2 && worker_name && server;
        LocalFree(args);
    }
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&report_bootstrap), &self);
    wchar_t module[32768]{}; DWORD length = GetModuleFileNameW(self, module, _countof(module));
    if (!self || !length || length >= _countof(module)) return;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, module, length, nullptr, 0, nullptr, nullptr);
    std::string module_utf8(bytes, '\0'); WideCharToMultiByte(CP_UTF8, 0, module, length, module_utf8.data(), bytes, nullptr, nullptr);
    std::ostringstream out;
    out << "{\"artifact_kind\":\"rgbm_runtime_bootstrap\",\"pid\":" << GetCurrentProcessId()
        << ",\"creation_filetime\":\"" << stamp.QuadPart << "\",\"state\":" << json_string(state_name(value))
        << ",\"exe_sha256\":" << json_string(hash) << ",\"reason\":" << json_string(error)
        << ",\"hook_module\":" << json_string(module_utf8) << ",\"asset_import_worker\":" << (worker ? "true" : "false")
        << ",\"hook_installed\":" << (value == State::Ready ? "true" : "false") << "}\n";
    std::string ignored; publish_report(path, out.str(), ignored);
}
}
