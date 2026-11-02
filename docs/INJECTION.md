# 外挂形式（Editor Plugin / Hook DLL）实现说明与方案评估

## v0.5.0 当前 RGBM 路径

下面的导出桩方案评估及单函数流程描述的是基础 BC7 RDO。v0.5.0 的 RGBM 分类另有两个 Unity 私有语义入口，使用独立协调器／gate 的 Detours 事务，不能用固定长度导出桩 patch 代替。当前部署须完整保留：

```text
Bc7Rdo/
  hook_loader.cs                  单一 C# 加载器，含绑定准备与 Worker 协调
  Bc7Rdo.Editor.asmdef
  bc7_rdo_hook.dll
  bc7_rgbm_coordinator.dll
  bc7_rgbm_loader_gate.dll
  bc7_rdo_config.txt
  RgbmBindings~/                  resolver、认证器、私有 release DIA、来源与许可
  Licenses~/                      bc7enc_rdo 和 Detours 许可（发行包）
```

1. 设置 `unity_rgbm_context_hooks=1` 并重新启动目标工程，loader 在加载主 DLL 前准备按实际 exe SHA256 区分的 Library 绑定缓存。匹配 PDB 优先，缺少 PDB 才尝试受限扫描；候选必须完整通过身份、ABI 与语义认证。
2. 主进程在 `DllMain` 外独立复核当前映像及绑定，协调器通过 gate 安装两个语义 Hook；BC7 导出 Hook 仍独立等待 `compress_bc7e.dll`。
3. Worker 监控核对直接父子关系、工程路径、Worker 命令行、exe 及 PID／创建时间，先准备该 exe 的缓存，再完成注入并检查原生引导报告。只有 `Ready` 和 `hook_installed=true` 才算成功；x64 远端线程 DWORD 返回值不作模块句柄。
4. 并发 Job 经作用域槽和回调 TLS 关联 usage，不读取固定 JobData 偏移。Unknown／Unavailable 的启用分类直接走原生 BC7，严重事务恢复错误显式 Faulted 并非零退出。

已验收 Windows x64 Unity 2022.3.62f1／f2 的 Editor、batchmode、真实 Worker；其他构建不保证适配。配置默认仍关闭 RGBM 语义 Hook。升级、缓存、日志、质量／性能边界见[插件说明](../src/editor_plugin/README.md)及 [P5 实测](features/rgbm-hook-portability-acceptance.md)。无 VS 环境未测试，额外 Unity 内先 Ready 后首次 BC7 加载场景未实测。关闭 Editor／Worker 后才能替换或删除 DLL，不支持原生热卸载。

本文档回答两个问题：

1. 在不修改 Unity 安装目录的前提下实现相同 BC7 RDO 效果，可选方案
   **DLL 劫持**与 **Shellcode Hijacking 注入**的难度与可行性评估，以及
   最终落地选择；
2. 落地实现的架构、构建、使用与测试方法。

## 1. 目标回顾

现有壳 DLL 方案（`compress_bc7e_rdo.cpp` + `enable_unity_rdo.ps1`）通过
**替换 Unity 安装目录中的 `compress_bc7e.dll`** 实现 BC7 RDO。新需求：
不改安装目录任何文件，以"外挂"形式（Editor Plugin + 运行时挂钩）达到
相同效果。

关键事实（来自 `docs/ANALYSIS.md`）：

- Unity 通过**绝对路径**加载 `C:\Program Files\Unity\...\Editor\compress_bc7e.dll`；
- 该 DLL 导出 10 个 `bc7elib_*` 函数，每个导出是 16 字节跳转桩
  （`E9 rel32` 尾跳，5 字节指令 + 填充）；
- Unity 通过 `GetProcAddress`（或 IAT）拿到导出桩地址后调用——所有调用
  必然经过导出桩地址，patch 该地址即全量拦截。

## 2. 候选方案评估

