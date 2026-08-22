# Oodle Texture 2.9.16（UE 5.8）BC7 RDO 完整分析报告

> 分析对象：`C:\Program Files\Epic Games\UE_5.8\Engine\Source\Developer\TextureFormatOodle\Sdks\2.9.16`
> 版本：Oodle Texture **2.9.16**（`OODLE2TEX_VERSION_MAJOR=9`，`OODLE2TEX_VERSION_MINOR=16`）
> 配套测试环境：`../oodle_testenv/`（官方预编译 DLL 实测）

---

## 1. 概述

Unreal Engine 通过 Epic 的 **Oodle Texture** 库实现 BCn（含 BC7）纹理压缩的
**率失真优化（RDO）**。RDO 的核心思想是：BC7 每块固定 16 字节，直接看大小无差别；
但 BC7 块流之后还会被 LZ（Kraken/LZ4/LZMA/zlib 等）二次压缩，RDO 会刻意引入少量、
且在人眼不敏感处的失真，使块流对二次压缩"更友好"，从而显著缩小最终体积。

本报告完整梳理该 SDK 的构成、API 表面、RDO 核心概念、UE 集成方式，并与本仓库既有的
Unity `bc7enc_rdo` 方案做横向对比；最后一节给出基于**官方预编译 DLL** 的实测率失真曲线。

---

## 2. SDK 清单与版本

`Sdks/` 目录下同时保留了多个历史版本（`2.9.5` ~ `2.9.16`），本报告聚焦 2.9.16：

```
2.9.16/
├─ include/
│  ├─ oodle2base.h        5,103 B   基础类型、调用约定、宏
│  ├─ oodle2tex.h        80,217 B   编码库 API（主头文件，1459 行）
│  └─ oodle2texrt.h      21,516 B   运行时解码库 API（444 行）
├─ lib/
│  ├─ Win64/
│  │  ├─ oo2tex_win64.lib        13,534,542 B
│  │  ├─ oo2tex_win64_debug.lib  12,153,206 B
│  │  ├─ oo2texrt_win64.lib       1,006,056 B
│  │  └─ oo2texrt_win64_debug.lib   908,230 B
│  └─ Linux/
│     ├─ liboo2texlinux64.so.9       802,544 B
│     ├─ liboo2texlinux64_dbg.so.9 4,883,480 B
│     ├─ liboo2texrtlinux64.so.9      84,088 B
│     └─ liboo2texrtlinux64_dbg.so.9 714,944 B
└─ redist/
   └─ Win64/
      ├─ oo2tex_win64_2.9.16.dll    703,416 B   ← 编码/解码主库（工具侧）
      └─ oo2texrt_win64_2.9.16.dll  110,520 B   ← 运行时库（BC7Prep GPU/CPU 解码）
```

要点：

- **`oo2tex`（Oodle Texture）** = 编码 + 解码 + BC7Prep 编码，用于工具链（编辑器/Cook）。
- **`oo2texrt`（Oodle Texture RT）** = 仅 BC7Prep 解码，用于运行时（游戏端）。
  游戏若使用 BC7Prep 处理过的纹理，只需随包带 RT 库，主库只需在工具侧。
- `lib/` 下的 `.lib`/`.so` 为链接库（含 debug 变体）；`redist/Win64/` 为可再分发的动态库。
- 本报告的实测环境**不链接 `.lib`**，而是运行时 `LoadLibrary` 官方 `oo2tex_win64_2.9.16.dll`
  直接解析导出函数（UE 自身也是动态加载，见 §5.1）。

---

## 3. 头文件与 API 全景

### 3.1 `oodle2base.h` — 基础类型与调用约定

