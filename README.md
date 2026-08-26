# Unity BC7 RDO 实验（项目内安全版）

针对 Unity 2022.3 的 `compress_bc7e.dll`，在**可选的替换（支持一键回滚）**前提下，实现并验证：

1. 完整复刻其导出接口（10 个 `bc7elib_*` 函数）；
2. 默认转发到项目内的原 DLL 副本，压缩输出逐字节一致；
3. 可选开启 RDO 后处理（bc7enc_rdo 的 ERT），让 BC7 块流对 LZ 压缩更友好，等效于“BC7 Crunched”的率失真权衡。

## 目录

```text
vendor/
  compress_bc7e_original.dll    # 从 Unity 目录复制的原 DLL（分析/转发用，只读）
  bc7enc_rdo/                   # 开源 RDO 编码器（ERT + BC7 解码器，仅保留构建所需文件）
src/
  bc7elib.h                     # 与原 DLL 一致的接口/结构体定义
  compress_bc7e_rdo.cpp         # 壳 DLL 实现
tools/
  build.ps1                     # 一键构建
  enable_unity_rdo.ps1          # Unity 中 DLL 的替换/回滚脚本
  unity_dirs.ps1                # 脚本依赖的 Unity 安装目录检测模块
tests/
  test_compat.py                # 兼容性与 RDO 验证
docs/
  CUSTOM_PLAN.md                # 定制计划与结果
  ANALYSIS.md                   # 接口逆向分析
out/                            # 构建产物与测试报告（git 忽略）
```

## 构建

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

详细的前置要求、原理、常见问题见 [docs/BUILD.md](docs/BUILD.md)。

产物：

- `out/compress_bc7e.dll` —— 壳 DLL（导出与原 DLL 一致 + 1 个调试导出）
- `out/compress_bc7e_original.dll` —— 原 DLL 副本（壳 DLL 转发目标）
- `out/exports.txt` —— 导出表自检

> 注意：`vendor/compress_bc7e_original.dll` 是 Unity 专有二进制，**不纳入版本库**。
> 克隆/切换环境后运行 `tools/build.ps1` 会自动检测 Unity 安装目录恢复副本
> （优先用已替换目录里的 `compress_bc7e_original.dll`，其次原版
> `compress_bc7e.dll`）；也可用 `tools\build.ps1 -UnityDir` 手动指定
> Unity Editor 目录。

## 发布 Release

```powershell
powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 -Version v0.1.0
```

脚本先构建，然后**分别生成两个独立发布包**：

1. **壳 DLL 版**（替换 Unity 目录，需管理员，支持一键回滚）：
   `release\v0.1.0\` 目录 → `release\compress_bc7e_rdo-v0.1.0-win-x64.zip`
   （含壳 DLL、安装/回滚脚本、配置示例、README、导出表与 SHA256SUMS）。
2. **Editor Plugin 版**（放进工程 `Assets` 即生效，无需管理员）：
   `release\v0.1.0-editor-plugin\` 目录 →
   `release\compress_bc7e_rdo-editor-plugin-v0.1.0-win-x64.zip`
   （含 hook_loader.cs、asmdef、hook DLL、配置、README 与 SHA256SUMS）。

打完包后可用 git 标签标记版本：

```powershell
git tag -a v0.1.0 -m "Unity BC7 RDO v0.1.0"
git push origin main --tags
```

若要发布到 GitHub Releases（需安装 `gh` CLI），两个包作为**独立资产**一并附带：

```powershell
gh release create v0.1.0 `
  release/compress_bc7e_rdo-v0.1.0-win-x64.zip `
  release/compress_bc7e_rdo-editor-plugin-v0.1.0-win-x64.zip