| 维度 | 方案 A：DLL 劫持（搜索顺序/代理 DLL） | 方案 B：Shellcode Hijacking 注入 | 方案 C（落地）：Hook DLL + 进程内加载/worker 注入（Editor Plugin） |
| --- | --- | --- | --- |
| 原理 | 在搜索顺序更靠前的目录放同名 DLL，让进程误加载 | 远程进程内写 shellcode（手工解析 PE/PEB，挂钩导出） | 编辑器脚本进程内 `LoadLibraryW` 加载 hook DLL（worker 由插件 `CreateRemoteThread + LoadLibraryW` 注入），DLL 内对导出桩做 inline detour |
| 实现难度 | ★☆☆☆☆（放个 DLL 即可） | ★★★★★（手写 x64 shellcode、反射加载/PEB 遍历、模块时机协调、调试地狱） | ★★★☆☆（常规 Win32 API，可单步调试） |
| 对本目标可行性 | **不可行**：`compress_bc7e.dll` 是绝对路径加载，不走搜索顺序；唯一可劫持点（Editor 目录）正是"安装目录"，违反约束。除非找到 Unity 以"仅名称"加载且搜索路径含可控目录的其它 DLL——脆弱、随版本漂移、无保证 | **可行**：不依赖加载路径，任何加载方式都能拦截 | **可行**：不依赖加载路径 |
| 安装目录写入 | 无（但可劫持点本身在安装目录内） | 无 | 无（原 DLL 保持只读） |
| 稳定性/可维护性 | 依赖运气与版本 | 高（但开发/调试成本极高） | 高；装钩失败有日志、可回退 |
| 自动化验证 | - | 极难 | 已实现（`tests/test_hook.py`、`tests/test_editor_plugin.py`） |
| 适用场景 | 游戏类"放根目录同名 DLL" | 免杀/对抗场景（本项目无此需求） | 本地开发/贴图压缩研究（本场景） |

### 2.1 为什么不选纯 DLL 劫持（方案 A）

`compress_bc7e.dll` 由 Unity 以**完整绝对路径**加载，Windows 加载器不
参与搜索顺序，因此把同名 DLL 放到任何"外挂目录"都不会被 Unity 加载。
唯一能实现"同名 DLL 顶替"的位置就是安装目录本身——与现有替换方案等价，
不满足需求。结论：**对 Unity Editor 目标，DLL 劫持不可行**。

### 2.2 为什么不选纯 Shellcode（方案 B）

可行性没问题，但手写 x64 shellcode（或反射式 DLL 加载器）需要自行完成：
PE 解析与重定位、导入表修复、PEB/导出表遍历、线程劫持与时机协调等，
工程量与调试成本是方案 C 的数倍，而本项目是本地 mod 工具，没有任何
"免杀/隐身"需求，纯 shellcode 的收益为零。

### 2.3 落地选择：方案 C（Hook DLL（载荷）+ Editor Plugin 加载方式）

它是方案 B 的**实用简化版**：主编辑器进程内由 C# 加载器
`hook_loader.cs` 在编辑器域加载时 `LoadLibraryW` 加载 hook DLL；并行导入
的 AssetImportWorker 子进程由插件用 `CreateRemoteThread + LoadLibraryW`
注入。hook DLL 内用对导出桩的 5 字节 `E9` inline detour 实现挂钩。
三个关键洞察使本方案特别简洁：

1. **只需挂钩 1 个函数**：`bc7elib_compress_blocks`。其余 9 个 `params_init_*`
   无需改动（原样通过导出桩调用即可），行为与壳 DLL 完全一致；
2. **无需加载原 DLL 副本**：已加载的模块就是原 DLL，其真实实现地址直接
   从导出桩的 `E9 rel32` 相对目标解析（`real = stub + 5 + rel32`）；
3. **原 DLL 全程只读**：不写安装目录任何字节。

## 3. 架构

Editor Plugin 布局（放进工程 Assets 下即生效）：