- 类型别名：`OO_U8/OO_S32/OO_F32/...`，`OO_SINTa`（指针宽有符号整数），`OO_BOOL`。
- 调用约定：Windows 下 `OOLINK`/`OOEXPLINK` = `__stdcall`（x64 上实际与 `__cdecl` 等价）。
- 导出：`OODEFFUNC` = `extern "C"`，`OOFUNC1/OOFUNC2` 组合成标准导出；**导出名无修饰**
  （`dumpbin /exports` 已确认，如 `OodleTex_EncodeBCN_RDO_Ex`）。
- 结构体打包：MSVC 用 `#pragma pack(push, 8)`，GCC/Clang 用 `__attribute__((packed))`。
- 内存对齐约定：`OODLE_MALLOC_MINIMUM_ALIGNMENT = 2*sizeof(void*)`（64 位为 16）。
- 任务系统常量：`OODLE_JOB_MAX_DEPENDENCIES=4`、`OODLE_JOB_NULL_HANDLE=0`。

### 3.2 `oodle2tex.h` — 编码库 API

#### 3.2.1 错误码 `OodleTex_Err`（负数表示错误）

`OK=0`；`UnsupportedCPU=-1`（需 SSE4.1）、`BadBCnFormat=-2`、`BadPixelFormat=-3`、
`SurfaceCountMismatch=-4`、`BlockCountMismatch=-5`、`LayoutFormatMismatch=-6`、
`NegativeLambda=-7`、`Internal=-8`、BC7Prep 系列 `-9..-14`、`InvalidSurfaceIndex=-15`、
`MalformedBlockIDs=-16`、`BadMetric=-17`、`BadEncodeEffortLevel=-18`、
`SurfaceSizeMismatch=-19`、`BufferTooSmall=-21`、`SurfaceTooLarge=-22`、
`BadUniversalTiling=-23`、`LayoutAndUniversalTilingIncompatible=-24`、
`BadSRGBFormat=-25`、`BadSurfaceCount=-26`、`Canceled=-27`。

#### 3.2.2 像素格式 `OodleTex_PixelFormat`（1–21）

`4_U8_RGBA=1`、`4_U8_BGRA=2`、`4_F32_RGBA=3`、`4_F16_RGBA=4`、`3_F32_RGB=5`、
`3_U8_RGB=6`、`3_U8_BGR=7`、`2_U16=8`、`2_S16=9`、`2_U8=10`、`2_S8=11`、
`1_U16=12`、`1_S16=13`、`1_U8=14`、`1_S8=15`、`4_U8_RGBx=16`、`4_U8_BGRx=17`、
`4_U16=18`、`3_U16=19`、`2_F32=20`、`1_F32=21`。

- 整数格式按 UNORM/SNORM 处理；浮点格式供 BC6H 或各 BCn 输入。
- BC7 推荐 `4_U8_RGBA` 输入；`RGBx/BGRx` 与 `RGB` 隐含"忽略 alpha"。

#### 3.2.3 BC 格式 `OodleTex_BC`（1–12）

`BC1=1`、`BC1_WithTransparency=2`、`BC2=3`、`BC3=4`、`BC4U=5`、`BC4S=6`、
`BC5U=7`、`BC5S=8`、`BC6U=9`、`BC6S=10`、`BC7RGBA=11`、`BC7RGB=12`。

- `BC7RGBA`：8-bit UNORM RGBA；`BC7RGB`：忽略 alpha，RGB 质量更高但 A 未定义。
- 每块字节数：BC1/BC4 = 8；其余（含 BC7）= 16。`OodleTex_BC_BytesPerBlock()` 可查。

#### 3.2.4 表面 `OodleTex_Surface`

```c
typedef OOSTRUCT OodleTex_Surface {
    const void * pixels;        // 首行像素指针
    OO_SINTa     rowStrideBytes;// 行间距（字节），≥ width * 每像素字节
    OO_S32       width, height; // 像素尺寸（≤ OODLETEX_MAX_SURFACE_DIMENSION = 2097152）
} OodleTex_Surface;
```

