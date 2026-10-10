# P6：v0.5.0 交付与发行验收

- 日期：2026-10-10（Asia/Shanghai）
- Feature：`rgbm-hook-portability`
- 状态：P6 已完成，v0.5.0 已正式发布
- 发布：[GitHub Release](https://github.com/Mabinogi-Cichol/UnityBc7_rdo/releases/tag/v0.5.0)
- 构建及标签提交：`689cdb8eb99d8613a52be12af0192bbc93ab1688`
- 配套：[中文发行说明](../releases/v0.5.0.md)、[施工记录](rgbm-hook-portability-construction.md)、[P5 验收](rgbm-hook-portability-acceptance.md)

## 提交与源码身份

按功能模块分批提交，保留英文 conventional prefix、正文中文：依赖／事务后端、绑定认证／缓存、RGBM 运行时、C#／Worker／部署、验收矩阵、DLL 合并 TODO、实际工程验证包装脚本修复、设计施工证据、发行工具与说明。

构建前确认工作区干净、无未跟踪源码，从提交 `689cdb8` 的 `git archive` 创建独立快照。Unity 原始 BC7 DLL 和本地 Oodle 仅用于构建／测试，均未进入源码提交或发行包。本记录是发布后的文档提交；v0.5.0 标签保持指向实际构建提交，不移动标签。

## 实际发行包

| 包 | 字节 | SHA256 |
| --- | ---: | --- |
| `compress_bc7e_rdo-v0.5.0-win-x64.zip` | 156,388 | `B583AD79A1667639D127F073ACB8BAD9932FCFA0AE15448DC2BA573F1FA5EED3` |
| `compress_bc7e_rdo-editor-plugin-v0.5.0-win-x64.zip` | 1,939,812 | `2362CBF52892A4E7FE066F21E550ED3970820093450914292ECD7C7C533688DE` |

发布另附 `SHA256SUMS-v0.5.0.txt` 和 `v0.5.0-validation.json`。两个 ZIP 内的递归 SHA256 覆盖全部文件及工具子目录；严格白名单、校验清单覆盖、x64 PE、依赖清单一致性通过 `tools/verify_release.py`。

插件只有一个生产 C# 脚本，包含三个原生 DLL、asmdef、默认配置、`RgbmBindings~` 工具／DIA、bc7enc_rdo／Detours 许可证。无 Oodle、Unity exe／PDB／原 DLL、测试脚本／资源、日志或 Library 缓存。壳包保持替换／回滚形式，不提供本次新增的 RGBM 语义分类。

未修改的 release DIA 版本 `14.51.36257.0`、Microsoft 签名 Valid，SHA256 为 `55E1CE73E8FFD54C88A75C0FDCB11A56DF4BA0F80949CF585741855E9E4181AA`。包内保留 [Microsoft 适用分发条件](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution#dia-sdk)，依赖清单不含开发机 DIA 绝对路径。

## 重新构建产物的验证

发行主 DLL SHA256 为 `B62CFC531FCCF51D1AA2E76BBB89445EF07552F129E78F7FEE32B540F374AAF3`，与 P5 冻结二进制身份不同。未将历史 14 轮、1,140 份输出直接当作本次发行二进制的重跑结果。

从实际插件 ZIP 解压到新隔离工程，测试时启用语义 Hook；正常 Oodle 矩阵临时加入用户自有运行库，回退矩阵删除这两份文件后运行。f1／f2 各验证主进程、真实 Worker、无 Oodle 主进程和 Worker，全部使用 batchmode，真实退出 0，运行时 Ready。

| 运行 | 主 PID | 实际 Worker 数 | 参数／路径组 | 输出 | 调用 |
| --- | ---: | ---: | ---: | ---: | ---: |
| f1 main | 54116 | 0 | 9 | 108 | 1,944 |
| f1 worker | 34220 | 2 | 9 | 108 | 1,944 |
| f1 fallback main | 59080 | 0 | 5 | 60 | 1,080 |
| f1 fallback worker | 67268 | 2 | 5 | 60 | 1,080 |
| f2 main | 64136 | 0 | 9 | 108 | 1,944 |
| f2 worker | 54068 | 2 | 9 | 108 | 1,944 |
| f2 fallback main | 57604 | 0 | 5 | 60 | 1,080 |
| f2 fallback worker | 55024 | 2 | 5 | 60 | 1,080 |

总计 **56 组参数／路径、672 份输出、12,096 次编码调用、230,496 个块**；每组日志块覆盖与实际输出一致，**336 组主进程／Worker 输出完全相同**。参数／usage／RGBM metric／零 λ／继承／ERT／Unknown／Oodle 缺失回退均由既有 P5 驱动断言；安装代码与所有输出文件再做 SHA256 核对。

原生壳兼容、立即／晚加载 Hook、绑定单元、通用 Detour、运行时单元、协调器安装／恢复／故障矩阵通过。当前发行 resolver／认证器通过 20 个缓存／扫描场景；首次读取配置回归为 120 独立进程×64 线程，全部退出 0，耗时 21.631 秒。

首轮辅助缓存测试因 Python 未载入已有 Capstone 而未执行，设置任务已有依赖路径后通过；Unity 验证在 f1 两轮通过后因本地 Oodle 文件为只读而清理失败，确认无目标 Unity 进程后修正任务自有文件清理并完成剩余轮次。两次都是验证包装问题，原始失败日志保留，未计为通过的编码轮次。

本次验收 Unity 均已退出，用户原来打开的 StarwardX PID 27876 保持运行。本次未替换 ClientDevelopment 的生产 DLL；该工程仍保留 R29 已验证的插件与配置。

## 线上核对与边界

用户明确授权向 `Mabinogi-Cichol/UnityBc7_rdo` 的 main 推送并公开发布 v0.5.0 后执行。发布不是 draft／prerelease，标签解引用为构建提交。线上说明与本地完全一致；下载全部四份资产，逐一确认大小、SHA256、GitHub digest 和外部校验清单一致。

使用文档明确已验证 Windows x64 Unity 2022.3.62f1／f2；未保证任意版本适配。无 VS 环境按用户要求未测试，严格 Unity 内先 Ready 后首次 BC7 加载未实测；误差与计时仍受 P5 的质量／性能边界约束。独立原生 DLL 合并仍为 [TODO](rgbm-hook-dll-consolidation-design.md)。

本地证据在 Git 忽略的 `out/p6/v0.5.0`：源码快照与 provenance、构建日志、ZIP 检查、原生汇总、Unity 汇总与全部输出 SHA256、线上 Release／refs／下载资产及 `online-verification.json`。公开计数与包身份见 Release 附件，不上传私有资源或运行缓存。
