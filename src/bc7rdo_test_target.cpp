// bc7rdo_test_target.cpp
// 自动化测试用的受控目标程序：
//   用于验证"远程注入 -> hook 生效"的完整链路。
//   注入器附加本进程后，bc7_rdo_hook.dll 会在 ~100ms 内装好；
//   RDO 开启后（lambda>0），后续轮次的压缩输出哈希应改变。
//   测试脚本据此断言：早期输出 = 基线，后期输出 ≠ 基线。
//
// 使用方式：与测试环境的 compress_bc7e.dll 副本同目录运行，
// 运行结束后在本目录生成 bc7rdo_test_target.log（每轮哈希一行）。
// 可选参数：bc7rdo_test_target.exe [轮数=12] [轮间隔毫秒=1000]
// （测试脚本用较短间隔加速，如 6 轮 × 400ms）

#include <windows.h>

#include <stdint.h>
#include <stdio.h>

#include <string>

#include "bc7elib.h"

namespace
{

// FNV-1a 64 位哈希
uint64_t fnv1a64(const uint8_t* data, size_t len)
{
    uint64_t hash = 14695981039346656037ull;  // 0xcbf29ce484222325
    for (size_t i = 0; i < len; i++)
    {
        hash ^= data[i];
        hash *= 1099511628211ull;  // 0x100000001b3
    }
    return hash;
}

std::wstring get_module_directory()
{
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, (DWORD)(_countof(path) - 1));
    if (!n)
        return L".";
    std::wstring full(path, n);
    const size_t slash = full.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : full.substr(0, slash);
}

} // namespace

typedef void (*pfn_init)(void);
typedef void (*pfn_params_init_basic)(bc7e_compress_block_params*, bool);
typedef void (*pfn_compress_blocks)(uint32_t, uint64_t*, const uint32_t*,
                                    const bc7e_compress_block_params*);

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    const std::wstring exe_dir = get_module_directory();
    const std::wstring dll_path = exe_dir + L"\\compress_bc7e.dll";

    // 1. 加载同目录的 DLL（测试环境会放一份原 DLL 副本在该目录）
    HMODULE h_mod = LoadLibraryW(dll_path.c_str());
    if (!h_mod)
    {
        fprintf(stderr, "[错误] LoadLibraryW 失败：%ls（GetLastError=%lu）\n",
                dll_path.c_str(), (unsigned long)GetLastError());
        return 1;
    }
    printf("已加载：%ls（0x%p）\n", dll_path.c_str(), (void*)h_mod);

    // 2. 取三个导出函数并调用 init 一次（不调用 debug 导出）
    pfn_init init = (pfn_init)GetProcAddress(h_mod, "bc7elib_compress_block_init");
    pfn_params_init_basic params_init_basic =
        (pfn_params_init_basic)GetProcAddress(h_mod, "bc7elib_compress_block_params_init_basic");
    pfn_compress_blocks compress_blocks =
        (pfn_compress_blocks)GetProcAddress(h_mod, "bc7elib_compress_blocks");

    if (!init || !params_init_basic || !compress_blocks)
    {
        fprintf(stderr, "[错误] compress_bc7e.dll 缺少所需导出函数（bc7elib_compress_block_init/"
                "bc7elib_compress_block_params_init_basic/bc7elib_compress_blocks）\n");
        return 1;
    }

    init();

    // 3. 构造 8 个 BC7 块 = 128 像素 RGBA（每像素 32 位，R 在低字节）
    //    确定性梯度：值 = A<<24 | B<<16 | G<<8 | R
    uint32_t px[128];
    for (int k = 0; k < 128; k++)
        px[k] = (0xFFu << 24) | (((k * 37) & 0xFF) << 16) |
                (((k * 13) & 0xFF) << 8) | ((k * 7) & 0xFF);

    // 4. 日志文件：启动时先截断清空
    const std::wstring log_path = exe_dir + L"\\bc7rdo_test_target.log";
    FILE* log = _wfopen(log_path.c_str(), L"w");
    if (log)
    {
        fclose(log);
        log = nullptr;
    }
    else
    {
        fprintf(stderr, "[警告] 无法截断日志文件：%ls\n", log_path.c_str());
    }

    // 5. 循环 N 轮：basic 预设 -> 压缩 8 块 -> 输出 128 字节算 FNV-1a 64 哈希
    //    可选参数：argv[1]=轮数（默认 12），argv[2]=轮间隔毫秒（默认 1000）
    int rounds = 12;
    DWORD interval_ms = 1000;
    if (argc >= 2)
        rounds = (int)wcstoul(argv[1], nullptr, 10);
    if (argc >= 3)
        interval_ms = (DWORD)wcstoul(argv[2], nullptr, 10);
    if (rounds < 1)
        rounds = 1;

    bc7e_compress_block_params params;

    for (int round = 1; round <= rounds; round++)
    {
        params_init_basic(&params, TRUE);

        uint64_t blocks[8 * 2] = {};  // 每块 16 字节
        compress_blocks(8, blocks, px, &params);

        const uint64_t hash = fnv1a64(reinterpret_cast<const uint8_t*>(blocks),
                                      sizeof(blocks));

        // 追加写入日志
        log = _wfopen(log_path.c_str(), L"a");
        if (log)
        {
            fprintf(log, "round %d: 0x%016llx\n", round, (unsigned long long)hash);
            fclose(log);
            log = nullptr;
        }
        else
        {
            fprintf(stderr, "[警告] 无法写入日志：%ls\n", log_path.c_str());
        }

        printf("round %d: 0x%016llx\n", round, (unsigned long long)hash);

        // 每轮后睡 interval_ms（最后一轮不睡），留时间让注入的 hook 生效
        if (round < rounds)
            Sleep(interval_ms);
    }

    printf("bc7rdo_test_target 完成，共 %d 轮。日志：%ls\n", rounds, log_path.c_str());
    return 0;
}