- 行主序（row-major），可带行尾对齐 padding。
- `OodleTex_Surface_NumBlocks(surf) = ((w+3)/4)*((h+3)/4)`；不足 4×4 会对齐复制边缘像素。

#### 3.2.5 关键枚举

| 枚举 | 取值 | 说明 |
| --- | --- | --- |
| `OodleTex_EncodeEffortLevel` | Default=0, Low=10, Normal=20, High=30 | 编码耗时 vs 质量；RDO_Ex 中 `Normal` 约为 `High` 的 1.5–2 倍快 |
| `OodleTex_RDOLagrangeLambda` | NearLossless=1, VeryHighQuality=10, HighQuality=20, **Default=30**, MediumQuality=40, LowQuality=60, Min=1, Max=100 | 参考 λ 档位；**任意 >0 整数均可**（如 5） |
| `OodleTex_BCNFlags` | PreserveExtremes_BC3457=1, BC6_NonRGBData=2, AvoidWideVectors=4, PreferWideVectors=8, LinearToSRGB=16 | BCn 编码位标志 |
| `OodleTex_BCNDecodeFlags` | SRGBToLinear=1 | 解码位标志 |
| `OodleTex_RDO_ErrorMetric` | Default=0(Perceptual), RMSE_RGBA=1, Perceptual_RGBA=2 | RDO 的失真度量 |
| `OodleTex_RDO_UniversalTiling` | Disable=0, 256KB=1, 64KB=2 | 通用分块（跨平台折中） |
| `OodleTex_RDO_Flags` | None=0 | 预留扩展 |

#### 3.2.6 RDO 选项结构 `OodleTex_RDO_Options`

```c
typedef OOSTRUCT OodleTex_RDO_Options {
    OodleTex_EncodeEffortLevel   effort;
    OodleTex_RDO_ErrorMetric     metric;
    OodleTex_BCNFlags            bcn_flags;
    OodleTex_RDO_UniversalTiling universal_tiling;
    OodleTex_RDO_Flags           rdo_flags;
    OO_BOOL                      use_bc3_alpha_lambda;
    OO_S32                       bc3_alpha_lambda;
    OO_U32                       not_yet_used_zero_me[8];
} OodleTex_RDO_Options;
```

- **必须零初始化**（`= {0}`）后只设置所需字段——未初始化的 padding/预留字段会导致输出错乱。
- `use_bc3_alpha_lambda`/`bc3_alpha_lambda` 仅对 BC3 生效。

#### 3.2.7 编码 / 解码 / 工具函数

**编码：**

- `OodleTex_EncodeBCN_Blocks` — 块序输入（4×4 块数组），非 RDO。
- `OodleTex_EncodeBCN_LinearSurfaces` — 线性表面输入，非 RDO。
- `OodleTex_EncodeBCN_RDO` — 旧 RDO 接口（等效 `_Ex` + effort=High，其余默认）。
- `OodleTex_EncodeBCN_RDO_Ex` — **新 RDO 接口（UE 使用）**：
  ```c
  OodleTex_Err OodleTex_EncodeBCN_RDO_Ex(
      OodleTex_BC to_bcn, void* to_bcn_blocks, OO_SINTa num_blocks,
      const OodleTex_Surface* from_surfaces, OO_SINTa num_from_surfaces,
      OodleTex_PixelFormat from_format, const OodleTex_Layout* layout,
      int rdo_lagrange_lambda, const OodleTex_RDO_Options* options,
      int num_job_threads, void* jobify_user_ptr);
  ```

**解码：**

- `OodleTex_DecodeBCN_Blocks` / `_Ex` — 输出到 4×4 块序像素。
- `OodleTex_DecodeBCN_LinearSurfaces` / `_Ex` — 输出到线性表面。
- 注意 BC1–5 解码仅"误差范围内"精确（折中近似，符合 D3D12 容差），BC7 解码确定性。

**BC7Prep（编码侧，在主库）：**

