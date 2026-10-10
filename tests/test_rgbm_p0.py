"""P0 contract matrix. Runs copied native bodies with a substituted scheduler.

This does NOT launch Unity or certify the production detour/encoding pipeline.
"""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--resolver", type=Path, required=True)
    parser.add_argument("--clone", type=Path, required=True)
    parser.add_argument("--dia", type=Path, required=True)
    parser.add_argument("--f1", type=Path, required=True)
    parser.add_argument("--f2", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    reports = []
    commands = []
    for version, editor in (("f1", args.f1), ("f2", args.f2)):
        report_path = args.out / f"{version}-evidence.json"
        result = subprocess.run([str(args.resolver), "--exe", str(editor / "Unity.exe"),
                                 "--pdb", str(editor / "unity_x64.pdb"), "--dia", str(args.dia),
                                 "--report", str(report_path)], capture_output=True, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
        report = json.loads(report_path.read_text())
        assert report["binary_identity"]["pdb_validated"] and report["installable"] is False
        process_rva = report["process_candidates"][0]["rva"]
        dispatcher = [edge["target_rva"] for edge in report["process_edges"] if edge["kind"] == "dispatcher_rdx"]
        assert len(dispatcher) == 1
        helpers = []
        for prefix in ("?ScheduleJobForEachInternal@@", "?CompleteFenceInternal@@", "?ClearFenceWithoutSync@@"):
            found = [item["rva"] for item in report["process_dependencies"] if item["name"].startswith(prefix)]
            assert len(found) == 1, prefix
            helpers.extend(found)
        output = args.out / f"{version}-callback-clone.json"
        command = [str(args.clone), report["binary_identity"]["exe_path"], report["binary_identity"]["sha256"],
                   *(f"{rva:X}" for rva in [process_rva, dispatcher[0], *helpers]), str(output)]
        result = subprocess.run(command, capture_output=True, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
        observed = json.loads(output.read_text())
        assert observed["passed"] and observed["external_scheduler_substituted"] and not observed["unity_integration"]
        assert observed["jobs"] > 128 and observed["callbacks"] > 256
        assert observed["schedule"] == observed["complete"] == observed["clear"]
        assert observed["callbacks"] == observed["copied_payload_verified"] == observed["context_verified"] == observed["worker_tls_restored"]
        assert observed["active_slots_after_normal_exit"] == 0 and observed["pool_exhaustion_rejected"]
        assert observed["quarantined_slots"] == 2 and observed["abandon_is_unknown"]
        assert observed["exception_tls_restored"] and observed["quarantine_not_reused"]
        reports.append(report)
        commands.append(command)
    # Incorrect binary identity or old relocated RVA is refused before execution.
    wrong_sha = commands[0].copy(); wrong_sha[2] = "0" * 64; wrong_sha[-1] = str(args.out / "wrong-sha.json")
    result = subprocess.run(wrong_sha, capture_output=True, timeout=10)
    assert result.returncode == 1 and b"identity mismatch" in result.stderr and not Path(wrong_sha[-1]).exists()
    overwrite = commands[0].copy(); overwrite[-1] = overwrite[1]
    result = subprocess.run(overwrite, capture_output=True, timeout=10)
    assert result.returncode == 1 and b"unsafe output path" in result.stderr
    old_address = commands[1].copy(); old_address[3] = commands[0][3]; old_address[-1] = str(args.out / "old-rva.json")
    result = subprocess.run(old_address, capture_output=True, timeout=10)
    assert result.returncode == 1 and b"unknown process shape" in result.stderr and not Path(old_address[-1]).exists()
    assert reports[0]["compress_candidates"][0]["signature"] == reports[1]["compress_candidates"][0]["signature"]
    summary = {"passed": True, "versions": ["f1", "f2"], "same_clone_executable": str(args.clone),
               "production_plugin_tested": False, "wrong_sha_rejected": True, "old_rva_rejected": True}
    (args.out / "matrix-summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("P0 f1/f2 copied-body matrix and identity/relocation rejection passed")


if __name__ == "__main__":
    main()
