// bc7_hook_dll.cpp
// 外挂版 BC7 RDO（Editor Plugin 载荷）：
//   - 不替换、不修改 Unity 安装目录中的任何文件；
//   - 由 Editor Plugin 的 hook_loader.cs 在编辑器域加载时 LoadLibraryW 加载进
//     主编辑器进程；并行导入的 AssetImportWorker 子进程由插件用
//     CreateRemoteThread + LoadLibraryW 注入。对本进程内已加载的
//     compress_bc7e.dll 的 bc7elib_compress_blocks 导出桩做 5 字节
//     E9 inline detour，跳转到本 DLL 内分配的跳板，跳板再做绝对跳转进入本
//     DLL 的 hook 函数；
//   - hook 函数调用原始实现后做与壳 DLL 完全一致的 ERT RDO 后处理（配置
//     bc7_rdo_config.txt 从本 DLL 所在目录热读，复用 rdo_common.h）。
//
// 约束与设计：
//   - x64 only，/MT，无导出函数，提供 DllMain；
//   - 不做任何 IAT 修改，不 LoadLibrary 原 DLL（已加载模块本身即原 DLL，
//     原函数指针从导出桩的 E9 rel32 目标解析）；
//   - 本 DLL 永不卸载（注入方不会 FreeLibrary），无需处理 DLL_PROCESS_DETACH；
//   - 日志：本 DLL 目录下 bc7_rdo_hook.log，追加写，简单 FILE* + 单锁防并发。

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <string>
#include <mutex>
#include <atomic>
#include <vector>

#include "bc7elib.h"
#include "rdo_common.h"
#include "oodle_hook.h"

namespace
{

void hook_log(const char* fmt, ...);

// ---------------------------------------------------------------------------
// 日志（本 DLL 目录下 bc7_rdo_hook.log，追加写）
// ---------------------------------------------------------------------------

std::mutex g_log_mutex;

void hook_log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);

    std::wstring log_path =
        rdo::module_directory((LPCVOID)&hook_log) + L"\\bc7_rdo_hook.log";

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
// 原始实现指针（从导出桩 E9 rel32 目标解析）
// ---------------------------------------------------------------------------

typedef void (*pfn_compress_blocks)(uint32_t, uint64_t*, const uint32_t*,
                                    const bc7e_compress_block_params*);

// 原 bc7elib_compress_blocks 真实实现地址；装钩完成后才暴露给并发调用，
// 用 Interlocked 原子写，保证多线程可见性。
volatile pfn_compress_blocks g_real = nullptr;

// ---------------------------------------------------------------------------
// Unity RGBM 上下文（仅 Unity 2022.3.62f1，默认关闭）
// ---------------------------------------------------------------------------
//
// Unity 的 BC7 压缩实际在 Job Worker 中执行，bc7elib_compress_blocks 的参数
// 本身不携带 TextureUsageMode。该版本的 TextureImporting 模块在压缩前调用
// ConvertImageToRGBM(ImageReference&)，随后将同一 ImageReference::pixels
// （布局 +0x10）传入 BC7 JobData。这里在转换点标记像素缓冲区，在 BC7 Job
// 生命周期内激活标记，并在 lambda 中恢复 thread_local 上下文，从而让最终
// BC7 hook 可以按 RGBM 关闭 RDO。三个内联 detour 都有固定版本的 12-byte
// 指纹，任一不匹配即不安装。

typedef void (*pfn_convert_image_to_rgbm)(void* image_reference);
typedef void (*pfn_compress_single_image_texture)(
    void* image_reference, uint32_t texture_format, uint32_t texture_usage_mode,
    int quality, bool flag0, bool flag1, bool flag2, const uint8_t* flag_out,
    const void* texture);
typedef void* (*pfn_convert_image_for_compression)(void* source_image_reference,
                                                    void* temporary_image,
                                                    uint32_t graphics_format,
                                                    uint32_t texture_format);
typedef void (*pfn_bc7_threaded_process)(uint32_t format, void* job_data,
                                         void* job_function);
typedef void (*pfn_bc7_job_lambda)(uint32_t worker_index, void* job_data,
                                   uint32_t begin, uint32_t end);

volatile pfn_convert_image_to_rgbm g_real_convert_image_to_rgbm = nullptr;
volatile pfn_compress_single_image_texture g_real_compress_single_image_texture = nullptr;
volatile pfn_convert_image_for_compression g_real_convert_image_for_compression = nullptr;
volatile pfn_bc7_threaded_process g_real_bc7_threaded_process = nullptr;
volatile pfn_bc7_job_lambda g_real_bc7_job_lambda = nullptr;