- `OodleTex_BC7Prep_MinEncodeOutputSize(nblocks)`
- `OodleTex_BC7Prep_MinEncodeScratchSize(nblocks)`
- `OodleTex_BC7Prep_Encode(out_header, output_buf, ..., input_bc7, num_blocks, scratch, ...)`
- 编码不分配内存（用 scratch）；返回写出的 payload 字节数。
- 解码在 RT 库 `OodleTexRT_BC7Prep_Decode`（§3.3）。

**Layout（硬件内存布局）：**

- `OodleTex_Layout_Create` / `Destroy`
- `OodleTex_Layout_GenerateBlockIDs` / `SetBlockLayout`（借助控制台 SDK 的 tiling 得到物理块序）
- `OodleTex_Layout_CopyBCNToLinear`

**工具：**

- `OodleTex_RMSE_Normalized_BCNAware` — 归一化、BCn 感知的逐像素 RMSE（U8 尺度，×255）。
- `OodleTex_BlitNormalized` — 归一化像素格式转换。

**插件回调：**

- `OodleTex_Plugins_SetAllocators`（默认 HeapAlloc）
- `OodleTex_Plugins_SetJobSystemAndCount`（默认同步单线程）
- `OodleTex_Plugins_SetPrintf`（默认 debug 用 OutputDebugString）
- `OodleTex_Plugins_SetAssertion`
- `OodleTex_Plugins_SetCancelRequested`（交互式取消）

**版本/名称工具：** `OodleTex_LogVersion`、`OodleTex_Err_GetName`、
`OodleTex_PixelFormat_GetName`、`OodleTex_BC_GetName`、`OodleTex_RDO_UniversalTiling_GetName`、
`OodleTex_BC_BytesPerBlock`、`OodleTex_PixelFormat_BytesPerPixel`、`OodleTex_BC_GetNaturalDecodeFormat`。

> 实测 `dumpbin /exports oo2tex_win64_2.9.16.dll` 共 **43 个导出函数**，与头文件声明一致，
> 全部为无修饰 `extern "C"` 名。

### 3.3 `oodle2texrt.h` — 运行时解码库

- `OodleTexRT_Err`：`OK=0`，`Internal=-101`，BC7Prep 系列 `-102..-107`，GPU 系列 `-108..-109`。
- `OodleTexRT_BC7PrepHeader`（与主库同定义，`OODLETEXRT_BC7PREP_MODE_COUNT=10`）：
  ```c
  typedef OOSTRUCT OodleTexRT_BC7PrepHeader {
      OO_U32 version;
      OO_U32 flags;
      OO_U32 mode_counts[10];   // 8 常规 BC7 模式 + 1 原始透传 + 1 特殊
  } OodleTexRT_BC7PrepHeader;
  ```
- `OodleTexRT_BC7PrepDecodeFlags`：`DestinationIsCached=1`（缓存目标，快 ≤20%）、`AvoidWideVectors=2`。
- 函数：`OodleTexRT_LogVersion`、`OodleTexRT_Err_GetName`、
  `OodleTexRT_BC7Prep_ReadHeader`、`OodleTexRT_BC7Prep_MinDecodeScratchSize`、
  `OodleTexRT_BC7Prep_Decode`。
- 解码同样不分配内存（scratch 复用）；GPU 变体存在块数上限（`TooManyBlocks=-107`）。

---

## 4. RDO 核心概念

### 4.1 Lambda（拉格朗日乘子）

RDO 的目标函数为 `J = D + λ·R`（D = 失真，R = 码率，λ = 权衡系数）：

- λ=0 → 仅最小化失真 = 基线 BC7（与 `OodleTex_EncodeBCN_Blocks` 等价）。
- λ 越大 → 更偏向缩小"二次压缩后体积"，代价是失真增加。
- 文档档位描述：λ=1 近无损（且建议 1 优于 0，常能白捡体积收益）；λ=5–10 仍超高质量；
  λ=10–30 视觉损失很小；λ=30–60 有可见但通常可接受的损失；λ≈50 是"广泛使用且不逐张检查"的上限；
  **λ=30 是推荐的默认起点**；λ=50–100 可进一步压缩但结果不可预测。
