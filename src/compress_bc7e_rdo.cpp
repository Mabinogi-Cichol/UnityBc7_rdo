// compress_bc7e_rdo.cpp
// 兼容 Unity compress_bc7e.dll 的壳 DLL：
//   - 默认把全部调用转发给项目内的原始 DLL 副本（输出逐字节一致）。
//     原 DLL 副本必须与壳 DLL 同目录：替换脚本会把 Unity 原 DLL 改名为
//     compress_bc7e_original.dll 放入同目录；
//   - 可通过配置文件开启 RDO 后处理（bc7enc_rdo 的 ERT），
//     在 BC7 块流上做率失真优化，使后续 LZ 压缩体积更小。
//
// 本 DLL 不会读取或修改 Unity 安装目录中的任何文件。

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include <mutex>

#include "bc7elib.h"
#include "ert.h"
#include "bc7decomp.h"

namespace
{

// ---------------------------------------------------------------------------
// 原始 DLL 转发
// ---------------------------------------------------------------------------

typedef void      (*pfn_init)(void);
typedef void      (*pfn_params_init)(bc7e_compress_block_params*, bool);
typedef void      (*pfn_compress_blocks)(uint32_t, uint64_t*, const uint32_t*,
                                         const bc7e_compress_block_params*);

struct original_api
{
    HMODULE module = nullptr;
    pfn_init init = nullptr;
    pfn_params_init params_init = nullptr;
    pfn_params_init params_init_basic = nullptr;
    pfn_params_init params_init_fast = nullptr;
    pfn_params_init params_init_slow = nullptr;
    pfn_params_init params_init_slowest = nullptr;
    pfn_params_init params_init_ultrafast = nullptr;
    pfn_params_init params_init_veryfast = nullptr;
    pfn_params_init params_init_veryslow = nullptr;
    pfn_compress_blocks compress_blocks = nullptr;
};

original_api g_orig;
std::once_flag g_orig_once;
char g_orig_error[512] = "";

std::wstring get_module_directory()
{
    wchar_t path[MAX_PATH * 4] = {};
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&get_module_directory, &self))
        return L".";
    DWORD n = GetModuleFileNameW(self, path, (DWORD)(_countof(path) - 1));
    if (!n)
        return L".";
    std::wstring full(path, n);
    size_t slash = full.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : full.substr(0, slash);
}

