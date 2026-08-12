#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
验证注入器（bc7rdo_injector.exe）+ hook DLL 的端到端远程注入链路：
  1. 启动测试目标（bc7rdo_test_target.exe：加载假 compress_bc7e.dll 并循环压缩）
  2. 注入器 -p <pid> 附加进程，CreateRemoteThread + LoadLibraryW 注入 hook DLL
  3. 断言：注入前输出哈希 = 基线；注入后输出哈希改变（RDO 生效）
  4. 校验目标进程内 hook 日志（install_hook: OK）

整个测试不依赖 Unity：目标程序与注入器都跑在临时目录里。

用法: python tests/test_injector.py
"""

import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out"
ORIGINAL_DLL = ROOT / "vendor" / "compress_bc7e_original.dll"

NEEDED = ["bc7rdo_injector.exe", "bc7rdo_test_target.exe", "bc7_rdo_hook.dll"]


def main():
    for name in NEEDED:
        if not (OUT / name).exists():
            sys.exit(f"缺少 {name}，请先运行 tools/build.ps1")

    work = Path(tempfile.mkdtemp(prefix="injecttest_", dir=OUT))
    try:
        for name in NEEDED:
            shutil.copyfile(OUT / name, work / name)
        shutil.copyfile(ORIGINAL_DLL, work / "compress_bc7e.dll")
        (work / "bc7_rdo_config.txt").write_text(
            "lambda=4\nlookback=128\nsmooth_scale=15\n"
            "max_rms_increase_ratio=10\ntwo_matches=1\ndebug=0\n",
            encoding="utf-8")

        rounds, interval_ms = 6, 400  # 全程约 2 秒
        proc = subprocess.Popen(
            [str(work / "bc7rdo_test_target.exe"), str(rounds), str(interval_ms)],
            cwd=work, creationflags=subprocess.CREATE_NO_WINDOW,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            time.sleep(0.6)  # 第一轮基线已写入日志

            # 附加注入
            r = subprocess.run(
                [str(work / "bc7rdo_injector.exe"), "-p", str(proc.pid)],
                cwd=work, capture_output=True, text=True, timeout=40,
                creationflags=subprocess.CREATE_NO_WINDOW)
            print(r.stdout, end="")
            if r.stderr:
                print(r.stderr, end="")
            if r.returncode != 0:
                sys.exit(f"注入器退出码 {r.returncode}，注入失败")

            # 等目标程序自然退出
            try:
                rc = proc.wait(timeout=rounds * interval_ms / 1000 + 15)
            except subprocess.TimeoutExpired:
                proc.kill()
                sys.exit("测试目标超时未退出")
            if rc != 0:
                sys.exit(f"测试目标退出码 {rc}")

            # 断言：注入前 = 基线，注入后发生变化
            log = (work / "bc7rdo_test_target.log").read_text(
                encoding="utf-8", errors="replace")
            hashes = re.findall(r"round \d+: (0x[0-9a-fA-F]{16})", log)
            if len(hashes) != rounds:
                sys.exit(f"日志轮数异常: {len(hashes)} != {rounds}\n{log}")
            if len(set(hashes)) < 2 or hashes[0] == hashes[-1]:
                sys.exit("所有轮次哈希一致，hook 未生效:\n" + log)

            hlog_path = work / "bc7_rdo_hook.log"
            if not hlog_path.exists():
                sys.exit("hook 日志不存在，装钩未触发")
            hlog = hlog_path.read_text(encoding="utf-8", errors="replace")
            if "install_hook: OK" not in hlog:
                sys.exit("装钩未成功:\n" + hlog)

            print("=== hook 日志（目标进程内） ===")
            print(hlog.strip())
            print("\n=== 测试目标日志 ===")
            print(log.strip())
            print(f"\n注入端到端测试通过: 注入前哈希 {hashes[0]}，注入后 {hashes[-1]}")
        finally:
            if proc.poll() is None:
                proc.kill()
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