```
<工程>/Assets/<任意位置>/Bc7Rdo/       （位置随意，必须在 Assets 下）
  hook_loader.cs      C# 加载器（[InitializeOnLoad] 自动执行）
  Bc7Rdo.Editor.asmdef  Editor-only 程序集定义（必须保留，见 3.5）
  bc7_rdo_hook.dll    原生 hook DLL（载荷）
  bc7_rdo_config.txt  RDO 配置（同目录热更新）
```

加载流程（主编辑器进程）：

```
Unity Editor 打开工程
  ├─ 编辑器域加载 → hook_loader.cs [InitializeOnLoad] 静态构造自动执行
  ├─ LoadLibraryW(bc7_rdo_hook.dll) → 进入主编辑器进程
  └─ hook DLL DllMain
        ├─ compress_bc7e.dll 已加载？→ 立即装钩；否则 50ms 轮询等待
        ├─ 读导出桩 5 字节（必须 0xE9），解析 real = stub + 5 + rel32
        ├─ 模块 base ±2GB 内扫描空闲页做跳板（写 48 B8 <imm64> FF E0）
        ├─ VirtualProtect → 写 5 字节 E9 rel32（到跳板）→ 恢复保护
        └─ 之后 Unity 每次调 bc7elib_compress_blocks → 我们的 hook：
           调 real（原实现）→ rdo::apply_rdo（读插件目录配置，热更新）→ 返回
```

worker 注入流程（并行导入子进程）：

```
主编辑器进程内 hook_loader.cs 后台监控线程（每 500ms）
  ├─ 枚举当前进程的子进程，发现 Unity.exe / AssetImportWorker*
  ├─ CreateRemoteThread + LoadLibraryW 注入 bc7_rdo_hook.dll（幂等，失败重试）
  └─ worker 内 hook DLL 自身 50ms 轮询等 compress_bc7e.dll 出现后装钩
```

设计要点：

- **g_real 先发布、后打桩**：桩未 patch 前调用全走原实现，发布无副作用；
  避免"打桩后调用命中 hook 但 real 未就绪"的竞态；
- **装钩只执行一次**：`std::call_once` 保护；`InterlockedExchangePointer`
  原子发布；
- **跳板优先 5 字节 patch**（与 MinHook 同思路），扫描失败回退为直接在
  导出桩写 12 字节绝对跳转（16 字节桩空间足够）；
- **日志**：插件目录下 `Log~\bc7_rdo_hook.log`（装钩成功/失败、模块地址等；
  `Log~` 以 `~` 结尾，Unity 会忽略整个目录，日志不进资源流程、不生成
  `.meta`），配置 `debug=1` 时记录每次 RDO 调用；
- 与原 DLL 同名的配置读取逻辑、ERT 参数完全复用壳 DLL（`src/rdo_common.h`）。

## 3.5 Editor Plugin 形式（推荐：放进工程即生效，免注入器）

既然目标工程本身是 Unity 工程，最简形式是**把 C# 插件放进工程**：

```
<工程>/Assets/Editor/Bc7Rdo/          （目录名可改，位置随意，必须在 Assets 下）
  hook_loader.cs      编辑器脚本（[InitializeOnLoad] 自动加载 hook DLL）
  Bc7Rdo.Editor.asmdef  Editor-only 程序集定义（必须保留，见下）
  bc7_rdo_hook.dll    原生 hook DLL（与注入版完全同一个）
  bc7_rdo_config.txt  RDO 配置（同目录热更新）
```

原理：编辑器脚本跑在 Unity Editor 进程内，`[InitializeOnLoad]` 静态构造在
编辑器域加载时自动执行 `LoadLibraryW` 加载 hook DLL——**不需要注入器、
不需要 CreateRemoteThread**。hook DLL 的 DllMain 自带 50ms 轮询，等 Unity
首次导入 BC7 纹理加载 `compress_bc7e.dll` 后自动装钩。之后的转发 + RDO
逻辑与注入版完全一致。

