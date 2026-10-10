# P5：RGBM Hook 跨版本编码验收

- 日期：2026-10-10（Asia/Shanghai）
- Feature：`rgbm-hook-portability`
- 配套：[设计](rgbm-hook-portability-design.md)、[施工记录](rgbm-hook-portability-construction.md)、[P4 部署](rgbm-hook-portability-deployment.md)
- 状态：P5 已完成；最终同一插件的跨版本矩阵、原生回归及实际工程验收均通过。P6 已发布 v0.5.0，重新构建的发行二进制验证见 [P6 报告](rgbm-hook-portability-release.md)。

本文及 `out/rgbm-portability/acceptance/Bc7Rdo` 保留 P5 当时的双 C# 脚本快照。后续已将绑定准备合入 `hook_loader.cs`，原生产物不变；新的四轮跨版本编码和实际工程部署验证见[施工记录 R29](rgbm-hook-portability-construction.md)。后续证据位于 `out/rgbm-portability/loader-merge`，不覆盖本文的历史验收身份。

## 环境与产物身份

验收使用相同的生产插件快照，不为 f1／f2 重建或填写入口地址。所有正常矩阵使用相同主 DLL、协调器、gate、loader、解析工具及私有 DIA；缺少 Oodle 的矩阵仅移除两份本地专有 Oodle DLL。

| 项目 | 身份 |
| --- | --- |
| f1 | `C:\Program Files\Unity\Unity 2022.3.62f1\Editor\Unity.exe` |
| f1 exe SHA256 | `E564374ACF68AD36EC7383D6B423EA3862F9280A3A1A61E34B997E694567F0DF` |
| f2 | `C:\Program Files\Unity\Hub\Editor\2022.3.62f2\Editor\Unity.exe` |
| f2 exe SHA256 | `316507F85AEAEDF602966BE741E0C2C69D12D44850610FCF3A4AC0A164316BA3` |
| 最终主 DLL SHA256 | `C25C24F72BFA1F8BE0A8D840CE5C23E7DBF8CFDF3A8C501CE82022FA26F403AB` |
| 真实 HDR SHA256 | `F94FD87B90A4AE5AD3E2D931B50B31021FAC4D103185D4A102ADD797A36C11F5` |
| 实际工程 | `E:\Unity\StarWings\ClientDevelopment`，2022.3.62f2 |

HDR 来源为实际工程的 `Assets/GameData/GameLevels/MVPSceneTest/LMGroup_MVPSceneTest_64_01_final.hdr`。隔离工程每轮使用 6 份 HDR 副本和 6 份确定性普通 PNG；实际工程只使用 1 份 HDR 副本和 1 份普通 PNG。测试副本均限制为 64×64、BC7、可读、启用 mipmap，不重导入原始 HDR。

## 验收方法

`tests/unity/RgbmP5Validation.cs` 仅为临时驱动，没有 AssetPostprocessor。它创建唯一命名目录，逐组更改插件配置和自有导入器 metadata，执行真实导入，查询 `TextureUtil.GetUsageMode`／`IsRGBMUsageMode`，保存全部 mip 的 BC7 块并计算 SHA256。

