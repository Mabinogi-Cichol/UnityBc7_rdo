// bc7rdo_injector.cpp
// 注入器控制台程序（外挂注入版）：
//   1. 枚举本机 Unity Editor 安装（注册表 / Hub 布局 / 旧式目录）；
//   2. 启动 Unity Editor（或附加到已运行进程）；
//   3. 把与注入器同目录的 bc7_rdo_hook.dll 用 CreateRemoteThread + LoadLibraryW
//      注入目标进程，随后退出。Unity 安装目录零写入。
//
// 纯 Win32 + CRT 实现，x64 /MT 构建，无第三方库。
//
// 命令行规范：
//   bc7rdo_injector.exe -list                      枚举安装并打印，退出码 0
//   bc7rdo_injector.exe -u <Unity.exe 绝对路径> [--] [unity 参数...]
//                                                   启动该 Unity 并注入
//   bc7rdo_injector.exe -p <PID>                    附加到指定 PID 注入
//   bc7rdo_injector.exe                             自动检测最新 Unity Editor 并注入
//
// 退出码：
//   0 成功；1 用法错误；2 未找到 Unity 安装；
//   3 创建进程/打开进程失败；4 注入失败（含 hook DLL 缺失）；5 注入超时

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include <string>
#include <utility>
#include <vector>

namespace
{

const wchar_t k_registry_root[] = L"SOFTWARE\\Unity Technologies\\Installer";
const wchar_t k_hub_root[] = L"C:\\Program Files\\Unity\\Hub\\Editor";
const wchar_t k_legacy_exe[] = L"C:\\Program Files\\Unity\\Editor\\Unity.exe";
const wchar_t k_hook_dll_name[] = L"bc7_rdo_hook.dll";
const DWORD k_inject_timeout_ms = 30000;

// ---------------------------------------------------------------------------
// advapi32 注册表 API（运行时动态加载，避免链接期依赖 advapi32.lib）
// ---------------------------------------------------------------------------

typedef LONG(WINAPI* pfn_reg_open_key_ex_w)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
typedef LONG(WINAPI* pfn_reg_enum_key_ex_w)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD,
                                            LPWSTR, LPDWORD, PFILETIME);
typedef LONG(WINAPI* pfn_reg_query_value_ex_w)(HKEY, LPCWSTR, LPDWORD, LPDWORD,
                                               LPBYTE, LPDWORD);
typedef LONG(WINAPI* pfn_reg_close_key)(HKEY);

struct reg_api
{
    HMODULE module = nullptr;
    pfn_reg_open_key_ex_w open_key_ex = nullptr;
    pfn_reg_enum_key_ex_w enum_key_ex = nullptr;
    pfn_reg_query_value_ex_w query_value_ex = nullptr;
    pfn_reg_close_key close_key = nullptr;

    bool ok() const
    {
        return module && open_key_ex && enum_key_ex && query_value_ex && close_key;
    }
};

// advapi32.dll 系统必定存在，加载一次即可
const reg_api& get_reg_api()
{
    static reg_api api;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        api.module = LoadLibraryW(L"advapi32.dll");
        if (api.module)
        {
            api.open_key_ex = (pfn_reg_open_key_ex_w)GetProcAddress(api.module, "RegOpenKeyExW");
            api.enum_key_ex = (pfn_reg_enum_key_ex_w)GetProcAddress(api.module, "RegEnumKeyExW");
            api.query_value_ex =
                (pfn_reg_query_value_ex_w)GetProcAddress(api.module, "RegQueryValueExW");
            api.close_key = (pfn_reg_close_key)GetProcAddress(api.module, "RegCloseKey");
        }
    }
    return api;
}

// ---------------------------------------------------------------------------
// 版本解析与比较
// ---------------------------------------------------------------------------

struct unity_version
{
    int seg[8] = {};  // 主.次.patch 数字段，缺失段按 0 计
    int count = 0;
};

