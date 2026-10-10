// Development-only driver; no AssetPostprocessor and no fixed native addresses.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using UnityEditor;
using UnityEngine;

namespace Bc7Rdo
{
    public static class RgbmP5Validation
    {
        [Serializable] public sealed class Sample {
            public string asset, usage, format, sha256, file;
            public int usage_value, width, height, mip_count, bytes, changed_blocks;
            public bool rgbm;
            public double encoded_mse_from_native, linear_rgb_times_m_mse_from_native;
        }
        [Serializable] public sealed class Phase {
            public string name, route, encoder;
            public int ordinary_lambda, rgbm_lambda, main_calls, worker_calls;
            public float ert_lambda, ert_rgbm_lambda;
            public double elapsed_ms;
            public Sample[] samples;
            public string[] native_events;
        }
        [Serializable] public sealed class Result {
            public bool passed, batchmode, fallback_mode, unknown_oodle_native, unknown_ert_native, config_restored, fixtures_removed;
            public int pid, worker_count, fixture_pairs, operational_workers_before_matrix;
            public string unity_version, exe, exe_sha256, hook_sha256, source_hdr_sha256, config_sha256, lightmap_encoding_quality, route, startup_worker_stats, error;
            public uint state;
            public string[] runtime_reports;
            public Phase[] phases;
        }
        private sealed class Setting {
            internal string Name, Encoder; internal int Ordinary, Rgbm; internal float Ert, ErtRgbm;
            internal Setting(string n, string e, int o, int r, float l=4, float rl=0.25f) { Name=n; Encoder=e; Ordinary=o; Rgbm=r; Ert=l; ErtRgbm=rl; }
        }
        private static int Pairs = 6;
        private static string output, config, hookLog, folder;
        private static int revision, self;
        private static readonly List<string> assets = new List<string>();
        private static readonly Dictionary<string,byte[]> native = new Dictionary<string,byte[]>();
        private static readonly Dictionary<string,Color[]> nativePixels = new Dictionary<string,Color[]>();
        private static readonly Dictionary<string,byte[]> oodleReference = new Dictionary<string,byte[]>();
        private static readonly Dictionary<string,byte[]> ertReference = new Dictionary<string,byte[]>();
        private static readonly List<Phase> phases = new List<Phase>();
        private static readonly HashSet<int> workers = new HashSet<int>();
        private static readonly Dictionary<int,string> verifiedWorkers = new Dictionary<int,string>();
        private static readonly BindingFlags Flags=BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Static;
        private static int? startupWorkerCount;
        private static int operationalWorkers;
        private static string startupStats;

        public static void Start() {
            if(Environment.GetEnvironmentVariable("BC7_RGBM_P5_ROUTE")!="worker") { Run(); return; }
            if(!HookLoader.Install() || HookLoader.RgbmState!=4) { Run(); return; }
            startupWorkerCount=AssetDatabase.DesiredWorkerCount;
            AssetDatabase.DesiredWorkerCount=2;
            AssetDatabase.ForceToDesiredWorkerCount();
            var statsMethod=typeof(AssetDatabase).GetMethod("GetWorkerStats",Flags);
            var deadline=DateTime.UtcNow.AddSeconds(60);
            EditorApplication.CallbackFunction poll=null;
            poll=()=> {
                try {
                    Require(statsMethod!=null,"Unity worker readiness query unavailable");
                    object stats=statsMethod.Invoke(null,null);
                    startupStats=JsonUtility.ToJson(stats);
                    operationalWorkers=(int)stats.GetType().GetField("operationalWorkerCount").GetValue(stats);
                    int idle=(int)stats.GetType().GetField("idleWorkerCount").GetValue(stats);
                    int connecting=(int)stats.GetType().GetField("connectingWorkerCount").GetValue(stats);
                    if(operationalWorkers>=2 && idle>=2 && connecting==0) { EditorApplication.update-=poll; Run(); return; }
                    Require(DateTime.UtcNow<deadline,"two Unity Workers did not become operational");
                } catch(Exception error) {
                    EditorApplication.update-=poll;
                    AssetDatabase.DesiredWorkerCount=startupWorkerCount.Value;
                    var failed=new Result { pid=Process.GetCurrentProcess().Id,error=error.ToString(),startup_worker_stats=startupStats,fixtures_removed=true,config_restored=true };
                    File.WriteAllText(Environment.GetEnvironmentVariable("BC7_RGBM_P5_REPORT"),JsonUtility.ToJson(failed,true));
                    EditorApplication.Exit(1);
                }
            };
            // Returning to the Editor loop lets both workers complete their
            // startup handshake; native Ready alone does not mean operational.
            EditorApplication.update+=poll;
        }

