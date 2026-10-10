"""P3 production context engine, controlled faults and actual-body clone matrix.

Clones substitute external scheduling; this is not real Unity JobQueue/encoding.
"""
import json
from pathlib import Path
import subprocess
import shutil

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "out/rgbm-portability/runtime"


def run(command, expected=0):
    result = subprocess.run([str(x) for x in command], capture_output=True, timeout=90)
    assert (result.returncode & 0xFFFFFFFF) == expected, (command, result.returncode, result.stdout, result.stderr)
    return result


def main():
    exe = OUT / "test_rgbm_runtime.exe"
    result = run([exe])
    observed = json.loads(result.stdout.decode().strip())
    assert observed["passed"] and observed["slots_remaining"] == 0
    faults = {}
    for fault, expected in (("unjoined-cpp", 0xE0427001), ("unjoined-seh", 0xE0427001),
                            ("join-contract-violation", 0xE0427002)):
        result = run([exe, fault], expected)
        faults[fault] = f"0x{result.returncode & 0xFFFFFFFF:08X}"
    clones = {}
    for version in ("f1", "f2"):
        evidence = json.loads((OUT.parent / f"{version}-p0.json").read_text(encoding="utf-8-sig"))
        process = evidence["process_candidates"][0]["rva"]
        dispatcher = next(x["target_rva"] for x in evidence["process_edges"]
                          if x["kind"] == "dispatcher_rdx")
        helpers = [next(x["rva"] for x in evidence["process_dependencies"] if x["name"].startswith(prefix))
                   for prefix in ("?ScheduleJobForEachInternal@@", "?CompleteFenceInternal@@", "?ClearFenceWithoutSync@@")]
        report = OUT / f"clone-{version}.json"
        identity = evidence["binary_identity"]
        run([OUT / "test_runtime_clone.exe", identity["exe_path"], identity["sha256"],
             *(f"{x:X}" for x in [process, dispatcher, *helpers]), report])
        item = json.loads(report.read_text())
        assert item["passed"] and item["production_runtime"] and not item["unity_integration"]
        assert item["active_slots_after_normal_exit"] == 0
        assert item["callbacks"] == item["copied_payload_verified"] == item["context_verified"] == item["worker_tls_restored"]
        assert item["schedule"] == item["complete"] == item["clear"]
        clones[version] = item
    summary = {"passed": True, "engine": observed, "fail_closed_exit_codes": faults,
               "clones": clones, "real_unity_scheduler_tested": False}
    # Re-run the prior 64-thread first-read crash regression against this build.
    # Owned files stay below ignored out; no project or Unity installation writes.
    concurrency = OUT / "concurrency"
    concurrency.mkdir(exist_ok=True)
    for source, name in ((ROOT / "vendor/compress_bc7e_original.dll", "compress_bc7e.dll"),
                         (ROOT / "out/bc7_rdo_hook.dll", "bc7_rdo_hook.dll"),
                         (OUT / "test_hook_concurrency.exe", "test_hook_concurrency.exe")):
        shutil.copyfile(source, concurrency / name)
    (concurrency / "bc7_rdo_config.txt").write_text("encoder=oodle\noodle_lambda=0\nunity_rgbm_context_hooks=0\n")
    for _ in range(120):
        result = subprocess.run([str(concurrency / "test_hook_concurrency.exe")], cwd=concurrency,
                                capture_output=True, timeout=30)
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    summary["config_first_read_regression"] = {"processes": 120, "threads_each": 64, "all_exit_zero": True}
    (OUT / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({"passed": True, "jobs": observed["jobs"], "faults": faults,
                      "clone_callbacks": {key: value["callbacks"] for key, value in clones.items()}}))


if __name__ == "__main__":
    main()
