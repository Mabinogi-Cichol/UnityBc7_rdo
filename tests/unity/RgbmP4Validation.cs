// Temporary batch validation script, deployed only for development validation.
using System;
using System.IO;
using System.Reflection;
using System.Diagnostics;
using System.Collections.Generic;
using UnityEngine;
using UnityEditor;

namespace Bc7Rdo
{
    public static class RgbmP4Validation
    {
        [Serializable] private sealed class Result
        {
            public bool passed, tools_ignored, native_plugins_editor_only, late_bc7_loaded, bc7_previously_loaded;
            public uint state;
            public int pid, actual_workers_ready;
            public string unity_version, exe_sha256, config_sha256, error;
            public string[] runtime_reports;
        }
        public static void Run()
        {
            string fixture = null; PropertyInfo desired = null; object priorCount = null;
            var result = new Result { pid = Process.GetCurrentProcess().Id, unity_version = Application.unityVersion };
            string output = Environment.GetEnvironmentVariable("BC7_RGBM_P4_REPORT");
            try
            {
                if (string.IsNullOrEmpty(output)) throw new Exception("BC7_RGBM_P4_REPORT missing");
                bool installed = HookLoader.Install(); result.state = HookLoader.RgbmState;
                if (!installed || result.state != 4) throw new Exception("RGBM not Ready: " + result.state);
                result.state = HookLoader.RgbmState;
                result.exe_sha256 = RgbmBindingsLoader.HashFile(EditorApplication.applicationPath);
                result.config_sha256 = RgbmBindingsLoader.HashFile(Path.Combine(RgbmBindingsLoader.PluginDirectory, "bc7_rdo_config.txt"));
                string relative = "Assets" + RgbmBindingsLoader.PluginDirectory.Substring(Application.dataPath.Length).Replace('\\', '/');
                result.tools_ignored = !AssetDatabase.IsValidFolder(relative + "/RgbmBindings~") &&
                    AssetDatabase.LoadMainAssetAtPath(relative + "/RgbmBindings~/msdia140.dll") == null &&
                    File.Exists(Path.Combine(RgbmBindingsLoader.PluginDirectory, "RgbmBindings~/msdia140.dll"));
                result.native_plugins_editor_only = true;
                foreach (string file in new[] { "bc7_rdo_hook.dll", "bc7_rgbm_coordinator.dll", "bc7_rgbm_loader_gate.dll" })
                {
                    var importer = AssetImporter.GetAtPath(relative + "/" + file) as PluginImporter;
                    result.native_plugins_editor_only &= importer != null && !importer.GetCompatibleWithAnyPlatform() &&
                        importer.GetCompatibleWithEditor() && !importer.isPreloaded && importer.GetEditorData("CPU") == "x86_64" && importer.GetEditorData("OS") == "Windows";
                }
                // Ready before explicitly loading BC7. This also works when Unity
                // has already loaded BC7 during startup; the report records that fact.
                var flags = BindingFlags.NonPublic | BindingFlags.Static;
                result.bc7_previously_loaded = (IntPtr)typeof(HookLoader).GetMethod("GetModuleHandle", flags).Invoke(null,
                    new object[] { "compress_bc7e.dll" }) != IntPtr.Zero;
                var module = (IntPtr)typeof(HookLoader).GetMethod("LoadLibraryW", flags).Invoke(null,
                    new object[] { Path.Combine(Path.GetDirectoryName(EditorApplication.applicationPath), "compress_bc7e.dll") });
                result.late_bc7_loaded = module != IntPtr.Zero;
                desired = typeof(AssetDatabase).GetProperty("DesiredWorkerCount", BindingFlags.Public | BindingFlags.Static);
                if (desired != null && desired.CanWrite) { priorCount = desired.GetValue(null, null); desired.SetValue(null, 2, null); }
                typeof(AssetDatabase).GetMethod("ForceToDesiredWorkerCount", BindingFlags.Public | BindingFlags.Static)?.Invoke(null, null);
                string name = "Bc7RdoP4Validation_" + Guid.NewGuid().ToString("N");
                fixture = "Assets/" + name; AssetDatabase.CreateFolder("Assets", name);
                var texture = new Texture2D(64, 64, TextureFormat.RGBA32, false);
                try {
                    var pixels = new Color32[4096];
                    for (int i = 0; i < pixels.Length; ++i) pixels[i] = new Color32((byte)i, (byte)(i >> 3), (byte)(i >> 5), 255);
                    texture.SetPixels32(pixels); texture.Apply(); var png = texture.EncodeToPNG();
                    for (int i = 0; i < 24; ++i) File.WriteAllBytes(Path.Combine(RgbmBindingsLoader.ProjectRoot, fixture, "rgba" + i + ".png"), png);
                } finally { UnityEngine.Object.DestroyImmediate(texture); }
                AssetDatabase.Refresh(ImportAssetOptions.ForceUpdate);
                // Wait for the independent bootstrap after the actual import.
                // This is a bounded assertion wait, not an import-log poll.
                var deadline = DateTime.UtcNow.AddSeconds(15);
                while (DateTime.UtcNow < deadline) {
                    int found = 0;
                    foreach (var file in Directory.GetFiles(RgbmBindingsLoader.CacheDirectory, "*.runtime.json")) {
                        var candidate = JsonUtility.FromJson<RgbmBindingsLoader.RuntimeReport>(File.ReadAllText(file));
                        if (candidate != null && candidate.asset_import_worker && candidate.state == "Ready" &&
                            File.GetLastWriteTimeUtc(file) >= Process.GetCurrentProcess().StartTime.ToUniversalTime()) ++found;
                    }
                    if (found >= 2) break;
                    System.Threading.Thread.Sleep(100);
                }
                // Background monitor runs during imports. Only identity-matched
                // native reports are counted; Unity child Editors are not workers.
                var reports = new List<string>();
                foreach (var file in Directory.GetFiles(RgbmBindingsLoader.CacheDirectory, "*.runtime.json"))
                {
                    if (File.GetLastWriteTimeUtc(file) < Process.GetCurrentProcess().StartTime.ToUniversalTime()) continue;
                    var report = JsonUtility.FromJson<RgbmBindingsLoader.RuntimeReport>(File.ReadAllText(file));
                    if (report == null || report.state != "Ready" || report.exe_sha256 != result.exe_sha256) continue;
                    if (report.asset_import_worker && report.pid != result.pid) {
                        try {
                            using (var process = Process.GetProcessById((int)report.pid))
                                if (process.StartTime.ToUniversalTime().ToFileTimeUtc().ToString() == report.creation_filetime) ++result.actual_workers_ready;
                        } catch (ArgumentException) { }
                    }
                    reports.Add(File.ReadAllText(file));
                }
                result.runtime_reports = reports.ToArray();
                result.passed = result.state == 4 && result.tools_ignored && result.native_plugins_editor_only && result.late_bc7_loaded && result.actual_workers_ready >= 2;
                if (!result.passed) throw new Exception("platform/dependency/bootstrap validation failed");
            }
            catch (Exception e) { result.error = e.ToString(); UnityEngine.Debug.LogError("[BC7RDO][P4] " + e); }
            finally
            {
                if (fixture != null) AssetDatabase.DeleteAsset(fixture);
                if (desired != null && priorCount != null) desired.SetValue(null, priorCount, null);
                if (!string.IsNullOrEmpty(output)) File.WriteAllText(output, JsonUtility.ToJson(result, true));
                EditorApplication.Exit(result.passed ? 0 : 1);
            }
        }
    }
}
