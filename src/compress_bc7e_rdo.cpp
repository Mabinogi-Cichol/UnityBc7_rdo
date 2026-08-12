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
#include "rdo_common.h"  // 共享 RDO 代码（配置解析/热重载/apply_rdo/模块目录工具）

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

bool resolve_original_dll_path(std::wstring& out_path)
{
    // 与本 DLL 同目录下的原始副本（替换脚本会把 Unity 原 DLL 改名为
    // compress_bc7e_original.dll 放入同目录）
    // （模块目录由共享工具 rdo::module_directory 解析，&resolve_original_dll_path
    //   即本 DLL 内地址）
    std::wstring dir = rdo::module_directory((LPCVOID)&resolve_original_dll_path);
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
// RDO 配置与后处理：已提取到共享头 rdo_common.h（rdo::load_config /
// rdo::watched_config / rdo::apply_rdo / rdo::module_directory 等），
// 行为与重构前完全一致，供本 DLL 与注入版 hook DLL 共用。
// ---------------------------------------------------------------------------

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

    rdo::apply_rdo(num_blocks, pBlocks, pPixelsRGBA);
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
