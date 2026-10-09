// BC7 Hook 首次并发读取配置的回归测试；每次启动都是独立进程，重复运行
// 可覆盖只发生在首次访问时的缓存竞争。每个线程使用独立输出缓冲区。
// 使用 MSVC x64 编译（/O2 /EHsc /std:c++17 /MT /I src），然后在隔离目录
// 放入原 DLL 副本（命名 compress_bc7e.dll）、待测 bc7_rdo_hook.dll 和配置：
// encoder=oodle, oodle_lambda=0, unity_rgbm_context_hooks=0。
// 这样会读取共享配置，但不会调用 Oodle、ERT 或 Unity 私有函数。
// 重复启动 120 次，全部退出码应为 0；失败时输出异常码及模块内偏移。
#include <windows.h>
#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>
#include "bc7elib.h"

LONG WINAPI crash_filter(EXCEPTION_POINTERS* p) {
    HMODULE mod = nullptr;
    auto address = p->ExceptionRecord->ExceptionAddress;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &mod);
    wchar_t name[1024] = {};
    GetModuleFileNameW(mod, name, 1024);
    fprintf(stderr, "exception=0x%08lX module=%ls rva=0x%llX\n",
        p->ExceptionRecord->ExceptionCode, name,
        (unsigned long long)((uintptr_t)address - (uintptr_t)mod));
    fflush(stderr);
    ExitProcess(p->ExceptionRecord->ExceptionCode);
}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(crash_filter);
    auto original = LoadLibraryW(L"compress_bc7e.dll");
    if (!original) return 10;
    auto init = (void (*)())GetProcAddress(original, "bc7elib_compress_block_init");
    auto params_init = (void (*)(bc7e_compress_block_params*, bool))GetProcAddress(
        original, "bc7elib_compress_block_params_init_ultrafast");
    auto compress = (void (*)(uint32_t, uint64_t*, const uint32_t*,
        const bc7e_compress_block_params*))GetProcAddress(original, "bc7elib_compress_blocks");
    if (!init || !params_init || !compress) return 11;
    init();
    bc7e_compress_block_params params = {};
    params_init(&params, true);
    if (!LoadLibraryW(L"bc7_rdo_hook.dll")) return 12;
    Sleep(100);
    const int count = 64;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    for (int i = 0; i < count; ++i) {
        workers.emplace_back([&]() {
            uint32_t pixels[16];
            for (int j = 0; j < 16; ++j) pixels[j] = 0xff803020u + j;
            uint64_t blocks[2] = {};
            ready.fetch_add(1);
            while (!go.load()) SwitchToThread();
            for (int j = 0; j < 8; ++j) compress(1, blocks, pixels, &params);
        });
    }
    while (ready.load() != count) SwitchToThread();
    go.store(true);
    for (auto& worker : workers) worker.join();
    return 0;
}