bool resolve_original_dll_path(std::wstring& out_path)
{
    // 与本 DLL 同目录下的原始副本（替换脚本会把 Unity 原 DLL 改名为
    // compress_bc7e_original.dll 放入同目录）
    std::wstring dir = get_module_directory();
    out_path = dir + L"\\compress_bc7e_original.dll";
    if (GetFileAttributesW(out_path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return true;

    out_path.clear();
    return false;
}

void load_original_once()
{
    std::wstring path;
    if (!resolve_original_dll_path(path))
    {
        snprintf(g_orig_error, sizeof(g_orig_error),
                 "compress_bc7e_original.dll not found next to this DLL");
        return;
    }

    HMODULE mod = LoadLibraryW(path.c_str());
    if (!mod)
    {
        snprintf(g_orig_error, sizeof(g_orig_error),
                 "LoadLibrary failed for %ls (error %lu)", path.c_str(),
                 (unsigned long)GetLastError());
        return;
    }

    g_orig.module = mod;
    g_orig.init = (pfn_init)GetProcAddress(mod, "bc7elib_compress_block_init");
    g_orig.params_init = (pfn_params_init)GetProcAddress(mod, "bc7elib_compress_block_params_init");
    g_orig.params_init_basic = (pfn_params_init)GetProcAddress(mod, "bc7elib_compress_block_params_init_basic");
    g_orig.params_init_fast = (pfn_params_init)GetProcAddress(mod, "bc7elib_compress_block_params_init_fast");
    g_orig.params_init_slow = (pfn_params_init)GetProcAddress(mod, "bc7elib_compress_block_params_init_slow");
    g_orig.params_init_slowest = (pfn_params_init)GetProcAddress(mod, "bc7elib_compress_block_params_init_slowest");
    g_orig.params_init_ultrafast = (pfn_params_init)GetProcAddress(mod, "bc7elib_compress_block_params_init_ultrafast");
    g_orig.params_init_veryfast = (pfn_params_init)GetProcAddress(mod, "bc7elib_compress_block_params_init_veryfast");
    g_orig.params_init_veryslow = (pfn_params_init)GetProcAddress(mod, "bc7elib_compress_block_params_init_veryslow");
    g_orig.compress_blocks = (pfn_compress_blocks)GetProcAddress(mod, "bc7elib_compress_blocks");

    if (!g_orig.init || !g_orig.params_init || !g_orig.params_init_basic ||
        !g_orig.params_init_fast || !g_orig.params_init_slow ||
        !g_orig.params_init_slowest || !g_orig.params_init_ultrafast ||
        !g_orig.params_init_veryfast || !g_orig.params_init_veryslow ||
        !g_orig.compress_blocks)
    {
        snprintf(g_orig_error, sizeof(g_orig_error),
                 "original DLL %ls is missing expected exports", path.c_str());
        FreeLibrary(mod);
        g_orig = original_api();
        return;
    }

    snprintf(g_orig_error, sizeof(g_orig_error), "%ls", path.c_str());
}

const original_api& original()
{
    std::call_once(g_orig_once, load_original_once);
    return g_orig;
}

// ---------------------------------------------------------------------------
// RDO 配置（仅读取与壳 DLL 同目录的 bc7_rdo_config.txt，文件修改后自动重读）
// ---------------------------------------------------------------------------

struct rdo_config
{
    float lambda = 0.0f;
    uint32_t lookback = 128;
    float smooth_scale = 15.0f;
    float max_rms_increase_ratio = 10.0f;
    bool two_matches = true;
    bool debug = false;
};

std::wstring get_config_path()
{
    // 与本 DLL 同目录下的配置文件
    return get_module_directory() + L"\\bc7_rdo_config.txt";
}

rdo_config parse_config_file(const std::wstring& path)
{
    rdo_config c;

    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f)
        return c;

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char key[64] = {}, value[128] = {};
        if (sscanf(line, " %63[^= \t] = %127s", key, value) != 2)
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
    }
    fclose(f);
    return c;
}

const rdo_config& config()
{
    // 每次调用前检查配置文件是否新建/修改，避免首次解析后无法感知变化。
    static rdo_config cached;
    static FILETIME cached_time = {};
    static bool first = true;
    static std::wstring cached_path;

    std::wstring path = get_config_path();
    if (path != cached_path)
    {
        first = true;
        cached_time = FILETIME{};
        cached_path = path;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    BOOL exists = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad);

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
        cached = parse_config_file(path);
        cached_time = fad.ftLastWriteTime;
        first = false;
    }
    return cached;
}

bool unpack_bc7_callback(const void* pBlock, ert::color_rgba* pPixels,
                         uint32_t /*block_index*/, void* /*pUserData*/)
{
    return bc7decomp::unpack_bc7(pBlock,
                                 reinterpret_cast<bc7decomp::color_rgba*>(pPixels));
}

void apply_rdo(uint32_t num_blocks, uint64_t* pBlocks, const uint32_t* pPixelsRGBA)
{
    const rdo_config& cfg = config();
    float lambda = cfg.lambda;
    if (lambda <= 0.0f || !num_blocks)
        return;

    ert::reduce_entropy_params ep;
    ep.m_lambda = lambda;
    ep.m_lookback_window_size = cfg.lookback;
    ep.m_smooth_block_max_mse_scale = cfg.smooth_scale;
    ep.m_max_allowed_rms_increase_ratio = cfg.max_rms_increase_ratio;
    ep.m_try_two_matches = cfg.two_matches;
    ep.m_debug_output = cfg.debug;

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

} // namespace

