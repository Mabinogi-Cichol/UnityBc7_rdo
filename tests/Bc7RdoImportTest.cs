// Bc7RdoImportTest.cs
// 自动化验证：Unity 真实导入管线（TextureImporter + BC7 reimport）下 hook 是否生效。
// 由 tests/test_editor_plugin.py 复制进临时工程（Assets/Editor/）后执行：
//   -executeMethod Bc7RdoImportTest.Run
//
// 流程：
//   1. 生成 32 张确定性渐变纹理，全部设为 BC7（isReadable=true，可读压缩块）
//   2. 配置 lambda=0 -> 同步强制重导 -> 统计全部纹理 BC7 块 Deflate 体积 z0（基线）
//   3. 配置 lambda=4 -> 异步重导（ForceUpdate 不带同步标志；并行导入开启时
//      压缩会分派给 AssetImportWorker 进程）-> z4async
//   4. 配置 lambda=4 -> 同步强制重导（主进程内执行）-> z4sync
//   5. 判定：PASS = z4sync < z0（导入管线内 RDO 生效）；z4async 用于诊断
//      worker 进程路径（若 z4async 明显大于 z4sync，说明 worker 进程未被 hook）。

using System;
using System.IO;
using System.IO.Compression;
using UnityEditor;
using UnityEngine;

public static class Bc7RdoImportTest
{
    private const int Count = 32;

    public static void Run()
    {
        try
        {
            string cfg = FindPluginConfig();
            if (cfg == null)
            {
                Fail("未找到 bc7_rdo_config.txt（插件未部署）");
                return;
            }

            // 1. 生成 32 张确定性渐变纹理
            for (int i = 0; i < Count; i++)
            {
                Texture2D t = new Texture2D(64, 64, TextureFormat.RGBA32, false);
                for (int y = 0; y < 64; y++)
                {
                    for (int x = 0; x < 64; x++)
                    {
                        t.SetPixel(x, y, new Color(
                            (float)(0.5 + 0.4 * Math.Sin((x + i * 7) / 13.0) + 0.1 * Math.Sin(y / 9.0)),
                            (float)(0.5 + 0.3 * Math.Sin((x + y + i * 3) / 17.0)),
                            (float)(0.5 + 0.4 * Math.Cos((x * 3 + y * 5 + i) / 23.0)),
                            1f));
                    }
                }
                File.WriteAllBytes(Path.Combine(Application.dataPath, "imp_" + i + ".png"),
                                   t.EncodeToPNG());
                UnityEngine.Object.DestroyImmediate(t);
            }
            AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);

            // 2. 全部设为 BC7
            for (int i = 0; i < Count; i++)
            {
                string p = "Assets/imp_" + i + ".png";
                TextureImporter imp = (TextureImporter)AssetImporter.GetAtPath(p);
                imp.textureCompression = TextureImporterCompression.Compressed;
                imp.isReadable = true;
                imp.SetPlatformTextureSettings(new TextureImporterPlatformSettings
                {
                    name = "Standalone",
                    overridden = true,
                    format = TextureImporterFormat.BC7,
                    compressionQuality = 100,
                });
                imp.SaveAndReimport();
            }

            // 3. lambda=0 基线（同步强制重导）
            WriteConfig(cfg, 0f);
            ReimportAll(sync: true);
            int z0 = TotalZ();
            Debug.Log("[BC7RDO][IMPORT] lambda=0  sync  total=" + z0);

            // 4. lambda=4 异步重导（并行导入开启时走 AssetImportWorker 进程）
            WriteConfig(cfg, 4f);
            ReimportAll(sync: false);
            int z4a = TotalZ();
            Debug.Log("[BC7RDO][IMPORT] lambda=4  async total=" + z4a);

            // 5. lambda=4 同步强制重导（主进程内）
            ReimportAll(sync: true);
            int z4s = TotalZ();
            Debug.Log("[BC7RDO][IMPORT] lambda=4  sync  total=" + z4s);

            // 6. 判定
            bool pass = z4s < z0;
            double savedSync = (z0 - z4s) * 100.0 / z0;
            double savedAsync = (z0 - z4a) * 100.0 / z0;
            Debug.Log(string.Format(
                "[BC7RDO][IMPORT] {0} z0={1} z4async={2} z4sync={3} " +
                "同步节省={4:0.0}% 异步节省={5:0.0}%",
                pass ? "PASS" : "FAIL", z0, z4a, z4s, savedSync, savedAsync));
            Exit(pass ? 0 : 1);
        }
        catch (Exception e)
        {
            Debug.LogException(e);
            Exit(1);
        }
    }

    // -----------------------------------------------------------------------
    // 工具
    // -----------------------------------------------------------------------

    private static void ReimportAll(bool sync)
    {
        for (int i = 0; i < Count; i++)
        {
            string p = "Assets/imp_" + i + ".png";
            ImportAssetOptions opts = ImportAssetOptions.ForceUpdate;
            if (sync)
                opts |= ImportAssetOptions.ForceSynchronousImport;
            AssetDatabase.ImportAsset(p, opts);
        }
    }

    private static int TotalZ()
    {
        int total = 0;
        for (int i = 0; i < Count; i++)
        {
            Texture2D tex = AssetDatabase.LoadAssetAtPath<Texture2D>("Assets/imp_" + i + ".png");
            byte[] raw = tex.GetRawTextureData();
            total += DeflateLen(raw);
        }
        return total;
    }

    private static int DeflateLen(byte[] data)
    {
        using (MemoryStream ms = new MemoryStream())
        {
            // leaveOpen: true —— Mono 的 DeflateStream 二参构造默认会在 Dispose
            // 时关闭底层流（leaveOpen=false），关闭后访问 ms.Length 会抛
            // ObjectDisposedException（MemoryStream.ToArray 关闭后仍可用，
            // 但 Length 会抛）。
            using (DeflateStream ds = new DeflateStream(
                       ms, System.IO.Compression.CompressionLevel.Optimal, leaveOpen: true))
            {
                ds.Write(data, 0, data.Length);
            }
            return (int)ms.Length;
        }
    }

    private static string FindPluginConfig()
    {
        string[] hits = Directory.GetFiles(Application.dataPath, "bc7_rdo_config.txt",
                                           SearchOption.AllDirectories);
        return hits.Length > 0 ? hits[0] : null;
    }

    private static void WriteConfig(string path, float lambda)
    {
        File.WriteAllText(path,
            "lambda=" + lambda.ToString(System.Globalization.CultureInfo.InvariantCulture) +
            "\nlookback=128\nsmooth_scale=15\nmax_rms_increase_ratio=10\n" +
            "two_matches=1\ndebug=0\n");
    }

    private static void Fail(string reason)
    {
        Debug.LogError("[BC7RDO][IMPORT] FAIL " + reason);
        Exit(1);
    }

    private static void Exit(int code)
    {
        if (Application.isBatchMode)
            EditorApplication.Exit(code);
    }
}
