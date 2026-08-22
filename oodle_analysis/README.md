# Oodle Texture 2.9.16 — BC7 RDO 分析

本目录是对 Unreal Engine 5.8 内置 Oodle Texture 2.9.16 SDK 的独立分析，
分析对象为 UE 中用于 BC7 率失真优化（Rate-Distortion Optimization, RDO）压缩的官方库。

## 内容索引

| 文件 | 内容 |
| --- | --- |
| [REPORT.md](REPORT.md) | **完整分析报告**（SDK 清单、API 全景、RDO 概念、UE 集成、与 Unity 方案对比、实测、许可） |

## 关联产物

- **测试环境**：[`../oodle_testenv/`](../oodle_testenv/) — 独立、可运行、自包含的
  BC7 RDO 测试环境（C++ 壳 + Python 扫描驱动），直接加载 UE 5.8 官方预编译 DLL
  `oo2tex_win64_2.9.16.dll`，产出实测率失真曲线与 `sweep_ratedistortion.csv`。
- **既有对比文档**：[`../docs/OODLE_LAMBDA_COMPARISON.md`](../docs/OODLE_LAMBDA_COMPARISON.md)
  — Oodle λ 与本库（bc7enc_rdo / ERT）λ 的源码级换算。

## 一句话结论

Oodle Texture 2.9.16 通过 `OodleTex_EncodeBCN_RDO_Ex` 提供 BC7 RDO 编码，
以整数 `rdo_lagrange_lambda`（0–100，默认 30）控制"压缩体积 vs 质量"的权衡；
UE 在 `TextureFormatOodle` 模块中以动态加载方式调用它，并把纹理的
`LossyCompressionAmount` 映射为该 lambda。实测（官方 DLL、1024×1024、λ 扫 0–100）
zlib9 体积可较基线下降约 **46%**（PSNR 43.2 → 39.4 dB），符合文档描述的 RDO 行为。