```

Release Notes 应分别说明两个包的用法：壳 DLL 版运行 `enable_unity_rdo.ps1`
替换/回滚；Plugin 版把目录复制进工程 `Assets` 即生效。

## 测试

```powershell
python tests\test_compat.py
```

测试内容：

- 10 个导出函数齐全；
- 8 个质量预设 × 2 种感知模式的结构体初始化结果与原 DLL 逐字节一致；
- 3 种纹理 × 4 预设 × 2 种批大小的转发压缩输出与原 DLL 逐字节一致；
- RDO 率失真曲线（zlib9 体积 vs PSNR）；
- 配置文件侧信道。

报告：`out/test_report.json`；对比图：`out/test_photo_source.png`、`out/test_bc7_baseline.png`、`out/test_bc7_rdo_lambda1.png`。

## 使用（替换 Unity 中的 DLL）

三步走：把 release 包解压到任意目录 → 以管理员身份运行 `enable_unity_rdo.ps1`
（自动检测 Unity 安装目录，多个版本时按编号选择）→ 选"替换"。

1. 解压 release 包（如 `release\compress_bc7e_rdo-v0.1.0-win-x64.zip`）到任意目录；
2. **以管理员身份**运行 `enable_unity_rdo.ps1`，脚本自动检测 Unity 安装目录
   （`C:\Program Files\Unity` 下直接安装与 `Hub\Editor` 两种布局；多版本时编号选择，
   也可用 `-UnityDir` 手动指定）；
3. 选择"替换"，按提示输入 lambda（默认 1.0）。脚本自动完成：
   把 Unity Editor 目录中的原版 `compress_bc7e.dll` 改名为 `compress_bc7e_original.dll`
   → 复制壳 DLL 为 `compress_bc7e.dll` → 写入 `bc7_rdo_config.txt`；
4. 完全退出并重启 Unity，重新导入纹理。

**修改 lambda**：直接编辑 Unity Editor 目录中（与壳 DLL 同目录）的
`bc7_rdo_config.txt`，保存后自动生效，无需重启。

**已替换检测**：目标 Unity Editor 目录存在 `compress_bc7e_original.dll` 即表示已替换。

**回滚**：再次运行 `enable_unity_rdo.ps1`，在菜单中选"回滚"即可一键恢复原状
（删除壳 DLL → 把 `compress_bc7e_original.dll` 改名回 `compress_bc7e.dll` → 删除
`bc7_rdo_config.txt`）。

壳 DLL 只读取**与自身同目录**的 `compress_bc7e_original.dll`（转发目标）与
`bc7_rdo_config.txt`（RDO 配置），无任何环境变量。

详见 [docs/USING_WITH_UNITY.md](docs/USING_WITH_UNITY.md)。

## 外挂形式（Editor Plugin，不修改安装目录）

不想动 Unity 安装目录？仓库提供 **Editor Plugin** 形式，效果与替换版相同
（Unity 安装目录零写入、无需管理员）：

1. 把 release 的 editor-plugin 包（或构建产物 `out\editor-plugin\Bc7Rdo\`）
   整个目录复制到工程 `Assets` 下任意位置（含 C# 脚本 +
   `Bc7Rdo.Editor.asmdef` + hook DLL + 配置）。**release 包不含任何 Oodle
   DLL**；`out\editor-plugin\Bc7Rdo\` 本地暂存时可能含（见"编码器切换"）；
2. 打开/刷新工程即自动生效：加载 hook DLL → 自动装钩 → RDO，无需管理员；
   asmdef 将脚本编入 Editor-only 程序集（自动引用 UnityEditor），放
   `Assets\Plugins` 下也不会编译报错；hook DLL 与 Oodle DLL 会被自动设为
   仅编辑器平台，不会打进玩家包；
3. 并行导入的 AssetImportWorker 子进程会被自动监控并注入，批量 reimport
   同样生效。

**编码器切换**：插件默认使用 `bc7enc_rdo`（ERT 后处理，开源、随包分发）。
在插件目录 `bc7_rdo_config.txt` 中把 `encoder` 改为 `oodle` 即可切换到
**Oodle Texture RDO**（需把两个 Epic 专有 DLL `oo2tex_win64_2.9.16.dll` 与
`oo2texrt_win64_2.9.16.dll` 放进插件目录，见下方许可说明）。两种编码器
使用**各自独立**的 RDO 强度旋钮，互不换算：`lambda`（浮点，ERT）只作用于
`bc7enc_rdo`；`oodle_lambda`（整数 0..100，默认 30）只作用于 `oodle`；
`oodle_effort=low|normal|high` 调节 Oodle 耗时。两编码器档位对照参考见
[docs/OODLE_LAMBDA_COMPARISON.md](docs/OODLE_LAMBDA_COMPARISON.md)
（λ_ert ≈ λ_oodle×0.0375）。**Oodle 不可用（DLL 缺失或编码失败）时，hook
自动回退到默认 `bc7enc_rdo` / ERT RDO 路径**——调用原 bc7e 压缩后接 ERT
后处理，与 `encoder=bc7enc_rdo` 行为完全一致，而非降级为无 RDO 的普通 bc7e。

配置 `bc7_rdo_config.txt` 与插件同目录热更新；菜单
`Tools/BC7 RDO/Reload Hook DLL`、`Smoke Test` 可手动重载/冒烟；卸载 =
删除该目录。方案评估与细节见
[docs/INJECTION.md](docs/INJECTION.md)。

> **Oodle 许可与获取**：`oo2tex*_win64_2.9.16.dll` 是 Epic 专有二进制，
> **既不在本仓库中**（已 gitignore，不提交），**也不包含在任何 release
> 发布包中**。需要时请从获得授权的 Oodle SDK 获取两个 DLL，按下列任一
> 手动方式放置后即可启用 `encoder=oodle`：
> - **本地构建暂存**：把两个 DLL 拷进 `oodle_testenv\bin\`，再运行
>   `tools\build.ps1`，本地构建会将其暂存到 `out\editor-plugin\Bc7Rdo\`
>   供本地测试/安装；
> - **发布包安装后**：解压 editor-plugin 包并复制进工程 `Assets` 后，
>   把两个 DLL 直接拷进插件目录（与 `bc7_rdo_hook.dll` 同目录）。

## 实测结果（photo 256×256，basic，perceptual）

| λ | zlib9 体积 | 相比基线 | PSNR | 改变块比例 |
| --- | --- | --- | --- | --- |
| 0（基线） | 55,520 B | — | 41.28 dB | 0% |
| 0.25 | 54,122 B | −2.52% | −0.27 dB | 37.5% |
| 0.5 | 52,824 B | −4.86% | −0.86 dB | 47.3% |
| 1.0 | 50,916 B | −8.29% | −2.39 dB | 58.8% |
| 2.0 | 47,753 B | −13.99% | −4.93 dB | 71.1% |
| 4.0 | 41,243 B | −25.71% | −8.55 dB | 85.5% |

## 重要说明

- 替换行为由脚本完成：把 Unity 目录中的原版 DLL 改名为 `compress_bc7e_original.dll`
  并装入壳 DLL，支持一键回滚。壳 DLL 只调用与自身同目录的原 DLL 副本，无任何环境变量。
- BC7 每块固定 16 字节，RDO 收益体现在块流被 LZ 再次压缩时（如 AssetBundle 的 LZ4/LZMA 阶段）。
- 若要在 Unity 编辑器内实际替换 DLL，请先自行评估 Unity EULA 与版本兼容风险；本项目仅提供可验证的实验实现。
- 第三方代码许可：`bc7enc_rdo`（MIT / Public Domain，见 `vendor/bc7enc_rdo/LICENSE`），`bc7e`（Apache-2.0，仅作对照参考，未编译进壳 DLL）。
