// oodle_hook.cpp - Oodle Texture 编码器加载 + BC7 RDO 编码
//
// 与 oodle_testenv/src/oodle_bc7.cpp 相同的运行时加载方式：LoadLibraryW +
// GetProcAddress 解析导出（OOEXPLINK == __stdcall，x64 无名字修饰），不做
// .lib 静态链接。所有解析出的函数指针保存在静态变量中。
//
// 设计：
//   - 懒加载 + 线程安全：std::call_once 保证 LoadLibraryW/GetProcAddress 只执行
//     一次，后续线程直接读解析结果；
//   - 加载/解析失败不影响本 DLL 其他功能：available() 返回 false，调用方
//     （bc7_hook_dll.cpp）回退原 bc7e 编码；
//   - 日志与 bc7_hook_dll.cpp 相同的模式：本 DLL 目录下 bc7_rdo_hook.log 追加写。

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#include <string>
#include <mutex>
#include <vector>

#include "rdo_common.h"      // rdo::module_directory（hook DLL 自身目录）
#include "oodle2tex.h"       // Oodle 枚举/结构/常量（仅类型与值，不链接）

#include "oodle_hook.h"

namespace oodle_hook
{

namespace
{

// ---------------------------------------------------------------------------
// 日志（本 DLL 目录下 bc7_rdo_hook.log，追加写；与 bc7_hook_dll.cpp 的
// hook_log 完全同模式，独立实现避免跨 TU 暴露匿名命名空间符号）
// ---------------------------------------------------------------------------

std::mutex g_oodle_log_mutex;

void oodle_log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_oodle_log_mutex);

    std::wstring log_path =
        rdo::module_directory((LPCVOID)&oodle_log) + L"\\bc7_rdo_hook.log";

    FILE* f = _wfopen(log_path.c_str(), L"a");
    if (!f)
        return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Oodle 导出函数指针 typedef（与 oodle_testenv/src/oodle_bc7.cpp 完全一致：
// 全部 __stdcall / OOEXPLINK，undecorated 名字）
// ---------------------------------------------------------------------------

typedef OodleTex_Err (OOEXPLINK *pfn_LogVersion)(void);
typedef const char* (OOEXPLINK *pfn_Err_GetName)(OodleTex_Err e);
typedef OodleTex_Err (OOEXPLINK *pfn_EncodeBCN_RDO_Ex)(
    OodleTex_BC to_bcn, void* to_bcn_blocks, OO_SINTa num_blocks,
    const OodleTex_Surface* from_surfaces, OO_SINTa num_from_surfaces,
    OodleTex_PixelFormat from_format, const OodleTex_Layout* layout,
    int rdo_lagrange_lambda, const OodleTex_RDO_Options* options,
    int num_job_threads, void* jobify_user_ptr);
typedef t_fp_OodleTex_Plugin_Printf* (OOEXPLINK *pfn_Plugins_SetPrintf)(
    t_fp_OodleTex_Plugin_Printf*);

// 解析出的入口点（load_impl 内一次性赋值，成功后不再改变）
bool g_loaded = false;
pfn_EncodeBCN_RDO_Ex g_EncodeBCN_RDO_Ex = nullptr;
pfn_Err_GetName g_Err_GetName = nullptr;
pfn_LogVersion g_LogVersion = nullptr;
pfn_Plugins_SetPrintf g_Plugins_SetPrintf = nullptr;

// ---------------------------------------------------------------------------
// 懒加载（std::call_once 保证只执行一次）
// ---------------------------------------------------------------------------

void load_impl()
{
    const std::wstring dll_path =
        rdo::module_directory((LPCVOID)&load_impl) + L"\\oo2tex_win64_2.9.16.dll";

    HMODULE h = LoadLibraryW(dll_path.c_str());
    if (!h)
    {
        oodle_log("oodle: LoadLibraryW(%ls) 失败 error=%lu（Oodle 编码器不可用，"
                  "hook 将回退原 bc7e）",
                  dll_path.c_str(), (unsigned long)GetLastError());
        return;
    }

    g_EncodeBCN_RDO_Ex = (pfn_EncodeBCN_RDO_Ex)GetProcAddress(
        h, "OodleTex_EncodeBCN_RDO_Ex");
    g_Err_GetName = (pfn_Err_GetName)GetProcAddress(h, "OodleTex_Err_GetName");
    g_LogVersion = (pfn_LogVersion)GetProcAddress(h, "OodleTex_LogVersion");
    g_Plugins_SetPrintf = (pfn_Plugins_SetPrintf)GetProcAddress(
        h, "OodleTex_Plugins_SetPrintf");

    if (!g_EncodeBCN_RDO_Ex || !g_Err_GetName || !g_LogVersion)
    {
        oodle_log("oodle: GetProcAddress 部分失败 (EncodeBCN_RDO_Ex=%p "
                  "Err_GetName=%p LogVersion=%p)，编码器不可用",
                  (void*)g_EncodeBCN_RDO_Ex, (void*)g_Err_GetName,
                  (void*)g_LogVersion);
        return;
    }

    // 静默 Oodle 自身输出（不装 job system / allocators）
    if (g_Plugins_SetPrintf)
        g_Plugins_SetPrintf(nullptr);

    OodleTex_Err err = g_LogVersion();
    if (err != OodleTex_Err_OK)
        oodle_log("oodle: LogVersion 失败: %s",
                  g_Err_GetName ? g_Err_GetName(err) : "?");
    else
        oodle_log("oodle: Oodle Texture %s 已加载", OodleTextureVersion);

    g_loaded = true;
}

std::once_flag g_load_once;

} // namespace