> **为什么必须有 asmdef**：`Assets\Plugins` 下的脚本默认编入
> `Assembly-CSharp-firstpass`，该程序集**不引用 UnityEditor.dll**，
> `[InitializeOnLoad]`/`[MenuItem]` 会编译报错（CS0246），打包/重新编译
> 场景直接失败。`Bc7Rdo.Editor.asmdef` 声明 `includePlatforms: ["Editor"]`
> 的自定义程序集，编辑器平台程序集自动引用 UnityEditor.dll，因此插件
> 放 Assets 任意位置（含 Plugins）都能编译。另外 `hook_loader.cs` 内置
> AssetPostprocessor，首次导入时自动把 `bc7_rdo_hook.dll` 的插件导入器
> 设为**仅编辑器平台**，不会打进玩家包。

### 3.5.1 并行导入 worker（关键）

Unity 2022.3 默认启用**并行导入**：批量 reimport 的纹理压缩分派给
`AssetImportWorker` 子进程（进程名 `Unity.exe`，命令行含
`-name AssetImportWorkerN`，由主编辑器进程 fork）。**worker 进程不运行
编辑器脚本**，因此 `[InitializeOnLoad]` 的 LoadLibrary 只发生在主进程——
这正是"插件已装、hook 日志 OK，但 AB 包体积与非 RDO 相同"的根因：
纹理压缩发生在未装钩的 worker 里。

插件已内置**worker 自动注入**：`Install()` 成功后启动后台监控线程，
每 500 毫秒枚举当前进程的子进程，发现 `Unity.exe` / `AssetImportWorker*`
即用 `CreateRemoteThread + LoadLibraryW` 注入 hook DLL（幂等，失败自动
重试；worker 可能随导入批次重启，新 PID 会被再次注入）。注入后的 worker
由 hook DLL 自身的 50ms 轮询完成对 `compress_bc7e.dll` 的装钩。菜单
`Tools/BC7 RDO/Inject Import Workers` 可手动触发一次扫描。

验证（真实 Unity 批处理，见 `tests/test_editor_plugin.py`）：

- 阶段2 导入管线：32 张纹理 BC7 reimport，λ=4 产物 Deflate 体积
  **−31.8%**（163564 → 111499）；
- 阶段3 worker 链路：Unity 批处理进程启动子进程（改名 `Unity.exe`
  模拟 worker，父进程 = Unity 进程），`ScanAndInject` 识别并注入后，
  子进程压缩输出哈希 `0x7eb392a47708a169`（基线）→
  `0x3d37e0edcf5c89d5`（RDO），证明"子进程识别 → 注入 → hook 生效"
  全链路正确。

已知边界：

- worker 进程创建后约 0.5–1 秒内完成注入（500ms 扫描周期）；worker 刚启动
  瞬间处理的首批极少量纹理可能漏钩，再次 reimport 即覆盖。若打包后 AB
  体积仍无变化，先检查 Console 是否有 `[Bc7Rdo] worker 注入成功 PID=...`
  （无则说明该会话未启用并行导入，reimport 在主进程执行，主进程 hook
  已覆盖）。
- **`Importer(TextureImporter) generated inconsistent result` 警告**：
  Unity 的 TextureImporter 压缩（与 AssetPipeline 并行导入开关无关）
  内部以 **64 块/批** 的多线程 job 调度执行，同一纹理的"正式导入 +
  ConsistencyChecker 校验导入"两次执行的**批次划分不固定**（实测批间
  缓冲区地址零连续性）。ERT 的 RDO 参考"批内前块"，输出随批划分变化，
  因此两次导入产生不同字节 → 触发该警告。**警告无害**：最终 artifact
  是正式导入的 RDO 输出，AB 体积收益已在真实环境实测确认；警告无法从
  hook 侧消除——块间参考类 RDO 的收益本质上依赖批内连续性（实测：
  参考固定模板或逐块独立时收益归零）。若无法容忍警告，将 `lambda=0`
  或移除插件即恢复原版无警告行为。