std::mutex g_rgbm_sources_mutex;
std::vector<void*> g_rgbm_sources;
std::vector<void*> g_active_rgbm_sources;
std::atomic<uint32_t> g_rgbm_convert_count{ 0 };
std::atomic<uint32_t> g_rgbm_copy_count{ 0 };
std::atomic<uint32_t> g_rgbm_process_count{ 0 };
std::atomic<uint32_t> g_rgbm_job_count{ 0 };
thread_local uint32_t g_rgbm_context_depth = 0;
thread_local uint32_t g_rgbm_compress_single_depth = 0;

constexpr uintptr_t kUnityConvertImageToRgbmRva = 0x1EDFED0;
constexpr uintptr_t kUnityCompressSingleImageTextureRva = 0x1EDC1A0;
constexpr uintptr_t kUnityConvertImageForCompressionRva = 0x1EDFBF0;
constexpr uintptr_t kUnityBc7ThreadedProcessRva = 0x1ED6540;
constexpr uintptr_t kUnityBc7JobLambdaRva = 0x1ED9710;
constexpr size_t kImageReferencePixelsOffset = 0x10;
constexpr size_t kBc7JobDataSourcePixelsOffset = 0x08;

bool is_rgbm_source(void* pixels)
{
    if (!pixels)
        return false;
    std::lock_guard<std::mutex> lock(g_rgbm_sources_mutex);
    for (void* marked : g_active_rgbm_sources)
        if (marked == pixels)
            return true;
    return false;
}

bool is_pending_rgbm_source(void* pixels)
{
    if (!pixels)
        return false;
    std::lock_guard<std::mutex> lock(g_rgbm_sources_mutex);
    for (void* marked : g_rgbm_sources)
        if (marked == pixels)
            return true;
    return false;
}

void mark_rgbm_source(void* pixels)
{
    if (!pixels)
        return;
    std::lock_guard<std::mutex> lock(g_rgbm_sources_mutex);
    for (void* marked : g_rgbm_sources)
        if (marked == pixels)
            return;

    // 仅用于活跃导入批次的关联；上限避免长时间编辑会话无限积累。
    constexpr size_t kMaxRememberedSources = 4096;
    if (g_rgbm_sources.size() == kMaxRememberedSources)
        g_rgbm_sources.erase(g_rgbm_sources.begin());
    g_rgbm_sources.push_back(pixels);
}

bool activate_rgbm_source(void* pixels)
{
    if (!pixels)
        return false;
    std::lock_guard<std::mutex> lock(g_rgbm_sources_mutex);
    for (auto it = g_rgbm_sources.begin(); it != g_rgbm_sources.end(); ++it)
    {
        if (*it == pixels)
        {
            g_rgbm_sources.erase(it);
            g_active_rgbm_sources.push_back(pixels);
            return true;
        }
    }
    return false;
}

void deactivate_rgbm_source(void* pixels)
{
    std::lock_guard<std::mutex> lock(g_rgbm_sources_mutex);
    for (auto it = g_active_rgbm_sources.begin(); it != g_active_rgbm_sources.end(); ++it)
    {
        if (*it == pixels)
        {
            g_active_rgbm_sources.erase(it);
            return;
        }
    }
}

bool is_rgbm_usage_mode(uint32_t usage_mode)
{
    // Unity 2022.3.62f1 TextureUtil::IsRGBMUsageMode 的同一判定：枚举
    // BakedLightmapRGBM（以及另外两个 RGBM usage）对应 bit 2/5/9。
    return usage_mode <= 9 && ((0x224u >> usage_mode) & 1u) != 0;
}

extern "C" void unity_compress_single_image_texture_hook(
    void* image_reference, uint32_t texture_format, uint32_t texture_usage_mode,
    int quality, bool flag0, bool flag1, bool flag2, const uint8_t* flag_out,
    const void* texture)
{
    const bool is_rgbm = is_rgbm_usage_mode(texture_usage_mode);
    if (is_rgbm)
    {
        ++g_rgbm_compress_single_depth;
        hook_log("RGBMCTX compress-single RGBM usage=%u format=%u image=0x%p",
                 texture_usage_mode, texture_format, image_reference);
    }

    const pfn_compress_single_image_texture real = g_real_compress_single_image_texture;
    if (real)
        real(image_reference, texture_format, texture_usage_mode, quality,
             flag0, flag1, flag2, flag_out, texture);

    if (is_rgbm)
        --g_rgbm_compress_single_depth;
}

