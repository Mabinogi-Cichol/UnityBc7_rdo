var resident = false;
var coordinator = false;
foreach (System.Diagnostics.ProcessModule module in System.Diagnostics.Process.GetCurrentProcess().Modules)
{
    if (module.ModuleName == "rgbm_runtime_unavailable_probe.dll") resident = true;
    if (module.ModuleName == "bc7_rgbm_coordinator.dll") coordinator = true;
}
var cacheRestored = System.Environment.GetEnvironmentVariable("BC7_RGBM_P3_CACHE") == null;
var reportRestored = System.Environment.GetEnvironmentVariable("BC7_RGBM_P3_REPORT") == null;
var json = "{\"probe_resident\":" + resident.ToString().ToLowerInvariant() +
    ",\"coordinator_resident\":" + coordinator.ToString().ToLowerInvariant() +
    ",\"probe_environment_restored\":" + (cacheRestored && reportRestored).ToString().ToLowerInvariant() +
    ",\"is_compiling\":" + UnityEditor.EditorApplication.isCompiling.ToString().ToLowerInvariant() +
    ",\"is_updating\":" + UnityEditor.EditorApplication.isUpdating.ToString().ToLowerInvariant() + "}";
System.IO.File.WriteAllText(@"E:\Git\UnityBc7_rdo\out\rgbm-portability\runtime\cleanup-f2.json", json);
return json;
