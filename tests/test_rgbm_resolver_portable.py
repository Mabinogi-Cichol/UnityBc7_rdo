"""Local private-DIA smoke test; this does not certify a machine without VS."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--source-dia", type=Path, required=True)
    parser.add_argument("--f1", type=Path, required=True)
    parser.add_argument("--f2", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    package = args.package.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    dia = package / "msdia140.dll"
    assert hashlib.sha256(dia.read_bytes()).digest() == hashlib.sha256(args.source_dia.read_bytes()).digest()
    env = os.environ.copy()
    windows = ctypes.create_unicode_buffer(32768)
    assert ctypes.windll.kernel32.GetWindowsDirectoryW(windows, len(windows))
    env["PATH"] = str(Path(windows.value) / "System32")
    results = []
    for name, editor, dependency, expected in (("f1", args.f1, dia, 0),
                                               ("f2", args.f2, dia, 0),
                                               ("missing-dia", args.f1, package / "missing.dll", 3)):
        path = args.out / f"{name}.json"
        result = subprocess.run([str(package / "bc7_rgbm_resolver.exe"), "--exe", str(editor / "Unity.exe"),
                                 "--pdb", str(editor / "unity_x64.pdb"), "--dia", str(dependency),
                                 "--report", str(path)], capture_output=True, cwd=package, env=env, timeout=60)
        assert result.returncode == expected, result.stdout + result.stderr
        report = json.loads(path.read_text())
        assert report["dia_load_method"] == "private_DllGetClassObject"
        if expected == 0:
            assert Path(report["dia_loaded_path"]).resolve() == dia
            assert report["binary_identity"]["pdb_validated"]
        else:
            assert not report["dia_loaded_path"] and report["reason"].startswith("dia_load_failed:")
            assert not report["process_candidates"]
        results.append({"case": name, "exit_code": expected, "loaded_dia": report["dia_loaded_path"]})
    summary = {"passed": True, "path_scope": "System32 only", "clean_machine_tested": False,
               "global_com_registration_used": False, "results": results}
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("Private staged DIA: f1/f2 and missing dependency rejection passed; clean-machine test pending")


if __name__ == "__main__":
    main()