// 从字符串中定位版本号起点（第一个数字字符），找不到返回 nullptr
const wchar_t* find_version_start(const wchar_t* s)
{
    for (; *s; s++)
        if (*s >= L'0' && *s <= L'9')
            return s;
    return nullptr;
}

// 截取版本号文本，如 "Unity 2022.3.62f1" -> "2022.3.62f1"
std::wstring extract_version_text(const wchar_t* s)
{
    const wchar_t* p = find_version_start(s);
    if (!p)
        return L"";
    std::wstring out;
    for (; *p; p++)
    {
        const wchar_t c = *p;
        if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
            (c >= L'A' && c <= L'Z') || c == L'.')
            out += c;
        else
            break;
    }
    return out;
}

// 解析数字段："2022.3.62f1" -> {2022, 3, 62}，字母后缀忽略
unity_version parse_version(const wchar_t* s)
{
    unity_version v = {};
    int i = 0;
    while (s && *s && i < 8)
    {
        if (*s < L'0' || *s > L'9')
            break;
        wchar_t* end = nullptr;
        v.seg[i++] = (int)wcstoul(s, &end, 10);
        if (*end != L'.')
            break;
        s = end + 1;
    }
    v.count = i;
    return v;
}

// 版本比较：a<b 返回 -1，相等返回 0，a>b 返回 1
int compare_version(const unity_version& a, const unity_version& b)
{
    const int n = a.count > b.count ? a.count : b.count;
    for (int i = 0; i < n; i++)
    {
        const int av = i < a.count ? a.seg[i] : 0;
        const int bv = i < b.count ? b.seg[i] : 0;
        if (av != bv)
            return av < bv ? -1 : 1;
    }
    return 0;
}

// 是否为 2022.3.x 系列（自动检测时优先选择）
bool is_2022_3(const unity_version& v)
{
    return v.count >= 2 && v.seg[0] == 2022 && v.seg[1] == 3;
}

// ---------------------------------------------------------------------------
// Unity Editor 安装枚举
// ---------------------------------------------------------------------------

struct editor_install
{
    std::wstring version_text;  // 版本文本（如 "2022.3.62f1"），可能为空
    std::wstring editor_dir;    // 包含 Unity.exe 的 Editor 目录
    std::wstring exe_path;      // Unity.exe 完整路径
    unity_version ver;
};

// 追加候选（exe 必须存在；按 exe 路径去重）
void add_candidate(std::vector<editor_install>& list, const std::wstring& version_text,
                   const std::wstring& editor_dir, const std::wstring& exe)
{
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    for (const auto& c : list)
        if (_wcsicmp(c.exe_path.c_str(), exe.c_str()) == 0)
            return;
    editor_install e;
    e.version_text = version_text;
    e.editor_dir = editor_dir;
    e.exe_path = exe;
    e.ver = parse_version(version_text.c_str());
    list.push_back(std::move(e));
}

// 读取注册表 REG_SZ/REG_EXPAND_SZ 值；REG_EXPAND_SZ 做环境变量展开
bool query_reg_string(HKEY h_sub, const wchar_t* value_name, std::wstring& out)
{
    const reg_api& api = get_reg_api();
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD bytes = sizeof(buf);
    DWORD type = 0;
    const LONG qr = api.query_value_ex(h_sub, value_name, nullptr, &type,
                                       (LPBYTE)buf, &bytes);
    if (qr != ERROR_SUCCESS || buf[0] == L'\0')
        return false;

    out.assign(buf);
    if (type == REG_EXPAND_SZ)
    {
        wchar_t expanded[MAX_PATH * 2] = {};
        const DWORD n = ExpandEnvironmentStringsW(out.c_str(), expanded, _countof(expanded));
        if (n && n < _countof(expanded))
            out.assign(expanded);
    }
    return true;
}

