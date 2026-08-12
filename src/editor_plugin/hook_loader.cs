// hook_loader.cs
// Unity Editor Plugin：把外挂注入的 BC7 RDO hook 封装成"放进工程即生效"。
//
// 用法：
//   把 out\editor-plugin\Bc7Rdo\ 整个目录复制到任意工程的 Assets 下任意位置
//   （如 Assets\Editor\Bc7Rdo\），目录内包含：
//     - hook_loader.cs      本脚本（自动加载 hook DLL）
//     - bc7_rdo_hook.dll    原生 hook DLL（inline detour + ERT RDO 后处理）
//     - bc7_rdo_config.txt  RDO 配置（同目录热更新）
//   重新打开 / 刷新工程即自动生效，无需注入器、无需管理员权限。
//
// 原理：
//   C# 代码跑在 Unity Editor 进程内（[InitializeOnLoad] 静态构造在编辑器域
//   加载时执行），只需 P/Invoke LoadLibraryW 加载 bc7_rdo_hook.dll。
//   hook DLL 的 DllMain 会自行轮询等待 compress_bc7e.dll（Unity 首次导入
//   BC7 纹理时才加载），出现后立刻对 bc7elib_compress_blocks 导出桩做
//   inline detour，并对输出做 RDO 后处理——不需要 CreateRemoteThread/注入器。
//
// 契约：
//   - Install()  加载 hook DLL（幂等）；失败返回 false 并打日志
//   - SmokeTest() 端到端冒烟：加载hook -> 加载compress_bc7e.dll -> 装钩
//     -> 转发 -> RDO 生效（批处理环境可经 -executeMethod 调用）
//   - 菜单：Tools/BC7 RDO/Reload Hook DLL、Tools/BC7 RDO/Smoke Test

using System;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Threading;

using UnityEditor;
using UnityEngine;

namespace Bc7Rdo
{
    [InitializeOnLoad]
    public static class HookLoader
    {
        // -------------------------------------------------------------------
        // kernel32 P/Invoke（只用于显式加载模块与取导出地址）
        // -------------------------------------------------------------------

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryW(string lpFileName);

        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr hModule, string lpProcName);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool FreeLibrary(IntPtr hModule);

        // -------------------------------------------------------------------
        // bc7e_compress_block_params 结构体（MSVC x64 共 124 字节）
        // 偏移与 src/bc7elib.h 完全一致；C++ bool 用 byte 表示。
        // -------------------------------------------------------------------

