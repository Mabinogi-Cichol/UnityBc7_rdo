"""Validate completed P5 reports and actual block files; never poll Unity logs."""
import argparse
import hashlib
import json
import math
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "out/rgbm-portability/acceptance"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reports", nargs="+", default=["f1-batch", "f2-batch", "f1-editor", "f2-editor", "f1-fallback", "f2-fallback", "user-f2-batch"])
    args = parser.parse_args()
    hook_hash = digest(OUT / "Bc7Rdo/bc7_rdo_hook.dll")
    matrix = []
    total_imports = total_calls = total_blocks = 0
    output_files = set()
    main_outputs = {}
    production_files = [path for path in (OUT / "Bc7Rdo").rglob("*") if path.suffix in (".cs", ".dll", ".exe")]
    physical_names = [f"{name}-{route}" for name in args.reports for route in ("main", "worker")]
    for name in physical_names:
        report_path = OUT / f"{name}.json"
        report = json.loads(report_path.read_text(encoding="utf-8-sig"))
        exit_report = json.loads((OUT / f"{name}.exit.json").read_text(encoding="utf-8-sig"))
        assert exit_report["exit_code"] == 0 and report["passed"], (name, report.get("error"))
        assert report["pid"] == exit_report["pid"]
        assert report["batchmode"] != exit_report["editor_mode"]
        assert report["hook_sha256"] == hook_hash
        project_plugin = Path(exit_report["project"]) / "Assets/Plugins/Bc7Rdo"
        for source in production_files:
            relative = source.relative_to(OUT / "Bc7Rdo")
            target = project_plugin / relative
            if exit_report["fallback"] and source.name.startswith("oo2tex"):
                assert not target.exists(), (name, "Oodle unexpectedly present")
            else:
                assert digest(target) == digest(source), (name, "production file differs", relative)
        assert report["state"] == 4
        pairs = report.get("fixture_pairs") or 6
        assert report["worker_count"] >= min(2, pairs) if report["route"] == "worker" else report["worker_count"] == 0
        assert report["route"] == exit_report["route"]
        assert report["config_restored"] and report["fixtures_removed"]
        assert report["unknown_oodle_native"] and report["unknown_ert_native"]
        assert report["lightmap_encoding_quality"] == "Normal"
        assert len(report["phases"]) == (5 if report["fallback_mode"] else 9)
        processes = {}
        for text in report["runtime_reports"]:
            runtime = json.loads(text)
            assert runtime["state"] == "Ready" and runtime["hook_installed"]
            assert runtime["exe_sha256"] == report["exe_sha256"]
            processes[runtime["pid"]] = runtime
        phase_rows = []
        for phase in report["phases"]:
            assert phase["route"] in ("main", "worker")
            assert phase["route"] == report["route"]
            assert (phase["main_calls"] > 0) == (phase["route"] == "main")
            assert (phase["worker_calls"] > 0) == (phase["route"] == "worker")
            calls = phase["main_calls"] + phase["worker_calls"]
            assert len(phase["native_events"]) == calls
            logged_blocks = sum(int(re.search(r"\bnum_blocks=(\d+)", event).group(1)) for event in phase["native_events"])
            assert logged_blocks == sum(sample["bytes"] // 16 for sample in phase["samples"]), (name, phase["name"], "native log block coverage incomplete")
            total_blocks += logged_blocks
            for event in phase["native_events"]:
                if "RDO called (bc7enc_rdo)" in event:
                    metric = int(re.search(r"\brgbm=(\d+)", event).group(1))
                    context = int(re.search(r"\bcontext_rgbm=(\d+)", event).group(1))
                    assert metric == context, (name, "ERT metric differs from actual usage", event)
            rgbm_count = ordinary_count = 0
            changes = {"ordinary": 0, "rgbm": 0}
            losses = {"ordinary": 0, "rgbm": 0}
            for sample in phase["samples"]:
                path = Path(sample["file"])
                assert path.is_file() and digest(path) == sample["sha256"]
                assert path.stat().st_size == sample["bytes"] and sample["bytes"] % 16 == 0
                assert sample["width"] <= 64 and sample["height"] <= 64 and sample["mip_count"] > 1
                assert "BC7" in sample["format"]
                key = (name.rsplit("-", 1)[0], phase["name"], path.name)
                if report["route"] == "main":
                    main_outputs[key] = sample["sha256"]
                else:
                    assert sample["sha256"] == main_outputs[key], (name, "Worker output differs from main output", path.name)
                if sample["rgbm"]:
                    assert sample["usage"] == "BakedLightmapRGBM" and sample["usage_value"] == 2
                    rgbm_count += 1
                else:
                    ordinary_count += 1
                lane = "rgbm" if sample["rgbm"] else "ordinary"
                changes[lane] += sample["changed_blocks"]
                mse = sample["linear_rgb_times_m_mse_from_native"] if sample["rgbm"] else sample["encoded_mse_from_native"]
                assert math.isfinite(mse) and mse >= 0
                losses[lane] = max(losses[lane], mse)
                output_files.add(path)
            assert rgbm_count == ordinary_count == pairs
            total_imports += len(phase["samples"])
            total_calls += calls
            phase_rows.append({"route": phase["route"], "case": phase["name"], "calls": calls, "changed_blocks": changes, "max_native_reference_mse": losses,
                               "elapsed_ms": phase.get("elapsed_ms")})
        matrix.append({"name": name, "unity_version": report["unity_version"], "batchmode": report["batchmode"], "fallback_mode": report["fallback_mode"],
                       "pid": report["pid"], "workers": sorted(pid for pid, item in processes.items() if item["asset_import_worker"]),
                       "exe_sha256": report["exe_sha256"], "source_hdr_sha256": report["source_hdr_sha256"], "phases": phase_rows})
    regression = json.loads((OUT / "native-regression.json").read_text())
    assert regression["passed"] and regression["all_exit_zero"] and regression["processes"] == 120 and regression["threads_each"] == 64
    assert regression["hook_sha256"] == hook_hash
    user_cleanup = None
    if "user-f2-batch" in args.reports:
        user_cleanup = json.loads((OUT / "user-cleanup.json").read_text(encoding="utf-8-sig"))
        assert user_cleanup["passed"] and user_cleanup["protected_files_unchanged"] and user_cleanup["driver_removed"]
        assert user_cleanup["fixtures_remaining"] == 0 and user_cleanup["hook_sha256"] == hook_hash
        for item in user_cleanup["files"]:
            assert digest(Path(item["path"])) == item["sha256"], ("protected user file changed after final compile", item["path"])
        final_compile = json.loads((OUT / "user-final-compile.exit.json").read_text(encoding="utf-8-sig"))
        assert final_compile["exit_code"] == 0
        user_project = Path(json.loads((OUT / "user-f2-batch-main.exit.json").read_text(encoding="utf-8-sig"))["project"])
        runtime_paths = list((user_project / "Library/Bc7Rdo/HookBindings").rglob(f'{final_compile["pid"]}.*.runtime.json'))
        assert len(runtime_paths) == 1, ("final compile runtime identity missing or ambiguous", runtime_paths)
        runtime = json.loads(runtime_paths[0].read_text())
        assert runtime["state"] == "Ready" and runtime["hook_installed"] and not runtime["asset_import_worker"]
        assert runtime["exe_sha256"] == report["exe_sha256"] and runtime["pid"] == final_compile["pid"]
        (OUT / "user-final-runtime.json").write_text(json.dumps(runtime, indent=2) + "\n")
        user_cleanup["final_compile"] = final_compile
        user_cleanup["final_runtime"] = runtime
    summary = {"passed": True, "same_production_hook_sha256": hook_hash, "matrix": matrix, "texture_imports_checked": total_imports,
               "native_encoding_calls_checked": total_calls, "output_files_verified": len(output_files),
               "native_blocks_checked": total_blocks, "main_worker_output_pairs_checked": total_imports // 2,
               "configuration_concurrency_regression": regression, "actual_project_cleanup": user_cleanup,
               "quality_scope": "Decoded differences from Unity native BC7; no source-HDR or visual quality acceptance claim.",
               "additional_unverified_scenario": "Unity semantic Ready before the first BC7 module load; not a P4 gate."}
    (OUT / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    files = set(output_files)
    files.update(OUT / f"{name}{suffix}" for name in physical_names for suffix in (".json", ".exit.json", ".log"))
    files.update((OUT / "Bc7Rdo").rglob("*"))
    files.add(OUT / "summary.json")
    files.update(OUT / name for name in ("native-regression.json", "concurrency.log", "test-hook-immediate.log", "test-hook-late.log"))
    if user_cleanup:
        files.update(OUT / name for name in ("user-cleanup.json", "user-files-before.json", "user-final-compile.exit.json", "user-final-compile.log", "user-final-runtime.json"))
    manifest = [{"path": str(path), "sha256": digest(path)} for path in sorted(files) if path.is_file()]
    (OUT / "artifact-sha256.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "runs": len(matrix), "texture_imports": total_imports, "encoding_calls": total_calls, "outputs": len(output_files)}))


if __name__ == "__main__":
    main()
