"""Repeat the first-read configuration regression against the frozen P5 DLL."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "out/rgbm-portability/acceptance"
work = OUT / "concurrency"
work.mkdir(exist_ok=True)
hook = OUT / "Bc7Rdo/bc7_rdo_hook.dll"
for source, name in ((hook, "bc7_rdo_hook.dll"),
                     (ROOT / "vendor/compress_bc7e_original.dll", "compress_bc7e.dll"),
                     (ROOT / "out/rgbm-portability/runtime/test_hook_concurrency.exe", "test_hook_concurrency.exe")):
    shutil.copyfile(source, work / name)
(work / "bc7_rdo_config.txt").write_text("encoder=oodle\noodle_lambda=0\nunity_rgbm_context_hooks=0\n")
start = time.perf_counter()
with (OUT / "concurrency.log").open("wb") as log:
    for index in range(120):
        result = subprocess.run([str(work / "test_hook_concurrency.exe")], cwd=work,
                                stdout=log, stderr=subprocess.STDOUT, timeout=30)
        assert result.returncode == 0, (index, result.returncode)
summary = {"passed": True, "processes": 120, "threads_each": 64, "all_exit_zero": True,
           "hook_sha256": hashlib.sha256(hook.read_bytes()).hexdigest().upper(),
           "elapsed_seconds": time.perf_counter() - start}
(OUT / "native-regression.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary))
