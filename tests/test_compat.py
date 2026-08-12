#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
验证 compress_bc7e_rdo.dll（壳 DLL）：
  1. 导出表与原始 DLL 一致
  2. 转发模式下压缩输出与原始 DLL 逐字节一致
  3. 参数初始化结果与原始 DLL 一致（结构体布局正确）
  4. RDO 模式下 BC7 块流经 zlib 压缩后体积更小、质量略降（率失真曲线）

用法: python tests/test_compat.py
"""

import ctypes
import json
import math
import random
import sys
import zlib
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
ORIGINAL_DLL = ROOT / "vendor" / "compress_bc7e_original.dll"
WRAPPER_DLL = ROOT / "out" / "compress_bc7e.dll"
REPORT = ROOT / "out" / "test_report.json"


# ---------------------------------------------------------------------------
# 结构体定义（与 src/bc7elib.h 一致，MSVC x64 布局）
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


assert ctypes.sizeof(Params) == 124, f"Params size = {ctypes.sizeof(Params)}, expected 124"


class Bc7eDll:
    def __init__(self, path: Path, label: str):
        self.label = label
        self.path = path
        self.lib = ctypes.WinDLL(str(path))

        self.init_fn = self.lib.bc7elib_compress_block_init
        self.init_fn.restype = None

        self.params_init = self._params_proto("bc7elib_compress_block_params_init")
        self.params_init_basic = self._params_proto("bc7elib_compress_block_params_init_basic")
        self.params_init_fast = self._params_proto("bc7elib_compress_block_params_init_fast")
        self.params_init_slow = self._params_proto("bc7elib_compress_block_params_init_slow")
        self.params_init_slowest = self._params_proto("bc7elib_compress_block_params_init_slowest")
        self.params_init_ultrafast = self._params_proto("bc7elib_compress_block_params_init_ultrafast")
        self.params_init_veryfast = self._params_proto("bc7elib_compress_block_params_init_veryfast")
        self.params_init_veryslow = self._params_proto("bc7elib_compress_block_params_init_veryslow")

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

    def _params_proto(self, name):
        fn = getattr(self.lib, name)
        fn.restype = None
        fn.argtypes = [ctypes.POINTER(Params), ctypes.c_bool]
        return fn

    def make_params(self, preset: str, perceptual: bool) -> Params:
        p = Params()
        ctypes.memset(ctypes.byref(p), 0, ctypes.sizeof(p))
        init = {
            "default": self.params_init,
            "ultrafast": self.params_init_ultrafast,
            "veryfast": self.params_init_veryfast,
            "fast": self.params_init_fast,
            "basic": self.params_init_basic,
            "slow": self.params_init_slow,
            "veryslow": self.params_init_veryslow,
            "slowest": self.params_init_slowest,
        }[preset]
        init(ctypes.byref(p), perceptual)
        return p

    def compress_bytes(self, pixels: bytes, num_blocks: int, params: Params) -> bytes:
        pixels_arr = (ctypes.c_uint32 * (num_blocks * 16)).from_buffer_copy(pixels)
        blocks = bytearray(num_blocks * 16)
        blocks_arr = (ctypes.c_uint64 * (num_blocks * 2)).from_buffer(blocks)
        self.compress(num_blocks, blocks_arr, pixels_arr, ctypes.byref(params))
        return bytes(blocks)

    def decompress_bytes(self, blocks: bytes, num_blocks: int) -> bytes:
        if self.unpack_debug is None:
            raise RuntimeError(f"{self.label}: 没有调试解码导出")
        blocks_arr = (ctypes.c_uint64 * (num_blocks * 2)).from_buffer_copy(blocks)
        out = bytearray(num_blocks * 64)
        out_arr = (ctypes.c_uint8 * (num_blocks * 64)).from_buffer(out)
        failed = self.unpack_debug(num_blocks, blocks_arr, out_arr)
        if failed:
            raise RuntimeError(f"{self.label}: {failed} blocks failed to decode")
        return bytes(out)


# ---------------------------------------------------------------------------
# 测试纹理生成
# ---------------------------------------------------------------------------

def make_photo_texture(w: int = 256, h: int = 256, seed: int = 7):
    rnd = random.Random(seed)
    img = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(img)

    # 背景渐变
    for y in range(h):
        for x in range(w):
            r = int(90 + 70 * math.sin(x / 41.0) + 50 * math.sin(y / 53.0))
            g = int(70 + 60 * math.sin((x + y) / 47.0))
            b = int(120 + 80 * math.cos(x / 37.0) + 40 * math.sin(y / 61.0))
            a = 255
            d.point((x, y), fill=(r, g, b, a))

    # 圆形/椭圆色块（模拟贴图内容）
    for _ in range(36):
        cx = rnd.randrange(w)
        cy = rnd.randrange(h)
        rx = rnd.randrange(8, 42)
        ry = rnd.randrange(8, 42)
        color = (rnd.randrange(256), rnd.randrange(256), rnd.randrange(256),
                 rnd.randrange(120, 256))
        d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=color)

    # 半透明条带
    for y in range(h // 3, h // 2):
        for x in range(w):
            r, g, b, a = img.getpixel((x, y))
            img.putpixel((x, y), (r, g, b, int(a * 0.55)))

    # 像素噪声
    px = img.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            n = rnd.randint(-14, 14)
            px[x, y] = (max(0, min(255, r + n)),
                        max(0, min(255, g + n)),
                        max(0, min(255, b + n)),
                        a)
    return img


def make_gradient_texture(w: int = 256, h: int = 256):
    img = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        for x in range(w):
            d.point((x, y), fill=(x * 255 // w, y * 255 // h,
                                  (x + y) * 255 // (w + h), 255))
    return img


def make_noise_texture(w: int = 256, h: int = 256, seed: int = 11):
    rnd = random.Random(seed)
    img = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        for x in range(w):
            d.point((x, y), fill=(rnd.randrange(256), rnd.randrange(256),
                                  rnd.randrange(256), rnd.randrange(256)))
    return img


def image_to_blocks(img: Image.Image):
    w, h = img.size
    bw, bh = (w + 3) // 4, (h + 3) // 4
    px = img.load()
    out = bytearray(bw * bh * 64)
    idx = 0
    for by in range(bh):
        for bx in range(bw):
            for yy in range(4):
                for xx in range(4):
                    x = min(bx * 4 + xx, w - 1)
                    y = min(by * 4 + yy, h - 1)
                    r, g, b, a = px[x, y]
                    out[idx:idx + 4] = bytes((r, g, b, a))
                    idx += 4
    return bytes(out), bw * bh


# ---------------------------------------------------------------------------
# 指标
# ---------------------------------------------------------------------------

def zlib_size(data: bytes, level: int = 9) -> int:
    return len(zlib.compress(data, level))


def psnr(src: bytes, decoded: bytes) -> float:
    n = len(src) // 4
    mse = 0.0
    for i in range(n):
        for c in range(4):
            d = src[i * 4 + c] - decoded[i * 4 + c]
            mse += d * d
    mse /= n * 4
    if mse == 0:
        return float("inf")
    return 10.0 * math.log10(255.0 * 255.0 / mse)


def block_diff_count(a: bytes, b: bytes) -> int:
    assert len(a) == len(b)
    return sum(1 for i in range(0, len(a), 16) if a[i:i + 16] != b[i:i + 16])


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

def main():
    results = {"exports": {}, "params": {}, "forward": {}, "rdo": {}}
    failed = []

    if not ORIGINAL_DLL.exists():
        sys.exit(f"原始 DLL 副本不存在: {ORIGINAL_DLL}")
    if not WRAPPER_DLL.exists():
        sys.exit(f"壳 DLL 不存在，请先运行 tools/build.ps1: {WRAPPER_DLL}")

    orig = Bc7eDll(ORIGINAL_DLL, "original")
    wrap = Bc7eDll(WRAPPER_DLL, "wrapper")

    # 1. 导出表存在性
    expected = [
        "bc7elib_compress_block_init",
        "bc7elib_compress_block_params_init",
        "bc7elib_compress_block_params_init_basic",
        "bc7elib_compress_block_params_init_fast",
        "bc7elib_compress_block_params_init_slow",
        "bc7elib_compress_block_params_init_slowest",
        "bc7elib_compress_block_params_init_ultrafast",
        "bc7elib_compress_block_params_init_veryfast",
        "bc7elib_compress_block_params_init_veryslow",
        "bc7elib_compress_blocks",
    ]
    for name in expected:
        ok_orig = hasattr(orig.lib, name)
        ok_wrap = hasattr(wrap.lib, name)
        results["exports"][name] = {"original": ok_orig, "wrapper": ok_wrap}
        if not (ok_orig and ok_wrap):
            failed.append(f"missing export {name}")

    # 2. 参数结构体：包装 DLL 转发后应与原始 DLL 逐字节一致
    for preset in ["default", "ultrafast", "veryfast", "fast", "basic",
                   "slow", "veryslow", "slowest"]:
        for perceptual in [False, True]:
            p1 = orig.make_params(preset, perceptual)
            p2 = wrap.make_params(preset, perceptual)
            b1 = bytes(ctypes.string_at(ctypes.byref(p1), ctypes.sizeof(p1)))
            b2 = bytes(ctypes.string_at(ctypes.byref(p2), ctypes.sizeof(p2)))
            key = f"{preset}/perceptual={int(perceptual)}"
            ok = b1 == b2
            results["params"][key] = {"identical": ok,
                                      "size": len(b1),
                                      "m_uber_level": p1.m_uber_level}
            if not ok:
                failed.append(f"params mismatch {key}")

    # 3. 转发模式：压缩输出逐字节一致
    textures = {
        "photo": make_photo_texture(),
        "gradient": make_gradient_texture(),
        "noise": make_noise_texture(),
    }
    image_data = {}
    for name, img in textures.items():
        blocks_bytes, num_blocks = image_to_blocks(img)
        image_data[name] = (blocks_bytes, num_blocks)

    for tname, (pixels, num_blocks) in image_data.items():
        for preset in ["ultrafast", "fast", "basic", "slowest"]:
            for chunk in [num_blocks, 64]:
                params = orig.make_params(preset, True)
                out_orig = orig.compress_bytes(pixels, num_blocks, params)

                # 逐块调用包装 DLL（模拟 Unity 分批调用）
                chunks = []
                for start in range(0, num_blocks, chunk):
                    n = min(chunk, num_blocks - start)
                    p = pixels[start * 64:(start + n) * 64]
                    chunks.append(wrap.compress_bytes(p, n, params))
                out_wrap = b"".join(chunks)

                key = f"{tname}/{preset}/chunk={chunk}"
                ok = out_orig == out_wrap
                results["forward"][key] = {"identical": ok,
                                           "blocks": num_blocks,
                                           "bytes": len(out_orig)}
                if not ok:
                    diff = block_diff_count(out_orig, out_wrap)
                    failed.append(f"forward mismatch {key}: {diff} blocks differ")

    # 4. RDO 模式：体积 vs 质量（通过同目录 bc7_rdo_config.txt 控制 lambda）
    cfg_path = WRAPPER_DLL.parent / "bc7_rdo_config.txt"
    cfg_path.unlink(missing_ok=True)  # 确保基线是无 RDO 的转发输出
    pixels, num_blocks = image_data["photo"]
    params = orig.make_params("basic", True)
    baseline = wrap.compress_bytes(pixels, num_blocks, params)
    baseline_z = zlib_size(baseline)
    baseline_dec = wrap.decompress_bytes(baseline, num_blocks)
    baseline_psnr = psnr(pixels, baseline_dec)

    for lam in [0.0, 0.25, 0.5, 1.0, 2.0, 4.0]:
        cfg_path.write_text(
            f"lambda={lam}\nlookback=128\nsmooth_scale=15\n"
            f"max_rms_increase_ratio=10\ntwo_matches=1\ndebug=0\n",
            encoding="utf-8")
        rdo_bytes = wrap.compress_bytes(pixels, num_blocks, params)

        rdo_z = zlib_size(rdo_bytes)
        rdo_dec = wrap.decompress_bytes(rdo_bytes, num_blocks)
        rdo_psnr = psnr(pixels, rdo_dec)
        changed = block_diff_count(baseline, rdo_bytes)
        results["rdo"][f"lambda={lam}"] = {
            "zlib9_bytes": rdo_z,
            "zlib9_saved_vs_baseline_pct": round((baseline_z - rdo_z) * 100.0 / baseline_z, 3) if baseline_z else 0.0,
            "psnr_db": round(rdo_psnr, 3),
            "psnr_delta_db": round(rdo_psnr - baseline_psnr, 3),
            "blocks_changed": changed,
            "blocks_changed_pct": round(changed * 100.0 / num_blocks, 2),
        }
        if lam == 1.0:
            rdo_sample_bytes = rdo_bytes

    results["rdo"]["_baseline"] = {
        "zlib9_bytes": baseline_z,
        "psnr_db": round(baseline_psnr, 3),
        "blocks": num_blocks,
    }

    # 保存可视化对比图
    def blocks_to_image(decoded: bytes, num_blocks: int, blocks_x: int, blocks_y: int,
                        path: Path):
        img = Image.new("RGBA", (blocks_x * 4, blocks_y * 4))
        d = ImageDraw.Draw(img)
        idx = 0
        for by in range(blocks_y):
            for bx in range(blocks_x):
                for yy in range(4):
                    for xx in range(4):
                        r, g, b, a = decoded[idx:idx + 4]
                        d.point((bx * 4 + xx, by * 4 + yy), fill=(r, g, b, a))
                        idx += 4
        img.save(path)

    blocks_x = int(math.sqrt(num_blocks))
    blocks_y = num_blocks // blocks_x
    textures["photo"].save(ROOT / "out" / "test_photo_source.png")
    blocks_to_image(baseline_dec, num_blocks, blocks_x, blocks_y,
                    ROOT / "out" / "test_bc7_baseline.png")
    rdo1_dec = wrap.decompress_bytes(rdo_sample_bytes, num_blocks)
    blocks_to_image(rdo1_dec, num_blocks, blocks_x, blocks_y,
                    ROOT / "out" / "test_bc7_rdo_lambda1.png")

    # 5. 配置文件侧信道（bc7_rdo_config.txt，与壳 DLL 同目录）
    cfg_path.write_text("lambda=1.5\nlookback=256\ntwo_matches=1\n", encoding="utf-8")
    try:
        out_cfg = wrap.compress_bytes(pixels, num_blocks, params)
        results["config_file"] = {
            "file": str(cfg_path),
            "zlib9_bytes": zlib_size(out_cfg),
            "differs_from_baseline": out_cfg != baseline,
        }
    finally:
        cfg_path.unlink(missing_ok=True)

    # 输出
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")

    print("=== 导出表 ===")
    for name, st in results["exports"].items():
        print(f"  {name}: orig={st['original']} wrapper={st['wrapper']}")

    print("\n=== 参数结构体一致性 ===")
    mism = [k for k, v in results["params"].items() if not v["identical"]]
    print(f"  {len(results['params'])} 组全部一致" if not mism else f"  不一致: {mism}")

    print("\n=== 转发模式字节一致性 ===")
    mism = [k for k, v in results["forward"].items() if not v["identical"]]
    print(f"  {len(results['forward'])} 组全部一致" if not mism else f"  不一致: {mism}")

    print("\n=== RDO 率失真（photo 256x256, basic, perceptual） ===")
    base = results["rdo"]["_baseline"]
    print(f"  baseline: zlib9={base['zlib9_bytes']}B  psnr={base['psnr_db']}dB")
    for k, v in results["rdo"].items():
        if k.startswith("_"):
            continue
        print(f"  {k:9s} zlib9={v['zlib9_bytes']:6d}B ({v['zlib9_saved_vs_baseline_pct']:+.2f}%)  "
              f"psnr={v['psnr_db']:7.3f}dB ({v['psnr_delta_db']:+.3f})  "
              f"changed={v['blocks_changed_pct']}%")

    print(f"\n配置文件侧信道: {results['config_file']}")
    print(f"\n报告已写入: {REPORT}")

    if failed:
        print("\n失败项:")
        for f in failed:
            print("  -", f)
        sys.exit(1)
    print("\n全部测试通过")


if __name__ == "__main__":
    main()