- λ 是"码率与失真的汇率"，不是质量参数——同一 λ 在不同图上质量不同（这是有意设计）。

### 4.2 误差度量 `OodleTex_RDO_ErrorMetric`

- `Perceptual`（默认）：把误差放在人眼不敏感处（噪声/高频），保护平滑渐变。
- `RMSE`：单纯最小化各通道平方误差（适合非图像标量场，或用 PSNR/RMSE 度量时对齐目标）。
- 注：用 RMSE/PSNR 度量编解码器性能时，应使用 RMSE 度量，使编解码器与度量目标一致。

### 4.3 Effort Level

`Low/Normal/High` 调编码耗时 vs 质量；`Normal` 相对 `High` 平均省 1.5–2 倍时间、质量略降。
建议迭代期用 Normal/Low，发布/过夜用 High。

### 4.4 Universal Tiling

- 控制台 GPU 常用分块（tiling）内存布局；RDO 结果依赖块存储顺序。
- `256KB/64KB` 通用分块是"跨平台折中"：以单一 RDO 编码缓存供多目标 tiling 复用。
- 指定 `Layout` 时必须用 `Disable`（两者互斥）。

### 4.5 BC7Prep

- 把 BC7 块流再编码为更紧凑的"预处理"流（8 常规模式 + 原始透传 + 特殊，共 10 路模式流）。
- 编码在主库（工具侧）、解码在 RT 库（游戏侧），CPU 或 GPU（compute shader）均可解码。
- 需 `scratch` 工作缓冲，编解码均不动态分配内存；多线程需各自独立的 scratch。

---

## 5. UE 集成方式（`TextureFormatOodle` 模块）

来源：`Engine/Source/Developer/TextureFormatOodle/Source/Private/TextureFormatOodle.cpp`
（约 72 KB）+ `TextureFormatOodle.Build.cs`。

### 5.1 动态加载（非静态链接）

UE 通过 `FPlatformProcess::GetDllExport` 在运行时解析 Oodle 导出，维护一个函数指针 VTable：

```cpp
fp_OodleTex_EncodeBCN_RDO_Ex = (t_fp_OodleTex_EncodeBCN_RDO_Ex *)
    FPlatformProcess::GetDllExport(DynamicLib, TEXT("OodleTex_EncodeBCN_RDO_Ex"));
check(fp_OodleTex_EncodeBCN_RDO_Ex != nullptr);
```

> 与本报告测试环境采用的 `LoadLibrary + GetProcAddress` 思路完全一致。

### 5.2 参数映射

`GetOodleCompressParameters(...)` 把 UE 的构建设置映射为 Oodle 参数：

| UE 输入 | Oodle 输出 | 逻辑 |
| --- | --- | --- |
| `LossyCompressionAmount`（有损压缩量） | `rdo_lagrange_lambda` | `RDOLambda = clamp(OodleRDO, 0, 100)`；≤0 时置 1；受 `GlobalLambdaMultiplier` 缩放（默认 1.0） |
| `OodleEncodeEffort` | `effort` | 直接强转 `OodleTex_EncodeEffortLevel`，非法值回退 Default |
| `OodleUniversalTiling` | `universal_tiling` | 直接强转，非法值回退 Disable |
| `bOodlePreserveExtremes` | `bcn_flags` | 置 `OodleTex_BCNFlag_PreserveExtremes_BC3457` |
| RDOLambda==0 | — | 走非 RDO 路径，且强制 Disable tiling |

