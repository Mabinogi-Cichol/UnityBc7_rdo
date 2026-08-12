# Oodle Texture (UE) 与本库 BC7E RDO 的 lambda 对照总结

> 结论：**UE/Oodle λ=30 ↔ 本库 λ≈1.0**（源码级精确换算为 1.1，本库默认值正好是 1.0）。
> 通用换算公式：`λ_本库 ≈ λ_UE × 0.0375`（BC7、4×4、RGBA、权重 1、activity≈1）。

---

## 1. 背景

用户问题：参考 Unreal Engine 文档（Oodle Texture RDO，默认 λ=30），问 UE 的 30 对应本库（Unity BC7 RDO 壳 DLL，基于 richgel999 BC7E RDO）的 lambda 值是多少。

本库通过 `bc7_rdo_config.txt`（与壳 DLL 同目录）控制 RDO 强度（lambda=0 关闭，建议 0.25–4.0，默认 1.0）。

## 2. 结论摘要

| 项目 | 值 |
|---|---|
| UE/Oodle 默认 λ | 30 |
| 对应本库 λ | **≈1.0–1.1（推荐 1.0）** |
| Oodle 内部实际 λ | 30 × 0.6 = **18** |
| 换算系数 | ×0.6（Oodle 内部缩放）÷ 16（D 单位）≈ ×0.0375 |

## 3. 推导过程（源码级）

### 3.1 两端目标函数形式相同

- Oodle（bc7rd.cpp）：`J = D + lambda * R`，注释 "lambda is in steps of 10"，R = LZ 比特数
- 本库（ert.cpp:360）：`t = cur_mse * smooth_block_mse_scale + cur_bits * params.m_lambda`，R = LZ 比特数

两者都是 `J = D + λ·R`，因此只需对齐 λ 的定义尺度。

### 3.2 Oodle 内部 λ 缩放（int → float）

Oodle 公开 API 的 λ 是 0–100 的整数（`OodleTex_RDOLagrangeLambda_Min=1, Max=100`），内部换算：

```
λ_internal = int_λ × BC7RD_LAMBDA_SCALE
BC7RD_LAMBDA_SCALE = 0.8 × OO2TEX_GLOBAL_LAMBDA_SCALE = 0.8 × 0.75 = 0.6
（bc7rd.cpp:40，perceptualactivity.h:17）
→ UE λ=30 → 内部 λ = 18
```

### 3.3 D 单位比（关键因子 16）

| | Oodle | 本库 |
|---|---|---|
| D 定义 | VQD（perceptualactivity.cpp:936）= 整块 16 像素误差平方和（per-block SSD） | `compute_block_mse`（ert.cpp:221）= **逐像素平均** MSE（`total_err / total_block_pixels`，÷16） |

验收准则 `ΔD < λ·ΔR`：D 单位小 16 倍 ⇒ λ 也要小 16 倍才有相同的"每比特愿意接受的失真"。

```
λ_ert = λ_oodle_internal / 16 = 18 / 16 ≈ 1.1
```

### 3.4 档位对齐验证（两端文档相互印证）

| Oodle/UE λ | Oodle 官方描述 | 换算 λ_ert | 本库文档描述 |
|---|---|---|---|
| 0 | 关闭 RDO | 0 | 0（关闭） |
| 1 | NearLossless，"still super high quality"（5–10 也可） | 0.04–0.4 | 最小有效 0.1–0.25 |
| 10 | "super high quality" | 0.6 | vendor README: 0.5 = "highest quality" ✓ |
| **30** | **默认，"good default"** | **1.1** | **默认 1.0** ✓ |
| 40–60 | 有些可见损失 | 1.5–2.3 | 2–4 激进档 ✓ |
| 100 | 最大压缩 | 3.75 | 建议上限 4.0 ✓ |

## 4. 实测交叉验证（过程记录）

### 4.1 已搭建的验证管线

- Oodle 2.9.8 全源码编译（`oodlelib.vcxproj` 需把 v143 工具集改为本机 v145）
- `test.exe` 参数化：`test.exe <lambda> <out.dds> [in.tga]`
- `decode_oodle.exe`：用 Oodle 自家 `OodleTex_DecodeBCN_Blocks` 解码 BC7 DDS
- `out/cross_validate.py`：同一张 2048×2048 图（`oodle/source.tga`），双编码器扫描 λ，计算 PSNR(RGBA/RGB) + zlib9/lzma 体积

### 4.2 发现：该 Oodle SDK 源码构建编码输出损坏

- 输出块内容确定性错乱：块 (by,bx) 读到的是源图行 `4by+(bx mod 4)`、列 `4(bx//4)` 的像素（逐块反解映射验证）
- 已排除：解码端（bc7decomp 与 Oodle 自家解码器结果一致；纯色图完美往返）、任务系统（`OODLETEX_JOBS_DISABLE` 单线程无效）、SIMD（`OodleTex_BCNFlag_AvoidWideVectors` 无效）、λ=0 同样损坏
- 结论：该 SDK 文件夹的源码构建本身有问题（疑为文件版本混装或构建配置问题），**实测率失真曲线不可信，未用于最终结论**

### 4.3 间接佐证（方向一致）

损坏输出的 zlib9 体积仍随 λ 单调（λ=10→50：3.82→2.87MB），Oodle λ=30 ≈ 3.08MB，落在本库 λ=0.5（3.71MB）与 λ=1.0（2.56MB）之间——与 λ≈1.0 结论同向（注：内容损坏会使体积偏高，真实 Oodle λ=30 体积会更小，对应本库 λ 约 0.9–1.0）。

## 5. 使用建议

- UE 项目中习惯用 λ=30（默认）的话，本库直接配置 `lambda=1.0`
- 迁移参考：UE TLCA 档位 ↔ 本库：TLCA_Low(10) ↔ 0.4、TLCA_Medium(20) ↔ 0.75、TLCA_High(30) ↔ 1.1、TLCA_Highest(40) ↔ 1.5
- 法线/非漫反射贴图两端都建议降低 λ（UE 5–20 ↔ 本库 0.2–0.75）

## 6. 复现验证

如需补全实测曲线，用官方预编译 Oodle 库（`oodle2.dll` + `oodletex.dll`）替换源码构建后：

1. 重新生成 Oodle λ=10/20/30/40/50 的 BC7 输出
2. 跑本库 λ=0.25/0.5/1.0/1.5/2.0/3.0/4.0 扫描
3. 比对（zlib9/lzma 体积, PSNR）最近点

（原 `out/cross_validate.py` 脚本结构可直接复用。）
