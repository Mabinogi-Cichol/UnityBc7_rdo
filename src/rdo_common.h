// rdo_common.h - BC7 RDO 共享代码（壳 DLL 与注入版 hook DLL 共用）
//
// 从 compress_bc7e_rdo.cpp 原样提取的 RDO 相关代码，行为与重构前完全一致
// （tests/test_compat.py 要求输出逐字节一致）：
//   - rdo::module_directory     : 由 DLL 内任意函数地址解析该 DLL 所在目录
//   - rdo::config_path_in_module_dir : 模块目录 + \bc7_rdo_config.txt
//   - rdo::load_config          : 解析配置文件
//   - rdo::watched_config       : 配置热重载（缓存 + 文件时间检查）
//   - rdo::apply_rdo            : 对 BC7 块流做 ERT RDO 后处理
//   - rdo::unpack_bc7_callback  : ERT 解包回调
//
// 本头文件不导出任何函数；ert.h / bc7decomp.h 由 /I vendor\bc7enc_rdo 提供。

#pragma once

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <string>
#include <mutex>

#include "ert.h"
#include "bc7decomp.h"

namespace rdo
{

// ---------------------------------------------------------------------------
// 工具：由 DLL 内任意函数地址解析该 DLL 所在目录
// ---------------------------------------------------------------------------

// GetModuleHandleExW(FROM_ADDRESS) + GetModuleFileNameW 解析 address_in_module
// 所属模块的完整路径并截取目录部分；失败时返回 L"."（与原实现一致）。
inline std::wstring module_directory(LPCVOID address_in_module)
{
    wchar_t path[MAX_PATH * 4] = {};
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)address_in_module, &self))
        return L".";
    DWORD n = GetModuleFileNameW(self, path, (DWORD)(_countof(path) - 1));
    if (!n)
        return L".";
    std::wstring full(path, n);
    size_t slash = full.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : full.substr(0, slash);
}

// 配置路径 = module_fn 所属 DLL 所在目录 + "\bc7_rdo_config.txt"
inline std::wstring config_path_in_module_dir(LPCVOID module_fn)
{
    return module_directory(module_fn) + L"\\bc7_rdo_config.txt";
}

// ---------------------------------------------------------------------------
// RDO 配置
// ---------------------------------------------------------------------------

struct rdo_config
{
    float lambda = 0.0f;
    uint32_t lookback = 128;
    float smooth_scale = 15.0f;
    float max_rms_increase_ratio = 10.0f;
    bool two_matches = true;
    bool debug = false;

    // 仅供定位 Unity 调用路径：记录前 N 次 bc7elib_compress_blocks 调用的
    // 原生返回地址栈。0=关闭；不参与编码决策。
    uint32_t trace_bc7_callstacks = 0;

    // Unity 2022.3.62f1 专用的 RGBM 上下文探针/RDO 配置。启用上下文 hook
    // 需要在 Unity 进程启动前设置；运行中热改为 1 不会补装 native hook。
    bool unity_rgbm_context_hooks = false;
    // 仅 bc7enc_rdo/ERT 路径：以 Linear(sRGB)*M HDR 重建误差替换普通 RGBA MSE。
    // Oodle 自己完成 RDO，始终直接调用 Oodle，不使用该 ERT 选项。
    bool rgbm_rdo = true;
    float rgbm_lambda = 0.25f;
    float rgbm_smooth_scale = 30.0f;
    float rgbm_max_rms_increase_ratio = 1.5f;

    // Oodle 编码器选项（仅 encoder=oodle 时生效；默认 bc7enc_rdo 路径忽略）：
    //   use_oodle   : encoder=oodle 时为 true（大小写不敏感）
    //   oodle_effort: low=10, normal=20, high=30（其他值一律映射为 high=30）
    //   oodle_lambda: Oodle RDO 强度（整数 0..100，默认 30）；与 ERT 的 lambda
    //                 相互独立，不做换算
    //   rgbm_oodle_lambda: 仅 RGBM Job 的覆盖值；-1 表示沿用 oodle_lambda
    bool use_oodle = false;
    int oodle_effort = 30;
    int oodle_lambda = 30;
    int rgbm_oodle_lambda = -1;
};

