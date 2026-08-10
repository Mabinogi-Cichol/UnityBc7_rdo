#pragma once
// bc7elib.h - 与 Unity 2022.3.62f1 的 compress_bc7e.dll 导出一致的接口定义。
//
// 布局依据：
//  1) 原始 DLL 反汇编（init_basic 等函数写入的偏移，见 docs/ANALYSIS.md）
//  2) richgel999/bc7enc_rdo 的 bc7e.ispc 中 struct bc7e_compress_block_params
//
// MSVC x64 下 sizeof(bc7e_compress_block_params) == 124。

#include <stdint.h>

#if defined(_MSC_VER)
#define BC7ELIB_API __declspec(dllexport)
#else
#define BC7ELIB_API
#endif

struct bc7e_compress_block_params_opaque
{
    uint32_t m_max_mode13_partitions_to_try; // +0x4C
    uint32_t m_max_mode0_partitions_to_try;  // +0x50
    uint32_t m_max_mode2_partitions_to_try;  // +0x54
    bool     m_use_mode[7];                  // +0x58
    bool     m_unused1;                      // +0x5F
};

struct bc7e_compress_block_params_alpha
{
    uint32_t m_max_mode7_partitions_to_try;  // +0x60
    uint32_t m_mode67_error_weight_mul[4];   // +0x64
    bool     m_use_mode4;                    // +0x74
    bool     m_use_mode5;                    // +0x75
    bool     m_use_mode6;                    // +0x76
    bool     m_use_mode7;                    // +0x77
    bool     m_use_mode4_rotation;           // +0x78
    bool     m_use_mode5_rotation;           // +0x79
    bool     m_unused2;                      // +0x7A
    bool     m_unused3;                      // +0x7B
};

struct bc7e_compress_block_params
{
    uint32_t m_max_partitions_mode[8];       // +0x00
    uint32_t m_weights[4];                   // +0x20
    uint32_t m_uber_level;                   // +0x30
    uint32_t m_refinement_passes;            // +0x34
    uint32_t m_mode4_rotation_mask;          // +0x38
    uint32_t m_mode4_index_mask;             // +0x3C
    uint32_t m_mode5_rotation_mask;          // +0x40
    uint32_t m_uber1_mask;                   // +0x44
    bool     m_perceptual;                   // +0x48
    bool     m_pbit_search;                  // +0x49
    bool     m_mode6_only;                   // +0x4A
    bool     m_unused0;                      // +0x4B
    bc7e_compress_block_params_opaque m_opaque_settings; // +0x4C
    bc7e_compress_block_params_alpha m_alpha_settings;   // +0x60
};

#ifdef __cplusplus
extern "C" {
#endif

// 与 Unity 原始 DLL 完全一致的 10 个导出函数。
BC7ELIB_API void bc7elib_compress_block_init(void);
BC7ELIB_API void bc7elib_compress_block_params_init(bc7e_compress_block_params* p, bool perceptual);
BC7ELIB_API void bc7elib_compress_block_params_init_basic(bc7e_compress_block_params* p, bool perceptual);
BC7ELIB_API void bc7elib_compress_block_params_init_fast(bc7e_compress_block_params* p, bool perceptual);
BC7ELIB_API void bc7elib_compress_block_params_init_slow(bc7e_compress_block_params* p, bool perceptual);
BC7ELIB_API void bc7elib_compress_block_params_init_slowest(bc7e_compress_block_params* p, bool perceptual);
BC7ELIB_API void bc7elib_compress_block_params_init_ultrafast(bc7e_compress_block_params* p, bool perceptual);
BC7ELIB_API void bc7elib_compress_block_params_init_veryfast(bc7e_compress_block_params* p, bool perceptual);
BC7ELIB_API void bc7elib_compress_block_params_init_veryslow(bc7e_compress_block_params* p, bool perceptual);
BC7ELIB_API void bc7elib_compress_blocks(uint32_t num_blocks, uint64_t* pBlocks,
                                          const uint32_t* pPixelsRGBA,
                                          const bc7e_compress_block_params* pComp_params);

// 额外调试导出（仅测试用，Unity 不会调用）：
// 把 BC7 块解码回 RGBA（每块 16 像素 x 4 字节），返回解码失败的块数。
BC7ELIB_API uint32_t bc7elib_debug_unpack_blocks(uint32_t num_blocks, const uint64_t* pBlocks, uint8_t* pPixelsOut);

#ifdef __cplusplus
}
#endif