        [StructLayout(LayoutKind.Sequential)]
        private struct Bc7eOpaque
        {
            public uint m_max_mode13_partitions_to_try;      // +0x4C
            public uint m_max_mode0_partitions_to_try;       // +0x50
            public uint m_max_mode2_partitions_to_try;       // +0x54
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 7)]
            public byte[] m_use_mode;                        // +0x58
            public byte m_unused1;                           // +0x5F
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct Bc7eAlpha
        {
            public uint m_max_mode7_partitions_to_try;       // +0x60
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)]
            public uint[] m_mode67_error_weight_mul;         // +0x64
            public byte m_use_mode4;                         // +0x74
            public byte m_use_mode5;                         // +0x75
            public byte m_use_mode6;                         // +0x76
            public byte m_use_mode7;                         // +0x77
            public byte m_use_mode4_rotation;                // +0x78
            public byte m_use_mode5_rotation;                // +0x79
            public byte m_unused2;                           // +0x7A
            public byte m_unused3;                           // +0x7B
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct Bc7eParams
        {
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)]
            public uint[] m_max_partitions_mode;             // +0x00
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)]
            public uint[] m_weights;                         // +0x20
            public uint m_uber_level;                        // +0x30
            public uint m_refinement_passes;                 // +0x34
            public uint m_mode4_rotation_mask;               // +0x38
            public uint m_mode4_index_mask;                  // +0x3C
            public uint m_mode5_rotation_mask;               // +0x40
            public uint m_uber1_mask;                        // +0x44
            public byte m_perceptual;                        // +0x48
            public byte m_pbit_search;                       // +0x49
            public byte m_mode6_only;                        // +0x4A
            public byte m_unused0;                           // +0x4B
            public Bc7eOpaque m_opaque_settings;             // +0x4C
            public Bc7eAlpha m_alpha_settings;               // +0x60
        }

        // -------------------------------------------------------------------
        // compress_bc7e.dll 导出委托（Cdecl；x64 下单一调用约定）
        // -------------------------------------------------------------------

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void ParamsInitBasicDelegate(ref Bc7eParams p, byte perceptual);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void CompressBlocksDelegate(uint numBlocks, IntPtr pBlocks,
                                                     IntPtr pPixelsRGBA, ref Bc7eParams p);

        // -------------------------------------------------------------------
        // 状态
        // -------------------------------------------------------------------

        private static IntPtr _hookModule = IntPtr.Zero;   // hook DLL 句柄（永不释放）
        private static string _pluginDir = "";             // 插件目录（DLL 所在目录）

        // 静态构造：编辑器域加载时自动执行（无需用户操作）
        static HookLoader()
        {
            Install();
        }

        // 供菜单/批处理调用：加载 hook DLL；成功返回 true
        public static bool Install()
        {
            try
            {
                if (_hookModule != IntPtr.Zero)
                    return true;  // 已加载，幂等

                // 在工程 Assets 下任意位置查找 hook DLL（取第一个）
                string[] hits = Directory.GetFiles(Application.dataPath,
                                                   "bc7_rdo_hook.dll",
                                                   SearchOption.AllDirectories);
                if (hits.Length == 0)
                {
                    Debug.LogWarning("[Bc7Rdo] 未找到 bc7_rdo_hook.dll，请确认插件目录" +
                                     "（Assets 下任意位置，与 bc7_rdo_config.txt 同目录）");
                    return false;
                }

                string dllPath = hits[0];
                _pluginDir = Path.GetDirectoryName(dllPath);

                _hookModule = LoadLibraryW(dllPath);
                if (_hookModule == IntPtr.Zero)
                {
                    Debug.LogError("[Bc7Rdo] LoadLibraryW 失败: " + dllPath +
                                   " error=" + Marshal.GetLastWin32Error());
                    _hookModule = IntPtr.Zero;
                    return false;
                }

                Debug.Log("[Bc7Rdo] hook DLL 已加载: " + dllPath +
                          "（配置/日志同目录: " + _pluginDir + "）");

                // 监控并行导入 worker 进程并注入（批量 reimport 的压缩走 worker）
                ImportWorkerInjector.Start();
                return true;
            }
            catch (Exception ex)
            {
                Debug.LogError("[Bc7Rdo] Install 异常: " + ex);
                return false;
            }
        }

        // -------------------------------------------------------------------
        // 菜单
        // -------------------------------------------------------------------

        [MenuItem("Tools/BC7 RDO/Reload Hook DLL")]
        private static void ReloadHookDllMenu() { Install(); }

        [MenuItem("Tools/BC7 RDO/Smoke Test")]
        private static void SmokeTestMenu() { SmokeTest(); }

        [MenuItem("Tools/BC7 RDO/Inject Import Workers")]
        private static void InjectWorkersMenu() { ImportWorkerInjector.ScanAndInject(); }

        // -------------------------------------------------------------------
        // 冒烟测试
        // -------------------------------------------------------------------

        // 批处理冒烟测试入口（-executeMethod Bc7Rdo.HookLoader.SmokeTest）：
        // 完整验证 加载hook DLL -> 加载compress_bc7e.dll -> 装钩 -> 转发 -> RDO 生效
        public static void SmokeTest()
        {
            try
            {
                // 结构体布局自检
                if (Marshal.SizeOf(typeof(Bc7eParams)) != 124)
                {
                    SmokeFail("Bc7eParams 大小 = " + Marshal.SizeOf(typeof(Bc7eParams)) +
                              "（期望 124，结构体布局与 C++ 不符）");
                    return;
                }

                // 1. 加载 hook DLL
                if (!Install())
                {
                    SmokeFail("Install() 失败，hook DLL 未加载");
                    return;
                }

                // 2. 从 Unity Editor 目录加载 compress_bc7e.dll
                string editorDir = Path.GetDirectoryName(EditorApplication.applicationPath);
                if (string.IsNullOrEmpty(editorDir))
                {
                    SmokeFail("无法解析 Unity Editor 目录");
                    return;
                }
                string bc7ePath = Path.Combine(editorDir, "compress_bc7e.dll");
                IntPtr hBc7e = LoadLibraryW(bc7ePath);
                if (hBc7e == IntPtr.Zero)
                {
                    SmokeFail("LoadLibraryW 失败: " + bc7ePath +
                              " error=" + Marshal.GetLastWin32Error());
                    return;
                }

                try
                {
                    // 3. 取导出函数并初始化参数（basic 预设，perceptual=1）
                    IntPtr pInit = GetProcAddress(hBc7e,
                                                  "bc7elib_compress_block_params_init_basic");
                    IntPtr pCompress = GetProcAddress(hBc7e, "bc7elib_compress_blocks");
                    if (pInit == IntPtr.Zero || pCompress == IntPtr.Zero)
                    {
                        SmokeFail("GetProcAddress 找不到导出函数" +
                                  " init_basic=" + (pInit != IntPtr.Zero) +
                                  " compress=" + (pCompress != IntPtr.Zero));
                        return;
                    }

                    ParamsInitBasicDelegate initBasic =
                        (ParamsInitBasicDelegate)Marshal.GetDelegateForFunctionPointer(
                            pInit, typeof(ParamsInitBasicDelegate));
                    CompressBlocksDelegate compress =
                        (CompressBlocksDelegate)Marshal.GetDelegateForFunctionPointer(
                            pCompress, typeof(CompressBlocksDelegate));

                    Bc7eParams p = NewParams();
                    initBasic(ref p, 1);

                    // 4. 轮询等待装钩（hook DLL 检测到 compress_bc7e.dll 后
                    //    在约 50ms 内完成 inline detour 并写日志）
                    if (!WaitHookInstalled(3000))
                    {
                        SmokeFail("装钩超时（3 秒），hook 日志内容:\n" + ReadHookLog());
                        return;
                    }

                    // 5. 功能验证：8 个 BC7 块 = 128 像素 RGBA 确定性梯度
                    //    与 C++ 测试目标公式一致（R 在低字节，小端 4 字节）
                    byte[] px = new byte[128 * 4];
                    for (int k = 0; k < 128; k++)
                    {
                        px[k * 4 + 0] = (byte)((k * 7) & 0xFF);    // R
                        px[k * 4 + 1] = (byte)((k * 13) & 0xFF);   // G
                        px[k * 4 + 2] = (byte)((k * 37) & 0xFF);   // B
                        px[k * 4 + 3] = 0xFF;                      // A
                    }
                    byte[] blocks = new byte[8 * 16];   // 8 块 x 16 字节

                    GCHandle pxPin = GCHandle.Alloc(px, GCHandleType.Pinned);
                    GCHandle blkPin = GCHandle.Alloc(blocks, GCHandleType.Pinned);
                    try
                    {
                        IntPtr pxPtr = pxPin.AddrOfPinnedObject();
                        IntPtr blkPtr = blkPin.AddrOfPinnedObject();

                        // lambda=0：转发模式基线
                        WriteConfig(0f);
                        compress(8, blkPtr, pxPtr, ref p);
                        byte[] baseline = (byte[])blocks.Clone();

                        // lambda=4：RDO 后处理
                        WriteConfig(4f);
                        compress(8, blkPtr, pxPtr, ref p);
                        byte[] rdoBytes = (byte[])blocks.Clone();

                        // 6. 断言：RDO 改变了输出，且对 LZ 更友好
                        bool differ = !ByteEquals(baseline, rdoBytes);
                        byte[] zb = Deflate(baseline);
                        byte[] zr = Deflate(rdoBytes);
                        bool smaller = zr.Length <= zb.Length;

                        if (!differ)
                        {
                            SmokeFail("baseline 与 RDO 输出相同，RDO 未生效");
                            return;
                        }
                        if (!smaller)
                        {
                            SmokeFail("RDO 压缩后反而更大: zlibBase=" + zb.Length +
                                      " zlibRdo=" + zr.Length);
                            return;
                        }

                        Debug.Log("[Bc7Rdo][SMOKE] PASS baseline=" + baseline.Length +
                                  " rdo=" + rdoBytes.Length +
                                  " zlibBase=" + zb.Length + " zlibRdo=" + zr.Length);
                        Exit(0);
                    }
                    finally
                    {
                        blkPin.Free();
                        pxPin.Free();
                    }
                }
                finally
                {
                    // 冒烟测试的 compress_bc7e.dll 用完释放；hook DLL 永不释放
                    FreeLibrary(hBc7e);
                }
            }
            catch (Exception ex)
            {
                Debug.LogException(ex);
                Exit(1);
            }
        }

        // -------------------------------------------------------------------
        // 工具
        // -------------------------------------------------------------------

        // 新建并初始化 Bc7eParams（ByValArray 字段必须先分配正确大小）
        private static Bc7eParams NewParams()
        {
            Bc7eParams p = new Bc7eParams();
            p.m_max_partitions_mode = new uint[8];
            p.m_weights = new uint[4];
            p.m_opaque_settings.m_use_mode = new byte[7];
            p.m_alpha_settings.m_mode67_error_weight_mul = new uint[4];
            return p;
        }

        // 写 RDO 配置到插件目录（hook DLL 每次调用都会热重读该文件）
        private static void WriteConfig(float lambda)
        {
            string cfgPath = Path.Combine(_pluginDir, "bc7_rdo_config.txt");
            File.WriteAllText(cfgPath,
                "lambda=" + lambda.ToString(System.Globalization.CultureInfo.InvariantCulture) +
                "\nlookback=128\nsmooth_scale=15\nmax_rms_increase_ratio=10\n" +
                "two_matches=1\ndebug=0\n");
        }

        // 轮询等待装钩：hook 日志出现 "install_hook: OK" 即返回 true
        private static bool WaitHookInstalled(int timeoutMs)
        {
            string logPath = Path.Combine(_pluginDir, "bc7_rdo_hook.log");
            int waited = 0;
            while (waited < timeoutMs)
            {
                try
                {
                    if (File.Exists(logPath))
                    {
                        string content = File.ReadAllText(logPath);
                        if (content.Contains("install_hook: OK"))
                            return true;
                    }
                }
                catch { /* 文件可能正被写入，重试即可 */ }
                Thread.Sleep(100);
                waited += 100;
            }
            return false;
        }

        private static string ReadHookLog()
        {
            try
            {
                string logPath = Path.Combine(_pluginDir, "bc7_rdo_hook.log");
                if (File.Exists(logPath))
                    return File.ReadAllText(logPath);
            }
            catch { }
            return "(无 hook 日志)";
        }

        private static byte[] Deflate(byte[] data)
        {
            using (MemoryStream ms = new MemoryStream())
            {
                // leaveOpen: true —— Mono 的 DeflateStream 二参构造默认在
                // Dispose 时关闭底层流（leaveOpen=false），显式保持打开，
                // 避免关闭后访问 ms 抛 ObjectDisposedException
                using (DeflateStream ds = new DeflateStream(
                           ms, System.IO.Compression.CompressionLevel.Optimal,
                           leaveOpen: true))
                {
                    ds.Write(data, 0, data.Length);
                }
                return ms.ToArray();
            }
        }

        private static bool ByteEquals(byte[] a, byte[] b)
        {
            if (a.Length != b.Length)
                return false;
            for (int i = 0; i < a.Length; i++)
                if (a[i] != b[i])
                    return false;
            return true;
        }

        // 失败：打错误日志；批处理环境退出码 1
        private static void SmokeFail(string reason)
        {
            Debug.LogError("[Bc7Rdo][SMOKE] FAIL " + reason);
            Exit(1);
        }

        // 只在批处理环境退出（菜单调用时仅打印结果）
        private static void Exit(int code)
        {
            if (Application.isBatchMode)
                EditorApplication.Exit(code);
        }

        // -------------------------------------------------------------------
        // 并行导入 worker 注入
        // Unity 2022 的批量 reimport 由 AssetImportWorker 子进程执行
        // （进程名 Unity.exe，命令行含 -name AssetImportWorkerN，由主编辑器
        // 进程 fork）。worker 不运行编辑器脚本，必须由主进程主动注入 hook
        // DLL，否则 worker 内的 BC7 压缩走原版，AB 包体积与无 RDO 相同。
        // -------------------------------------------------------------------

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr CreateToolhelp32Snapshot(uint dwFlags, uint th32ProcessID);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool Process32FirstW(IntPtr hSnapshot, ref ProcessEntry32W lppe);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool Process32NextW(IntPtr hSnapshot, ref ProcessEntry32W lppe);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr GetModuleHandle(string lpModuleName);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr OpenProcess(uint dwDesiredAccess,
            [MarshalAs(UnmanagedType.Bool)] bool bInheritHandle, uint dwProcessId);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress,
            UIntPtr dwSize, uint flAllocationType, uint flProtect);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool VirtualFreeEx(IntPtr hProcess, IntPtr lpAddress,
            UIntPtr dwSize, uint dwFreeType);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress,
            byte[] lpBuffer, int nSize, out IntPtr lpNumberOfBytesWritten);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes,
            UIntPtr dwStackSize, IntPtr lpStartAddress, IntPtr lpParameter,
            uint dwCreationFlags, out uint lpThreadId);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetExitCodeThread(IntPtr hThread, out uint lpExitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr hObject);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct ProcessEntry32W
        {
            public uint dwSize;
            public uint cntUsage;
            public uint th32ProcessID;
            public IntPtr th32DefaultHeapID;
            public uint th32ModuleID;
            public uint cntThreads;
            public uint th32ParentProcessID;
            public int pcPriClassBase;
            public uint dwFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
            public string szExeFile;
        }

        private sealed class ImportWorkerInjector
        {
            private const uint TH32CS_SNAPPROCESS = 0x00000002;
            private const uint MEM_COMMIT = 0x1000;
            private const uint MEM_RESERVE = 0x2000;
            private const uint MEM_RELEASE = 0x8000;
            private const uint PAGE_READWRITE = 0x04;
            private const uint PROCESS_CREATE_THREAD = 0x0002;
            private const uint PROCESS_VM_OPERATION = 0x0008;
            private const uint PROCESS_VM_READ = 0x0010;
            private const uint PROCESS_VM_WRITE = 0x0020;
            private const uint PROCESS_QUERY_INFORMATION = 0x0400;
            private const uint STILL_ACTIVE = 259;

            private static readonly System.Collections.Generic.HashSet<uint> s_injected =
                new System.Collections.Generic.HashSet<uint>();
            private static readonly object s_lock = new object();
            private static int s_monitorStarted;

            // 后台轮询：每 500ms 扫描一次子进程里的 worker 并注入（幂等）。
            // worker 可能频繁重建（每批任务后重启），间隔取小值减少漏网。
            public static void Start()
            {
                if (Interlocked.Exchange(ref s_monitorStarted, 1) != 0)
                    return;
                Thread t = new Thread(MonitorLoop);
                t.IsBackground = true;
                t.Start();
            }

            private static void MonitorLoop()
            {
                for (;;)
                {
                    try
                    {
                        ScanAndInject();
                    }
                    catch (Exception e)
                    {
                        Debug.LogWarning("[Bc7Rdo] worker 扫描异常: " + e.Message);
                    }
                    Thread.Sleep(500);
                }
            }

            // 扫描当前进程的子进程，把 worker（Unity.exe / AssetImportWorker*）
            // 注入 hook DLL（幂等）
            public static void ScanAndInject()
            {
                uint self = (uint)System.Diagnostics.Process.GetCurrentProcess().Id;
                IntPtr snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snap == IntPtr.Zero)
                    return;
                try
                {
                    ProcessEntry32W entry = new ProcessEntry32W();
                    entry.dwSize = (uint)Marshal.SizeOf(typeof(ProcessEntry32W));
                    bool ok = Process32FirstW(snap, ref entry);
                    while (ok)
                    {
                        if (entry.th32ParentProcessID == self && IsWorkerName(entry.szExeFile))
                            InjectIfNeeded(entry.th32ProcessID);
                        ok = Process32NextW(snap, ref entry);
                    }
                }
                finally
                {
                    CloseHandle(snap);
                }
            }

            private static bool IsWorkerName(string exeFile)
            {
                if (string.IsNullOrEmpty(exeFile))
                    return false;
                string n = exeFile.ToLowerInvariant();
                return n == "unity.exe" ||
                       n.StartsWith("assetimportworker", StringComparison.Ordinal);
            }

            // 注入指定 PID（幂等；失败时移出集合以便下次重试）
            public static bool InjectIfNeeded(uint pid)
            {
                lock (s_lock)
                {
                    if (!s_injected.Add(pid))
                        return true;  // 已注入
                }
                bool ok = Inject(pid);
                if (!ok)
                {
                    lock (s_lock)
                        s_injected.Remove(pid);
                }
                return ok;
            }

            // 查询 PID 是否已注入（测试/诊断用）
            public static bool IsInjected(uint pid)
            {
                lock (s_lock)
                    return s_injected.Contains(pid);
            }

            // CreateRemoteThread + LoadLibraryW 注入（与注入器 exe 同理；
            // kernel32 基址在同一次系统启动的所有进程中相同）
            private static bool Inject(uint pid)
            {
                if (_pluginDir.Length == 0)
                    return false;
                string hookDll = _pluginDir + "\\bc7_rdo_hook.dll";
                if (!File.Exists(hookDll))
                    return false;

                IntPtr hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                           PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE,
                                           false, pid);
                if (hProc == IntPtr.Zero)
                {
                    Debug.LogWarning("[Bc7Rdo] worker 注入 OpenProcess 失败 PID=" + pid +
                                     " error=" + Marshal.GetLastWin32Error());
                    return false;
                }
                try
                {
                    byte[] path = System.Text.Encoding.Unicode.GetBytes(hookDll + "\0");
                    IntPtr remote = VirtualAllocEx(hProc, IntPtr.Zero, (UIntPtr)path.Length,
                                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                    if (remote == IntPtr.Zero)
                        return false;

                    if (!WriteProcessMemory(hProc, remote, path, path.Length, out _))
                    {
                        VirtualFreeEx(hProc, remote, UIntPtr.Zero, MEM_RELEASE);
                        return false;
                    }

                    IntPtr loadLibW = GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryW");
                    IntPtr hThread = CreateRemoteThread(hProc, IntPtr.Zero, UIntPtr.Zero,
                                                        loadLibW, remote, 0, out _);
                    if (hThread == IntPtr.Zero)
                    {
                        VirtualFreeEx(hProc, remote, UIntPtr.Zero, MEM_RELEASE);
                        return false;
                    }

                    WaitForSingleObject(hThread, 10000);
                    GetExitCodeThread(hThread, out uint code);
                    CloseHandle(hThread);
                    VirtualFreeEx(hProc, remote, UIntPtr.Zero, MEM_RELEASE);

                    bool ok = code != 0 && code != STILL_ACTIVE;
                    if (ok)
                        Debug.Log("[Bc7Rdo] worker 注入成功 PID=" + pid + " hook=" + hookDll);
                    else
                        Debug.LogWarning("[Bc7Rdo] worker 注入 LoadLibrary 返回 0 PID=" + pid);
                    return ok;
                }
                finally
                {
                    CloseHandle(hProc);
                }
            }
        }

        // 批处理验证入口：从环境变量 BC7RDO_INJECT_PID 读 PID 并注入 hook DLL
        public static void InjectPidTest()
        {
            try
            {
                string pidStr = System.Environment.GetEnvironmentVariable("BC7RDO_INJECT_PID");
                if (string.IsNullOrEmpty(pidStr) || !uint.TryParse(pidStr, out uint pid))
                {
                    Debug.LogError("[BC7RDO][INJECT] FAIL 环境变量 BC7RDO_INJECT_PID 缺失或非法");
                    Exit(1);
                    return;
                }
                bool ok = ImportWorkerInjector.InjectIfNeeded(pid);
                Debug.Log("[BC7RDO][INJECT] " + (ok ? "PASS" : "FAIL") + " pid=" + pid);
                Exit(ok ? 0 : 1);
            }
            catch (Exception e)
            {
                Debug.LogException(e);
                Exit(1);
            }
        }

        // 批处理验证入口：模拟"Unity 子进程 = worker"场景。
        // 启动环境变量 BC7RDO_CHILD_EXE 指向的子进程（测试用，exe 名必须为
        // Unity.exe 才能匹配 worker 识别规则），等待其加载 compress_bc7e.dll
        // 跑出基线轮后，执行一次 ScanAndInject（正常场景由后台监控线程
        // 每 500ms 自动执行），验证"子进程识别 -> 注入 -> hook 生效"链路。
        public static void ScanInjectTest()
        {
            try
            {
                string childExe = System.Environment.GetEnvironmentVariable("BC7RDO_CHILD_EXE");
                if (string.IsNullOrEmpty(childExe) || !File.Exists(childExe))
                {
                    Debug.LogError("[BC7RDO][SCANINJECT] FAIL 环境变量 BC7RDO_CHILD_EXE 缺失或文件不存在");
                    Exit(1);
                    return;
                }

                System.Diagnostics.ProcessStartInfo psi = new System.Diagnostics.ProcessStartInfo
                {
                    FileName = childExe,
                    Arguments = "20 1000",
                    WorkingDirectory = Path.GetDirectoryName(childExe),
                    CreateNoWindow = true,
                    UseShellExecute = false,
                };
                using (System.Diagnostics.Process child = System.Diagnostics.Process.Start(psi))
                {
                    if (child == null)
                    {
                        Debug.LogError("[BC7RDO][SCANINJECT] FAIL 子进程启动失败");
                        Exit(1);
                        return;
                    }
                    Thread.Sleep(1200);  // 等子进程加载 compress_bc7e.dll 并跑出基线轮

                    ImportWorkerInjector.ScanAndInject();  // 正常场景由监控线程自动执行
                    bool injected = ImportWorkerInjector.IsInjected((uint)child.Id);
                    Debug.Log("[BC7RDO][SCANINJECT] " + (injected ? "PASS" : "FAIL") +
                              " pid=" + child.Id);
                    // 等子进程跑完（20 轮 x 1s）再退出，避免 Unity 退出时
                    // 作业对象清理误杀子进程、日志不完整
                    for (int i = 0; i < 30 && !child.HasExited; i++)
                        Thread.Sleep(1000);
                    Exit(injected ? 0 : 1);
                }
            }
            catch (Exception e)
            {
                Debug.LogException(e);
                Exit(1);
            }
        }
    }

    // -----------------------------------------------------------------------
    // 资源后处理：自动把 bc7_rdo_hook.dll 的插件导入器限制为"仅编辑器"，
    // 避免打包时把原生 hook DLL 打进玩家包（也避免非 x64 目标平台报
    // "plugin incompatible"）。幂等：已设置过则跳过。
    // -----------------------------------------------------------------------

    public class HookDllPlatformPostprocessor : AssetPostprocessor
    {
        // SaveAndReimport 会再次触发 OnPostprocessAllAssets，用集合防递归
        private static readonly System.Collections.Generic.HashSet<string> s_processed =
            new System.Collections.Generic.HashSet<string>(System.StringComparer.OrdinalIgnoreCase);

        private static void OnPostprocessAllAssets(string[] importedAssets,
            string[] deletedAssets, string[] movedAssets, string[] movedFromAssetPaths)
        {
            foreach (string path in importedAssets)
            {
                if (!path.EndsWith("bc7_rdo_hook.dll", StringComparison.OrdinalIgnoreCase))
                    continue;
                if (!s_processed.Add(path))
                    continue;  // 本会话已处理过（防 SaveAndReimport 递归）

                PluginImporter imp = AssetImporter.GetAtPath(path) as PluginImporter;
                if (imp == null)
                    continue;
                if (!imp.GetCompatibleWithAnyPlatform() && imp.GetCompatibleWithEditor())
                    continue;  // 已是仅编辑器设置

                imp.SetCompatibleWithAnyPlatform(false);
                imp.SetCompatibleWithEditor(true);
                imp.SaveAndReimport();
                Debug.Log("[Bc7Rdo] bc7_rdo_hook.dll 已设为仅编辑器平台（不会打进玩家包）: " + path);
            }
        }
    }
}