extern "C" void unity_convert_image_to_rgbm_hook(void* image_reference)
{
    const pfn_convert_image_to_rgbm real = g_real_convert_image_to_rgbm;
    if (real)
        real(image_reference);

    void* pixels = image_reference
        ? *(void**)((uint8_t*)image_reference + kImageReferencePixelsOffset)
        : nullptr;
    mark_rgbm_source(pixels);

    const uint32_t index = g_rgbm_convert_count.fetch_add(1,
                                                            std::memory_order_relaxed);
    if (index < 32)
        hook_log("RGBMCTX convert index=%u image=0x%p pixels=0x%p",
                 index, image_reference, pixels);
}

extern "C" void* unity_convert_image_for_compression_hook(
    void* source_image_reference, void* temporary_image, uint32_t graphics_format,
    uint32_t texture_format)
{
    void* source_pixels = source_image_reference
        ? *(void**)((uint8_t*)source_image_reference + kImageReferencePixelsOffset)
        : nullptr;
    const bool source_is_rgbm = is_pending_rgbm_source(source_pixels);

    const pfn_convert_image_for_compression real = g_real_convert_image_for_compression;
    void* output_image_reference = real
        ? real(source_image_reference, temporary_image, graphics_format, texture_format)
        : source_image_reference;

    void* output_pixels = output_image_reference
        ? *(void**)((uint8_t*)output_image_reference + kImageReferencePixelsOffset)
        : nullptr;
    const uint32_t index = g_rgbm_copy_count.fetch_add(1,
                                                         std::memory_order_relaxed);
    if (index < 64)
    {
        hook_log("RGBMCTX compression-copy index=%u rgbm=%d source=0x%p "
                 "output=0x%p format=%u/%u",
                 index, source_is_rgbm ? 1 : 0, source_pixels, output_pixels,
                 graphics_format, texture_format);
    }
    if (source_is_rgbm)
        mark_rgbm_source(output_pixels);
    return output_image_reference;
}

extern "C" void unity_bc7_threaded_process_hook(uint32_t format, void* job_data,
                                                   void* job_function)
{
    void* source_pixels = job_data
        ? *(void**)((uint8_t*)job_data + kBc7JobDataSourcePixelsOffset)
        : nullptr;
    // CompressSingleImageTexture 的第三个参数就是 TextureUsageMode；它包住
    // 此 Job 的完整调度/等待周期，因而是比中间 ImageReference 指针可靠的
    // RGBM 上下文来源。
    const bool is_rgbm = g_rgbm_compress_single_depth != 0;
    if (is_rgbm)
    {
        std::lock_guard<std::mutex> lock(g_rgbm_sources_mutex);
        g_active_rgbm_sources.push_back(source_pixels);
    }
    const uint32_t index = g_rgbm_process_count.fetch_add(1,
                                                            std::memory_order_relaxed);
    if (index < 64)
        hook_log("RGBMCTX bc7-process index=%u rgbm=%d format=%u job=0x%p "
                 "source=0x%p function=0x%p",
                 index, is_rgbm ? 1 : 0, format, job_data, source_pixels,
                 job_function);

    const pfn_bc7_threaded_process real = g_real_bc7_threaded_process;
    if (real)
        real(format, job_data, job_function);

    // ThreadedProcessImage 在返回前已等待自己的 worker fence 完成；此时移除
    // active 标记，避免同一内存地址日后被非 RGBM 图片复用时误命中。
    if (is_rgbm)
        deactivate_rgbm_source(source_pixels);
}

extern "C" void unity_bc7_job_lambda_hook(uint32_t worker_index, void* job_data,
                                             uint32_t begin, uint32_t end)
{
    void* source_pixels = job_data
        ? *(void**)((uint8_t*)job_data + kBc7JobDataSourcePixelsOffset)
        : nullptr;
    const bool is_rgbm = is_rgbm_source(source_pixels);
    if (is_rgbm)
    {
        ++g_rgbm_context_depth;
        const uint32_t index = g_rgbm_job_count.fetch_add(1,
                                                            std::memory_order_relaxed);
        if (index < 32)
            hook_log("RGBMCTX bc7-job index=%u worker=%u job=0x%p source=0x%p "
                     "range=[%u,%u)",
                     index, worker_index, job_data, source_pixels, begin, end);
    }

    const pfn_bc7_job_lambda real = g_real_bc7_job_lambda;
    if (real)
        real(worker_index, job_data, begin, end);

    if (is_rgbm)
        --g_rgbm_context_depth;
}