// 注册表：HKLM\SOFTWARE\Unity Technologies\Installer 下枚举子键
void enum_registry_installs(std::vector<editor_install>& out)
{
    const reg_api& api = get_reg_api();
    if (!api.ok())
        return;

    HKEY h_root = nullptr;
    if (api.open_key_ex(HKEY_LOCAL_MACHINE, k_registry_root, 0,
                        KEY_READ | KEY_WOW64_64KEY, &h_root) != ERROR_SUCCESS &&
        api.open_key_ex(HKEY_LOCAL_MACHINE, k_registry_root, 0,
                        KEY_READ, &h_root) != ERROR_SUCCESS)
        return;

    wchar_t sub_name[256] = {};
    DWORD index = 0;
    for (;;)
    {
        DWORD name_len = (DWORD)_countof(sub_name);
        const LONG r = api.enum_key_ex(h_root, index++, sub_name, &name_len,
                                       nullptr, nullptr, nullptr, nullptr);
        if (r != ERROR_SUCCESS)
            break;

        // 子键名形如 "Unity 2022.3.62f1"
        std::wstring key_name(sub_name, name_len);

        HKEY h_sub = nullptr;
        if (api.open_key_ex(h_root, key_name.c_str(), 0, KEY_QUERY_VALUE, &h_sub) != ERROR_SUCCESS)
            continue;

        // 优先读 "Location Editor"，旧版安装器可能只有 "Location x64"
        std::wstring path;
        if (!query_reg_string(h_sub, L"Location Editor", path) &&
            !query_reg_string(h_sub, L"Location x64", path))
        {
            api.close_key(h_sub);
            continue;
        }
        api.close_key(h_sub);

        // 该值可能指向 Editor 目录本身，也可能指向外层目录，据此修正
        while (!path.empty() && (path.back() == L' ' || path.back() == L'\t'))
            path.pop_back();

        std::wstring editor_dir, exe;
        if (GetFileAttributesW((path + L"\\Unity.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            editor_dir = path;
            exe = path + L"\\Unity.exe";
        }
        else if (GetFileAttributesW((path + L"\\Editor\\Unity.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            editor_dir = path + L"\\Editor";
            exe = path + L"\\Editor\\Unity.exe";
        }
        else
            continue;

        // 版本号从键名解析（如 "Unity 2022.3.62f1"）
        std::wstring version_text = extract_version_text(key_name.c_str());
        add_candidate(out, version_text, editor_dir, exe);
    }
    api.close_key(h_root);
}

// Hub 布局：C:\Program Files\Unity\Hub\Editor\<版本>\Editor\Unity.exe
void enum_hub_installs(std::vector<editor_install>& out)
{
    WIN32_FIND_DATAW fd = {};
    HANDLE h_find = FindFirstFileW((std::wstring(k_hub_root) + L"\\*").c_str(), &fd);
    if (h_find == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == L'.')
            continue;

        std::wstring dir_name = fd.cFileName;
        std::wstring editor_dir = std::wstring(k_hub_root) + L"\\" + dir_name + L"\\Editor";
        std::wstring exe = editor_dir + L"\\Unity.exe";
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;

        // 目录名即版本号
        std::wstring version_text = extract_version_text(dir_name.c_str());
        add_candidate(out, version_text, editor_dir, exe);
    } while (FindNextFileW(h_find, &fd));

    FindClose(h_find);
}

// 旧式：C:\Program Files\Unity\Editor\Unity.exe（路径不含版本，注册表已收录则去重）
void enum_legacy_installs(std::vector<editor_install>& out)
{
    if (GetFileAttributesW(k_legacy_exe) == INVALID_FILE_ATTRIBUTES)
        return;
    add_candidate(out, L"", L"C:\\Program Files\\Unity\\Editor", k_legacy_exe);
}

// 按版本号从高到低排序（未知版本排最后）
void sort_installs(std::vector<editor_install>& list)
{
    std::sort(list.begin(), list.end(),
              [](const editor_install& a, const editor_install& b)
              {
                  return compare_version(a.ver, b.ver) > 0;
              });
}

void print_installs(const std::vector<editor_install>& list)
{
    for (const auto& c : list)
    {
        if (c.version_text.empty())
            printf("  %-15ls  %ls\n", L"(未知版本)", c.exe_path.c_str());
        else
            printf("  %-15ls  %ls\n", c.version_text.c_str(), c.exe_path.c_str());
    }
}

// 选出目标版本：优先 2022.3.x，其次取最高版本号
const editor_install* select_best(const std::vector<editor_install>& list)
{
    const editor_install* best = nullptr;
    for (const auto& c : list)
    {
        if (!best)
        {
            best = &c;
            continue;
        }
        const bool b22 = is_2022_3(best->ver);
        const bool c22 = is_2022_3(c.ver);
        if (c22 && !b22)
            best = &c;
        else if (b22 == c22 && compare_version(c.ver, best->ver) > 0)
            best = &c;
    }
    return best;
}

// ---------------------------------------------------------------------------
// 注入实现
// ---------------------------------------------------------------------------

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

// 把 hook DLL 注入目标进程；成功返回 true，失败设置 *exit_code 并返回 false
bool inject_into_process(HANDLE h_process, const std::wstring& hook_dll, int& exit_code)
{
    // 1. 在目标进程分配内存存放 DLL 路径
    const SIZE_T bytes = (hook_dll.size() + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(h_process, nullptr, bytes,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
    {
        fprintf(stderr, "[错误] VirtualAllocEx 失败，GetLastError=%lu（退出码 4）\n",
                (unsigned long)GetLastError());
        exit_code = 4;
        return false;
    }

    // 2. 写入宽字符串
    if (!WriteProcessMemory(h_process, remote, hook_dll.c_str(), bytes, nullptr))
    {
        fprintf(stderr, "[错误] WriteProcessMemory 失败，GetLastError=%lu（退出码 4）\n",
                (unsigned long)GetLastError());
        VirtualFreeEx(h_process, remote, 0, MEM_RELEASE);
        exit_code = 4;
        return false;
    }

    // 3. 创建远程线程调用 LoadLibraryW
    DWORD tid = 0;
    HANDLE h_thread = CreateRemoteThread(h_process, nullptr, 0,
                                         (LPTHREAD_START_ROUTINE)&LoadLibraryW,
                                         remote, 0, &tid);
    if (!h_thread)
    {
        fprintf(stderr, "[错误] CreateRemoteThread 失败，GetLastError=%lu（退出码 4）\n",
                (unsigned long)GetLastError());
        VirtualFreeEx(h_process, remote, 0, MEM_RELEASE);
        exit_code = 4;
        return false;
    }

    // 4. 等待 LoadLibraryW 返回
    const DWORD wait = WaitForSingleObject(h_thread, k_inject_timeout_ms);
    if (wait == WAIT_TIMEOUT)
    {
        fprintf(stderr, "[错误] 注入超时（%lu 毫秒）（退出码 5）\n",
                (unsigned long)k_inject_timeout_ms);
        CloseHandle(h_thread);
        VirtualFreeEx(h_process, remote, 0, MEM_RELEASE);
        exit_code = 5;
        return false;
    }
    if (wait != WAIT_OBJECT_0)
    {
        fprintf(stderr, "[错误] WaitForSingleObject 失败，GetLastError=%lu（退出码 5）\n",
                (unsigned long)GetLastError());
        CloseHandle(h_thread);
        VirtualFreeEx(h_process, remote, 0, MEM_RELEASE);
        exit_code = 5;
        return false;
    }

    // 5. 取远程线程返回值（LoadLibraryW 返回的 hModule）
    DWORD code = 0;
    GetExitCodeThread(h_thread, &code);
    if (code == 0 || code == STILL_ACTIVE)
    {
        fprintf(stderr,
                "[错误] LoadLibraryW 返回 0（0x%p），hook 加载失败："
                "目标进程可能不是 x64，或权限不足（退出码 4）\n",
                (void*)(ULONG_PTR)code);
        CloseHandle(h_thread);
        VirtualFreeEx(h_process, remote, 0, MEM_RELEASE);
        exit_code = 4;
        return false;
    }

    // 6. 成功：hook 必须常驻，绝不调用 FreeLibrary
    printf("injected: hook module = 0x%p\n", (void*)(ULONG_PTR)code);

    CloseHandle(h_thread);
    VirtualFreeEx(h_process, remote, 0, MEM_RELEASE);
    exit_code = 0;
    return true;
}

// 追加一个命令行参数（含空格/引号时重新加引号，避免被拆成多个 token）
void append_argument(std::wstring& cmd, const std::wstring& arg)
{
    if (arg.find_first_of(L" \t\"") == std::wstring::npos)
    {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (const wchar_t c : arg)
    {
        if (c == L'"')
            cmd += L"\\\"";
        else
            cmd += c;
    }
    cmd += L'"';
}

// 启动模式：创建进程（不挂起，避免主线程持 loader lock 导致注入死锁）后注入
int launch_and_inject(const std::wstring& unity_exe,
                      const std::vector<std::wstring>& args,
                      const std::wstring& hook_dll)
{
    if (GetFileAttributesW(unity_exe.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        fprintf(stderr, "[错误] Unity.exe 不存在：%ls（退出码 3）\n", unity_exe.c_str());
        return 3;
    }

    // 构造宽字符命令行："<UnityExe>" [args...]
    std::wstring cmdline = L"\"" + unity_exe + L"\"";
    for (const auto& a : args)
        append_argument(cmdline, a);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi))
    {
        fprintf(stderr, "[错误] CreateProcessW 失败，GetLastError=%lu（%ls）（退出码 3）\n",
                (unsigned long)GetLastError(), unity_exe.c_str());
        return 3;
    }

    // 主线程句柄无需保留
    CloseHandle(pi.hThread);

    printf("进程已启动：PID=%lu，开始注入 %ls\n",
           (unsigned long)pi.dwProcessId, hook_dll.c_str());

    int code = 0;
    inject_into_process(pi.hProcess, hook_dll, code);

    CloseHandle(pi.hProcess);
    return code;
}

// 附加模式：打开目标进程后注入，不启动
int attach_and_inject(DWORD pid, const std::wstring& hook_dll)
{
    HANDLE h_process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                                       PROCESS_VM_WRITE | PROCESS_VM_READ |
                                       PROCESS_QUERY_INFORMATION,
                                   FALSE, pid);
    if (!h_process)
    {
        fprintf(stderr, "[错误] OpenProcess(PID=%lu) 失败，GetLastError=%lu（退出码 3）\n",
                (unsigned long)pid, (unsigned long)GetLastError());
        return 3;
    }

    printf("已附加：PID=%lu，开始注入 %ls\n", (unsigned long)pid, hook_dll.c_str());

    int code = 0;
    inject_into_process(h_process, hook_dll, code);

    CloseHandle(h_process);
    return code;
}

void print_usage()
{
    printf("用法：\n");
    printf("  bc7rdo_injector.exe -list                        枚举 Unity Editor 安装并打印\n");
    printf("  bc7rdo_injector.exe -u <Unity.exe 绝对路径> [--] [unity 参数...]\n");
    printf("                                                     启动该 Unity 并注入\n");
    printf("  bc7rdo_injector.exe -p <PID>                      附加到指定 PID 注入\n");
    printf("  bc7rdo_injector.exe                               自动检测最新 Unity Editor 并注入\n");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    // ---- 参数解析 ----
    std::wstring unity_exe;
    std::vector<std::wstring> unity_args;
    DWORD attach_pid = 0;
    bool want_list = false;

    if (argc >= 2)
    {
        const std::wstring first = argv[1];
        if (first == L"-list")
        {
            want_list = true;
            if (argc > 2)
            {
                fprintf(stderr, "[错误] -list 不接受多余参数（退出码 1）\n");
                print_usage();
                return 1;
            }
        }
        else if (first == L"-u")
        {
            if (argc < 3)
            {
                fprintf(stderr, "[错误] -u 缺少 Unity.exe 绝对路径参数（退出码 1）\n");
                print_usage();
                return 1;
            }
            unity_exe = argv[2];
            int i = 3;
            // 可选的 "--" 分隔符，其后参数原样传给 Unity
            if (i < argc && wcscmp(argv[i], L"--") == 0)
                i++;
            for (; i < argc; i++)
                unity_args.push_back(argv[i]);
        }
        else if (first == L"-p")
        {
            if (argc < 3)
            {
                fprintf(stderr, "[错误] -p 缺少 PID 参数（退出码 1）\n");
                print_usage();
                return 1;
            }
            wchar_t* end = nullptr;
            attach_pid = (DWORD)wcstoul(argv[2], &end, 10);
            if (end == argv[2] || *end != L'\0' || attach_pid == 0)
            {
                fprintf(stderr, "[错误] 无效的 PID：%ls（退出码 1）\n", argv[2]);
                print_usage();
                return 1;
            }
            if (argc > 3)
            {
                fprintf(stderr, "[错误] -p 不接受多余参数（退出码 1）\n");
                print_usage();
                return 1;
            }
        }
        else
        {
            fprintf(stderr, "[错误] 未知参数：%ls（退出码 1）\n", first.c_str());
            print_usage();
            return 1;
        }
    }

    // hook DLL 路径 = 注入器所在目录\bc7_rdo_hook.dll
    const std::wstring hook_dll = get_module_directory() + L"\\" + k_hook_dll_name;

    // ---- -list：仅枚举并打印 ----
    if (want_list)
    {
        std::vector<editor_install> installs;
        enum_registry_installs(installs);
        enum_hub_installs(installs);
        enum_legacy_installs(installs);
        sort_installs(installs);

        if (installs.empty())
            printf("未检测到 Unity Editor 安装。\n");
        else
        {
            printf("已检测到的 Unity Editor 安装：\n");
            print_installs(installs);
        }
        return 0;
    }

    // hook DLL 必须存在
    if (GetFileAttributesW(hook_dll.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        fprintf(stderr, "[错误] 找不到 hook DLL：%ls（退出码 4）\n", hook_dll.c_str());
        return 4;
    }

    // ---- -u：启动该 Unity 并注入 ----
    if (!unity_exe.empty())
    {
        printf("启动 Unity：%ls\n", unity_exe.c_str());
        return launch_and_inject(unity_exe, unity_args, hook_dll);
    }

    // ---- -p：附加到指定 PID 注入 ----
    if (attach_pid != 0)
        return attach_and_inject(attach_pid, hook_dll);

    // ---- 自动检测最新 Unity Editor ----
    std::vector<editor_install> installs;
    enum_registry_installs(installs);
    enum_hub_installs(installs);
    enum_legacy_installs(installs);
    sort_installs(installs);

    if (installs.empty())
    {
        fprintf(stderr, "[错误] 未检测到 Unity Editor 安装（退出码 2）\n");
        return 2;
    }

    printf("已检测到的 Unity Editor 安装：\n");
    print_installs(installs);

    const editor_install* sel = select_best(installs);
    printf("自动选择：%ls  %ls\n",
           sel->version_text.empty() ? L"(未知版本)" : sel->version_text.c_str(),
           sel->exe_path.c_str());

    return launch_and_inject(sel->exe_path, std::vector<std::wstring>(), hook_dll);
}
