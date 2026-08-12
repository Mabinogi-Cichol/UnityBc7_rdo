#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
验证注入版 hook DLL（bc7_rdo_hook.dll）：
  1. 对已加载的 compress_bc7e.dll（原 DLL 改名副本）的导出桩自动装钩
     （DLL_PROCESS_ATTACH 时模块已加载 -> 立即装钩路径）
  2. 转发：lambda=0 时压缩输出与未挂钩的原始 DLL 逐字节一致
  3. RDO：lambda>0 时输出改变、zlib9 体积更小、PSNR 略降

原理：hook DLL 通过 GetModuleHandleW(L"compress_bc7e.dll") 定位模块并
patch 其 bc7elib_compress_blocks 导出桩；本测试把原 DLL 副本以
"compress_bc7e.dll" 这个名字加载进本进程（vendor 副本名字不同，保持不挂钩
作基线），再加载 hook DLL 触发装钩，全程不需要 Unity。

用法: python tests/test_hook.py
"""

import ctypes
import shutil
import sys
import tempfile
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_compat import (Bc7eDll, make_photo_texture, image_to_blocks,
                         zlib_size, psnr, block_diff_count)

ORIGINAL_DLL = ROOT / "vendor" / "compress_bc7e_original.dll"
HOOK_DLL = ROOT / "out" / "bc7_rdo_hook.dll"
WRAPPER_DLL = ROOT / "out" / "compress_bc7e.dll"  # 仅用其 debug 解码导出算 PSNR

CONFIG_TEXT = (
    "lambda={lam}\nlookback=128\nsmooth_scale=15\n"
    "max_rms_increase_ratio=10\ntwo_matches=1\ndebug=0\n"
)


def main():
    if not ORIGINAL_DLL.exists():
        sys.exit(f"原始 DLL 副本不存在: {ORIGINAL_DLL}")
    if not HOOK_DLL.exists():
        sys.exit(f"hook DLL 不存在，请先运行 tools/build.ps1: {HOOK_DLL}")
    if not WRAPPER_DLL.exists():
        sys.exit(f"壳 DLL 不存在，请先运行 tools/build.ps1: {WRAPPER_DLL}")

    work = Path(tempfile.mkdtemp(prefix="hooktest_", dir=ROOT / "out"))
    try:
        target_dll = work / "compress_bc7e.dll"   # 被挂钩的模块（模块名必须是这个名字）
        hook_dll = work / "bc7_rdo_hook.dll"
        cfg = work / "bc7_rdo_config.txt"

        shutil.copyfile(ORIGINAL_DLL, target_dll)
        shutil.copyfile(HOOK_DLL, hook_dll)
        cfg.write_text(CONFIG_TEXT.format(lam=0), encoding="utf-8")

        # 1. 基线：vendor 副本（模块名 compress_bc7e_original.dll，永远不会被挂钩）
        baseline = Bc7eDll(ORIGINAL_DLL, "baseline")

        # 2. 以 "compress_bc7e.dll" 名字加载副本 -> 再加载 hook DLL -> 立即装钩
        hooked = Bc7eDll(target_dll, "hooked")
        ctypes.WinDLL(str(hook_dll))
        time.sleep(0.3)  # 等 hook 日志落盘

        # 3. 校验装钩日志
        log = work / "bc7_rdo_hook.log"
        if not log.exists():
            sys.exit("hook 日志不存在，装钩未触发: " + str(log))
        log_text = log.read_text(encoding="utf-8", errors="replace")
        if "install_hook: OK" not in log_text:
            sys.exit("装钩未成功:\n" + log_text)
        print("=== hook 日志 ===")
        print(log_text.strip())

        # 4. 转发一致性：lambda=0 时压缩输出与未挂钩原始 DLL 逐字节一致
        pixels, num_blocks = image_to_blocks(make_photo_texture())
        params = baseline.make_params("basic", True)
        out_base = baseline.compress_bytes(pixels, num_blocks, params)
        out_forward = hooked.compress_bytes(pixels, num_blocks, params)
        if out_forward != out_base:
            sys.exit(f"lambda=0 转发输出不一致: {block_diff_count(out_base, out_forward)} blocks 不同")
        print(f"\n转发一致: lambda=0 输出与基线逐字节相同（{len(out_base)} 字节, {num_blocks} 块）")

        # 5. RDO 生效：lambda=4 时输出改变、zlib9 更小、PSNR 略降
        cfg.write_text(CONFIG_TEXT.format(lam=4), encoding="utf-8")
        out_rdo = hooked.compress_bytes(pixels, num_blocks, params)

        changed = block_diff_count(out_base, out_rdo)
        if changed == 0:
            sys.exit("lambda=4 输出与基线一致，RDO 未生效")
        z_base, z_rdo = zlib_size(out_base), zlib_size(out_rdo)
        if z_rdo > z_base:
            sys.exit(f"RDO 后 zlib9 未变小: {z_rdo} vs {z_base}")

        decoder = Bc7eDll(WRAPPER_DLL, "decoder")
        p_base = psnr(pixels, decoder.decompress_bytes(out_base, num_blocks))
        p_rdo = psnr(pixels, decoder.decompress_bytes(out_rdo, num_blocks))
        if p_rdo > p_base + 0.5:
            sys.exit(f"PSNR 异常（RDO 后反而更高）: {p_rdo:.3f} vs {p_base:.3f}")

        print(f"RDO 生效: lambda=4 改变 {changed}/{num_blocks} 块 "
              f"({changed * 100.0 / num_blocks:.1f}%)")
        print(f"  zlib9: {z_base} -> {z_rdo} B（-{100.0 * (z_base - z_rdo) / z_base:.2f}%）")
        print(f"  PSNR: {p_base:.3f} -> {p_rdo:.3f} dB")

        print("\nhook 测试全部通过")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
