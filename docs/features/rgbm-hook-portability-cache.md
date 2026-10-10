# P1：动态绑定认证、受限扫描和缓存

- 日期：2026-10-10（Asia/Shanghai）
- 首个接口：`unity.threaded-copy-for-each-callback.v1`
- 状态：该接口的 P1 交付项已实现并验证；[P2 后端](rgbm-hook-portability-detours.md)与 [P3 运行时](rgbm-hook-portability-runtime.md)关口已通过，项目部署和逐进程加载器接入仍待 P4。
- 关联：[P0 接口证据](rgbm-hook-portability-interface.md)、[施工记录](rgbm-hook-portability-construction.md)

## 1. 实现范围

`src/rgbm_hook_contract.{h,cpp}` 提供统一认证器，DIA 主路径、无 PDB 扫描和缓存读取均复用它。业务源码没有 f1／f2 RVA 表或版本号白名单。首个接口描述从两个匹配 PDB 的 P0 报告建立，保存在 `rgbm_hook_profile.h`；开发工具 `derive_profile.py` 必须先通过独立 P0 证明，再要求两版全部形态一致，不能从未知缓存自动学习新形态。

本接口采用保守的完整核心形态与语义边校验，明确只支持已经取证的编译形态及最多 16 个入口前单字节 NOP。地址／RIP 位移变化允许；入口前增加 NOP 的合成变体通过。内部条件／无条件分支保留，不把改变控制流的跳转位移任意清零。其他寄存器保存方式、未知编译器产物、不同调度结构仍拒绝，不声称已经实现通用 x64 寄存器数据流求解器。

认证必须同时满足：

1. exe 的 x64 PE、可执行节、CodeView 身份和 `.pdata` 边界有效；DIA 路径还要求 PDB GUID／Age 匹配、唯一压缩／process／usage 入口、精确的压缩入口九参数公有签名。
2. 压缩入口的 BC7 调用点通过 R8 提供正确三参数 forwarder；process、forwarder、lambda 的核心形态有效，lambda 实际指向 `compress_bc7e.dll!bc7elib_compress_blocks` 导入。
3. process 的复制／调度／条件等待核心有效，唯一 dispatcher 和顺序的 Schedule／Complete／Clear 目标分别通过自身认证。
4. Complete 到唯一 JobGroup wait 的调用有效；wait 的尾跳目标也必须匹配已取证的底层等待体，不能把转发体形态相同当作任意目标都安全。
5. 原生 usage 查询核心形态有效，保留 P0 的 2／5／9 语义；11 个角色地址各不相同。

指令长度来自 Detours 的解码器。取证过程中发现普通 `DetourCopyInstruction` 会在 `FF 15/25` 上尝试跟随内存指针，磁盘缓冲区并非已加载映像。新的长度适配器仅在私有副本里暂时改为同地址长度的 `FF /0`，取长度后恢复字节；不读取指针、不采用该指令的重定位结果、不改上游 Detours 或目标代码。异常／超长解码拒绝；原样保留和不可读指针回归测试通过。

## 2. PDB 主路径和扫描后备

已有 `bc7_rgbm_resolver.exe` 默认仍生成不可安装的 `evidence_report`。增加 `--cache <path>` 后，在匹配 PDB 的候选及签名基础上执行 native 认证，成功才原子发布真正的 `.bindings` 文件。报告另外记录 `contract_verified`／`cache_written`；它仍不是可执行缓存，也不会报告 Hook 已安装。

`bc7_rgbm_bindings.exe --mode scan` 完全不加载 DIA、不使用 PDB 名称／lambda 哈希名／历史 RVA。在已验证的可执行 `.pdata` 范围中先按受限长度筛选、核对唯一压缩和 usage 根，再沿实际调用关系找到其余角色，执行同一认证。候选缺失、歧义、未知 ABI／usage／等待目标均退出 3。当前扫描覆盖这个接口形态，不承诺其他 Unity 架构、revision 或编译方式。

存在但错误的 PDB 是主路径的硬拒绝，不能作为扫描证据。准备脚本在 PDB 不存在时才选择后备扫描；现有缓存命中也须重新校验 exe 和整个契约。

## 3. 缓存格式与读取

缓存是严格解析的文本格式，魔数为 `BC7_RGBM_BINDINGS 1`，不接受 JSON 取证报告作为缓存。它记录接口 ID、exe SHA256、PE timestamp／SizeOfImage、CodeView GUID／Age，以及固定顺序的 11 个角色：模块相对 RVA、长度、磁盘入口 32 字节 SHA256，最后附完整正文的 SHA256 校验。

读取上限 8192 字节；旧 schema、未知接口、角色缺失／重复／错序、越界字段、空字节、尾部字段、截断和校验失败都拒绝。校验和只用于识别损坏：即使人为重算 checksum，读取器也会从当前 exe 重建认证链，再逐项比较角色／RVA／长度／入口摘要。可编辑的缓存字段不会变成地址执行依据。

