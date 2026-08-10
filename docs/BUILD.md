# 构建说明

本文档说明如何在本机从源码构建 `compress_bc7e.dll`（壳 DLL）并生成验证产物。

## 1. 前置要求

- Windows x64
- Visual Studio 2022/2026（或 Build Tools），需包含 **“使用 C++ 的桌面开发”** 组件（MSVC x64 编译器 `cl.exe`）
- PowerShell 5.1+（推荐 PowerShell 7）
- 仅运行测试时需要 Python 3.10+（可选，测试报告与对比图依赖 PIL；无 PIL 时测试会失败）
- `vendor/bc7enc_rdo/` 已随仓库提供（构建所需的三方源码）
- 原 DLL 副本：`vendor/compress_bc7e_original.dll` 不纳入 git，构建脚本会自动恢复：
  1. 优先使用环境变量 `UNITY_BC7E_DLL` 指向的 Unity 原 DLL；
  2. 否则从 `C:\Program Files\Unity\Unity 2022.3.62f1\Editor\compress_bc7e.dll` 复制；
  3. 若都不存在，构建失败并提示。

## 2. 一键构建

在仓库根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

脚本完成以下步骤：

1. 用 `vswhere` 自动查找带 VC 工具链的 Visual Studio 实例，并定位 `vcvars64.bat`；
2. 编译三个源文件：
   - `src/compress_bc7e_rdo.cpp`（壳 DLL 主逻辑）
   - `vendor/bc7enc_rdo/ert.cpp`（RDO 后处理）
   - `vendor/bc7enc_rdo/bc7decomp.cpp`（BC7 解码，供 ERT 使用）
3. 生成 `out/compress_bc7e.dll`；
4. 把原 DLL 副本复制到 `out/compress_bc7e_original.dll`（转发目标）；
5. 用 `dumpbin /exports` 导出导出表到 `out/exports.txt` 供自检。

## 3. 产物

| 文件 | 说明 |
| --- | --- |
| `out/compress_bc7e.dll` | 壳 DLL，导出与原 DLL 一致（+1 个测试用调试导出） |
| `out/compress_bc7e_original.dll` | Unity 原 DLL 的项目内副本（转发/回退目标） |
| `out/exports.txt` | 导出表 |
| `out/test_report.json` | 运行测试后的验证报告 |

## 4. 运行测试

```powershell
python tests\test_compat.py
```

测试覆盖：导出表存在性、参数结构体逐字节一致性、转发压缩输出逐字节一致性、RDO 率失真曲线、配置文件侧信道。

## 5. 常见问题

### cl.exe 找不到 / 构建脚本报“No Visual Studio C++ toolchain found”

安装 Visual Studio 的“使用 C++ 的桌面开发”工作负载，或安装 VS Build Tools 后重试。

### 报错“缺少 vendor\compress_bc7e_original.dll”

确认本机 Unity 版本为 2022.3.62f1，或用 `UNITY_BC7E_DLL` 指定原 DLL 路径：

```powershell
$env:UNITY_BC7E_DLL = "C:\Program Files\Unity\Unity 2022.3.62f1\Editor\compress_bc7e.dll"
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

### 测试报 PIL 相关错误

`python tests\test_compat.py` 依赖 Pillow 生成测试纹理；若未安装：

```powershell
pip install pillow
```

## 6. 发布 Release 包

```powershell
powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 -Version v0.1.0
```

脚本会先执行构建，然后在 `release\v0.1.0\` 生成：

- `compress_bc7e.dll`
- `compress_bc7e_original.dll`（运行时转发目标，Unity 专有二进制，仅限本地使用，禁止公开分发）
- `bc7_rdo_config.txt.example`
- `README-release.txt`
- `exports.txt`
- `SHA256SUMS.txt`

并打包为 `release\compress_bc7e_rdo-v0.1.0-win-x64.zip`。
