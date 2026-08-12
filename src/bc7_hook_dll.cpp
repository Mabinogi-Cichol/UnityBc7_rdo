// bc7_hook_dll.cpp
// 注入版 BC7 RDO：
//   - 不替换、不修改 Unity 安装目录中的任何文件；
//   - 由注入器（bc7rdo_injector.exe）注入 Unity Editor 进程，对本进程内已
//     加载的 compress_bc7e.dll 的 bc7elib_compress_blocks 导出桩做 5 字节
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

#include "bc7elib.h"
#include "rdo_common.h"

namespace
{

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
    const pfn_compress_blocks real = g_real;
    if (real)
        real(num_blocks, pBlocks, pPixelsRGBA, pComp_params);
    else
        memset(pBlocks, 0, (size_t)num_blocks * 16u);

    rdo::apply_rdo(num_blocks, pBlocks, pPixelsRGBA);

    // 配置 debug=1 时记录每次 RDO 调用（apply_rdo 不返回 modified，记 lambda）
    // 附带 num_blocks 与 pBlocks 地址：用于分析 Unity 的调用批划分
    //（ERT 的 lookback 参考依赖批内连续性，批划分不同输出就不同，
    //  这会让 Unity 的 ConsistencyChecker 报 inconsistent result）
    const rdo::rdo_config& cfg = rdo::watched_config(
        rdo::config_path_in_module_dir((LPCVOID)&install_hook));
    if (cfg.debug)
        hook_log("RDO called lambda=%f num_blocks=%u pBlocks=0x%p",
                 (double)cfg.lambda, num_blocks, (void*)pBlocks);
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
        {
            // 已加载：立即装钩
            install_hook();
        }
        else
        {
            // 未加载：后台轮询等待
            HANDLE th = CreateThread(nullptr, 0, poll_for_module, nullptr, 0, nullptr);
            if (th)
                CloseHandle(th);  // 线程自管生命周期，句柄用完即关
            else
                hook_log("DllMain: CreateThread 失败, error=%lu",
                         (unsigned long)GetLastError());
        }
    }
    return TRUE;
}