// trace_bc7_callstacks 的进程内计数。只在显式开启时自增，确保默认路径
// 完全没有额外开销；每个注入的 AssetImportWorker 独立采样。
std::atomic<uint32_t> g_bc7_trace_count{ 0 };

// 记录当前 BC7 调用的原生返回地址栈。CaptureStackBackTrace 只能提供地址，
// 所以同时打印所属模块和相对模块偏移；这能跨 ASLR 稳定地用于定位 Unity.exe
// 内的调用者。frame[0] 位于本 hook，通常 frame[1] 即 Unity 的直接调用点。
void log_bc7_callstack(uint32_t trace_index, uint32_t num_blocks,
                       const void* p_comp_params)
{
    void* frames[16] = {};
    const USHORT num_frames = CaptureStackBackTrace(
        0, (ULONG)_countof(frames), frames, nullptr);

    hook_log("BC7TRACE begin index=%u num_blocks=%u pCompParams=0x%p frames=%u",
             trace_index, num_blocks, p_comp_params, (unsigned)num_frames);

    for (USHORT i = 0; i < num_frames; ++i)
    {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)frames[i], &module))
        {
            wchar_t module_path[MAX_PATH * 4] = {};
            DWORD n = GetModuleFileNameW(module, module_path,
                                         (DWORD)_countof(module_path));
            const wchar_t* module_name = module_path;
            if (n)
            {
                const wchar_t* slash = wcsrchr(module_path, L'\\');
                if (slash)
                    module_name = slash + 1;
            }
            const uintptr_t offset = (uintptr_t)frames[i] - (uintptr_t)module;
            hook_log("BC7TRACE frame[%u]=%ls+0x%llX absolute=0x%p",
                     (unsigned)i, module_name,
                     (unsigned long long)offset, frames[i]);
        }
        else
        {
            hook_log("BC7TRACE frame[%u]=unknown absolute=0x%p",
                     (unsigned)i, frames[i]);
        }
    }
}

void maybe_log_bc7_callstack(const rdo::rdo_config& cfg, uint32_t num_blocks,
                             const void* p_comp_params)
{
    if (!cfg.trace_bc7_callstacks)
        return;

    const uint32_t trace_index = g_bc7_trace_count.fetch_add(1,
                                                               std::memory_order_relaxed);
    if (trace_index < cfg.trace_bc7_callstacks)
        log_bc7_callstack(trace_index, num_blocks, p_comp_params);
}

// ---------------------------------------------------------------------------
// 跳板分配（模块 base ± 2GB 内找空闲页）
// ---------------------------------------------------------------------------

// 写 12 字节绝对跳转：48 B8 <imm64 LE> FF E0（mov rax, imm64; jmp rax）
void write_abs_jump(uint8_t* p, uintptr_t target)
{
    p[0] = 0x48;
    p[1] = 0xB8;
    memcpy(p + 2, &target, sizeof(target));
    p[10] = 0xFF;
    p[11] = 0xE0;
}