- `GlobalLambdaMultiplier`（`[TextureFormatOodle] GlobalLambdaMultiplier`，默认 1.0）全局缩放所有 λ。
- DDC 缓存键含 `Oodle_CPF%d_L%d_E%d`（+ `_UT%d`），保证不同 λ/effort/tiling 各自缓存。

### 5.3 调用点

```cpp
OodleOptions.effort = EffortLevel;
OodleOptions.metric = OodleTex_RDO_ErrorMetric_Default;
OodleOptions.universal_tiling = RDOUniversalTiling;
// lambda==0 时走非 RDO 编码；否则：
OodleTex_Err OodleErr = (VTable->fp_OodleTex_EncodeBCN_RDO_Ex)(
    OodleBCN, OutSlicePtr, NumBlocksPerSlice,
    &InSurf, 1, OodlePF, NULL, RDOLambda, &OodleOptions, ...);
```

### 5.4 任务系统

`OodleTex_Plugins_SetJobSystemAndCount(TFO_RunJob, TFO_WaitJob, OodleJobifyNumThreads)`
接入 UE 的 jobify 线程池（`example_jobify.cpp` 提供参考实现）；纹理 BCN 编码建议
`target_parallelism` = 全部超线程数。

---

## 6. 与 Unity `bc7enc_rdo` 方案的对比

| 维度 | Oodle Texture（UE） | 本仓库（Unity 壳 DLL） |
| --- | --- | --- |
| 编码器 | Epic 官方闭源 `oo2tex` | 开源 `bc7enc_rdo`（ERT 后处理） |
| 接入方式 | UE 内置 `TextureFormatOodle` 模块 | 替换 `compress_bc7e.dll` / Editor Plugin 注入 |
| RDO 目标函数 | `J = D + λ·R`，R=LZ 比特 | `J = D + λ·R`，R=LZ 比特（同构） |
| λ 定义域 | 整数 0–100，默认 30 | 浮点，建议 0.25–4.0，默认 1.0 |
| D 定义 | 整块 16 像素 SSD（VQD） | 逐像素平均 MSE（÷16） |
| 失真度量 | Perceptual/RMSE 可选 | 固定 MSE 近似 |
| 硬件布局 | 支持 Layout + Universal Tiling | 仅线性块序 |
| BC7Prep | 有（主库编码 + RT 解码） | 无 |

### 6.1 λ 尺度换算（源码级，见 `docs/OODLE_LAMBDA_COMPARISON.md`）

两端目标函数同构，差别只在 λ 的尺度定义：

```
λ_ert = λ_oodle_internal / 16
      = (λ_UE × 0.6) / 16
      ≈ λ_UE × 0.0375
```

- Oodle 公开 λ 先经内部缩放 ×0.6（`BC7RD_LAMBDA_SCALE`）；D 单位再差 16 倍（整块 SSD vs 逐像素平均 MSE）。
- 结论：**UE/Oodle λ=30 ↔ 本库 λ≈1.0–1.1**（默认值 1.0 正好命中）。
- 档位对照：λ_UE 10/20/30/40 ↔ λ_ert 0.4/0.75/1.1/1.5。

### 6.2 实测交叉验证（本报告新增）

`docs/OODLE_LAMBDA_COMPARISON.md` §6 曾因"源码构建输出损坏"未能用官方库补全实测曲线。
本报告通过 `oodle_testenv/` 直接加载**官方预编译 `oo2tex_win64_2.9.16.dll`**，
产出正确的率失真曲线（见 §7），解决了该遗留问题。

---

## 7. 实测结果（官方预编译 DLL）

测试环境：`../oodle_testenv/`。条件：1024×1024 程序化 RGBA8 测试图，
`OodleTex_EncodeBCN_RDO_Ex`，effort=High，metric=Default（Perceptual），单线程（确定性），
`OodleTex_BC7RGBA`；λ=0 走 `OodleTex_EncodeBCN_Blocks` 基线。体积列为 BC7 块流再经 zlib9 压缩。

