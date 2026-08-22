#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
对比两种 BC7 RDO 实现，使用 contrast/image 下的图片：
  - 原版（本仓库 Unity 壳 DLL，bc7enc_rdo ERT）：lambda=1.0，预设 basic / perceptual
  - Oodle版（oodle_testenv，Oodle Texture 2.9.16）：lambda=30，effort=High / metric=Default

为保证公平，两种实现的 BC7 块流都用同一个解码器（壳 DLL 的 bc7elib_debug_unpack_blocks）
解码回 RGBA，再用同一套 MSE/PSNR 公式与 zlib9 压缩测量。

用法: python contrast/compare_rdo.py
"""

import ctypes
import math
import os
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
IMAGE_DIR = ROOT / "contrast" / "image"
WRAPPER_DLL = ROOT / "out" / "compress_bc7e.dll"
ORIGINAL_DLL = ROOT / "vendor" / "compress_bc7e_original.dll"
OODLE_EXE = ROOT / "oodle_testenv" / "out" / "oodle_bc7.exe"
WORK = ROOT / "contrast" / "_work"
OUT_CSV = ROOT / "contrast" / "compare_rd.csv"
OUT_MD = ROOT / "contrast" / "compare_rd.md"

ORIGINAL_LAMBDA = 1.0
OODLE_LAMBDA = 30


# ---------------------------------------------------------------------------
# 结构体定义（与 src/bc7elib.h 一致，MSVC x64 布局，复制自 tests/test_compat.py）
# ---------------------------------------------------------------------------

class Opaque(ctypes.Structure):
    _fields_ = [
        ("m_max_mode13_partitions_to_try", ctypes.c_uint32),
        ("m_max_mode0_partitions_to_try", ctypes.c_uint32),
        ("m_max_mode2_partitions_to_try", ctypes.c_uint32),
        ("m_use_mode", ctypes.c_bool * 7),
        ("m_unused1", ctypes.c_bool),
    ]


class Alpha(ctypes.Structure):
    _fields_ = [
        ("m_max_mode7_partitions_to_try", ctypes.c_uint32),
        ("m_mode67_error_weight_mul", ctypes.c_uint32 * 4),
        ("m_use_mode4", ctypes.c_bool),
        ("m_use_mode5", ctypes.c_bool),
        ("m_use_mode6", ctypes.c_bool),
        ("m_use_mode7", ctypes.c_bool),
        ("m_use_mode4_rotation", ctypes.c_bool),
        ("m_use_mode5_rotation", ctypes.c_bool),
        ("m_unused2", ctypes.c_bool),
        ("m_unused3", ctypes.c_bool),
    ]


class Params(ctypes.Structure):
    _fields_ = [
        ("m_max_partitions_mode", ctypes.c_uint32 * 8),
        ("m_weights", ctypes.c_uint32 * 4),
        ("m_uber_level", ctypes.c_uint32),
        ("m_refinement_passes", ctypes.c_uint32),
        ("m_mode4_rotation_mask", ctypes.c_uint32),
        ("m_mode4_index_mask", ctypes.c_uint32),
        ("m_mode5_rotation_mask", ctypes.c_uint32),
        ("m_uber1_mask", ctypes.c_uint32),
        ("m_perceptual", ctypes.c_bool),
        ("m_pbit_search", ctypes.c_bool),
        ("m_mode6_only", ctypes.c_bool),
        ("m_unused0", ctypes.c_bool),
        ("m_opaque_settings", Opaque),
        ("m_alpha_settings", Alpha),
    ]


class Bc7eDll:
    def __init__(self, path: Path):
        self.lib = ctypes.WinDLL(str(path))

        self.init_fn = self.lib.bc7elib_compress_block_init
        self.init_fn.restype = None

        self.params_init_basic = self.lib.bc7elib_compress_block_params_init_basic
        self.params_init_basic.restype = None
        self.params_init_basic.argtypes = [ctypes.POINTER(Params), ctypes.c_bool]

        self.compress = self.lib.bc7elib_compress_blocks
        self.compress.restype = None
        self.compress.argtypes = [
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint64),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(Params),
        ]

        self.unpack_debug = getattr(self.lib, "bc7elib_debug_unpack_blocks", None)
        if self.unpack_debug is not None:
            self.unpack_debug.restype = ctypes.c_uint32
            self.unpack_debug.argtypes = [
                ctypes.c_uint32,
                ctypes.POINTER(ctypes.c_uint64),
                ctypes.POINTER(ctypes.c_uint8),
            ]

        self.init_fn()

    def make_params_basic(self, perceptual=True) -> Params:
        p = Params()
        ctypes.memset(ctypes.byref(p), 0, ctypes.sizeof(p))
        self.params_init_basic(ctypes.byref(p), perceptual)
        return p

    def compress_bytes(self, pixels: bytes, num_blocks: int, params: Params) -> bytes:
        pixels_arr = (ctypes.c_uint32 * (num_blocks * 16)).from_buffer_copy(pixels)
        blocks = bytearray(num_blocks * 16)
        blocks_arr = (ctypes.c_uint64 * (num_blocks * 2)).from_buffer(blocks)
        self.compress(num_blocks, blocks_arr, pixels_arr, ctypes.byref(params))
        return bytes(blocks)

    def decompress_bytes(self, blocks: bytes, num_blocks: int) -> bytes:
        blocks_arr = (ctypes.c_uint64 * (num_blocks * 2)).from_buffer_copy(blocks)
        out = bytearray(num_blocks * 64)
        out_arr = (ctypes.c_uint8 * (num_blocks * 64)).from_buffer(out)
        failed = self.unpack_debug(num_blocks, blocks_arr, out_arr)
        if failed:
            raise RuntimeError(f"{failed} blocks failed to decode")
        return bytes(out)


# ---------------------------------------------------------------------------
# 图像 -> 块序 RGBA（与 tests/test_compat.py image_to_blocks 顺序一致）
# ---------------------------------------------------------------------------

def image_to_blocks(img: Image.Image):
    a = np.asarray(img, dtype=np.uint8)  # (h, w, 4) RGBA
    h, w = a.shape[:2]
    bw = (w + 3) // 4
    bh = (h + 3) // 4
    pad_h = bh * 4 - h
    pad_w = bw * 4 - w
    if pad_h or pad_w:
        a = np.pad(a, ((0, pad_h), (0, pad_w), (0, 0)), mode="edge")
    # (bh,4,bw,4,4) -> (bh,bw,4,4,4) -> (nblocks, 64)
    b = a.reshape(bh, 4, bw, 4, 4)
    b = b.transpose(0, 2, 1, 3, 4)
    blocks = b.reshape(-1, 64)
    return blocks.tobytes(), bh * bw


def linear_rgba(img: Image.Image) -> bytes:
    a = np.asarray(img.convert("RGBA"), dtype=np.uint8)
    return a.tobytes()


# ---------------------------------------------------------------------------
# 指标
# ---------------------------------------------------------------------------

def zlib9_size(data: bytes) -> int:
    return len(zlib.compress(data, 9))


def psnr_channels(src: bytes, dec: bytes, nchannels: int) -> float:
    s = np.frombuffer(src, np.uint8).reshape(-1, 4)
    d = np.frombuffer(dec, np.uint8).reshape(-1, 4)
    if nchannels == 3:
        s = s[:, :3]
        d = d[:, :3]
    diff = s.astype(np.float64) - d.astype(np.float64)
    mse = float(np.mean(diff * diff))
    if mse == 0.0:
        return float("inf")
    return 10.0 * math.log10(255.0 * 255.0 / mse)


# ---------------------------------------------------------------------------
# 原版编码（写配置文件控制 lambda）
# ---------------------------------------------------------------------------

def original_encode(wrap: Bc7eDll, orig: Bc7eDll, pixels: bytes, num_blocks: int,
                    lam: float, cfg_path: Path):
    # 始终写配置文件（lambda=0 表示纯转发原 DLL、无 RDO）；watched_config 靠
    # mtime 变化触发重载，删除文件不会使已缓存的值失效，故统一用显式写入。
    cfg_path.write_text(
        f"lambda={lam}\nlookback=128\nsmooth_scale=15\n"
        f"max_rms_increase_ratio=10\ntwo_matches=1\ndebug=0\n",
        encoding="utf-8")
    # 确保热重载能感知 mtime 变化
    time.sleep(0.05)
    params = orig.make_params_basic(True)
    blocks = wrap.compress_bytes(pixels, num_blocks, params)
    decoded = wrap.decompress_bytes(blocks, num_blocks)
    return blocks, decoded


# ---------------------------------------------------------------------------
# Oodle 编码（写线性 RGBA raw，调 exe，返回 bc7_l0 / bc7_l30 的块流）
# ---------------------------------------------------------------------------

def oodle_encode(img: Image.Image, workdir: Path, lam: int) -> bytes:
    outdir = workdir / "out"
    bin_path = outdir / f"bc7_l{lam}.bin"
    w, h = img.size
    num_blocks = ((w + 3) // 4) * ((h + 3) // 4)
    expected = num_blocks * 16
    # Oodle 输出确定性，已存在且大小一致则直接复用，避免重复编码
    if bin_path.exists() and bin_path.stat().st_size == expected:
        return bin_path.read_bytes()

    raw_path = workdir / "input.raw"
    raw_path.write_bytes(linear_rgba(img))
    outdir.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(OODLE_EXE), "sweep", str(outdir),
        "--image", str(raw_path),
        "--width", str(w), "--height", str(h),
        "--lambdas", str(lam),
    ]
    subprocess.run(cmd, cwd=str(OODLE_EXE.parent), check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return bin_path.read_bytes()


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

def main():
    if not WRAPPER_DLL.exists():
        sys.exit(f"壳 DLL 不存在: {WRAPPER_DLL}")
    if not ORIGINAL_DLL.exists():
        sys.exit(f"原始 DLL 副本不存在: {ORIGINAL_DLL}")
    if not OODLE_EXE.exists():
        sys.exit(f"oodle_bc7.exe 不存在: {OODLE_EXE}")

    wrap = Bc7eDll(WRAPPER_DLL)
    orig = Bc7eDll(ORIGINAL_DLL)
    cfg_path = WRAPPER_DLL.parent / "bc7_rdo_config.txt"

    WORK.mkdir(parents=True, exist_ok=True)
    images = sorted(IMAGE_DIR.glob("*.png"))

    rows = []
    print(f"共 {len(images)} 张图片\n")

    for img_path in images:
        name = img_path.stem
        img = Image.open(img_path).convert("RGBA")
        w, h = img.size
        pixels, num_blocks = image_to_blocks(img)
        print(f"[{name}] {w}x{h}  {num_blocks} blocks")

        workdir = WORK / name
        workdir.mkdir(parents=True, exist_ok=True)

        # ---- 原版：baseline (lambda=0) 与 RDO (lambda=1.0) ----
        t0 = time.time()
        base_blk, base_dec = original_encode(wrap, orig, pixels, num_blocks, 0.0, cfg_path)
        rdo_blk, rdo_dec = original_encode(wrap, orig, pixels, num_blocks,
                                           ORIGINAL_LAMBDA, cfg_path)
        t_orig = time.time() - t0

        # ---- Oodle：baseline (lambda=0) 与 RDO (lambda=30) ----
        t0 = time.time()
        oodle_base_blk = oodle_encode(img, workdir, 0)
        oodle_rdo_blk = oodle_encode(img, workdir, OODLE_LAMBDA)
        t_oodle = time.time() - t0

        # Oodle 块流用同一解码器解码
        oodle_base_dec = wrap.decompress_bytes(oodle_base_blk, num_blocks)
        oodle_rdo_dec = wrap.decompress_bytes(oodle_rdo_blk, num_blocks)

        def record(version, lam, blocks, decoded, label):
            z9 = zlib9_size(blocks)
            p3 = psnr_channels(pixels, decoded, 3)
            p4 = psnr_channels(pixels, decoded, 4)
            return {
                "image": name, "version": version, "lambda": lam, "label": label,
                "blocks": num_blocks, "bc7_bytes": len(blocks),
                "zlib9_bytes": z9, "psnr_rgb": p3, "psnr_rgba": p4,
            }

        o_base = record("original", 0, base_blk, base_dec, "base")
        o_rdo = record("original", ORIGINAL_LAMBDA, rdo_blk, rdo_dec, "rdo")
        d_base = record("oodle", 0, oodle_base_blk, oodle_base_dec, "base")
        d_rdo = record("oodle", OODLE_LAMBDA, oodle_rdo_blk, oodle_rdo_dec, "rdo")

        rows.extend([o_base, o_rdo, d_base, d_rdo])

        # 相对各自基线的 zlib9 节省
        o_save = (o_base["zlib9_bytes"] - o_rdo["zlib9_bytes"]) / o_base["zlib9_bytes"] * 100
        d_save = (d_base["zlib9_bytes"] - d_rdo["zlib9_bytes"]) / d_base["zlib9_bytes"] * 100
        print(f"  原版 λ=1.0 : zlib9 {o_base['zlib9_bytes']:,} -> {o_rdo['zlib9_bytes']:,} "
              f"({o_save:+.1f}%)  PSNR(RGB) {o_base['psnr_rgb']:.2f} -> {o_rdo['psnr_rgb']:.2f} dB")
        print(f"  Oodle λ=30 : zlib9 {d_base['zlib9_bytes']:,} -> {d_rdo['zlib9_bytes']:,} "
              f"({d_save:+.1f}%)  PSNR(RGB) {d_base['psnr_rgb']:.2f} -> {d_rdo['psnr_rgb']:.2f} dB")
        print(f"  (原版 {t_orig:.1f}s, Oodle {t_oodle:.1f}s)")

    # ---- 清理配置文件 ----
    cfg_path.unlink(missing_ok=True)

    # ---- 写 CSV ----
    import csv
    with open(OUT_CSV, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["image", "version", "lambda", "label", "blocks", "bc7_bytes",
                    "zlib9_bytes", "psnr_rgb_db", "psnr_rgba_db"])
        for r in rows:
            w.writerow([r["image"], r["version"], r["lambda"], r["label"],
                        r["blocks"], r["bc7_bytes"], r["zlib9_bytes"],
                        f"{r['psnr_rgb']:.4f}", f"{r['psnr_rgba']:.4f}"])

    # ---- 写 Markdown 汇总 ----
    by_image = {}
    for r in rows:
        by_image.setdefault(r["image"], []).append(r)

    def avg(ver, label, key):
        vals = [r[key] for r in rows if r["version"] == ver and r["label"] == label]
        return sum(vals) / len(vals) if vals else 0.0

    lines = []
    lines.append("# BC7 RDO 对比：原版（ERT λ=1.0） vs Oodle（2.9.16 λ=30）\n")
    lines.append("图片：`contrast/image/`；BC7 块流统一用壳 DLL 的 `bc7elib_debug_unpack_blocks` "
                 "解码，PSNR 用同一 MSE 公式，压缩体积用 zlib(9)。\n")

    # ---- 平均值汇总 ----
    lines.append(f"## 平均值汇总（{len(images)} 张图）\n")
    lines.append("| 版本 | λ | 平均 zlib9 字节 | 平均节省 | 平均 PSNR(RGB) dB | 平均 PSNR(RGBA) dB |")
    lines.append("| --- | ---: | ---: | ---: | ---: | ---: |")
    summary = []
    for ver, label, lam in [("original", "原版", ORIGINAL_LAMBDA),
                            ("oodle", "Oodle", OODLE_LAMBDA)]:
        b_z9 = avg(ver, "base", "zlib9_bytes")
        r_z9 = avg(ver, "rdo", "zlib9_bytes")
        save = (b_z9 - r_z9) / b_z9 * 100 if b_z9 else 0.0
        summary.append((label, lam, r_z9, save,
                        avg(ver, "rdo", "psnr_rgb"), avg(ver, "rdo", "psnr_rgba")))
        lines.append(
            f"| {label} | {lam} | {r_z9:,.0f} | {save:+.1f}% "
            f"| {avg(ver, 'rdo', 'psnr_rgb'):.2f} | {avg(ver, 'rdo', 'psnr_rgba'):.2f} |")
    # 基线对比
    lines.append("")
    lines.append("基线（λ=0，不应用 RDO）对比：")
    lines.append("")
    lines.append("| 版本 | 平均 zlib9 字节 | 平均 PSNR(RGB) dB | 平均 PSNR(RGBA) dB |")
    lines.append("| --- | ---: | ---: | ---: |")
    for ver, label, _ in [("original", "原版", None), ("oodle", "Oodle", None)]:
        lines.append(
            f"| {label} | {avg(ver, 'base', 'zlib9_bytes'):,.0f} "
            f"| {avg(ver, 'base', 'psnr_rgb'):.2f} | {avg(ver, 'base', 'psnr_rgba'):.2f} |")
    lines.append("")

    lines.append("| 图片 | 版本 | λ | zlib9 字节 | 相对基线 | PSNR(RGB) dB | PSNR(RGBA) dB |")
    lines.append("| --- | --- | ---: | ---: | ---: | ---: | ---: |")

    for img_name in images:
        name = img_name.stem
        recs = by_image[name]
        base = {r["version"]: r for r in recs if r["label"] == "base"}
        rdo = {r["version"]: r for r in recs if r["label"] == "rdo"}
        for ver, label in [("original", "原版"), ("oodle", "Oodle")]:
            b = base[ver]; r = rdo[ver]
            save = (b["zlib9_bytes"] - r["zlib9_bytes"]) / b["zlib9_bytes"] * 100
            lines.append(
                f"| {name} | {label} | {r['lambda']} | {r['zlib9_bytes']:,} "
                f"| {save:+.1f}% | {r['psnr_rgb']:.2f} | {r['psnr_rgba']:.2f} |")
            lines.append(
                f"| {name} | {label}(基线) | 0 | {b['zlib9_bytes']:,} | — "
                f"| {b['psnr_rgb']:.2f} | {b['psnr_rgba']:.2f} |")

    OUT_MD.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"\nCSV 已写入: {OUT_CSV}")
    print(f"Markdown 已写入: {OUT_MD}")


if __name__ == "__main__":
    main()