// 解析配置文件（key = value 格式，未知键忽略，缺省使用 rdo_config 默认值）
inline rdo_config load_config(const std::wstring& path)
{
    rdo_config c;

    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f)
        return c;

    char line[256];
    bool first_line = true;
    while (fgets(line, sizeof(line), f))
    {
        // 跳过文件首行的 UTF-8 BOM（EF BB BF）：否则 BOM 会混进第一个 key
        //（例如 encoder=），导致该键被静默忽略。
        char* p = line;
        if (first_line)
        {
            first_line = false;
            if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
                (unsigned char)p[2] == 0xBF)
                p += 3;
        }
        char key[64] = {}, value[128] = {};
        if (sscanf(p, " %63[^= \t] = %127s", key, value) != 2)
            continue;
        if (!_stricmp(key, "lambda"))
            c.lambda = (float)atof(value);
        else if (!_stricmp(key, "lookback"))
            c.lookback = (uint32_t)strtoul(value, nullptr, 10);
        else if (!_stricmp(key, "smooth_scale"))
            c.smooth_scale = (float)atof(value);
        else if (!_stricmp(key, "max_rms_increase_ratio"))
            c.max_rms_increase_ratio = (float)atof(value);
        else if (!_stricmp(key, "two_matches"))
            c.two_matches = atoi(value) != 0;
        else if (!_stricmp(key, "debug"))
            c.debug = atoi(value) != 0;
        else if (!_stricmp(key, "trace_bc7_callstacks"))
        {
            char* end = nullptr;
            errno = 0;
            unsigned long v = strtoul(value, &end, 10);
            if (errno == 0 && end != value && *end == '\0' && v <= 64)
                c.trace_bc7_callstacks = (uint32_t)v;
        }
        else if (!_stricmp(key, "unity_rgbm_context_hooks"))
            c.unity_rgbm_context_hooks = atoi(value) != 0;
        else if (!_stricmp(key, "rgbm_rdo"))
            c.rgbm_rdo = atoi(value) != 0;
        else if (!_stricmp(key, "rgbm_lambda"))
            c.rgbm_lambda = (float)atof(value);
        else if (!_stricmp(key, "rgbm_smooth_scale"))
            c.rgbm_smooth_scale = (float)atof(value);
        else if (!_stricmp(key, "rgbm_max_rms_increase_ratio"))
            c.rgbm_max_rms_increase_ratio = (float)atof(value);
        else if (!_stricmp(key, "encoder"))
            c.use_oodle = !_stricmp(value, "oodle");
        else if (!_stricmp(key, "oodle_effort"))
        {
            if (!_stricmp(value, "low"))
                c.oodle_effort = 10;
            else if (!_stricmp(value, "normal"))
                c.oodle_effort = 20;
            else // "high" 及任何其他值 -> 30
                c.oodle_effort = 30;
        }
        else if (!_stricmp(key, "oodle_lambda"))
        {
            // 仅接受完整解析成功且落在 0..100 的整数；否则保持默认值 30
            char* end = nullptr;
            errno = 0;
            long v = strtol(value, &end, 10);
            if (errno == 0 && end != value && *end == '\0' && v >= 0 && v <= 100)
                c.oodle_lambda = (int)v;
        }
        else if (!_stricmp(key, "rgbm_oodle_lambda"))
        {
            // -1=继承通用 oodle_lambda；0..100=RGBM 专用值。
            char* end = nullptr;
            errno = 0;
            long v = strtol(value, &end, 10);
            if (errno == 0 && end != value && *end == '\0' && v >= -1 && v <= 100)
                c.rgbm_oodle_lambda = (int)v;
        }
    }
    fclose(f);
    return c;
}

// 配置热重载：每次调用检查配置文件是否新建/修改，避免首次解析后无法感知变化。
// config_path 由调用方提供（例如 config_path_in_module_dir(模块内函数地址)）。
inline const rdo_config& watched_config(const std::wstring& config_path)
{
    static rdo_config cached;
    static FILETIME cached_time = {};
    static bool first = true;
    static std::wstring cached_path;

    if (config_path != cached_path)
    {
        first = true;
        cached_time = FILETIME{};
        cached_path = config_path;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    BOOL exists = GetFileAttributesExW(config_path.c_str(), GetFileExInfoStandard, &fad);

    if (!exists)
    {
        if (first)
        {
            cached = rdo_config();
            first = false;
        }
        return cached;
    }

    if (first || CompareFileTime(&fad.ftLastWriteTime, &cached_time) != 0)
    {
        cached = load_config(config_path);
        cached_time = fad.ftLastWriteTime;
        first = false;
    }
    return cached;
}

// ---------------------------------------------------------------------------
// ERT RDO 后处理
// ---------------------------------------------------------------------------

inline bool unpack_bc7_callback(const void* pBlock, ert::color_rgba* pPixels,
                                uint32_t /*block_index*/, void* /*pUserData*/)
{
    return bc7decomp::unpack_bc7(pBlock,
                                 reinterpret_cast<bc7decomp::color_rgba*>(pPixels));
}

// 对 num_blocks 个 16 字节 BC7 块做 ERT 率失真优化，使后续 LZ 压缩体积更小。
// 配置路径固定为本模块目录（&apply_rdo 即本 DLL 内地址，& 取址保证函数被
// 链接器真实发为独立函数，FROM_ADDRESS 可据此解析所属模块）。
inline void apply_rdo(uint32_t num_blocks, uint64_t* pBlocks,
                      const uint32_t* pPixelsRGBA, bool rgbm = false)
{
    const rdo_config& cfg = watched_config(config_path_in_module_dir((LPCVOID)&apply_rdo));
    // lambda 仍是 ERT 的总开关；RGBM 开启后再使用自己的保守强度。
    if (cfg.lambda <= 0.0f || !num_blocks)
        return;
    const float lambda = rgbm ? cfg.rgbm_lambda : cfg.lambda;
    if (lambda <= 0.0f)
        return;

    ert::reduce_entropy_params ep;
    ep.m_lambda = lambda;
    ep.m_lookback_window_size = cfg.lookback;
    ep.m_smooth_block_max_mse_scale = rgbm ? cfg.rgbm_smooth_scale : cfg.smooth_scale;
    ep.m_max_allowed_rms_increase_ratio = rgbm ?
        cfg.rgbm_max_rms_increase_ratio : cfg.max_rms_increase_ratio;
    ep.m_try_two_matches = cfg.two_matches;
    ep.m_debug_output = cfg.debug;
    ep.m_rgbm = rgbm;

    uint32_t modified = 0;
    ert::reduce_entropy(
        pBlocks, num_blocks,
        16 /*total_block_stride_in_bytes*/,
        16 /*block_size_to_optimize_in_bytes*/,
        4 /*block_width*/, 4 /*block_height*/, 4 /*num_comps*/,
        reinterpret_cast<const ert::color_rgba*>(pPixelsRGBA),
        ep, modified,
        unpack_bc7_callback, nullptr,
        nullptr /*pBlock_mse_scales*/);
}

} // namespace rdo