- **配置 `debug=1` 会大幅刷屏**：`ert::reduce_entropy` 每次调用都会向
  Editor.log 打印匹配直方图（每块批一次），并拖慢导入；调试完毕务必
  改回 `debug=0`。

特点：

- 打开工程即生效，全程零手动操作、零安装目录写入；
- 卸载 = 删除该目录；随工程/版本库分发，团队共享同一套配置；
- 菜单 `Tools/BC7 RDO/Reload Hook DLL`（手动重载）与
  `Tools/BC7 RDO/Smoke Test`（手动冒烟）；
- 冒烟测试也可批处理执行（`-batchmode -quit -executeMethod
  Bc7Rdo.HookLoader.SmokeTest`），验证"加载 hook → 加载 compress_bc7e.dll
  → 装钩 → 转发 → RDO 生效"全链路，自动化测试即用此方式；
- `bc7_rdo_hook.log` 出现在插件目录下的 `Log~` 子目录（Assets 内，但
  `~` 结尾目录被 Unity 导入器整体忽略，不生成 `.meta`、不进资源库），
  无需在 `.gitignore` 里额外处理导入问题。

两种形态对比：

| 形态 | 前置条件 | 操作 | 安装目录写入 |
| --- | --- | --- | --- |
| 壳 DLL 替换（原方案） | 管理员权限 | 运行 enable_unity_rdo.ps1 | 有（可回滚） |
| **Editor Plugin（本方案）** | 无 | **放进工程，打开即生效** | 无 |

## 4. 构建

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

产物（`out/`）：

| 文件 | 说明 |
| --- | --- |
| `compress_bc7e.dll` | 壳 DLL（替换版，原有） |
| `bc7_rdo_hook.dll` | 外挂版载荷（导出桩 detour + RDO） |
| `bc7rdo_test_target.exe` | 自动化测试用目标程序（供 Editor Plugin 的 worker 注入测试用） |
| `editor-plugin/Bc7Rdo/` | **Editor Plugin 分发包**：hook_loader.cs + bc7_rdo_hook.dll + bc7_rdo_config.txt，整个目录复制进工程 Assets 即用 |

## 5. 使用

### 5.1 Editor Plugin（推荐）

