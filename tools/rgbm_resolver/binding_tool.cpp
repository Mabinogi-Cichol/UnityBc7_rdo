#include "rgbm_hook_contract.h"
#include <map>
#include <cstdio>
#include <sstream>
int wmain(int argc, wchar_t** argv) {
    std::map<std::wstring, std::wstring> args;
    if (argc != 9) return 2;
    for (int i = 1; i < argc; i += 2) {
        std::wstring key(argv[i]);
        if ((key != L"--mode" && key != L"--exe" && key != L"--cache" && key != L"--report") ||
            !args.emplace(key, argv[i + 1]).second) return 2;
    }
    if (args.size() != 4 || (args[L"--mode"] != L"scan" && args[L"--mode"] != L"verify")) return 2;
    std::map<std::wstring, std::wstring> full;
    for (auto key : {L"--exe", L"--cache", L"--report"}) {
        std::wstring path(32768, L'\0'); DWORD count = GetFullPathNameW(args[key].c_str(), DWORD(path.size()), path.data(), nullptr);
        if (!count || count >= path.size()) return 2; path.resize(count); full[key] = path;
    }
    if (!_wcsicmp(full[L"--exe"].c_str(), full[L"--cache"].c_str()) ||
        !_wcsicmp(full[L"--exe"].c_str(), full[L"--report"].c_str()) ||
        !_wcsicmp(full[L"--cache"].c_str(), full[L"--report"].c_str())) return 2;
    rgbm::PeImage image; rgbm::ContractBindings bindings; std::string error;
    bool valid = image.load(full[L"--exe"], error);
    if (valid) valid = args[L"--mode"] == L"scan" ? rgbm::scan_contract(image, bindings, error) :
        rgbm::read_binding_cache(full[L"--cache"], image, bindings, error);
    if (valid && args[L"--mode"] == L"scan") valid = rgbm::write_binding_cache(full[L"--cache"], bindings, error);
    std::ostringstream report;
    report << "{\"artifact_kind\":\"binding_validation_report\",\"schema_version\":1,\"validated\":"
        << (valid ? "true" : "false") << ",\"hook_installed\":false,\"reason\":" << rgbm::json_string(error)
        << ",\"exe_sha256\":" << rgbm::json_string(image.identity().sha256)
        << ",\"interface_id\":" << rgbm::json_string(rgbm::interface_id)
        << ",\"source\":" << rgbm::json_string(bindings.source) << ",\"entries\":[";
    if (valid) for (size_t i = 0; i < rgbm::role_count; ++i) {
        auto& e = bindings.entries[i]; if (i) report << ',';
        report << "{\"role\":" << rgbm::json_string(rgbm::role_name(static_cast<rgbm::Role>(i)))
            << ",\"rva\":" << e.rva << ",\"length\":" << e.length
            << ",\"entry_sha256\":" << rgbm::json_string(e.entry_sha256) << '}';
    }
    report << "]}\n";
    if (!rgbm::publish_report(full[L"--report"], report.str(), error)) return 1;
    printf("binding_validated=%d hook_installed=0\n", valid); return valid ? 0 : 3;
}
