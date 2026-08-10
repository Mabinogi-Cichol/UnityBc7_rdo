# Unity BC7 RDO 实验：定制执行计划与结果

> 本文件把 `plan.md` 的思路定制为本机可执行、且**不触碰 Unity 安装目录**的安全实验计划，并记录执行结果。

## 1. 定制原则

原计划的核心是“逆向 `compress_bc7e.dll` 接口 → 编写伪装 DLL → 注入 RDO 编码器”。针对本项目做了以下定制：

| 原计划做法 | 本项目定制 |
| --- | --- |
| 直接分析/替换 Unity 安装目录下的 `compress_bc7e.dll` | **禁止**。只把原 DLL 复制到项目 `vendor/compress_bc7e_original.dll`，所有分析、验证都在项目内进行 |
| 编写完全独立的 BC7+RDO 编码器并整体替换 | 采用**“转发 + 后处理”**架构：默认把调用转发给项目内的原 DLL 副本（基线输出逐字节一致），RDO 开启时对 BC7 块流做 ERT 率失真后处理 |
| 侧信道靠环境变量 | 支持环境变量 **和** 配置文件 `bc7_rdo_config.txt`（文件修改后自动重读），环境变量优先级更高 |
| 直接替换后重启 Unity 验证 | 用 Python ctypes 测试台在项目内验证 ABI 兼容、字节一致性、压缩率与质量曲线 |

## 2. 环境

- Unity：`C:\Program Files\Unity\Unity 2022.3.62f1\Editor\compress_bc7e.dll`
  - 大小 1,227,776 字节，SHA-256 `83F9BF925C65CCBDF93610CDEFF1AB12A3169C9813D4BFAE85F96035AF04AB33`
- 工具链：Visual Studio 18 Community（MSVC 14.51），`vcvars64.bat`
- Python：Codex 自带运行时（3.12.13，含 PIL）
- 开源依赖：`richgel999/bc7enc_rdo`（ERT + BC7 解码器，MIT/Public Domain）

## 3. 执行步骤与结果

### 3.1 复制原始 DLL 到项目

已复制到 `vendor/compress_bc7e_original.dll` 并记录哈希（见上）。Unity 安装目录未被修改。

### 3.2 逆向接口

- 使用 `dumpbin /exports` 得到 10 个导出函数，与开源 `bc7e`（BinomialLLC/bc7e）API 一一对应，仅加了 `bc7elib_` 前缀：
  `bc7elib_compress_block_init`、8 个 `bc7elib_compress_block_params_init*`、`bc7elib_compress_blocks`。
- 反汇编导出区确认导出项是跳转桩（RVA 0x10B300 起，每 16 字节一个 `jmp`），真实实现在 0x1200–0x15C0。
- 反汇编参数初始化函数确认签名与结构体偏移，与 `bc7enc_rdo/bc7e.ispc` 中的 `bc7e_compress_block_params` 完全一致（大小 124 字节）。
- 确认 DLL 内部按 CPU 能力分派多个 ISPC 目标实现（SSE2/SSE4/AVX/AVX2）。

详细证据见 [ANALYSIS.md](ANALYSIS.md)。

### 3.3 壳 DLL

- 源码：`src/compress_bc7e_rdo.cpp`、`src/bc7elib.h`
- 构建：`tools/build.ps1` → `out/compress_bc7e.dll`（同时把原 DLL 副本放入 `out/`）
- 行为：
  1. 默认**转发**给 `compress_bc7e_original.dll`（查找顺序：环境变量 `BC7E_ORIGINAL_DLL` → 本 DLL 同目录 → 工作目录）；
  2. 若 `BC7_RDO_LAMBDA > 0`，对压缩后的块流调用 `ert::reduce_entropy` 做 RDO 后处理；
  3. 额外导出 `bc7elib_debug_unpack_blocks` 供测试解码（Unity 不会调用）。

### 3.4 验证

`tests/test_compat.py`（报告：`out/test_report.json`）：

| 项目 | 结果 |
| --- | --- |
| 导出表 | 10/10 与原 DLL 一致 |
| 参数结构体（8 预设 × 2 感知模式） | 16/16 与原始 DLL 逐字节一致 |
| 转发压缩输出（3 纹理 × 4 预设 × 2 批大小） | 24/24 与原始 DLL 逐字节一致 |
| RDO 压缩收益（photo 256×256, basic, perceptual） | λ=0.5：zlib9 −4.86%（PSNR −0.86 dB）；λ=1.0：−8.29%（−2.39 dB）；λ=4.0：−25.71%（−8.55 dB） |
| 配置文件侧信道 | `bc7_rdo_config.txt` 写入 λ=1.5 后生效（zlib9 48,817 B，比基线 −12.1%） |

对比图已生成：
- `out/test_photo_source.png`
- `out/test_bc7_baseline.png`
- `out/test_bc7_rdo_lambda1.png`

## 4. 结论

1. Unity 2022.3 的 BC7 压缩接口已被完整复刻（导出名、参数结构体、调用方式），壳 DLL 可作为原 DLL 的**逐字节等价**替代品用于实验。
2. ERT RDO 后处理在结构化纹理上显著降低后续 LZ 压缩体积，λ 越大体积越小、质量下降越多，是一条可用的“BC7 Crunched 式”率失真曲线。
3. 由于 BC7 每块固定 16 字节，RDO 的收益体现在**块流被再次压缩时**（如 AssetBundle 的 LZ4/LZMA 阶段），而非 BC7 原始数据本身。

## 5. 后续可选步骤（均不修改 Unity 目录）

- 用 `AssetPostprocessor` 在纹理导入前把 λ 写入环境变量/配置文件，实现“导入时按纹理设置 RDO 强度”。
- 对比不同 λ 下 AssetBundle（LZ4/LZMA）实际体积与视觉质量，确定默认 λ。
- 若要完整替代编码器（不依赖原 DLL），可用 `bc7enc_rdo` 的 `bc7e.ispc` + ISPC 编译器重新实现 `bc7elib_compress_blocks`。

## 6. 风险与合规提醒

- 本实验产物**没有**替换 Unity 安装目录中的任何文件；若未来要在编辑器内实际使用，替换行为本身可能违反 Unity EULA，且不同 Unity 版本的 DLL 接口可能变化，请自行评估。
- ERT 后处理假定调用是线程安全的；本项目未做并发压测。
