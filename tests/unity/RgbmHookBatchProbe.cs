// 临时复制到 Unity 工程 Assets/Editor 后，通过 -executeMethod 运行。
// 仅创建、重导入并删除专用测试资源，不修改原 HDR 的导入设置。
using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using UnityEditor;
using UnityEngine;

public static class RgbmHookBatchProbe
{
    private const string Folder = "Assets/__Bc7RgbmHookProbe";
    private const string Asset = Folder + "/Probe.hdr";
    private const string Source = "Assets/GameData/GameLevels/MVPSceneTest/LMGroup_MVPSceneTest_64_01_final.hdr";

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandleW(string name);

    private static void LogIdentity(string phase)
    {
        using (var process = System.Diagnostics.Process.GetCurrentProcess())
        {
            var exe = process.MainModule.FileName;
            var version = System.Diagnostics.FileVersionInfo.GetVersionInfo(exe);
            Debug.Log($"[RGBMHOOK-PROBE] phase={phase} pid={process.Id} " +
                $"exe={exe} version={version.ProductVersion} batch={Application.isBatchMode} " +
                $"worker={AssetDatabase.IsAssetImportWorkerProcess()} " +
                $"hook=0x{GetModuleHandleW("bc7_rdo_hook.dll").ToInt64():X}");
        }
        var unity = GetModuleHandleW(null);
        var rvas = new[] { 0x1EDC1A0, 0x1ED6540, 0x1ED9710 };
        foreach (int rva in rvas)
        {
            var bytes = new byte[15];
            Marshal.Copy(IntPtr.Add(unity, rva), bytes, 0, bytes.Length);
            Debug.Log($"[RGBMHOOK-PROBE] phase={phase} rva=0x{rva:X} bytes={BitConverter.ToString(bytes)}");
        }
    }

    public static void Run()
    {
        bool created = false;
        int exitCode = 1;
        try
        {
            if (Directory.Exists(Folder)) throw new InvalidOperationException("Probe folder already exists");
            if (!File.Exists(Source)) throw new FileNotFoundException(Source);
            LogIdentity("before-import");
            if (GetModuleHandleW("bc7_rdo_hook.dll") == IntPtr.Zero)
                throw new InvalidOperationException("Hook DLL is not loaded");
            AssetDatabase.CreateFolder("Assets", "__Bc7RgbmHookProbe");
            created = true;
            File.Copy(Source, Asset);
            AssetDatabase.ImportAsset(Asset, ImportAssetOptions.ForceSynchronousImport);
            var importer = (TextureImporter)AssetImporter.GetAtPath(Asset);
            importer.textureType = TextureImporterType.Lightmap;
            var settings = importer.GetPlatformTextureSettings("Standalone");
            settings.overridden = true;
            settings.format = TextureImporterFormat.BC7;
            settings.maxTextureSize = 64;
            importer.SetPlatformTextureSettings(settings);
            importer.SaveAndReimport();
            var texture = AssetDatabase.LoadAssetAtPath<Texture2D>(Asset);
            var type = typeof(Editor).Assembly.GetType("UnityEditor.TextureUtil");
            const BindingFlags flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static;
            var mode = type.GetMethod("GetUsageMode", flags).Invoke(null, new object[] { texture });
            bool rgbm = (bool)type.GetMethod("IsRGBMUsageMode", flags).Invoke(null, new[] { mode });
            Debug.Log($"[RGBMHOOK-PROBE] result size={texture.width}x{texture.height} " +
                $"format={texture.graphicsFormat} usage={mode} rgbm={rgbm}");
            if (!rgbm || texture.format != TextureFormat.BC7)
                throw new InvalidOperationException("Fixture is not RGBM BC7");
            LogIdentity("after-import");
            Debug.Log("[RGBMHOOK-PROBE] PASS");
            exitCode = 0;
        }
        catch (Exception ex) { Debug.LogException(ex); }
        finally
        {
            if (created) AssetDatabase.DeleteAsset(Folder);
            EditorApplication.Exit(exitCode);
        }
    }
}
