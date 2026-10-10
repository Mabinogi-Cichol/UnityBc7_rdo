// Execute as an in-memory method body through UnityMCP execute_code.
// Builds stay in the repository's ignored out directory; project files are untouched.
var flags = System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static;
var loader = typeof(Bc7Rdo.HookLoader);
var load = loader.GetMethod("LoadLibraryW", flags);
var proc = loader.GetMethod("GetProcAddress", flags);
var free = loader.GetMethod("FreeLibrary", flags);
var service = System.IntPtr.Zero;
var probe = System.IntPtr.Zero;
var prior = System.Environment.GetEnvironmentVariable("BC7_RGBM_P2_REPORT");
var root = @"E:\Git\UnityBc7_rdo\out\rgbm-portability\coordinator\";
var report = root + "live-owned-code-f2.json";
var started = System.DateTime.UtcNow;
string result = null;
try
{
    service = (System.IntPtr)load.Invoke(null, new object[] { root + "bc7_rgbm_coordinator.dll" });
    if (service == System.IntPtr.Zero) throw new System.Exception("coordinator LoadLibrary failed");
    probe = (System.IntPtr)load.Invoke(null, new object[] { root + "rgbm_coordinator_probe.dll" });
    if (probe == System.IntPtr.Zero) throw new System.Exception("probe LoadLibrary failed");
    var address = (System.IntPtr)proc.Invoke(null, new object[] { probe, "RgbmTestCoordinatorOwnedCode" });
    if (address == System.IntPtr.Zero) throw new System.Exception("probe export missing");
    System.Environment.SetEnvironmentVariable("BC7_RGBM_P2_REPORT", report);
    var call = (System.Threading.ThreadStart)System.Runtime.InteropServices.Marshal.GetDelegateForFunctionPointer(address, typeof(System.Threading.ThreadStart));
    call();
    if (!System.IO.File.Exists(report) || System.IO.File.GetLastWriteTimeUtc(report) < started)
        throw new System.Exception("fresh probe report missing");
    result = System.IO.File.ReadAllText(report);
}
finally
{
    System.Environment.SetEnvironmentVariable("BC7_RGBM_P2_REPORT", prior);
    if (probe != System.IntPtr.Zero) free.Invoke(null, new object[] { probe });
    if (service != System.IntPtr.Zero) free.Invoke(null, new object[] { service });
}
return result;
