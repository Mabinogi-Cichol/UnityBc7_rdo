// oodle_hook.h - Oodle Texture 编码器加载 + BC7 RDO 编码（供 bc7_hook_dll.cpp 使用）
//
// 仅在 hook DLL 中链接。运行时机：
//   - 懒加载 oo2tex_win64_2.9.16.dll（从 hook DLL 自身目录解析，LoadLibraryW +
//     GetProcAddress，不做 .lib 链接）；
//   - 线程安全（std::call_once），任何线程首次调用 available()/encode_bc7_rdo()
//     时触发加载；
//   - 加载/解析失败只影响本模块：available() 返回 false，调用方回退原 bc7e。

#pragma once

#include <stdint.h>

namespace oodle_hook
{
    // 返回 true 表示 Oodle DLL 已加载且所需导出已解析。
    // 懒加载：首次调用时执行 LoadLibraryW + GetProcAddress，线程安全。
    bool available();

    // 用 Oodle Texture 对 num_blocks 个 4x4 BC7 块做 RDO 编码。
    //   pPixelsRGBA : BLOCK-ORDER RGBA8 像素（每块 16 个像素，每像素 4 字节，
    //                 R 在低字节，即 bc7elib 的 uint32 布局），内部重排为
    //                 width=num_blocks*4, height=4 的行主序 surface 后调用
    //                 OodleTex_EncodeBCN_RDO_Ex。
    //   oodle_lambda: Oodle RDO 强度（0..100）。
    //   effort      : OodleTex_EncodeEffortLevel（10/20/30）。
    // 返回 true 表示 OodleTex_Err_OK，false 表示编码失败（调用方应回退）。
    bool encode_bc7_rdo(uint32_t num_blocks, uint64_t* pBlocks,
                        const uint32_t* pPixelsRGBA, int oodle_lambda, int effort);
}