// 在 mod 模块 base ± 2GB 内扫描空闲页：先向下（base-0x10000 每次减 0x10000
// 到 base-0x7FFF0000），失败再向上（base+0x10000 每次加 0x10000 到
// base+0x7FFF0000），首个成功即用；写入绝对跳转指向 target 并返回地址，
// 全部失败返回 nullptr。
uint8_t* alloc_trampoline(HMODULE mod, uintptr_t target)
{
    const uintptr_t kPage = 0x1000;
    const uintptr_t kStep = 0x10000;
    const uintptr_t base = (uintptr_t)mod;

    // 向下扫描（base 足够大时才可能往下有合法范围；防御回绕）
    if (base >= 0x80000000u)
    {
        const uintptr_t lo = base - 0x7FFF0000u;
        for (uintptr_t addr = base - 0x10000u;; addr -= kStep)
        {
            uint8_t* p = (uint8_t*)VirtualAlloc((LPVOID)addr, kPage,
                                                MEM_RESERVE | MEM_COMMIT,
                                                PAGE_EXECUTE_READWRITE);
            if (p)
            {
                write_abs_jump(p, target);
                return p;
            }
            if (addr < lo + kStep)
                break;
        }
    }

    // 向上扫描
    {
        const uintptr_t hi = base + 0x7FFF0000u;
        for (uintptr_t addr = base + 0x10000u;; addr += kStep)
        {
            uint8_t* p = (uint8_t*)VirtualAlloc((LPVOID)addr, kPage,
                                                MEM_RESERVE | MEM_COMMIT,
                                                PAGE_EXECUTE_READWRITE);
            if (p)
            {
                write_abs_jump(p, target);
                return p;
            }
            if (addr > hi - kStep)
                break;
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// 装钩（inline detour）
// ---------------------------------------------------------------------------

// 固定 12 字节 prologue 的最小 detour。仅用于经 PDB/反汇编确认的 Unity
// 2022.3.62f1 私有函数：复制无相对寻址的原始 prologue 到跳板后，跳回 +12。
template <size_t kPrologueSize>
bool install_unity_inline_detour(HMODULE unity, uintptr_t rva,
                                 const uint8_t (&expected)[kPrologueSize],
                                 uintptr_t hook, void** original,
                                 const char* label)
{
    static_assert(kPrologueSize >= 12, "absolute jump requires 12 bytes");
    uint8_t* target = (uint8_t*)unity + rva;
    if (memcmp(target, expected, sizeof(expected)) != 0)
    {
        hook_log("RGBMCTX %s: Unity signature mismatch at rva=0x%llX; not hooked",
                 label, (unsigned long long)rva);
        return false;
    }

    // alloc_trampoline 先写入一个跳转；随后将其起始覆写成原 prologue，+12
    // 处写回原函数余下部分。该 prologue 已人工确认不含 RIP-relative 指令。
    uint8_t* trampoline = alloc_trampoline(unity, (uintptr_t)target + kPrologueSize);
    if (!trampoline)
    {
        hook_log("RGBMCTX %s: trampoline allocation failed", label);
        return false;
    }
    memcpy(trampoline, target, kPrologueSize);
    write_abs_jump(trampoline + kPrologueSize,
                   (uintptr_t)target + kPrologueSize);

    // 在改写入口前发布 trampoline，避免 Worker 恰好进入 hook 时原函数为空。
    InterlockedExchangePointer((PVOID volatile*)original, trampoline);

    DWORD old_protect = 0;
    if (!VirtualProtect(target, kPrologueSize, PAGE_EXECUTE_READWRITE, &old_protect))
    {
        InterlockedExchangePointer((PVOID volatile*)original, nullptr);
        hook_log("RGBMCTX %s: VirtualProtect failed error=%lu", label,
                 (unsigned long)GetLastError());
        return false;
    }
    write_abs_jump(target, hook);
    DWORD ignored = 0;
    VirtualProtect(target, kPrologueSize, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, kPrologueSize);
    hook_log("RGBMCTX %s: installed rva=0x%llX target=0x%p trampoline=0x%p",
             label, (unsigned long long)rva, target, trampoline);
    return true;
}

void install_unity_rgbm_context_hooks_impl()
{
    const rdo::rdo_config cfg = rdo::load_config(
        rdo::config_path_in_module_dir((LPCVOID)&install_unity_rgbm_context_hooks_impl));
    if (!cfg.unity_rgbm_context_hooks)
        return;

    HMODULE unity = GetModuleHandleW(nullptr);
    if (!unity)
    {
        hook_log("RGBMCTX: Unity main module unavailable; not hooked");
        return;
    }

    // Unity.exe 2022.3.62f1:
    // ConvertImageToRGBM: push rbx; sub rsp,70h; cmp [rcx],34h; mov rbx,rcx
    static const uint8_t kConvertSignature[12] = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x70,
        0x83, 0x39, 0x34, 0x48, 0x8B, 0xD9
    };
    // CompressSingleImageTexture: save rbx/rsi/rdi (its r8d is TextureUsageMode)
    static const uint8_t kCompressSingleSignature[15] = {
        0x48, 0x89, 0x5C, 0x24, 0x08,
        0x48, 0x89, 0x74, 0x24, 0x10,
        0x48, 0x89, 0x7C, 0x24, 0x18
    };
    // ConvertImageForCompressionIfNeeded: save rbx/rbp/rsi, push rdi
    static const uint8_t kCompressionCopySignature[15] = {
        0x48, 0x89, 0x5C, 0x24, 0x08,
        0x48, 0x89, 0x6C, 0x24, 0x10,
        0x48, 0x89, 0x74, 0x24, 0x18
    };
    // BC7 job lambda: save rbx; push rbp/rsi/rdi/r12/r13
    static const uint8_t kLambdaSignature[12] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x55,
        0x56, 0x57, 0x41, 0x54, 0x41, 0x55
    };
    // ThreadedProcessImage<PerformBC7TextureCompression::JobData,8>
    static const uint8_t kThreadedProcessSignature[12] = {
        0x4C, 0x8B, 0xDC, 0x48, 0x81, 0xEC,
        0x58, 0x01, 0x00, 0x00, 0x85, 0xC9
    };

    const bool compress_single_ok = install_unity_inline_detour(
        unity, kUnityCompressSingleImageTextureRva, kCompressSingleSignature,
        (uintptr_t)&unity_compress_single_image_texture_hook,
        (void**)&g_real_compress_single_image_texture,
        "CompressSingleImageTexture");
    const bool process_ok = install_unity_inline_detour(
        unity, kUnityBc7ThreadedProcessRva, kThreadedProcessSignature,
        (uintptr_t)&unity_bc7_threaded_process_hook,
        (void**)&g_real_bc7_threaded_process,
        "ThreadedProcessImage<BC7 JobData>");
    const bool lambda_ok = install_unity_inline_detour(
        unity, kUnityBc7JobLambdaRva, kLambdaSignature,
        (uintptr_t)&unity_bc7_job_lambda_hook,
        (void**)&g_real_bc7_job_lambda, "PerformBC7TextureCompression lambda");

    hook_log("RGBMCTX: setup complete compressSingle=%d process=%d lambda=%d "
             "rgbm_rdo=%d",
             compress_single_ok ? 1 : 0, process_ok ? 1 : 0,
             lambda_ok ? 1 : 0, cfg.rgbm_rdo ? 1 : 0);
}

std::once_flag g_rgbm_context_hook_once;

void install_unity_rgbm_context_hooks()
{
    std::call_once(g_rgbm_context_hook_once, install_unity_rgbm_context_hooks_impl);
}

extern "C" void bc7elib_compress_blocks_hook(
    uint32_t num_blocks, uint64_t* pBlocks, const uint32_t* pPixelsRGBA,
    const bc7e_compress_block_params* pComp_params);

bool install_hook_impl()
{
    HMODULE mod = GetModuleHandleW(L"compress_bc7e.dll");
    if (!mod)
    {
        hook_log("install_hook: FAILED compress_bc7e.dll 未加载");
        return false;
    }

    uint8_t* fn = (uint8_t*)GetProcAddress(mod, "bc7elib_compress_blocks");
    if (!fn)
    {
        hook_log("install_hook: FAILED GetProcAddress(bc7elib_compress_blocks) 失败, "
                 "module=0x%p, error=%lu",
                 (void*)mod, (unsigned long)GetLastError());
        return false;
    }

    // 读导出桩前 5 字节，首字节必须是 0xE9（jmp rel32），否则不硬 patch
    uint8_t stub[5];
    memcpy(stub, fn, sizeof(stub));
    if (stub[0] != 0xE9)
    {
        hook_log("install_hook: FAILED 导出桩首字节 0x%02X 不是 0xE9, "
                 "module=0x%p, fn=0x%p（不硬 patch）",
                 (unsigned)stub[0], (void*)mod, (void*)fn);
        return false;
    }

    // 原函数 = fn + 5 + (int32_t)rel32（E9 的相对目标即真实实现）
    int32_t rel = 0;
    memcpy(&rel, stub + 1, sizeof(rel));
    pfn_compress_blocks real = (pfn_compress_blocks)(fn + 5 + (intptr_t)rel);
    if (!real)
    {
        hook_log("install_hook: FAILED 解析原实现地址为空, module=0x%p, fn=0x%p",
                 (void*)mod, (void*)fn);
        return false;
    }

    // 先发布 g_real 再打桩：桩尚未 patch 时所有调用仍走原实现，发布是
    // 严格无副作用的；反之若先打桩后发布，装钩瞬间并发调用会命中 hook 而
    // g_real 仍为 null，导致该次输出被 memset 清零。
    InterlockedExchangePointer((PVOID volatile*)&g_real, (PVOID)real);

    // 跳板：模块 base ±2GB 内空闲页，写绝对跳转指向 hook 函数
    uint8_t* tramp = alloc_trampoline(mod, (uintptr_t)&bc7elib_compress_blocks_hook);
    if (tramp)
        hook_log("install_hook: 跳板分配成功 tramp=0x%p", (void*)tramp);
    else
        hook_log("install_hook: 跳板分配失败，回退为直接在导出桩写 12 字节绝对跳转");

    // 打桩：导出桩有 16 字节空间，E9 只需 5 字节，12 字节绝对跳转也放得下
    DWORD old_protect = 0;
    if (!VirtualProtect(fn, 16, PAGE_EXECUTE_READWRITE, &old_protect))
    {
        hook_log("install_hook: FAILED VirtualProtect(RWX) 失败, fn=0x%p, error=%lu",
                 (void*)fn, (unsigned long)GetLastError());
        return false;
    }

    if (tramp)
    {
        // 跳板可用：写 5 字节 E9 rel32，rel32 = tramp - (fn + 5)
        const int64_t disp = (int64_t)(uintptr_t)tramp - (int64_t)(uintptr_t)(fn + 5);
        if (disp < INT32_MIN || disp > INT32_MAX)
        {
            // 防御：扫描结果理论上必在 int32 内，这里兜底
            hook_log("install_hook: 跳板位移超出 int32, 回退为绝对跳转");
            write_abs_jump(fn, (uintptr_t)&bc7elib_compress_blocks_hook);
        }
        else
        {
            fn[0] = 0xE9;
            const int32_t disp32 = (int32_t)disp;
            memcpy(fn + 1, &disp32, sizeof(disp32));
        }
    }
    else
    {
        // 回退：直接在导出桩写 12 字节绝对跳转
        write_abs_jump(fn, (uintptr_t)&bc7elib_compress_blocks_hook);
    }

    // 恢复原保护属性（导出桩位于可执行段，通常即 PAGE_EXECUTE_READ）
    DWORD ignored = 0;
    VirtualProtect(fn, 16, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), fn, 16);

    hook_log("install_hook: OK module=0x%p fn=0x%p real=0x%p tramp=0x%p",
             (void*)mod, (void*)fn, (void*)real, (void*)tramp);
    return true;
}