// ---------------------------------------------------------------------------
// 导出实现
// ---------------------------------------------------------------------------

extern "C" void bc7elib_compress_block_init(void)
{
    const original_api& api = original();
    if (api.init)
        api.init();
}

extern "C" void bc7elib_compress_block_params_init(
    bc7e_compress_block_params* p, bool perceptual)
{
    const original_api& api = original();
    if (api.params_init)
        api.params_init(p, perceptual);
    else
        memset(p, 0, sizeof(*p));
}

extern "C" void bc7elib_compress_block_params_init_basic(
    bc7e_compress_block_params* p, bool perceptual)
{
    const original_api& api = original();
    if (api.params_init_basic)
        api.params_init_basic(p, perceptual);
    else
        memset(p, 0, sizeof(*p));
}

extern "C" void bc7elib_compress_block_params_init_fast(
    bc7e_compress_block_params* p, bool perceptual)
{
    const original_api& api = original();
    if (api.params_init_fast)
        api.params_init_fast(p, perceptual);
    else
        memset(p, 0, sizeof(*p));
}

extern "C" void bc7elib_compress_block_params_init_slow(
    bc7e_compress_block_params* p, bool perceptual)
{
    const original_api& api = original();
    if (api.params_init_slow)
        api.params_init_slow(p, perceptual);
    else
        memset(p, 0, sizeof(*p));
}

extern "C" void bc7elib_compress_block_params_init_slowest(
    bc7e_compress_block_params* p, bool perceptual)
{
    const original_api& api = original();
    if (api.params_init_slowest)
        api.params_init_slowest(p, perceptual);
    else
        memset(p, 0, sizeof(*p));
}

extern "C" void bc7elib_compress_block_params_init_ultrafast(
    bc7e_compress_block_params* p, bool perceptual)
{
    const original_api& api = original();
    if (api.params_init_ultrafast)
        api.params_init_ultrafast(p, perceptual);
    else
        memset(p, 0, sizeof(*p));
}

extern "C" void bc7elib_compress_block_params_init_veryfast(
    bc7e_compress_block_params* p, bool perceptual)
{
    const original_api& api = original();
    if (api.params_init_veryfast)
        api.params_init_veryfast(p, perceptual);
    else
        memset(p, 0, sizeof(*p));
}

extern "C" void bc7elib_compress_block_params_init_veryslow(
    bc7e_compress_block_params* p, bool perceptual)
{
    const original_api& api = original();
    if (api.params_init_veryslow)
        api.params_init_veryslow(p, perceptual);
    else
        memset(p, 0, sizeof(*p));
}

extern "C" void bc7elib_compress_blocks(
    uint32_t num_blocks, uint64_t* pBlocks, const uint32_t* pPixelsRGBA,
    const bc7e_compress_block_params* pComp_params)
{
    const original_api& api = original();
    if (api.compress_blocks)
        api.compress_blocks(num_blocks, pBlocks, pPixelsRGBA, pComp_params);
    else
        memset(pBlocks, 0, (size_t)num_blocks * 16u);

    apply_rdo(num_blocks, pBlocks, pPixelsRGBA);
}

extern "C" uint32_t bc7elib_debug_unpack_blocks(
    uint32_t num_blocks, const uint64_t* pBlocks, uint8_t* pPixelsOut)
{
    uint32_t failed = 0;
    for (uint32_t i = 0; i < num_blocks; i++)
    {
        bc7decomp::color_rgba px[16];
        if (!bc7decomp::unpack_bc7(&pBlocks[i * 2], px))
        {
            failed++;
            memset(px, 0, sizeof(px));
        }
        memcpy(pPixelsOut + i * 64, px, sizeof(px));
    }
    return failed;
}
