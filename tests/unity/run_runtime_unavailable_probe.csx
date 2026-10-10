// In-memory UnityMCP method body. No project file changes or Unity hook patches.
var flags = System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static;
var loader = typeof(Bc7Rdo.HookLoader);
var load = loader.GetMethod("LoadLibraryW", flags);
var proc = loader.GetMethod("GetProcAddress", flags);
var free = loader.GetMethod("FreeLibrary", flags);
var priorCache = System.Environment.GetEnvironmentVariable("BC7_RGBM_P3_CACHE");
var priorReport = System.Environment.GetEnvironmentVariable("BC7_RGBM_P3_REPORT");
var root = @"E:\Git\UnityBc7_rdo\out\rgbm-portability\runtime\";
var report = root + "live-unavailable-f2.json";
var started = System.DateTime.UtcNow;
var probe = System.IntPtr.Zero;
string result = null;
try
{
    probe = (System.IntPtr)load.Invoke(null, new object[] { root + "rgbm_runtime_unavailable_probe.dll" });
    if (probe == System.IntPtr.Zero) throw new System.Exception("runtime probe LoadLibrary failed");
    var address = (System.IntPtr)proc.Invoke(null, new object[] { probe, "RgbmTestUnavailableRuntime" });
    if (address == System.IntPtr.Zero) throw new System.Exception("probe export missing");
    System.Environment.SetEnvironmentVariable("BC7_RGBM_P3_CACHE", @"E:\Git\UnityBc7_rdo\out\rgbm-portability\p1-tests\f2.bindings");
    System.Environment.SetEnvironmentVariable("BC7_RGBM_P3_REPORT", report);
    var call = (System.Threading.ThreadStart)System.Runtime.InteropServices.Marshal.GetDelegateForFunctionPointer(address, typeof(System.Threading.ThreadStart));
    call();
    if (!System.IO.File.Exists(report) || System.IO.File.GetLastWriteTimeUtc(report) < started)
        throw new System.Exception("fresh runtime probe report missing");
    result = System.IO.File.ReadAllText(report);
}
finally
{
    System.Environment.SetEnvironmentVariable("BC7_RGBM_P3_CACHE", priorCache);
    System.Environment.SetEnvironmentVariable("BC7_RGBM_P3_REPORT", priorReport);
    if (probe != System.IntPtr.Zero) free.Invoke(null, new object[] { probe });
}
return result;