// 装钩只执行一次（DllMain 与轮询线程可能并发触发）
std::once_flag g_hook_once;

void install_hook()
{
    std::call_once(g_hook_once, install_hook_impl);
}

// ---------------------------------------------------------------------------
// hook 函数：签名与 bc7elib.h 中 bc7elib_compress_blocks 完全一致
// ---------------------------------------------------------------------------

extern "C" void bc7elib_compress_blocks_hook(
    uint32_t num_blocks, uint64_t* pBlocks, const uint32_t* pPixelsRGBA,
    const bc7e_compress_block_params* pComp_params)
{
    // 配置在本函数顶部读取一次（含 debug 日志用），热重载由 watched_config 负责
    const rdo::rdo_config& cfg = rdo::watched_config(
        rdo::config_path_in_module_dir((LPCVOID)&install_hook));

    maybe_log_bc7_callstack(cfg, num_blocks, pComp_params);

    const pfn_compress_blocks real = g_real;

    // RGBM context comes from Unity's TextureUsageMode and is restored on the
    // BC7 worker thread. It selects ERT's RGB*M metric below; Oodle always
    // receives RGBM directly because it owns its own RDO metric.
    const bool is_rgbm = g_rgbm_context_depth != 0;

    if (cfg.use_oodle)
    {
        // RGBM 可选用独立 Oodle lambda；未设置（-1）时沿用通用值。
        // 选中的 lambda=0 是该类纹理的 Unity 原生 BC7 快路径：不加载/调用
        // Oodle，也不接 ERT 后处理。因此 oodle_lambda=0 仅影响普通 RGBA，
        // 而 rgbm_oodle_lambda=0 仅影响已识别的 RGBM Job。
        const int l = (is_rgbm && cfg.rgbm_oodle_lambda >= 0) ?
            cfg.rgbm_oodle_lambda : cfg.oodle_lambda;
        if (l == 0)
        {
            if (real)
                real(num_blocks, pBlocks, pPixelsRGBA, pComp_params);
            else
                memset(pBlocks, 0, (size_t)num_blocks * 16u);

            if (cfg.debug)
                hook_log("RDO bypassed (oodle lambda=0): Unity native BC7 "
                         "rgbm=%d num_blocks=%u pBlocks=0x%p",
                         is_rgbm ? 1 : 0, num_blocks, (void*)pBlocks);
            return;
        }

        // ---- Oodle 编码器路径：不调用原 bc7e，不做 ERT 后处理 ----
        if (oodle_hook::available() &&
            oodle_hook::encode_bc7_rdo(num_blocks, pBlocks, pPixelsRGBA, l,
                                       cfg.oodle_effort))
        {
            if (cfg.debug)
                hook_log("RDO called (oodle) rgbm=%d lambda=%d effort=%d "
                         "num_blocks=%u pBlocks=0x%p", is_rgbm ? 1 : 0, l,
                         cfg.oodle_effort, num_blocks, (void*)pBlocks);
            return;
        }

        // Oodle 不可用或编码失败：警告一次（仅一次），然后落到下方默认路径，
        // 按 encoder=bc7enc_rdo 的方式继续——调用原 bc7e 压缩后接
        // rdo::apply_rdo（ERT 后处理），而不是降级为无 RDO 的普通 bc7e。
        static std::once_flag g_oodle_fallback_once;
        std::call_once(g_oodle_fallback_once, []() {
            hook_log("oodle: 编码器不可用/编码失败，本次及后续调用回退默认 "
                     "bc7enc_rdo/ERT RDO 路径（bc7e 压缩 + ERT RDO 后处理）");
        });
    }

    // ---- 默认 bc7enc_rdo 路径（行为与重构前逐字节一致）----
    if (real)
        real(num_blocks, pBlocks, pPixelsRGBA, pComp_params);
    else
        memset(pBlocks, 0, (size_t)num_blocks * 16u);

    const bool use_rgbm_metric = is_rgbm && cfg.rgbm_rdo;
    rdo::apply_rdo(num_blocks, pBlocks, pPixelsRGBA, use_rgbm_metric);

    // 配置 debug=1 时记录每次 RDO 调用（apply_rdo 不返回 modified，记 lambda）
    // 附带 num_blocks 与 pBlocks 地址：用于分析 Unity 的调用批划分
    //（ERT 的 lookback 参考依赖批内连续性，批划分不同输出就不同，
    //  这会让 Unity 的 ConsistencyChecker 报 inconsistent result）
    if (cfg.debug)
        hook_log("RDO called (bc7enc_rdo) rgbm=%d lambda=%f num_blocks=%u "
                 "pBlocks=0x%p", use_rgbm_metric ? 1 : 0,
                 (double)(use_rgbm_metric ? cfg.rgbm_lambda : cfg.lambda),
                 num_blocks, (void*)pBlocks);
}