实际 RGBM 必须为 `BakedLightmapRGBM`、usage 值 2。默认 High Lightmap 质量会得到 FullHDR，因此隔离工程在启动前将磁盘设置准备为 Normal；实际工程原本使用 Normal，驱动不修改其 Lightmap 质量。[Unity Lightmap 编码说明](https://docs.unity3d.com/ja/2022.3/Manual/Lightmaps-TechnicalInformation.html)

每个上下文分成两个独立 Unity 运行：主进程使用 `-refreshImportMode InProcess`；Worker 使用 `OutOfProcess`。普通 Editor 运行不带 batchmode／nographics，使用隐藏窗口并自动退出；批处理运行带 `-batchmode -nographics`。驱动严格要求主路径编码 PID 等于主 Editor，Worker 路径编码 PID 不等于主 Editor。

Worker 路径在主 Editor Ready 后要求两个进程完成 native 认证，并通过 Unity 的 `GetWorkerStats` 确认 idle=2、connecting=0，再提交完整导入队列。初始化期间通过 Editor update 等待，让 Unity 完成 Worker 启动握手。每个实际编码 Worker 复核 PID、创建 FILETIME、真实 exe SHA256、asset-import-worker 身份、Ready 和 hook-installed 状态。[Unity 2022.3 并行导入](https://docs.unity3d.com/2022.3/Documentation/Manual/ParallelImport.html)、[对应 WorkerStats 源码](https://github.com/Unity-Technologies/UnityCsReference/blob/2022.3/Modules/AssetDatabase/Editor/ScriptBindings/AssetDatabase.bindings.cs)

生产日志增加 PID／TID 和 ERT 实际参数；整行构造后使用 `FILE_APPEND_DATA` 与单次 `WriteFile` 追加，避免不同 Worker 的 CRT seek／write 竞争。最终汇总同时要求每组日志的 `num_blocks` 总和等于实际导出块数。[Windows WriteFile 追加语义](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefile)

## 参数与输出断言

| 用例 | 实际参数 | 输出要求 |
| --- | --- | --- |
| native | Oodle 普通／RGBM = 0／0 | 建立原生 BC7 对照；Worker 与主进程原生输出逐字节相同 |
| oodle-split | 30／10 | 两类输出均有变化，实际日志分别使用 30 和 10 |
| oodle-rgbm-zero | 30／0 | RGBM 等于原生，普通输出保持 split 结果 |
| oodle-ordinary-zero | 0／10 | 普通等于原生，RGBM 输出保持 split 结果 |
| oodle-explicit-inherit-control | 30／30 | 建立显式继承对照 |
| oodle-inherit | 30／-1 | 与显式 30／30 逐字节相同 |
| ert-global-zero | 普通 λ=0，RGBM λ=0.25 | 两类均等于原生，全局开关生效 |
| ert-split | 普通 λ=4，RGBM λ=0.25 | 实际 RGBM metric=1，普通 metric=0；普通输出有变化 |
| ert-rgbm-zero | 普通 λ=4，RGBM λ=0 | RGBM 等于原生，普通保持 ERT split 结果 |
| oodle-unavailable | 请求 Oodle，实际无 Oodle DLL | 使用 ERT，与直接选择 ERT split 逐字节相同 |

ERT 普通使用 smooth_scale=15、max_rms_ratio=10；RGBM 使用 30 和 1.5。每条事件还核对 context_rgbm、rgbm metric、实际 λ 和 rdo_enabled。

正常运行每条路径执行前 9 组；缺少 Oodle 的运行每条路径执行 native、ERT 三组和 oodle-unavailable 共 5 组。每次导入矩阵结束后，直接调用已验证 ABI 的 BC7 导出，确认没有 Unity 压缩作用域的 Unknown 在 Oodle／ERT 非零 λ 下均使用原生输出，防止旧 RGBM／普通 TLS 残留。

## 结果与本地证据

最终 14 次 Unity 运行均真实退出 0，共 110 组参数／路径、1,140 份导入输出、20,520 次编码调用、391,020 个 BC7 块。每组日志块数与实际输出完全相等，全部块文件 SHA256 通过，570 组主进程／Worker 输出逐字节一致，包括非零 RDO 配置。

| 运行 | 主 PID | 实际编码 Worker PID | 参数组 | 编码调用 | exit |
| --- | --- | --- | --- | --- | --- |
| f1-batch-main | 53448 | — | 9 | 1944 | 0 |
| f1-batch-worker | 51596 | 54232, 54400 | 9 | 1944 | 0 |
| f2-batch-main | 56664 | — | 9 | 1944 | 0 |
| f2-batch-worker | 55564 | 21764, 57040 | 9 | 1944 | 0 |
| f1-editor-main | 51968 | — | 9 | 1944 | 0 |
| f1-editor-worker | 57652 | 11332, 56160 | 9 | 1944 | 0 |
| f2-editor-main | 48732 | — | 9 | 1944 | 0 |
| f2-editor-worker | 55260 | 45540, 57108 | 9 | 1944 | 0 |
| f1-fallback-main | 10548 | — | 5 | 1080 | 0 |
| f1-fallback-worker | 58100 | 41924, 56592 | 5 | 1080 | 0 |
| f2-fallback-main | 56488 | — | 5 | 1080 | 0 |
| f2-fallback-worker | 56428 | 30444, 44816 | 5 | 1080 | 0 |
| user-f2-batch-main | 46508 | — | 9 | 324 | 0 |
| user-f2-batch-worker | 51956 | 57784, 57856 | 9 | 324 | 0 |

最终生产 DLL 的 `tests/test_hook.py` 普通／`--late-module` 两次回归均退出 0：原生转发、Oodle 零 λ、不加载 Oodle、ERT 生效和 Unknown 原生退化通过。`tests/verify_rgbm_p5_native.py` 重复 120 个独立进程，每进程 64 线程首次读取配置，全部退出 0，总耗时 21.993 秒。晚加载宿主为 Disabled 引导，不替代 Unity 内语义 Ready 先于 BC7 首次加载的额外场景。

### 实际工程结果与恢复

实际工程普通／RGBM 每张各 343 个块。Oodle 30／10 改变普通 341、RGBM 343 个块；ERT 4／0.25 改变普通 299、RGBM 83 个块。零 λ 对应 lane 改变数为 0，另一 lane 与 split 配置完全相同；继承 λ、Unknown 和 Oodle 缺失回退均通过。

| 实际工程路径 | native 阶段 ms | Oodle split ms | ERT split ms |
| --- | --- | --- | --- |
| 主进程 | 707.566 | 653.298 | 667.229 |
| Worker | 2011.899 | 772.884 | 660.695 |

这是每组两张 64×64 纹理的单次全流程耗时，含配置、导入与证据采集；native 为首组，存在冷启动差异。不能据此声称 Oodle／ERT 更快，或给出纯 Hook 开销保证。

`tools/run_rgbm_p5_user.ps1` 在工程关闭时备份并部署最终快照；原 P4 插件另保存在 `p4-plugin-before-p5`。最终复测前插件备份为 `user-before-20261010-173212`。全部测试副本、临时驱动及 meta 已删除，保留通过验收的最终插件。

| 恢复项 | 最终 SHA256／结果 |
| --- | --- |
| 配置 | `A446D940E9806C2F0058B603AB7564F736A4898873FDCB4609E8305C362AF1F6`，原内容和时间恢复 |
| 源 HDR | `F94FD87B90A4AE5AD3E2D931B50B31021FAC4D103185D4A102ADD797A36C11F5`，未变 |
| HDR meta | `85C9894024CA9D6E65104F751A7F25B31327FF62A9310E12C4B7F1C3F06596B7`，未变 |
| EditorSettings | `8010C0008B31C7FCD72A4394F5E61C9E7F348ADF13B05A00881A4A4E203C0317`，未变 |
| ProjectSettings | `760DA6272D51B1D3484B42D7DB3F317890E74C2DA2BD56982BBEA730D30B7830`，未变 |
| 正式插件最终编译 | PID 56324，exit 0，native Ready、hook_installed=true |
| 本次 Unity 实例 | 全部退出；用户已打开的 StarwardX PID 27876 保持运行 |

`out/rgbm-portability/acceptance/` 保存每次 `.json`／`.exit.json`／`.log`、全部 BC7 块、正式插件快照、原生回归、工程恢复记录、`summary.json` 和 `artifact-sha256.json`。该目录被 Git 忽略。`tests/verify_rgbm_p5.py` 检查真实退出码、PID、同一插件所有生产文件、每组事件和输出块数、usage／参数／输出、恢复状态及全部文件 SHA256。

已有验收工程的复跑入口为 `tools/run_rgbm_p5.ps1 -UnityExe <exe> -ProjectPath <独立工程> -SourceHdr <真实HDR> -Name <矩阵名>-main -Route main`，Worker 将末尾改为 `-Name <矩阵名>-worker -Route worker`。普通 Editor 加 `-EditorMode`；无 Oodle 工程加 `-Fallback`，其目录必须确实没有 Oodle DLL。隔离工程须安装最终快照及临时驱动，首次创建时先由对应 Unity 生成 ProjectSettings。汇总执行 `python tests/verify_rgbm_p5.py`；脚本使用的真实 Python 为本机 Python 3.11。

## 调整过程与边界

初期驱动依次拒绝了错误的 RefreshImportMode 枚举名、2022.3 Lightmap 内部 API 命名差异、隔离工程默认 FullHDR、未保存到磁盘的 Worker 质量设置，以及通过 EditorSettings 切换启动固定导入模式的方案。普通 Editor 中 native Ready／operational 不等于 Unity Worker 可接任务，最终增加 idle／connecting 等待。失败 JSON 保留，未将错误路由、单个实际 Worker 或缺失事件改写为通过。

第一轮完整矩阵的参数和输出均通过，但共享 CRT 追加日志缺失部分 Worker 记录。旧报告和 DLL 保存于 `before-append-fix`；修复后重新冻结生产插件并重跑所有原生和 Unity 场景，最终通过要求包括完整块覆盖。

解码指标仅比较 Unity 原生 BC7 与 RDO 的差异。RGBM 记录 `Linear(sRGB)×M` 的三通道差异，不是相对源 HDR 的绝对误差，也不是视觉质量验收。全流程耗时包含导入、配置和证据采集，不能将单次差值归因于 Hook 开销或承诺性能。

支持证据限于当前两份 Windows x64 Unity 2022.3.62f1／f2 exe 和认证接口；新二进制仍需通过身份、定位及 ABI 验证。Unity 内语义 Ready 先于第一次 BC7 模块加载的额外场景尚未实测，不是 P4 关口。无 VS 测试按用户要求取消。P6 的使用文档、交付内容与发行检查仍单独跟踪，未提交或发布。