把 `out\editor-plugin\Bc7Rdo\` 整个目录复制到工程 `Assets` 下任意位置
（如 `Assets\Editor\Bc7Rdo\`），打开/刷新工程即自动生效：
加载 hook DLL → 轮询等 `compress_bc7e.dll` → 装钩 → RDO。无需注入器、
无需管理员。配置 `bc7_rdo_config.txt` 同目录热更新；日志
`Log~\bc7_rdo_hook.log`（`Log~` 被 Unity 忽略，不生成 .meta；首次写日志时
自动创建该目录）。卸载 = 删目录。

## 6. 测试

```powershell
python tests\test_hook.py            # 载荷链路：detour + 转发 + RDO（无需 Unity）
python tests\test_editor_plugin.py   # Editor Plugin 端到端（真实 Unity 批处理冒烟）
python tests\test_compat.py          # 壳 DLL 回归（rdo_common.h 重构无影响）
```

`test_hook.py` 在进程内以 `compress_bc7e.dll` 名字加载原 DLL 副本 + hook DLL，
验证：装钩日志 `install_hook: OK`、λ=0 转发逐字节一致、λ=4 RDO 生效
（zlib9 体积下降、PSNR 略降）、配置热重载。

`test_editor_plugin.py` 定位本机 Unity（`-u`/`UNITY_EXE`/注册表/Hub），
建临时工程放入插件，`-batchmode -nographics -quit -executeMethod
Bc7Rdo.HookLoader.SmokeTest` 后断言：退出码 0、unity.log 出现
`[Bc7Rdo][SMOKE] PASS`、插件目录 hook 日志含 `install_hook: OK`。
无 Unity 时打印 `[SKIP]` 退出 0。

## 7. 局限性

- **只拦截 `bc7elib_compress_blocks`**：装钩后新发起的压缩调用生效；
  已缓存函数指针的调用方仍会经过导出桩地址（patch 后同样被拦），
  但如果 Unity 在装钩前已完成某批压缩，则该批无 RDO——正常现象；
- **x64 only**：Unity 2022.3 Editor 与 hook DLL 均为 x64；
- **权限/安全软件**：Editor Plugin 形式是进程内 `LoadLibrary`（仅 worker
  注入用到 `CreateRemoteThread + LoadLibraryW`），与 Unity 同权限即可；
  个别杀软可能拦截远程注入模式（本地 mod 用途，通常不被拦截）；
- **Unity 版本漂移**：若未来版本改变导出桩布局（首字节非 `E9`），
  hook DLL 会记录日志并**拒绝 patch**，安全降级为无 RDO；
- **插件形式**：hook 日志出现在插件目录（Assets 下），建议加入工程
  `.gitignore`；脚本域重载（改脚本/进 Play 模式）会重复调用
  `Install()`，幂等无副作用；
- **卸载**：hook 常驻至 Unity 退出，无卸载机制（外挂生命周期即 Unity 生命周期）；
- 与替换版相同：请自行评估 Unity EULA 与版本兼容风险，仅作实验用途。

### 7.1 编码器切换（bc7enc_rdo / Oodle Texture）

插件目录 `bc7_rdo_config.txt` 新增 `encoder` 开关，可在两种编码器之间切换：

- **`encoder=bc7enc_rdo`（默认）**：既有路径——bc7e 压缩后接 ERT RDO
  后处理，全部逻辑开源随包分发；
- **`encoder=oodle`**：当 `oodle_lambda>0` 时，hook 改用 Oodle Texture RDO
  做**完整 RDO 编码**（跳过 bc7e+ERT）。RDO 强度由独立的 `oodle_lambda`
  （整数 0..100，默认 30）给出，与 ERT 的 `lambda` 互不换算；
  `oodle_effort=low|normal|high`（默认 `high`）调节耗时。普通 RGBA 的
  `oodle_lambda=0` 会直接使用 Unity 原生 BC7，不加载或调用 Oodle，也不做
  ERT 后处理；RGBM 若设置 `rgbm_oodle_lambda=0` 也独立走该原生路径，
  `rgbm_oodle_lambda=-1` 时才继承通用值。ERT 专用键
   `lambda`/`lookback`/`smooth_scale`/`max_rms_increase_ratio`/`two_matches`
   被忽略。需要插件目录存在两个 Epic 专有 DLL：
   `oo2tex_win64_2.9.16.dll` 与 `oo2texrt_win64_2.9.16.dll`（已 gitignore，
   **既不在仓库中，也不在 release 发布包内**）。获取方式二选一：本地构建
   前手动拷入 `oodle_testenv\bin\`（`tools\build.ps1` 会暂存到
   `out\editor-plugin\Bc7Rdo\`），或 release 包解压并复制进工程后直接拷入
   插件目录。两编码器档位对照参考
   [docs/OODLE_LAMBDA_COMPARISON.md](OODLE_LAMBDA_COMPARISON.md)。
- **已知 batching 限制同样适用于 Oodle**：Oodle RDO 设计上是面向**整张
  纹理**做率失真优化（UE 中即传整张图），而 hook 只看到 Unity 内部约
  **64 块/批**的调度切分。因此 UE 式"整图感知"的 RDO 收益在本插件中
  **无法完全实现**，Oodle 编码器同样只能获得批内 RDO 收益——这与 ERT
  路径的批划分限制同源，属 hook 形式的固有限制。
- **优雅降级**：`encoder=oodle` 但 Oodle DLL 缺失或编码失败时，hook
  自动回退到默认 `bc7enc_rdo`/ERT RDO 路径——调用原 bc7e 压缩后接
  `rdo::apply_rdo`（ERT 后处理），与 `encoder=bc7enc_rdo` 行为完全一致，
  而非降级为无 RDO 的普通 bc7e；仅打一次日志提示，不崩溃。