// ---------------------------------------------------------------------------
// 轮询线程：等待 compress_bc7e.dll 加载后装钩
// ---------------------------------------------------------------------------

DWORD WINAPI poll_for_module(LPVOID /*lpParam*/)
{
    const DWORD kIntervalMs = 50;
    const DWORD kTimeoutMs = 10 * 60 * 1000;  // 10 分钟安全超时
    DWORD waited = 0;

    for (;;)
    {
        if (GetModuleHandleW(L"compress_bc7e.dll"))
        {
            install_hook();
            // RGBM 上下文 hook 依赖 Unity.exe 固定版本，且只在配置启动时
            // 显式开启；放在 DllMain 之外避免在 loader lock 内做文件 I/O/patch。
            install_unity_rgbm_context_hooks();
            return 0;
        }

        Sleep(kIntervalMs);
        waited += kIntervalMs;
        if (waited >= kTimeoutMs)
        {
            hook_log("poll_for_module: 等待 compress_bc7e.dll 超时（10 分钟），放弃装钩");
            return 0;
        }
    }
}

// ---------------------------------------------------------------------------
// 入口（必须位于匿名命名空间之外且为 extern "C"：
// CRT 的 _DllMainCRTStartup 按 C 链接符号 DllMain 绑定入口；
// 若定义在匿名命名空间内（内部链接），链接器找不到它会退化为空桩，
// 且 /OPT:REF 会把本 DLL 的全部代码死剔除——运行时将没有任何装钩行为。）
// ---------------------------------------------------------------------------

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason,
                               LPVOID /*lpvReserved*/)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinstDLL);

        if (GetModuleHandleW(L"compress_bc7e.dll"))
            install_hook();  // 保持既有立即安装行为

        // 无论 compress_bc7e.dll 是否已加载，都启动初始化线程：已加载时它
        // 负责可选 RGBM context hook，未加载时则继续承担原有的轮询职责。
        HANDLE th = CreateThread(nullptr, 0, poll_for_module, nullptr, 0, nullptr);
        if (th)
            CloseHandle(th);  // 线程自管生命周期，句柄用完即关
        else
            hook_log("DllMain: CreateThread 失败, error=%lu",
                     (unsigned long)GetLastError());
    }
    return TRUE;
}