        public static void Run() {
            var result = new Result { pid=Process.GetCurrentProcess().Id, unity_version=Application.unityVersion, batchmode=Application.isBatchMode };
            byte[] originalConfig=null; DateTime configTime=default(DateTime);
            MethodInfo qualitySetter=null; object qualityTarget=null, priorQuality=null;
            var priorMode=EditorSettings.refreshImportMode;
            int priorWorkers=startupWorkerCount ?? AssetDatabase.DesiredWorkerCount;
            try {
                output=Environment.GetEnvironmentVariable("BC7_RGBM_P5_REPORT");
                string source=Environment.GetEnvironmentVariable("BC7_RGBM_P5_SOURCE_HDR");
                Require(!string.IsNullOrEmpty(output) && File.Exists(source), "report/source HDR arguments missing");
                Directory.CreateDirectory(Path.GetDirectoryName(output));
                self=result.pid; result.exe=EditorApplication.applicationPath;
                result.exe_sha256=RgbmBindingsLoader.HashFile(result.exe);
                Require(HookLoader.Install() && HookLoader.RgbmState==4, "semantic hooks not Ready");
                result.hook_sha256=RgbmBindingsLoader.HashFile(Path.Combine(RgbmBindingsLoader.PluginDirectory,"bc7_rdo_hook.dll"));
                result.source_hdr_sha256=RgbmBindingsLoader.HashFile(source);
                config=Path.Combine(RgbmBindingsLoader.PluginDirectory,"bc7_rdo_config.txt");
                hookLog=Path.Combine(RgbmBindingsLoader.PluginDirectory,"bc7_rdo_hook.log");
                originalConfig=File.ReadAllBytes(config); configTime=File.GetLastWriteTimeUtc(config);
                result.config_sha256=Hash(originalConfig);
                result.fallback_mode=Environment.GetEnvironmentVariable("BC7_RGBM_P5_MODE")=="fallback";
                string pairs=Environment.GetEnvironmentVariable("BC7_RGBM_P5_PAIRS");
                if(!string.IsNullOrEmpty(pairs)) Pairs=int.Parse(pairs,CultureInfo.InvariantCulture);
                Require(Pairs>=1 && Pairs<=6,"fixture pair count outside acceptance scope");
                result.fixture_pairs=Pairs;
                var getter=typeof(PlayerSettings).GetMethod("GetLightmapEncodingQualityForPlatformGroup",Flags) ??
                    typeof(PlayerSettings).GetMethod("GetLightmapEncodingQualityForPlatform",Flags);
                Require(getter!=null,"Lightmap encoding quality query unavailable");
                qualityTarget=Enum.Parse(getter.GetParameters()[0].ParameterType,
                    getter.GetParameters()[0].ParameterType==typeof(BuildTarget) ? "StandaloneWindows64" : "Standalone");
                var quality=getter.Invoke(null,new[]{qualityTarget});
                if(quality.ToString()!="Normal") {
                    string owned=Path.GetFullPath(Path.Combine(Path.GetDirectoryName(output),"projects"))+Path.DirectorySeparatorChar;
                    Require(Environment.GetEnvironmentVariable("BC7_RGBM_P5_ISOLATED")=="1" &&
                        Path.GetFullPath(RgbmBindingsLoader.ProjectRoot).StartsWith(owned,StringComparison.OrdinalIgnoreCase),
                        "project does not use Normal/RGBM Lightmap encoding; refusing to alter user settings");
                    qualitySetter=typeof(PlayerSettings).GetMethod("SetLightmapEncodingQualityForPlatformGroup",Flags) ??
                        typeof(PlayerSettings).GetMethod("SetLightmapEncodingQualityForPlatform",Flags); priorQuality=quality;
                    qualitySetter.Invoke(null,new[]{qualityTarget,Enum.Parse(quality.GetType(),"Normal")});
                    quality=getter.Invoke(null,new[]{qualityTarget});
                }
                Require(quality.ToString()=="Normal","isolated RGBM quality setup failed");
                result.lightmap_encoding_quality=quality.ToString();
                result.route=Environment.GetEnvironmentVariable("BC7_RGBM_P5_ROUTE");
                result.operational_workers_before_matrix=operationalWorkers;
                result.startup_worker_stats=startupStats;
                Require(result.route=="main" || result.route=="worker","invalid import route");
                EditorSettings.refreshImportMode=result.route=="main" ? AssetDatabase.RefreshImportMode.InProcess : AssetDatabase.RefreshImportMode.OutOfProcessPerQueue;
                WriteConfig(new Setting("setup","oodle",0,0));
                if(result.route=="worker") {
                    // Main Editor is Ready before these actual workers are born.
                    AssetDatabase.DesiredWorkerCount=2;
                    AssetDatabase.ForceToDesiredWorkerCount();
                    WaitWorkers();
                }
                Prepare(source);
                RunRoute(result.route, result.fallback_mode);
                Require(result.route=="main" || workers.Count>=Math.Min(2,Pairs), "too few actual Workers encoded BC7");
                result.worker_count=workers.Count;
                VerifyUnknown(result);
                result.state=HookLoader.RgbmState;
                Require(result.state==4, "runtime degraded during encoding matrix");
                var reports=new List<string>();
                foreach(var path in Directory.GetFiles(RgbmBindingsLoader.CacheDirectory,"*.runtime.json")) {
                    var report=JsonUtility.FromJson<RgbmBindingsLoader.RuntimeReport>(ReadShared(path));
                    if(report!=null && (report.pid==self || workers.Contains((int)report.pid))) reports.Add(ReadShared(path));
                }
                result.runtime_reports=reports.ToArray();
                result.passed=true;
            } catch(Exception e) { result.error=e.ToString(); UnityEngine.Debug.LogError("[BC7RDO][P5] "+e); }
            finally {
                try {
                    if(folder!=null) Require(AssetDatabase.DeleteAsset(folder), "fixture cleanup failed");
                    result.fixtures_removed=folder==null || !Directory.Exists(Path.Combine(RgbmBindingsLoader.ProjectRoot,folder));
                    EditorSettings.refreshImportMode=priorMode; AssetDatabase.DesiredWorkerCount=priorWorkers;
                    if(qualitySetter!=null) qualitySetter.Invoke(null,new[]{qualityTarget,priorQuality});
                    if(originalConfig!=null) { File.WriteAllBytes(config,originalConfig); File.SetLastWriteTimeUtc(config,configTime); result.config_restored=Hash(File.ReadAllBytes(config))==Hash(originalConfig); }
                    Require(result.fixtures_removed && (originalConfig==null || result.config_restored), "cleanup/config restore failed");
                } catch(Exception e) { result.passed=false; result.error+="\ncleanup: "+e; }
                result.phases=phases.ToArray();
                if(!string.IsNullOrEmpty(output)) File.WriteAllText(output,JsonUtility.ToJson(result,true),new UTF8Encoding(false));
                EditorApplication.Exit(result.passed ? 0 : 1);
            }
        }
        private static void Require(bool condition,string why) { if(!condition) throw new InvalidOperationException(why); }
        private static string Hash(byte[] bytes) { using(var h=SHA256.Create()) return BitConverter.ToString(h.ComputeHash(bytes)).Replace("-",""); }
        private static string ReadShared(string path) { using(var f=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.ReadWrite)) using(var r=new StreamReader(f)) return r.ReadToEnd(); }
        private static long LogLength() { return File.Exists(hookLog) ? new FileInfo(hookLog).Length : 0; }
        private static string ReadLog(long offset) { using(var f=new FileStream(hookLog,FileMode.Open,FileAccess.Read,FileShare.ReadWrite)) { f.Position=offset; using(var r=new StreamReader(f)) return r.ReadToEnd(); } }
        private static void WriteConfig(Setting setting,bool enabled=true) {
            string text="encoder="+setting.Encoder+"\noodle_lambda="+setting.Ordinary+"\nrgbm_oodle_lambda="+setting.Rgbm+
                "\nlambda="+setting.Ert.ToString(CultureInfo.InvariantCulture)+"\nrgbm_lambda="+setting.ErtRgbm.ToString(CultureInfo.InvariantCulture)+
                "\nunity_rgbm_context_hooks="+(enabled ? "1" : "0")+"\nrgbm_rdo=1\noodle_effort=high\ndebug=1\ntrace_bc7_callstacks=0"+
                "\nlookback=128\nsmooth_scale=15\nmax_rms_increase_ratio=10\nrgbm_smooth_scale=30\nrgbm_max_rms_increase_ratio=1.5\ntwo_matches=1\n";
            File.WriteAllText(config,text,new UTF8Encoding(false)); File.SetLastWriteTimeUtc(config,DateTime.UtcNow.AddMilliseconds(++revision));
        }
        private static void Prepare(string source) {
            string name="Bc7RdoP5_"+Guid.NewGuid().ToString("N"); folder="Assets/"+name;
            AssetDatabase.CreateFolder("Assets",name);
            var texture=new Texture2D(64,64,TextureFormat.RGBA32,false);
            try {
                var pixels=new Color32[4096];
                for(int y=0;y<64;++y) for(int x=0;x<64;++x) pixels[y*64+x]=new Color32(
                    (byte)(128+90*Math.Sin(x/13.0)+25*Math.Sin(y/9.0)),
                    (byte)(128+80*Math.Sin((x+y)/17.0)),(byte)(128+110*Math.Cos((x*3+y*5)/23.0)),255);
                texture.SetPixels32(pixels); texture.Apply(); var png=texture.EncodeToPNG();
                for(int i=0;i<Pairs;++i) {
                    string ordinary=folder+"/ordinary"+i+".png", rgbm=folder+"/rgbm"+i+".hdr";
                    File.WriteAllBytes(Path.Combine(RgbmBindingsLoader.ProjectRoot,ordinary),png);
                    File.Copy(source,Path.Combine(RgbmBindingsLoader.ProjectRoot,rgbm)); assets.Add(ordinary); assets.Add(rgbm);
                }
            } finally { UnityEngine.Object.DestroyImmediate(texture); }
            AssetDatabase.Refresh(ImportAssetOptions.ForceSynchronousImport);
            foreach(var asset in assets) {
                var importer=(TextureImporter)AssetImporter.GetAtPath(asset);
                importer.textureType=asset.EndsWith(".hdr") ? TextureImporterType.Lightmap : TextureImporterType.Default;
                importer.isReadable=true; importer.mipmapEnabled=true; importer.textureCompression=TextureImporterCompression.Compressed;
                importer.SetPlatformTextureSettings(new TextureImporterPlatformSettings { name="Standalone",overridden=true,format=TextureImporterFormat.BC7,maxTextureSize=64,compressionQuality=100 });
                importer.SaveAndReimport();
            }
        }
        private static void WaitWorkers() {
            var deadline=DateTime.UtcNow.AddSeconds(30);
            while(DateTime.UtcNow<deadline) {
                int ready=0;
                foreach(var path in Directory.GetFiles(RgbmBindingsLoader.CacheDirectory,"*.runtime.json")) {
                    var r=JsonUtility.FromJson<RgbmBindingsLoader.RuntimeReport>(ReadShared(path));
                    if(r==null || !r.asset_import_worker || r.state!="Ready") continue;
                    try { using(var p=Process.GetProcessById((int)r.pid)) if(p.StartTime.ToUniversalTime().ToFileTimeUtc().ToString()==r.creation_filetime) ++ready; }
                    catch(ArgumentException) { }
                }
                if(ready>=2) return;
                System.Threading.Thread.Sleep(100);
            }
            throw new TimeoutException("actual Worker bootstrap timed out");
        }
        private static void RunRoute(string route,bool fallback) {
            var cases=new List<Setting> { new Setting("native","oodle",0,0) };
            if(!fallback) {
                cases.Add(new Setting("oodle-split","oodle",30,10));
                cases.Add(new Setting("oodle-rgbm-zero","oodle",30,0));
                cases.Add(new Setting("oodle-ordinary-zero","oodle",0,10));
                cases.Add(new Setting("oodle-explicit-inherit-control","oodle",30,30));
                cases.Add(new Setting("oodle-inherit","oodle",30,-1));
            }
            cases.Add(new Setting("ert-global-zero","bc7enc_rdo",30,10,0,0.25f));
            cases.Add(new Setting("ert-split","bc7enc_rdo",30,10));
            cases.Add(new Setting("ert-rgbm-zero","bc7enc_rdo",30,10,4,0));
            if(fallback) cases.Add(new Setting("oodle-unavailable","oodle",30,10));
            foreach(var setting in cases) RunPhase(route,setting);
        }
        private static void RunPhase(string route,Setting setting) {
            var timer=Stopwatch.StartNew();
            WriteConfig(setting); long offset=LogLength();
            foreach(var asset in assets) {
                var importer=(TextureImporter)AssetImporter.GetAtPath(asset);
                // Owned metadata invalidates old artifacts for each config, without a global postprocessor.
                importer.userData="P5:"+route+":"+setting.Name;
                AssetDatabase.WriteImportSettingsIfDirty(asset);
            }
            if(route=="main") {
                foreach(var asset in assets) AssetDatabase.ImportAsset(asset,ImportAssetOptions.ForceUpdate|ImportAssetOptions.ForceSynchronousImport);
            } else {
                // Startup selects OutOfProcess for this dedicated run. Submit
                // the complete batch before waiting, so two ready workers can
                // claim imports instead of serial load requests using one worker.
                AssetDatabase.StartAssetEditing();
                try { foreach(var asset in assets) AssetDatabase.ImportAsset(asset,ImportAssetOptions.ForceUpdate); }
                finally { AssetDatabase.StopAssetEditing(); }
                AssetDatabase.Refresh(ImportAssetOptions.ForceUpdate);
            }
            var phase=new Phase { name=setting.Name,route=route,encoder=setting.Encoder,ordinary_lambda=setting.Ordinary,rgbm_lambda=setting.Rgbm,ert_lambda=setting.Ert,ert_rgbm_lambda=setting.ErtRgbm };
            var samples=new List<Sample>();
            string dir=Path.Combine(Path.GetDirectoryName(output),Path.GetFileNameWithoutExtension(output)+"-blocks",route+"-"+setting.Name); Directory.CreateDirectory(dir);
            foreach(var asset in assets) {
                var t=AssetDatabase.LoadAssetAtPath<Texture2D>(asset); Require(t!=null && t.format==TextureFormat.BC7,"not BC7: "+asset);
                var type=typeof(Editor).Assembly.GetType("UnityEditor.TextureUtil");
                var usage=type.GetMethod("GetUsageMode",Flags).Invoke(null,new object[]{t});
                bool rgbm=(bool)type.GetMethod("IsRGBMUsageMode",Flags).Invoke(null,new[]{usage});
                Require(rgbm==asset.EndsWith(".hdr"),"usage mismatch: "+asset+" "+usage);
                var bytes=t.GetRawTextureData(); var colors=t.GetPixels();
                if(setting.Name=="native") { native.Add(asset,bytes); nativePixels.Add(asset,colors); }
                bool mustBeNative=setting.Name=="native" || setting.Name=="ert-global-zero" ||
                    setting.Name=="oodle-rgbm-zero" && rgbm || setting.Name=="oodle-ordinary-zero" && !rgbm || setting.Name=="ert-rgbm-zero" && rgbm;
                if(mustBeNative) Require(Hash(bytes)==Hash(native[asset]),"zero-lambda/native output mismatch: "+route+" "+setting.Name+" "+asset);
                if(setting.Name=="oodle-split") oodleReference[route+asset]=bytes;
                if(setting.Name=="oodle-rgbm-zero" && !rgbm || setting.Name=="oodle-ordinary-zero" && rgbm)
                    Require(Hash(bytes)==Hash(oodleReference[route+asset]),"other lane changed when zeroing lambda");
                if(setting.Name=="oodle-explicit-inherit-control") oodleReference[route+"inherit"+asset]=bytes;
                if(setting.Name=="oodle-inherit") Require(Hash(bytes)==Hash(oodleReference[route+"inherit"+asset]),"RGBM -1 inheritance differs from explicit lambda");
                if(setting.Name=="ert-split") ertReference[route+asset]=bytes;
                if(setting.Name=="ert-rgbm-zero" && !rgbm) Require(Hash(bytes)==Hash(ertReference[route+asset]),"ordinary ERT changed when zeroing RGBM lambda");
                if(setting.Name=="oodle-unavailable") Require(Hash(bytes)==Hash(ertReference[route+asset]),"missing Oodle did not use ERT fallback");
                int changed=0; for(int i=0;i<bytes.Length;i+=16) for(int j=i;j<Math.Min(i+16,bytes.Length);++j) if(bytes[j]!=native[asset][j]) { ++changed; break; }
                if(setting.Name=="oodle-split" || setting.Name=="ert-split" && !rgbm) Require(changed>0,"nonzero RDO had no observable effect: "+asset);
                string file=Path.Combine(dir,Path.GetFileNameWithoutExtension(asset)+".bc7"); File.WriteAllBytes(file,bytes);
                double encoded=0,decoded=0; var baseline=nativePixels[asset]; Require(colors.Length==baseline.Length,"decoded size changed");
                for(int i=0;i<colors.Length;++i) {
                    for(int c=0;c<4;++c) { double d=colors[i][c]-baseline[i][c]; encoded+=d*d; }
                    for(int c=0;c<3;++c) { double d=Mathf.GammaToLinearSpace(colors[i][c])*colors[i].a-Mathf.GammaToLinearSpace(baseline[i][c])*baseline[i].a; decoded+=d*d; }
                }
                samples.Add(new Sample {asset=asset,usage=usage.ToString(),usage_value=Convert.ToInt32(usage),rgbm=rgbm,format=t.graphicsFormat.ToString(),width=t.width,height=t.height,mip_count=t.mipmapCount,bytes=bytes.Length,sha256=Hash(bytes),file=file,changed_blocks=changed,encoded_mse_from_native=encoded/(colors.Length*4),linear_rgb_times_m_mse_from_native=decoded/(colors.Length*3)});
            }
            phase.samples=samples.ToArray();
            var events=new List<string>(); var classes=new HashSet<int>();
            foreach(string line in ReadLog(offset).Split('\n')) {
                if(!line.Contains("RDO called (") && !line.Contains("RDO bypassed (")) continue;
                var id=Regex.Match(line,@"^pid=(\d+) tid=(\d+) "); Require(id.Success,"native log lacks process identity");
                int pid=int.Parse(id.Groups[1].Value); Require(route=="main" ? pid==self : pid!=self,"encoding occurred in wrong process: "+line);
                if(pid==self) ++phase.main_calls; else { ++phase.worker_calls; VerifyWorkerIdentity(pid); workers.Add(pid); }
                int classification=line.Contains("context_rgbm=") ? int.Parse(Regex.Match(line,@"context_rgbm=(\d+)").Groups[1].Value) : int.Parse(Regex.Match(line,@"rgbm=(\d+)").Groups[1].Value);
                classes.Add(classification); int expected=classification==1 ? (setting.Rgbm<0 ? setting.Ordinary : setting.Rgbm) : setting.Ordinary;
                if(setting.Encoder=="oodle" && setting.Name!="oodle-unavailable") {
                    Require(expected==0 ? line.Contains("RDO bypassed (oodle lambda=0)") : line.Contains("RDO called (oodle)") && line.Contains("lambda="+expected+" "),"wrong Oodle route/parameter: "+line);
                } else {
                    float lambda=classification==1 ? setting.ErtRgbm : setting.Ert;
                    Require(line.Contains("RDO called (bc7enc_rdo)") && line.Contains("rgbm="+classification+" lambda=") && line.Contains("lambda="+lambda.ToString("F6",CultureInfo.InvariantCulture)+" ") &&
                        line.Contains("rdo_enabled="+(setting.Ert>0 && lambda>0 ? "1" : "0")) &&
                        line.Contains("smooth_scale="+(classification==1 ? "30.000000" : "15.000000")) &&
                        line.Contains("max_rms_ratio="+(classification==1 ? "1.500000" : "10.000000")),"wrong ERT metric/parameter: "+line);
                }
                events.Add(line.TrimEnd('\r'));
            }
            Require(classes.Contains(0) && classes.Contains(1),"missing ordinary/RGBM native encoding events: "+route+" "+setting.Name);
            phase.native_events=events.ToArray(); phase.elapsed_ms=timer.Elapsed.TotalMilliseconds; phases.Add(phase);
        }
        private static void VerifyWorkerIdentity(int pid) {
            using(var p=Process.GetProcessById(pid)) {
                string birth=p.StartTime.ToUniversalTime().ToFileTimeUtc().ToString();
                string known; if(verifiedWorkers.TryGetValue(pid,out known) && known==birth) return;
                string path=Path.Combine(RgbmBindingsLoader.CacheDirectory,pid+"."+birth+".runtime.json");
                var r=JsonUtility.FromJson<RgbmBindingsLoader.RuntimeReport>(ReadShared(path));
                Require(r!=null && r.pid==pid && r.creation_filetime==birth && r.asset_import_worker && r.state=="Ready" && r.hook_installed,"Worker native identity/state mismatch");
                Require(r.exe_sha256==RgbmBindingsLoader.HashFile(p.MainModule.FileName),"Worker exe identity mismatch");
                verifiedWorkers[pid]=birth;
            }
        }
        private static void VerifyUnknown(Result result) {
            // Reuse the loader's checked 124-byte ABI, calling the BC7 export directly
            // outside CompressSingleImageTexture / Process context after RGBM imports.
            var type=typeof(HookLoader); var module=(IntPtr)type.GetMethod("LoadLibraryW",Flags).Invoke(null,new object[]{Path.Combine(Path.GetDirectoryName(EditorApplication.applicationPath),"compress_bc7e.dll")});
            object parameters=type.GetMethod("NewParams",Flags).Invoke(null,null);
            Func<string,IntPtr> proc=name=>(IntPtr)type.GetMethod("GetProcAddress",Flags).Invoke(null,new object[]{module,name});
            var init=Marshal.GetDelegateForFunctionPointer(proc("bc7elib_compress_block_params_init_basic"),type.GetNestedType("ParamsInitBasicDelegate",BindingFlags.NonPublic));
            object[] initArgs={parameters,(byte)1}; init.DynamicInvoke(initArgs); parameters=initArgs[0];
            var compress=Marshal.GetDelegateForFunctionPointer(proc("bc7elib_compress_blocks"),type.GetNestedType("CompressBlocksDelegate",BindingFlags.NonPublic));
            var pixels=new byte[1024]; for(int i=0;i<pixels.Length;++i) pixels[i]=(byte)(i*37); var blocks=new byte[256];
            var px=GCHandle.Alloc(pixels,GCHandleType.Pinned); var dst=GCHandle.Alloc(blocks,GCHandleType.Pinned);
            try {
                Action call=()=>compress.DynamicInvoke(new object[]{(uint)16,dst.AddrOfPinnedObject(),px.AddrOfPinnedObject(),parameters});
                WriteConfig(new Setting("direct-native","oodle",0,0,0),false); call(); string baseline=Hash(blocks);
                WriteConfig(new Setting("direct-unknown-oodle","oodle",30,10)); call(); result.unknown_oodle_native=Hash(blocks)==baseline;
                WriteConfig(new Setting("direct-unknown-ert","bc7enc_rdo",30,10)); call(); result.unknown_ert_native=Hash(blocks)==baseline;
                Require(result.unknown_oodle_native && result.unknown_ert_native,"Unknown leaked old context or applied RDO");
            } finally { px.Free(); dst.Free(); }
        }
    }
}
