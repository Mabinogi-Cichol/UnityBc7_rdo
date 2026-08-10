# compress_bc7e.dll 接口逆向分析

## 1. 样本信息

- 路径：`C:\Program Files\Unity\Unity 2022.3.62f1\Editor\compress_bc7e.dll`
- 项目内副本：`vendor/compress_bc7e_original.dll`
- SHA-256：`83F9BF925C65CCBDF93610CDEFF1AB12A3169C9813D4BFAE85F96035AF04AB33`
- 大小：1,227,776 字节（2025-04-30）
- 架构：x64（PE32+，`machine 8664`），子系统 Windows CUI
- 依赖：仅 `KERNEL32.dll`
- PDB 路径（内嵌字符串）：
  `C:\build\output\unity\graphics.bc7e.compressor\artifacts\compress_bc7e\release_Win_x64_VS2019_Win7_nonlump\compress_bc7e.pdb`

## 2. 导出表

`dumpbin /exports`（ordinal base = 1，10 个函数）：

| ordinal | name |
| --- | --- |
| 1 | bc7elib_compress_block_init |
| 2 | bc7elib_compress_block_params_init |
| 3 | bc7elib_compress_block_params_init_basic |
| 4 | bc7elib_compress_block_params_init_fast |
| 5 | bc7elib_compress_block_params_init_slow |
| 6 | bc7elib_compress_block_params_init_slowest |
| 7 | bc7elib_compress_block_params_init_ultrafast |
| 8 | bc7elib_compress_block_params_init_veryfast |
| 9 | bc7elib_compress_block_params_init_veryslow |
| 10 | bc7elib_compress_blocks |

这些名字与开源 `bc7e`（BinomialLLC/bc7e，即 `bc7e.ispc` 的 C API）一致，仅前缀 `bc7e_` → `bc7elib_`。

## 3. 导出桩与真实实现

导出 RVA 从 `0x10B300` 开始、每 16 字节一个：

```asm
000000018010B300: E9 FB 5E EF FF   jmp 0000000180001200   ; bc7elib_compress_block_init
000000018010B310: E9 3B 5F EF FF   jmp 0000000180001250   ; params_init
...
000000018010B390: E9 BB 61 EF FF   jmp 0000000180001550   ; bc7elib_compress_blocks
```

真实实现位于 0x1200–0x15C0。每个 init 函数都读取全局 `[0x18012B000]`（CPU 目标选择），按 `>=4 / >=2 / >=0 / <0` 分派到不同 ISPC 目标实现（AVX2/AVX/SSE4/SSE2 及错误路径）。

## 4. 函数签名（x64，C 调用约定，无名修饰）

```c
void bc7elib_compress_block_init(void);
void bc7elib_compress_block_params_init(bc7e_compress_block_params* p, bool perceptual);
void bc7elib_compress_block_params_init_basic(bc7e_compress_block_params* p, bool perceptual);
// ... fast / slow / slowest / ultrafast / veryfast / veryslow 同型
void bc7elib_compress_blocks(uint32_t num_blocks, uint64_t* pBlocks,
                             const uint32_t* pPixelsRGBA,
                             const bc7e_compress_block_params* pComp_params);
```

反汇编证据（`bc7elib_compress_blocks` @ 0x180001550）：

```asm
mov rsi, r9        ; pComp_params
mov rdi, r8        ; pPixelsRGBA
mov rbx, rdx       ; pBlocks
mov ebp, ecx       ; num_blocks（32 位）
...
mov ecx, ebp
mov rdx, rbx
mov r8, rdi
mov r9, rsi
call <目标实现>
```

init 函数（例如 @ 0x180001250）：

```asm
mov ebx, edx       ; perceptual
mov rsi, rcx       ; p
...
call <目标实现>     ; (rcx=p, edx=perceptual)
```

## 5. bc7e_compress_block_params 结构体布局

根据 init 函数反汇编（写入偏移）与 `bc7enc_rdo/bc7e.ispc` 源码交叉验证，MSVC x64 下大小 **124 字节**：

| 偏移 | 字段 |
| --- | --- |
| 0x00 | `uint32_t m_max_partitions_mode[8]` |
| 0x20 | `uint32_t m_weights[4]` |
| 0x30 | `uint32_t m_uber_level` |
| 0x34 | `uint32_t m_refinement_passes` |
| 0x38 | `uint32_t m_mode4_rotation_mask` |
| 0x3C | `uint32_t m_mode4_index_mask` |
| 0x40 | `uint32_t m_mode5_rotation_mask` |
| 0x44 | `uint32_t m_uber1_mask` |
| 0x48 | `bool m_perceptual` |
| 0x49 | `bool m_pbit_search` |
| 0x4A | `bool m_mode6_only` |
| 0x4B | `bool m_unused0` |
| 0x4C | `m_opaque_settings`：`u32 m_max_mode13_partitions_to_try` |
| 0x50 | `u32 m_max_mode0_partitions_to_try` |
| 0x54 | `u32 m_max_mode2_partitions_to_try` |
| 0x58 | `bool m_use_mode[7]` |
| 0x5F | `bool m_unused1` |
| 0x60 | `m_alpha_settings`：`u32 m_max_mode7_partitions_to_try` |
| 0x64 | `u32 m_mode67_error_weight_mul[4]` |
| 0x74 | `bool m_use_mode4/5/6/7`、`bool m_use_mode4_rotation/5_rotation`、`bool m_unused2/3` |

证据示例（`init_basic` @ 0x1800AE700）：

```asm
mov qword [rcx+4Ch], 100000001h        ; mode13=1, mode0=1
mov qword [rcx+54h], 0FFFFFFFF00000001h ; mode2=1, use_mode[0..2]=true
mov dword [rcx+5Bh], 0FFFFFFFFh         ; use_mode[3..6]=true
movups xmmword [rcx+60h], <常量>        ; max_mode7 + error weights
mov qword [rcx+70h], 0FFFFFFFF00000001h ; weights[3]=1, use_mode4..7=true
mov word  [rcx+78h], 0FFFFh             ; rotation flags
```

## 6. 与开源实现的关系

- 导出 API 与 `bc7e.ispc`（BinomialLLC/bc7e 及 bc7enc_rdo 中更新版）完全对应。
- `bc7elib_compress_blocks` 的实现等价于 `bc7e_compress_blocks`：输入为**连续 4x4 块**（每块 16 个 RGBA 像素，R 在低字节），输出每块 16 字节 BC7 数据；推荐一次调用 8–64 个块的倍数。
- `bc7enc_rdo` 的 `ert::reduce_entropy` 可直接对该输出做 RDO 后处理（本项目即采用此方案）。

## 7. 对实现“BC7 Crunched”的含义

BC7 块大小固定为 16 字节，RDO 不能减少块数；它让块流对 Deflate/LZMA/LZ4 等后续压缩更友好。因此收益只体现在**块流被再次无损压缩时**（AssetBundle、DDS+zlib 等容器），与本项目测试中 zlib9 体积下降一致。