// ---------------------------------------------------------------------------
// 公共接口
// ---------------------------------------------------------------------------

bool available()
{
    std::call_once(g_load_once, load_impl);
    return g_loaded;
}

bool encode_bc7_rdo(uint32_t num_blocks, uint64_t* pBlocks,
                    const uint32_t* pPixelsRGBA, int oodle_lambda, int effort)
{
    if (!available() || !g_EncodeBCN_RDO_Ex)
        return false;
    if (!num_blocks)
        return true;

    // 线程私有 scratch：把 BLOCK-ORDER 输入重排为行主序 surface
    //（width = num_blocks*4 像素，height = 4 像素，每像素 4 字节）。
    thread_local std::vector<uint8_t> scratch;
    const size_t scratch_bytes = (size_t)num_blocks * 64u;  // 16 px * 4 B per block
    if (scratch.size() < scratch_bytes)
        scratch.resize(scratch_bytes);

    const size_t width_px = (size_t)num_blocks * 4u;
    const size_t row_stride = (size_t)num_blocks * 16u;     // bytes
    uint8_t* dst = scratch.data();
    const uint8_t* src_pixels = (const uint8_t*)pPixelsRGBA;
    for (uint32_t b = 0; b < num_blocks; ++b)
    {
        // 块 b 的 4 行像素分别落到 surface 的第 py 行、列偏移 b*4
        const uint8_t* src_block = src_pixels + (size_t)b * 64u;
        for (int py = 0; py < 4; ++py)
            memcpy(dst + (size_t)py * row_stride + (size_t)b * 16u,
                   src_block + (size_t)py * 16u, 16u);
    }

    OodleTex_Surface surface = {};
    surface.pixels = dst;
    surface.rowStrideBytes = (OO_SINTa)row_stride;
    surface.width = (OO_S32)width_px;
    surface.height = 4;

    OodleTex_RDO_Options options = {};  // 必须零初始化（内部有 padding 与保留字段）
    options.effort = (OodleTex_EncodeEffortLevel)effort;
    options.metric = OodleTex_RDO_ErrorMetric_Default;
    options.universal_tiling = OodleTex_RDO_UniversalTiling_Disable;
    options.bcn_flags = OodleTex_BCNFlags_None;
    options.rdo_flags = OodleTex_RDO_Flags_None;

    OodleTex_Err err = g_EncodeBCN_RDO_Ex(
        OodleTex_BC7RGBA, pBlocks, (OO_SINTa)num_blocks,
        &surface, 1, OodleTex_PixelFormat_4_U8_RGBA,
        nullptr /*layout*/, oodle_lambda, &options,
        OODLETEX_JOBS_DISABLE /*-1 单线程*/, nullptr /*jobify_user_ptr*/);

    if (err != OodleTex_Err_OK)
    {
        oodle_log("oodle: EncodeBCN_RDO_Ex 失败 lambda=%d effort=%d blocks=%u "
                  "err=%d(%s)",
                  oodle_lambda, effort, num_blocks, (int)err,
                  g_Err_GetName ? g_Err_GetName(err) : "?");
        return false;
    }
    return true;
}

} // namespace oodle_hook