发布复用同卷临时文件和原子替换。文件名以当前 exe SHA256 为键，例如 `<sha256>.v1.bindings`；缓存里不记录上次进程绝对地址。生产目录规划仍是工程 `Library/Bc7Rdo/HookBindings/`，本轮验证文件全部留在仓库忽略的 `out/`，没有把诊断产物放入用户工程 `Assets`。

`validate_loaded_entries` 供后续运行时预检使用：核对已加载映像的架构／timestamp／尺寸／CodeView GUID 与 Age、角色所属映像及可读执行保护，并逐字节比较全部角色函数体与当前磁盘 exe，额外核对入口摘要。它不更改页保护、不写代码、不启动线程；成功只表示绑定与加载映像一致。

## 4. 使用入口

```powershell
# PDB 主路径：成功才写出经认证的缓存
& out/rgbm-portability/bc7_rgbm_resolver.exe `
  --exe '<实际 Unity.exe>' --pdb '<匹配 PDB>' --dia '<私有 msdia140.dll>' `
  --report '<证据.json>' --cache '<缓存.bindings>'

# 受限无 PDB 扫描及缓存复核
& out/rgbm-portability/bc7_rgbm_bindings.exe --mode scan `
  --exe '<实际 Unity.exe>' --cache '<缓存.bindings>' --report '<诊断.json>'
& out/rgbm-portability/bc7_rgbm_bindings.exe --mode verify `
  --exe '<实际 Unity.exe>' --cache '<缓存.bindings>' --report '<诊断.json>'

# 自动准备：命中复核；否则 PDB 主路径／缺 PDB 时扫描，再复核已发布缓存
& tools/prepare_rgbm_bindings.ps1 -UnityExe '<实际 Unity.exe>' `
  -CacheDirectory '<缓存目录>' -DiaDll '<私有 msdia140.dll>'
```

`scan/verify` 成功退出 0，契约／缓存拒绝退出 3，参数／输出覆盖输入拒绝退出 2，报告发布失败退出 1。所有成功结果都保留 `hook_installed=false`。依赖暂存脚本已包含 `/MT` 的 resolver 和 bindings 工具，不携带开发探针 DLL、Python／Capstone、Unity／PDB／Oodle。

## 5. 本轮实测

双版本矩阵共有 20 个报告场景，退出 0：同一工具的两版 PDB 缓存、扫描缓存及复核通过，同一 exe 的两条路线生成字节相同的缓存。错误 PDB、f1 缓存套到 f2、损坏／旧 schema／未知接口／重复角色、重算 checksum 的错误 RVA／摘要／长度、截断、未知 ABI、usage 位图变化、wait 尾跳目标变化、扫描歧义均拒绝。入口前 NOP 变体扫描通过，旧缓存因二进制身份变化拒绝；参数覆盖输入另按退出 2 拒绝。合成 exe 仅供磁盘解析，未执行。

准备脚本实测首次 `CacheHit=false`、随后 `CacheHit=true`，均 `HookInstalled=false`。既有合成 PE、匹配／错误 PDB、P0 callback clone、私有 DIA 及新磁盘解码回归通过。

用户开放的 MCP 实例为 `ClientDevelopment@b816e5fb3b1cec00`，主 Editor PID 41644，实际 exe 为 f2 `7670c08855a9`。通过 UnityMCP 在这个进程内临时加载只读测试 DLL `rgbm_binding_probe.dll`，调用生产候选组件的缓存读取／加载映像验证：f2 缓存的 11 个角色完整代码一致；f1 缓存拒绝为 `cache_binary_identity_mismatch`，损坏缓存拒绝为 `cache_checksum_mismatch`。调用后 DLL 已卸载，两项临时环境变量恢复 null，Editor 未编译／未导入，ready 状态正常。

证据：`out/rgbm-portability/p1-tests/summary.json`、`prepare-summary.json`、`live-f2.json`、`live-f1.json`、`live-bad-checksum.json`。测试 DLL 只在开发验证中使用，尚未链接／复制为项目生产插件。

## 6. 下一关口

当前工程的旧 DLL 已复现固定 RVA 的 `not hooked`，本轮保留这个基线。P1 通过的是定位、认证和缓存关口，**尚未消除该日志**。

P2 还需证明安装时目标调用／新调用线程的协调机制、撤销时活跃调用清退及恢复失败边界。Editor 的 idle／MCP ready 状态本身不提供这个屏障。现有 Detours 后端保留该前提，本轮不向它传入未经证明的 `true` 来修改运行中的 Editor。随后在 P3 接入两个 Hook、三态上下文和不可用时原生 BC7 退化，P4 接入 loader／Worker 的准备顺序，P5 再验证真正的 usage／编码参数与输出。
