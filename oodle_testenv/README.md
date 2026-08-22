# oodle_testenv — BC7 RDO 测试环境（Oodle Texture 2.9.16 官方预编译 DLL）

## 用途

本目录是一个自包含、可运行的 BC7 RDO 测试环境：直接加载 **UE 5.8 自带的官方预编译
Oodle Texture 2.9.16 DLL**（不做任何源码重建），对一张确定性的程序化 RGBA8 测试图做
一组 lambda 的 BC7 RDO 编码扫描，解压回 RGBA 计算 RMSE/PSNR，并测量 zlib9/lzma
压缩大小，输出率失真曲线数据。

> 注意：DLL 是 Epic 专有产物。这里仅用于本地测试；若要对外分发，需要获取正式的
> Oodle 授权。之前的"从源码重建 Oodle Texture"方案输出损坏（乱码），因此本环境
> 改为直接使用官方预编译 DLL，并通过"lambda=0 时 RMSE≈0"等校验确认输出正确。

## 前置条件

- Windows x64
- Visual Studio（含 C++ 桌面工具链，vswhere 可发现）——用于编译 `oodle_bc7.exe`
- Python 3（仅用标准库，无需 numpy/PIL）

## 构建

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

通过 `vswhere` 定位 VS，经 `vcvars64.bat` 调用 `cl` 把 `src\oodle_bc7.cpp`
编译为 `out\oodle_bc7.exe`，并把 `bin\` 下的两个 DLL 复制到 `out\` 紧挨 exe，
供运行时 `LoadLibrary` 使用（不链接任何 .lib）。

## 运行

```powershell
python run_sweep.py
```

该脚本会自动（如 exe 缺失则先构建）执行：

```
oodle_bc7.exe sweep out --size 1024 --lambdas 0,1,5,10,20,30,40,60,100
```

然后对每个 `out\bc7_l<lambda>.bin` 计算 `zlib.compress(data,9)` 与
`lzma.compress(data, preset=9|PRESET_EXTREME)` 大小，计算 PSNR 与相对 lambda=0
的 zlib9 压缩节省百分比，写入：

- `out\sweep_ratedistortion.csv` —— 率失真数据表
- `out\report.md` —— 率失真 Markdown 表格 + 趋势说明

## 文件说明

| 路径 | 说明 |
|---|---|
| `include/` | 官方 SDK 头文件（oodle2base.h / oodle2tex.h / oodle2texrt.h，原样拷贝） |
| `bin/` | 官方预编译 DLL（oo2tex_win64_2.9.16.dll + oo2texrt_win64_2.9.16.dll） |
| `src/oodle_bc7.cpp` | C++ 测试程序：LoadLibrary + GetProcAddress，生成测试图并做 lambda 扫描 |
| `tools/build.ps1` | 构建脚本（cl -> out\oodle_bc7.exe，复制 DLL） |
| `run_sweep.py` | 驱动脚本：构建+扫描+压缩测量+CSV/报告 |
| `out/` | 生成产物：exe、bc7_*.bin、sweep.csv、sweep_ratedistortion.csv、report.md（git 忽略） |

## 直接调用 C++ 程序

```
out\oodle_bc7.exe sweep <outdir> --size 1024 [--lambdas 0,1,5,10,20,30,40,60,100]
```

- lambda=0 走 `OodleTex_EncodeBCN_Blocks`（基线非 RDO）；lambda>0 走
  `OodleTex_EncodeBCN_RDO_Ex`，`OodleTex_RDO_Options` 零初始化后仅设置
  `effort=High`、`metric=Default`、`universal_tiling=Disable`、`bcn_flags=None`、
  `rdo_flags=None`；`num_job_threads = OODLETEX_JOBS_DISABLE(-1)` 保证单线程确定性。
- 编码后经 `OodleTex_DecodeBCN_Blocks` 解回 RGBA，用
  `OodleTex_RMSE_Normalized_BCNAware` 计算逐像素 U8 尺度的 RMSE。
- 输出 `sweep.csv`（lambda,blocks,bc7_bytes,rmse）与 `bc7_l<lambda>.bin`，内容确定性。

## 校验

- lambda=0 的 RMSE≈0（BC7 基线本身有少量损失），且 `bc7_bytes = blocks × 16`；
- lambda 增大 → zlib9 大小单调不增、RMSE 单调不减（符合率失真曲线预期）。