| λ | BC7 字节 | zlib9 字节 | 相对基线 | RMSE | PSNR (dB) |
| --- | ---: | ---: | ---: | ---: | ---: |
| 0（基线） | 1,048,576 | 827,908 | — | 1.764 | 43.20 |
| 1 | 1,048,576 | 762,926 | −7.8% | 1.788 | 43.09 |
| 5 | 1,048,576 | 629,364 | −24.0% | 1.957 | 42.30 |
| 10 | 1,048,576 | 558,175 | −32.6% | 2.079 | 41.77 |
| 20 | 1,048,576 | 507,207 | −38.7% | 2.198 | 41.29 |
| **30（默认）** | 1,048,576 | **488,652** | **−41.0%** | **2.273** | **41.00** |
| 40 | 1,048,576 | 476,701 | −42.4% | 2.339 | 40.75 |
| 60 | 1,048,576 | 461,877 | −44.2% | 2.481 | 40.24 |
| 100 | 1,048,576 | 443,458 | −46.4% | 2.744 | 39.36 |

**结论：**

1. BC7 每块 16 字节恒定，λ 不改变 BC7 原始大小（都是 1,048,576 B），收益完全体现在
   **二次压缩（LZ）** 阶段——与"BC7 Crunched"的率失真本质一致。
2. 最大收益出现在早期（λ 1–10：−33% 体积、仅 −1.4 dB），符合文档"λ 小档即可白捡体积"。
3. **λ=30 默认点：−41.0% 体积、PSNR 仍 41 dB**，验证了"λ=30 是良好默认"的文档描述。
4. λ 超 40 后收益递减而失真加速，验证"λ≈50 是广泛使用上限"的建议。
5. 输出**非错乱**（λ=0 的 RMSE 1.764 ≈ BC7 固有基线损失，PSNR 43.2 dB），确认官方 DLL 编码/解码链路正确。
6. 确定性验证：同一扫描两次运行 `sweep.csv` SHA256 完全一致。

> 注意：本节曲线与 `docs/OODLE_LAMBDA_COMPARISON.md` 的换算不直接点对点对比——
> 二者测试图不同（程序化 1024² vs 照片 256²），λ 尺度换算（×0.0375）是**源码级推导**，
> 独立于图像内容成立。本节价值在于用官方库补齐了正确的 Oodle 实测曲线并验证默认 λ 行为。

---

## 8. 许可与再分发

- Oodle 是 **Epic Games 专有闭源**库，随 UE 授权分发；`redist/` 的 DLL 可用于 UE 项目运行时。
- 脱离 UE 授权、或用于其他引擎/商业产品，需另行取得 Epic 的 Oodle 许可。
- 本仓库的 `oodle_testenv/` 仅作技术研究与对照实验，**不将 DLL 纳入版本库**（`.gitignore` 已排除
  `bin/` 与 `out/`），并复制自本机 UE 5.8 安装目录，仅限本地使用。
- 参考仓库 `masagrator/UnrealOodleWrapper` 实际封装的是 Oodle **Data**（Kraken/Leviathan）
  而非 Oodle Texture，与 BC7 RDO 无关（其 README 同样注明 Oodle 为 Epic 专有、需授权）。

---

## 9. 附录：参考资料与测试环境

- **测试环境**：`../oodle_testenv/`（`src/oodle_bc7.cpp` 壳 + `tools/build.ps1` + `run_sweep.py`，
  详见其 `README.md`）。
- **实测产物**：`../oodle_testenv/out/sweep_ratedistortion.csv`、`out/report.md`。
- **UE 集成源码**：`Engine/Source/Developer/TextureFormatOodle/Source/Private/TextureFormatOodle.cpp`。
- **λ 换算**：`../docs/OODLE_LAMBDA_COMPARISON.md`。
- **接口头文件**：SDK `include/oodle2tex.h`（1459 行，逐字段文档，权威参考）。
