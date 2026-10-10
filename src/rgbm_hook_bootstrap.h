#pragma once
#include <windows.h>
#include <string>
#include "rgbm_hook_runtime.h"
namespace rgbm::runtime {
std::wstring cache_from_command_line(std::string& error);
void report_bootstrap(const wchar_t* cache, State value, const std::string& error,
                      const std::string& exe_sha256);
}
