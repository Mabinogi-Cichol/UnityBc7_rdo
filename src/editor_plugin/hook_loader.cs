// hook_loader.cs
// Unity Editor Plugin：把外挂注入的 BC7 RDO hook 封装成"放进工程即生效"。
//
// 用法：
//   把 out\editor-plugin\Bc7Rdo\ 整个目录复制到任意工程的 Assets 下任意位置
//   （如 Assets\Editor\Bc7Rdo\），目录内包含：
//     - hook_loader.cs      本脚本（自动加载 hook DLL）
//     - bc7_rdo_hook.dll    原生 hook DLL（inline detour + ERT RDO 后处理）
//     - bc7_rgbm_coordinator.dll / bc7_rgbm_loader_gate.dll
//                           RGBM 私有入口的事务安装依赖
//     - RgbmBindings~      绑定解析、认证工具与私有 DIA
//     - oo2tex_win64_2.9.16.dll / oo2texrt_win64_2.9.16.dll
//                           Oodle Texture 编码器运行库（可选，encoder=oodle 时使用）
//     - bc7_rdo_config.txt  RDO 配置（同目录热更新；encoder= 可切换 bc7enc_rdo / oodle）
//   重新打开 / 刷新工程即自动生效，无需注入器、无需管理员权限。
//
// 原理：
//   C# 代码跑在 Unity Editor 进程内（[InitializeOnLoad] 静态构造在编辑器域
//   加载时执行），本脚本先准备当前 Unity 的 RGBM 绑定缓存，再通过
//   LoadLibraryW 加载 bc7_rdo_hook.dll 并初始化语义 Hook。
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
using System.Security.Cryptography;
using System.Text;

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
        private static string _bindingCache = "";
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private delegate uint RgbmInitializeDelegate([MarshalAs(UnmanagedType.LPWStr)] string cache);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint RgbmStateDelegate();
        public static uint RgbmState
        {
            get {
                if (_hookModule == IntPtr.Zero) return 5;
                var entry = GetProcAddress(_hookModule, "Bc7RgbmGetState");
                return entry == IntPtr.Zero ? 5 : ((RgbmStateDelegate)Marshal.GetDelegateForFunctionPointer(entry, typeof(RgbmStateDelegate)))();
            }
        }

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
                if (Application.platform != RuntimePlatform.WindowsEditor || IntPtr.Size != 8 || AssetDatabase.IsAssetImportWorkerProcess()) return false;
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

                if (hits.Length != 1) throw new IOException("Multiple BC7 hook DLLs found; choose one plugin directory before restarting.");
                string dllPath = Path.GetFullPath(hits[0]);
                _pluginDir = Path.GetDirectoryName(dllPath);
                RgbmBindingsLoader.ProjectRoot = Path.GetDirectoryName(Application.dataPath);
                RgbmBindingsLoader.PluginDirectory = _pluginDir;
                RgbmBindingsLoader.Enabled = RgbmBindingsLoader.ReadEnabled(Path.Combine(_pluginDir, "bc7_rdo_config.txt"));
                string bindingError;
                bool prepared = RgbmBindingsLoader.Prepare(EditorApplication.applicationPath, out _bindingCache, out bindingError);
                if (!prepared) Debug.LogWarning("[Bc7Rdo] RGBM cache preparation failed; native validation will decide: " + bindingError);

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
                var initialize = GetProcAddress(_hookModule, "Bc7RgbmInitialize");
                if (initialize == IntPtr.Zero) throw new IOException("Old hook DLL is resident; restart the Editor with the complete updated plugin.");
                var init = (RgbmInitializeDelegate)Marshal.GetDelegateForFunctionPointer(initialize, typeof(RgbmInitializeDelegate));
                uint state = init(_bindingCache);
                Debug.Log("[Bc7Rdo] RGBM runtime state=" + state + " pid=" + System.Diagnostics.Process.GetCurrentProcess().Id + " cache=" + _bindingCache);

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

        // 写 RDO 配置到插件目录（hook DLL 每次调用都会热重读该文件）。
        // 冒烟测试固定用默认编码器 bc7enc_rdo（ERT 路径），不切换 encoder=oodle。
        private static void WriteConfig(float lambda)
        {
            string cfgPath = Path.Combine(_pluginDir, "bc7_rdo_config.txt");
            File.WriteAllText(cfgPath,
                "encoder=bc7enc_rdo\n" +
                "lambda=" + lambda.ToString(System.Globalization.CultureInfo.InvariantCulture) +
                "\noodle_effort=high\noodle_lambda=30\nlookback=128\nsmooth_scale=15\n" +
                "max_rms_increase_ratio=10\ntwo_matches=1\ndebug=0\n");
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

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool QueryFullProcessImageNameW(IntPtr process, uint flags,
            System.Text.StringBuilder path, ref uint size);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetProcessTimes(IntPtr process, out long creation, out long exit, out long kernel, out long user);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetModuleHandleExW(uint flags, IntPtr address, out IntPtr module);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetModuleFileNameW(IntPtr module, System.Text.StringBuilder path, uint size);
        [DllImport("ntdll.dll")]
        private static extern int NtQueryInformationProcess(IntPtr process, int info, IntPtr buffer, uint length, out uint needed);
        [StructLayout(LayoutKind.Sequential)]
        private struct NativeUnicodeString { public ushort length, maximum; public IntPtr buffer; }
        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr CommandLineToArgvW(string command, out int count);
        [DllImport("kernel32.dll")]
        private static extern IntPtr LocalFree(IntPtr memory);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool Module32FirstW(IntPtr snapshot, ref ModuleEntry32W entry);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool Module32NextW(IntPtr snapshot, ref ModuleEntry32W entry);
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct ModuleEntry32W
        {
            public uint size, moduleId, processId, globalUsage, processUsage;
            public IntPtr baseAddress;
            public uint baseSize;
            public IntPtr module;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string name;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string path;
        }

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
            private const uint SYNCHRONIZE = 0x00100000;
            private const uint STILL_ACTIVE = 259;

            private sealed class Attempt { internal uint Pid; internal long Creation; internal volatile string Hash; internal volatile bool Ready; }
            private static readonly System.Collections.Generic.Dictionary<string, Attempt> s_attempts =
                new System.Collections.Generic.Dictionary<string, Attempt>();
            private static readonly object s_lock = new object();
            private static int s_monitorStarted;
            private static int s_stopped;

            // 后台轮询：每 500ms 扫描一次子进程里的 worker 并注入（幂等）。
            // worker 可能频繁重建（每批任务后重启），间隔取小值减少漏网。
            public static void Start()
            {
                if (Interlocked.Exchange(ref s_monitorStarted, 1) != 0)
                    return;
                AssemblyReloadEvents.beforeAssemblyReload += Stop;
                EditorApplication.quitting += Stop;
                Thread t = new Thread(MonitorLoop);
                t.IsBackground = true;
                t.Start();
            }

            private static void MonitorLoop()
            {
                while (Volatile.Read(ref s_stopped) == 0)
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
            private static void Stop() { Interlocked.Exchange(ref s_stopped, 1); }

            // 扫描当前进程的子进程，把 worker（Unity.exe / AssetImportWorker*）
            // 注入 hook DLL（幂等）
            public static void ScanAndInject()
            {
                uint self = (uint)System.Diagnostics.Process.GetCurrentProcess().Id;
                IntPtr snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snap == IntPtr.Zero || snap == new IntPtr(-1))
                    return;
                try
                {
                    ProcessEntry32W entry = new ProcessEntry32W();
                    entry.dwSize = (uint)Marshal.SizeOf(typeof(ProcessEntry32W));
                    bool ok = Process32FirstW(snap, ref entry);
                    while (ok)
                    {
                        if (entry.th32ParentProcessID == self && IsWorkerName(entry.szExeFile) && IsProjectWorker(entry.th32ProcessID))
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
            private static bool IsProjectWorker(uint pid)
            {
                // Query Windows' UNICODE_STRING result directly. Unsupported or
                // denied process-information queries safely skip this child.
                var process = OpenProcess(PROCESS_QUERY_INFORMATION, false, pid);
                if (process == IntPtr.Zero) return false;
                IntPtr memory = IntPtr.Zero, argv = IntPtr.Zero;
                try {
                    uint needed;
                    NtQueryInformationProcess(process, 60, IntPtr.Zero, 0, out needed);
                    if (needed < Marshal.SizeOf(typeof(NativeUnicodeString)) || needed > 131072) return false;
                    uint capacity = needed;
                    memory = Marshal.AllocHGlobal((int)needed);
                    if (NtQueryInformationProcess(process, 60, memory, needed, out needed) < 0) return false;
                    var value = (NativeUnicodeString)Marshal.PtrToStructure(memory, typeof(NativeUnicodeString));
                    long begin = value.buffer.ToInt64() - memory.ToInt64();
                    if ((value.length & 1) != 0 || value.length > value.maximum || begin < Marshal.SizeOf(typeof(NativeUnicodeString)) || begin + value.length > capacity) return false;
                    string command = Marshal.PtrToStringUni(value.buffer, value.length / 2);
                    int count; argv = CommandLineToArgvW(command, out count);
                    if (argv == IntPtr.Zero || count < 1 || count > 4096) return false;
                    bool worker = false, adb2 = false, workerName = false, server = false; string project = null;
                    for (int i = 1; i < count; ++i) {
                        var arg = Marshal.PtrToStringUni(Marshal.ReadIntPtr(argv, i * IntPtr.Size));
                        if (arg.Equals("-assetImportWorker", StringComparison.OrdinalIgnoreCase)) worker = true;
                        if (arg.Equals("-adb2", StringComparison.OrdinalIgnoreCase)) adb2 = true;
                        if (arg.Equals("-name", StringComparison.OrdinalIgnoreCase) && i + 1 < count) {
                            string name = Marshal.PtrToStringUni(Marshal.ReadIntPtr(argv, (++i) * IntPtr.Size));
                            const string prefix = "AssetImportWorker";
                            workerName = name != null && name.StartsWith(prefix, StringComparison.Ordinal) && name.Length > prefix.Length;
                            if (workerName) for (int j = prefix.Length; j < name.Length; ++j) workerName &= name[j] >= '0' && name[j] <= '9';
                        }
                        if (arg.Equals("-srvPort", StringComparison.OrdinalIgnoreCase) && i + 1 < count) {
                            int port; server = int.TryParse(Marshal.PtrToStringUni(Marshal.ReadIntPtr(argv, (++i) * IntPtr.Size)), out port) && port > 0 && port <= 65535;
                        }
                        if (arg.Equals("-projectPath", StringComparison.OrdinalIgnoreCase) && i + 1 < count)
                            project = Marshal.PtrToStringUni(Marshal.ReadIntPtr(argv, (++i) * IntPtr.Size));
                    }
                    return (worker || (adb2 && workerName && server)) && project != null && string.Equals(Path.GetFullPath(project).TrimEnd('\\', '/'),
                        RgbmBindingsLoader.ProjectRoot.TrimEnd('\\', '/'), StringComparison.OrdinalIgnoreCase);
                } catch (Exception) { return false; }
                finally { if (argv != IntPtr.Zero) LocalFree(argv); if (memory != IntPtr.Zero) Marshal.FreeHGlobal(memory); CloseHandle(process); }
            }

            // 注入指定 PID（幂等；失败时移出集合以便下次重试）
            public static bool InjectIfNeeded(uint pid)
            {
                long creation;
                using (var process = System.Diagnostics.Process.GetProcessById((int)pid)) creation = process.StartTime.ToUniversalTime().ToFileTimeUtc();
                string identity = pid + "." + creation;
                Attempt attempt;
                lock (s_lock)
                {
                    if (s_attempts.TryGetValue(identity, out attempt)) {
                        if (!attempt.Ready) attempt.Ready = Confirm(attempt);
                        return attempt.Ready;
                    }
                    attempt = new Attempt { Pid = pid, Creation = creation };
                    s_attempts.Add(identity, attempt);
                }
                bool started = false;
                try { attempt.Ready = Inject(attempt, out started); return attempt.Ready; }
                finally { if (!started) lock (s_lock) s_attempts.Remove(identity); }
            }

            // 查询 PID 是否已注入（测试/诊断用）
            public static bool IsInjected(uint pid)
            {
                return InjectIfNeeded(pid);
            }
            private static bool Confirm(Attempt attempt)
            {
                if (attempt.Hash == null) return false;
                if (RgbmBindingsLoader.Enabled) {
                    string path = RgbmBindingsLoader.ReportPath(attempt.Pid, attempt.Creation);
                    if (!File.Exists(path)) return false;
                    var report = JsonUtility.FromJson<RgbmBindingsLoader.RuntimeReport>(File.ReadAllText(path));
                    return report != null && report.artifact_kind == "rgbm_runtime_bootstrap" && report.pid == attempt.Pid &&
                        report.creation_filetime == attempt.Creation.ToString() && report.exe_sha256 == attempt.Hash &&
                        string.Equals(report.hook_module, Path.Combine(_pluginDir, "bc7_rdo_hook.dll"), StringComparison.OrdinalIgnoreCase) &&
                        report.state == "Ready" && report.hook_installed;
                }
                // HMODULE is 64-bit; GetExitCodeThread's DWORD is not a module handle.
                IntPtr snapshot = CreateToolhelp32Snapshot(0x8 | 0x10, attempt.Pid);
                if (snapshot == IntPtr.Zero || snapshot == new IntPtr(-1)) return false;
                try {
                    var entry = new ModuleEntry32W { size = (uint)Marshal.SizeOf(typeof(ModuleEntry32W)) };
                    bool next = Module32FirstW(snapshot, ref entry);
                    while (next) {
                        if (string.Equals(entry.path, Path.Combine(_pluginDir, "bc7_rdo_hook.dll"), StringComparison.OrdinalIgnoreCase)) return true;
                        next = Module32NextW(snapshot, ref entry);
                    }
                    return false;
                } finally { CloseHandle(snapshot); }
            }
            private static IntPtr RemoteLoadLibrary(uint pid)
            {
                var local = GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryW");
                IntPtr owner;
                if (local == IntPtr.Zero || !GetModuleHandleExW(6, local, out owner)) return IntPtr.Zero;
                var ownerPath = new System.Text.StringBuilder(32768);
                if (GetModuleFileNameW(owner, ownerPath, 32768) == 0) return IntPtr.Zero;
                long offset = local.ToInt64() - owner.ToInt64();
                IntPtr snapshot = CreateToolhelp32Snapshot(0x8 | 0x10, pid);
                if (snapshot == IntPtr.Zero || snapshot == new IntPtr(-1)) return IntPtr.Zero;
                try {
                    var entry = new ModuleEntry32W { size = (uint)Marshal.SizeOf(typeof(ModuleEntry32W)) };
                    bool next = Module32FirstW(snapshot, ref entry);
                    while (next) {
                        if (string.Equals(entry.path, ownerPath.ToString(), StringComparison.OrdinalIgnoreCase) && offset >= 0 && offset < entry.baseSize)
                            return new IntPtr(entry.baseAddress.ToInt64() + offset);
                        next = Module32NextW(snapshot, ref entry);
                    }
                    return IntPtr.Zero;
                } finally { CloseHandle(snapshot); }
            }

            // Resolve the actual owner module/RVA in the target (including a
            // forwarded KernelBase export); do not assume equal ASLR addresses.
            private static bool Inject(Attempt attempt, out bool started)
            {
                started = false;
                uint pid = attempt.Pid;
                if (_pluginDir.Length == 0)
                    return false;
                string hookDll = _pluginDir + "\\bc7_rdo_hook.dll";
                if (!File.Exists(hookDll))
                    return false;

                IntPtr hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | SYNCHRONIZE |
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
                    long creation, exited, kernel, user;
                    if (!GetProcessTimes(hProc, out creation, out exited, out kernel, out user) || creation != attempt.Creation) return false;
                    var image = new System.Text.StringBuilder(32768); uint size = 32768;
                    if (!QueryFullProcessImageNameW(hProc, 0, image, ref size)) return false;
                    string cache, error;
                    if (!RgbmBindingsLoader.Prepare(image.ToString(), out cache, out error)) {
                        Debug.LogWarning("[Bc7Rdo] worker cache rejected PID=" + pid + " " + error); return false;
                    }
                    attempt.Hash = RgbmBindingsLoader.Enabled ? RgbmBindingsLoader.HashFile(image.ToString()) : "";
                    byte[] path = System.Text.Encoding.Unicode.GetBytes(hookDll + "\0");
                    IntPtr remote = VirtualAllocEx(hProc, IntPtr.Zero, (UIntPtr)path.Length,
                                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                    if (remote == IntPtr.Zero)
                        return false;

                    IntPtr written;
                    if (!WriteProcessMemory(hProc, remote, path, path.Length, out written) || written.ToInt64() != path.Length)
                    {
                        VirtualFreeEx(hProc, remote, UIntPtr.Zero, MEM_RELEASE);
                        return false;
                    }

                    IntPtr loadLibW = RemoteLoadLibrary(pid);
                    if (loadLibW == IntPtr.Zero) { VirtualFreeEx(hProc, remote, UIntPtr.Zero, MEM_RELEASE); return false; }
                    IntPtr hThread = CreateRemoteThread(hProc, IntPtr.Zero, UIntPtr.Zero,
                                                        loadLibW, remote, 0, out _);
                    if (hThread == IntPtr.Zero)
                    {
                        VirtualFreeEx(hProc, remote, UIntPtr.Zero, MEM_RELEASE);
                        return false;
                    }

                    started = true;
                    uint wait = WaitForSingleObject(hThread, 10000);
                    CloseHandle(hThread);
                    // A timed-out remote thread may still read its argument.
                    // Retain it to process exit and do not inject this identity again.
                    if (wait != 0) {
                        Debug.LogWarning("[Bc7Rdo] worker LoadLibrary still pending PID=" + pid + "; argument retained until process exit");
                        return false;
                    }
                    VirtualFreeEx(hProc, remote, UIntPtr.Zero, MEM_RELEASE);
                    bool ok = false;
                    for (int i = 0; i < 100; ++i) {
                        if (Confirm(attempt)) { ok = true; break; }
                        if (WaitForSingleObject(hProc, 0) == 0) break;
                        Thread.Sleep(100);
                    }
                    if (ok)
                        Debug.Log("[Bc7Rdo] worker initialized PID=" + pid + " creation=" + attempt.Creation + " exe_sha256=" + attempt.Hash);
                    else
                        Debug.LogWarning("[Bc7Rdo] worker classification not Ready PID=" + pid + "; inspect its runtime report, restart worker after fixing dependencies");
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
    // 资源后处理：自动把插件原生 DLL（hook 载荷 + Oodle 编码器运行库）的
    // 插件导入器限制为"仅编辑器"，避免打包时把原生 DLL 打进玩家包（也避免
    // 非 x64 目标平台报 "plugin incompatible"）。幂等：已设置过则跳过。
    // -----------------------------------------------------------------------

    public class HookDllPlatformPostprocessor : AssetPostprocessor
    {
        // 需要设为"仅编辑器"的原生 DLL：
        //   - bc7_rdo_hook.dll          hook 载荷（inline detour + RDO）
        //   - oo2tex_win64_2.9.16.dll    Oodle Texture 主库（编码/解码）
        //   - oo2texrt_win64_2.9.16.dll  Oodle Texture RT（BC7Prep 解码）
        // Oodle DLL 是 Epic 专有库，仅在编辑器进程内 LoadLibrary 使用，
        // 必须确保不随玩家包分发。
        private static readonly string[] s_editorOnlyDlls =
        {
            "bc7_rdo_hook.dll",
            "bc7_rgbm_coordinator.dll",
            "bc7_rgbm_loader_gate.dll",
            "oo2tex_win64_2.9.16.dll",
            "oo2texrt_win64_2.9.16.dll",
        };

        // SaveAndReimport 会再次触发 OnPostprocessAllAssets，用集合防递归
        private static readonly System.Collections.Generic.HashSet<string> s_processed =
            new System.Collections.Generic.HashSet<string>(System.StringComparer.OrdinalIgnoreCase);

        private static void OnPostprocessAllAssets(string[] importedAssets,
            string[] deletedAssets, string[] movedAssets, string[] movedFromAssetPaths)
        {
            foreach (string path in importedAssets)
            {
                if (!IsManagedPluginDll(path))
                    continue;
                if (!s_processed.Add(path))
                    continue;  // 本会话已处理过（防 SaveAndReimport 递归）

                PluginImporter imp = AssetImporter.GetAtPath(path) as PluginImporter;
                if (imp == null)
                    continue;
                if (!imp.GetCompatibleWithAnyPlatform() && imp.GetCompatibleWithEditor() && !imp.isPreloaded &&
                    imp.GetEditorData("CPU") == "x86_64" && imp.GetEditorData("OS") == "Windows")
                    continue;  // 已是仅编辑器设置

                imp.SetCompatibleWithAnyPlatform(false);
                imp.SetCompatibleWithEditor(true);
                imp.isPreloaded = false;
                imp.SetEditorData("CPU", "x86_64");
                imp.SetEditorData("OS", "Windows");
                imp.SaveAndReimport();
                Debug.Log("[Bc7Rdo] " + System.IO.Path.GetFileName(path) +
                          " 已设为仅编辑器平台（不会打进玩家包）: " + path);
            }
        }

        // 文件名大小写不敏感匹配插件自有原生 DLL
        private static bool IsManagedPluginDll(string path)
        {
            foreach (string name in s_editorOnlyDlls)
            {
                if (path.EndsWith(name, StringComparison.OrdinalIgnoreCase))
                    return true;
            }
            return false;
        }
    }

    // Current-exe binding preparation and runtime reports share the loader file.
    internal static class RgbmBindingsLoader
    {
        internal static string ProjectRoot;
        internal static string PluginDirectory;
        internal static bool Enabled;
        internal static string CacheDirectory { get { return Path.Combine(ProjectRoot, "Library/Bc7Rdo/HookBindings"); } }
        internal static string HashFile(string path)
        {
            using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            using (var sha = SHA256.Create())
                return BitConverter.ToString(sha.ComputeHash(file)).Replace("-", "");
        }
        internal static bool ReadEnabled(string path)
        {
            if (!File.Exists(path)) return false;
            bool enabled = false;
            foreach (var raw in File.ReadAllLines(path))
            {
                var line = raw.Split('#', ';')[0].Trim(); var split = line.IndexOf('=');
                if (split < 0 || !line.Substring(0, split).Trim().Equals("unity_rgbm_context_hooks", StringComparison.OrdinalIgnoreCase)) continue;
                int value; if (int.TryParse(line.Substring(split + 1).Trim(), out value)) enabled = value != 0;
            }
            return enabled;
        }
        internal static bool Prepare(string exe, out string cache, out string error)
        {
            cache = ""; error = "";
            if (!Enabled) return true;
            try
            {
                exe = Path.GetFullPath(exe); string hash = HashFile(exe);
                Directory.CreateDirectory(CacheDirectory);
                cache = Path.Combine(CacheDirectory, hash + ".v1.bindings");
                string tools = Path.Combine(PluginDirectory, "RgbmBindings~");
                string validator = Path.Combine(tools, "bc7_rgbm_bindings.exe");
                if (!File.Exists(validator)) throw new IOException("binding validator missing: " + validator);
                // Per-project, per-exe writer lease; workers never acquire it or write bindings.
                string projectKey;
                using (var sha = SHA256.Create()) projectKey = BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(CacheDirectory.ToUpperInvariant()))).Replace("-", "");
                using (var lease = new Mutex(false, "Local\\Bc7RgbmBindings_" + projectKey + "_" + hash))
                {
                    bool acquired = false;
                    try
                    {
                        try { acquired = lease.WaitOne(30000); } catch (AbandonedMutexException) { acquired = true; }
                        if (!acquired) throw new TimeoutException("binding cache writer busy");
                        string report = Path.Combine(CacheDirectory, hash + ".validation.json");
                        string verify = "--mode verify --exe " + Quote(exe) + " --cache " + Quote(cache) + " --report " + Quote(report);
                        if (File.Exists(cache) && Run(validator, verify, hash + ".verify.log") == 0) return true;
                        string pdb = Path.Combine(Path.GetDirectoryName(exe), "unity_x64.pdb");
                        if (File.Exists(pdb))
                        {
                            string resolver = Path.Combine(tools, "bc7_rgbm_resolver.exe"), dia = Path.Combine(tools, "msdia140.dll");
                            if (!File.Exists(resolver) || !File.Exists(dia)) throw new IOException("private PDB resolver/DIA dependency missing");
                            string evidence = Path.Combine(CacheDirectory, hash + ".evidence.json");
                            int result = Run(resolver, "--exe " + Quote(exe) + " --pdb " + Quote(pdb) + " --dia " + Quote(dia) +
                                " --cache " + Quote(cache) + " --report " + Quote(evidence), hash + ".pdb.log");
                            if (result != 0) throw new IOException("PDB/contract rejected: " + result + " report=" + evidence);
                        }
                        else if (Run(validator, "--mode scan --exe " + Quote(exe) + " --cache " + Quote(cache) + " --report " + Quote(report), hash + ".scan.log") != 0)
                            throw new IOException("bounded scan rejected: " + report);
                        if (Run(validator, verify, hash + ".verify.log") != 0) throw new IOException("published cache failed verification");
                        return true;
                    }
                    finally { if (acquired) lease.ReleaseMutex(); }
                }
            }
            catch (Exception ex) { error = ex.Message; return false; }
        }
        private static string Quote(string value)
        {
            // These are file paths, passed directly to CreateProcess, not a shell.
            if (value.IndexOf('"') >= 0 || value.EndsWith("\\", StringComparison.Ordinal)) throw new ArgumentException("invalid file argument");
            return "\"" + value + "\"";
        }
        private static int Run(string exe, string arguments, string log)
        {
            var text = new StringBuilder(); var gate = new object();
            var start = new System.Diagnostics.ProcessStartInfo(exe, arguments) { UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = Path.GetDirectoryName(exe) };
            using (var process = new System.Diagnostics.Process { StartInfo = start })
            {
                process.OutputDataReceived += (s, e) => { if (e.Data != null) lock (gate) text.AppendLine(e.Data); };
                process.ErrorDataReceived += (s, e) => { if (e.Data != null) lock (gate) text.AppendLine(e.Data); };
                process.Start(); process.BeginOutputReadLine(); process.BeginErrorReadLine();
                if (!process.WaitForExit(60000)) { process.Kill(); process.WaitForExit(3000); throw new TimeoutException("binding helper timeout"); }
                process.WaitForExit(); // Drain asynchronous pipes only after real exit.
                File.WriteAllText(Path.Combine(CacheDirectory, log), text.ToString(), new UTF8Encoding(false));
                return process.ExitCode;
            }
        }
        [Serializable] internal sealed class RuntimeReport
        {
            public string artifact_kind, creation_filetime, state, exe_sha256, reason, hook_module;
            public uint pid;
            public bool hook_installed, asset_import_worker;
        }
        internal static string ReportPath(uint pid, long creation)
        { return Path.Combine(CacheDirectory, pid + "." + creation + ".runtime.json"); }
    }
}
