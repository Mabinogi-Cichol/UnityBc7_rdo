#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
验证 Unity Editor Plugin（外挂注入版）端到端链路：
  1. 定位真实 Unity Editor（-u 参数 / UNITY_EXE 环境变量 / 注册表 / Hub / 旧式路径）
  2. 建临时工程，把 out\\editor-plugin\\Bc7Rdo\\ 下文件放进 Assets\\Plugins\\Bc7Rdo\\
     （Plugins 路径复现用户场景：脚本经 Bc7Rdo.Editor.asmdef 编入 Editor-only
     自定义程序集，自动引用 UnityEditor，避免 firstpass 程序集编译报错）
  3. 阶段1 冒烟测试（Bc7Rdo.HookLoader.SmokeTest）：hook 链 + RDO 生效
  4. 阶段2 导入管线测试（Bc7RdoImportTest.Run）：真实 TextureImporter +
     BC7 reimport 下 RDO 生效（同步 vs 异步/worker 路径诊断）

环境相关测试：无 Unity Editor 时打印 [SKIP] 并 exit 0。

用法: python tests/test_editor_plugin.py [-u <Unity.exe 绝对路径>]
"""

import glob
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out"
PLUGIN_SRC = OUT / "editor-plugin" / "Bc7Rdo"
NEEDED = ["hook_loader.cs", "Bc7Rdo.Editor.asmdef",
          "bc7_rdo_hook.dll", "bc7_rdo_config.txt"]


def find_unity_exe(manual=None):
    if manual:
        p = Path(manual)
        if p.is_file():
            return p
        print(f"[提示] -u 指定的 Unity.exe 不存在: {manual}")

    env = _get_env("UNITY_EXE")
    if env and Path(env).is_file():
        return Path(env)

    # 1) 注册表 HKLM\SOFTWARE\Unity Technologies\Installer
    try:
        import winreg
        key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                             r"SOFTWARE\Unity Technologies\Installer")
        try:
            i = 0
            while True:
                name = winreg.EnumKey(key, i)
                i += 1
                if not re.match(r"Unity 2022\.3", name):
                    continue
                sub = winreg.OpenKey(key, name)
                try:
                    for value_name in ("Location Editor", "Location x64"):
                        try:
                            path, _ = winreg.QueryValueEx(sub, value_name)
                        except FileNotFoundError:
                            continue
                        cand = _locate_exe(path)
                        if cand:
                            return cand
                finally:
                    winreg.CloseKey(sub)
        except OSError:
            pass
        finally:
            winreg.CloseKey(key)
    except ImportError:
        pass

    # 2) Unity Hub 布局（取最高版本）
    hubs = sorted(glob.glob(r"C:\Program Files\Unity\Hub\Editor\2022.3*\Editor\Unity.exe"))
    if hubs:
        return Path(hubs[-1])

    # 3) 旧式固定路径
    legacy = Path(r"C:\Program Files\Unity\Editor\Unity.exe")
    if legacy.is_file():
        return legacy

    return None


def _get_env(name):
    import os
    return os.environ.get(name, "")


def _locate_exe(location):
    """注册表 Location 值可能指向 Editor 目录本身或外层目录"""
    for cand in (Path(location) / "Unity.exe",
                 Path(location) / "Editor" / "Unity.exe"):
        if cand.is_file():
            return cand
    return None


def _grep_log(text, pattern):
    return [ln for ln in text.splitlines() if re.search(pattern, ln)]


def _run_unity(unity, work, method, log_name, timeout=300, env=None):
    """运行一次 Unity 批处理，返回 (退出码, 日志文本)；超时返回 (None, '')"""
    unity_log = work / log_name
    cmd = [str(unity), "-batchmode", "-nographics", "-quit",
           "-projectPath", str(work), "-logFile", str(unity_log),
           "-executeMethod", method]
    print(f"[信息] 启动 Unity 批处理: {method} ...")
    t0 = time.time()
    try:
        proc = subprocess.run(cmd, cwd=work, timeout=timeout, env=env,
                              creationflags=subprocess.CREATE_NO_WINDOW,
                              capture_output=True, text=True)
    except subprocess.TimeoutExpired:
        print(f"[FAIL] Unity 批处理超时（{timeout} 秒）: {method}")
        return None, ""
    elapsed = time.time() - t0
    print(f"[信息] Unity 退出码: {proc.returncode}，耗时 {elapsed:.1f}s")
    log_text = unity_log.read_text(encoding="utf-8", errors="replace") \
        if unity_log.exists() else ""
    return proc.returncode, log_text


def _print_key_lines(log_text, patterns):
    for pat in patterns:
        for ln in _grep_log(log_text, pat):
            print("  | " + ln.strip())


def main():
    manual = None
    if len(sys.argv) >= 3 and sys.argv[1] == "-u":
        manual = sys.argv[2]

    unity = find_unity_exe(manual)
    if not unity:
        print("[SKIP] 未找到 Unity Editor（可用 -u <Unity.exe> 指定，或设 UNITY_EXE）")
        return 0
    print(f"[信息] Unity Editor: {unity}")

    missing = [n for n in NEEDED if not (PLUGIN_SRC / n).is_file()]
    if missing:
        print(f"[FAIL] out\\editor-plugin\\Bc7Rdo\\ 缺少文件: {', '.join(missing)}"
              "（请先运行 tools/build.ps1）")
        return 1

    work = Path(tempfile.mkdtemp(prefix="bc7rdo_plugin_", dir=ROOT / "out"))
    try:
        # 放到 Assets\Plugins 下（复现用户路径；asmdef 保证编译进 Editor-only 程序集）
        plugin_dir = work / "Assets" / "Plugins" / "Bc7Rdo"
        plugin_dir.mkdir(parents=True, exist_ok=True)
        for n in NEEDED:
            shutil.copyfile(PLUGIN_SRC / n, plugin_dir / n)

        # ---- 阶段 1：冒烟测试（hook 链 + RDO 生效） ----
        rc, log_text = _run_unity(unity, work, "Bc7Rdo.HookLoader.SmokeTest", "unity.log")
        if rc is None:
            return 1

        _print_key_lines(log_text, (r"\[Bc7Rdo\]", r"SMOKE", r"Exception", r"error CS"))

        # 断言：退出码 0 + PASS 标记 + 无脚本编译错误 + hook 日志 OK
        ok = rc == 0
        if not ok:
            print("[FAIL] Unity 批处理退出码 != 0")
        if "[Bc7Rdo][SMOKE] PASS" not in log_text:
            ok = False
            print("[FAIL] unity.log 未出现 [Bc7Rdo][SMOKE] PASS")
        if re.search(r"error CS\d+", log_text):
            ok = False
            print("[FAIL] unity.log 存在脚本编译错误（error CS），插件编译失败")
            for ln in _grep_log(log_text, r"error CS"):
                print("  | " + ln.strip())

        hook_log_path = plugin_dir / "bc7_rdo_hook.log"
        hook_log = ""
        if hook_log_path.exists():
            hook_log = hook_log_path.read_text(encoding="utf-8", errors="replace")
            if "install_hook: OK" not in hook_log:
                ok = False
                print("[FAIL] hook 日志未出现 install_hook: OK")
        else:
            ok = False
            print("[FAIL] 未生成 hook 日志: " + str(hook_log_path))

        print("=== 插件目录 hook 日志 ===")
        print(hook_log.strip() if hook_log else "(无)")

        if not ok:
            print("=== unity.log 尾部（诊断） ===")
            tail = log_text.splitlines()[-40:]
            print("\n".join(tail))
            return 1

        print("[PASS] 阶段1 冒烟测试通过")

        # ---- 阶段 2：导入管线测试（TextureImporter + BC7 reimport） ----
        import_cs = ROOT / "tests" / "Bc7RdoImportTest.cs"
        if not import_cs.exists():
            print(f"[FAIL] 缺少 {import_cs}（导入管线测试脚本）")
            return 1
        editor_dir = work / "Assets" / "Editor"
        editor_dir.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(import_cs, editor_dir / "Bc7RdoImportTest.cs")

        rc2, log2 = _run_unity(unity, work, "Bc7RdoImportTest.Run", "unity_import.log")
        if rc2 is None:
            return 1

        _print_key_lines(log2, (r"BC7RDO", r"error CS", r"Exception"))

        ok2 = rc2 == 0 and "[BC7RDO][IMPORT] PASS" in log2 \
            and not re.search(r"error CS\d+", log2)
        if not ok2:
            print("=== unity_import.log 尾部（诊断） ===")
            tail = log2.splitlines()[-40:]
            print("\n".join(tail))
            return 1

        print("[PASS] 阶段2 导入管线测试通过")

        # ---- 阶段 3：worker 识别 + 注入链路验证 ----
        # 模拟"Unity 子进程 = AssetImportWorker"：把测试目标复制成 Unity.exe
        # （worker 识别按 exe 名匹配），由 Unity 批处理方法自己启动该子进程
        # （父进程 = Unity 进程，满足子进程判定），执行 ScanAndInject 完成
        # "子进程识别 -> 注入 -> hook 生效"全链路；断言目标输出哈希从基线
        # 变为 RDO 值（等价于真实 worker 被注入后的行为）。
        import os
        target_exe = OUT / "bc7rdo_test_target.exe"
        if not target_exe.exists():
            print("[FAIL] 缺少 out\\bc7rdo_test_target.exe（请先运行 tools/build.ps1）")
            return 1
        tdir = work / "inject_target"
        tdir.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(target_exe, tdir / "Unity.exe")  # 名称匹配 worker 识别规则
        shutil.copyfile(ROOT / "vendor" / "compress_bc7e_original.dll",
                        tdir / "compress_bc7e.dll")

        env = dict(os.environ)
        env["BC7RDO_CHILD_EXE"] = str(tdir / "Unity.exe")
        rc3, log3 = _run_unity(unity, work, "Bc7Rdo.HookLoader.ScanInjectTest",
                               "unity_inject.log", env=env)
        if rc3 is None:
            return 1
        _print_key_lines(log3, (r"BC7RDO", r"error CS", r"Exception"))

        ok3 = rc3 == 0 and "[BC7RDO][SCANINJECT] PASS" in log3 \
            and not re.search(r"error CS\d+", log3)
        if not ok3:
            print("[FAIL] worker 扫描注入阶段失败（未识别子进程或注入未 PASS）")
            return 1

        # 等子进程日志写满（20 轮；子进程在 Unity 退出前已跑完）
        tgt_log_path = tdir / "bc7rdo_test_target.log"
        deadline = time.time() + 60
        while time.time() < deadline:
            if tgt_log_path.exists():
                tgt_log = tgt_log_path.read_text(encoding="utf-8", errors="replace")
                if len(re.findall(r"round \d+:", tgt_log)) >= 20:
                    break
            time.sleep(0.5)
        else:
            print("[FAIL] 等待测试目标日志超时")
            return 1

        hashes = re.findall(r"round \d+: (0x[0-9a-fA-F]{16})", tgt_log)
        if len(hashes) < 3 or len(set(hashes)) < 2 or hashes[0] == hashes[-1]:
            print("[FAIL] 子进程哈希未变化（注入未生效）:\n" + tgt_log)
            return 1
        print(f"[信息] 子进程 {len(hashes)} 轮，哈希 {hashes[0]} -> {hashes[-1]}")
        print("[PASS] 阶段3 worker 识别+注入链路验证通过")

        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
