"""Integration rejection tests against actual exe/PDB; no Unity process launched."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--resolver", type=Path, required=True)
    parser.add_argument("--dia", type=Path, required=True)
    parser.add_argument("--f1", type=Path, required=True, help="Editor directory")
    parser.add_argument("--f2", type=Path, required=True, help="Editor directory")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    cases = [("f1", args.f1, args.f1 / "unity_x64.pdb", 0),
             ("f2", args.f2, args.f2 / "unity_x64.pdb", 0),
             ("wrong-pdb", args.f1, args.f2 / "unity_x64.pdb", 3),
             ("missing-pdb", args.f1, args.out / "nonexistent.pdb", 3)]
    summary = []
    for name, editor, pdb, expected in cases:
        output = args.out / f"{name}.json"
        result = subprocess.run([str(args.resolver), "--exe", str(editor / "Unity.exe"),
                                 "--pdb", str(pdb), "--dia", str(args.dia), "--report", str(output)],
                                capture_output=True, timeout=60)
        if result.returncode != expected:
            raise AssertionError(f"{name}: exit={result.returncode} expected={expected} stdout={result.stdout!r}")
        report = json.loads(output.read_text())
        assert report["artifact_kind"] == "evidence_report"
        assert report["installable"] is False and report["state"] == "Unavailable"
        if expected == 0:
            assert report["binary_identity"]["pdb_validated"] is True
            assert len(report["callback_candidates"]) == 1
            assert "lambda_" not in report["callback_candidates"][0].get("source", "")
        else:
            assert report["binary_identity"]["pdb_validated"] is False
            assert not report["compress_candidates"] and not report["process_candidates"]
            assert not report["semantic_candidates"]
            assert not report["callback_candidates"] and not report["call_edges"]
        summary.append({"case": name, "exit_code": result.returncode, "reason": report["reason"]})
    # Directing output at an input must fail before attempting to open/publish it.
    exe = args.f1 / "Unity.exe"
    result = subprocess.run([str(args.resolver), "--exe", str(exe), "--pdb", str(args.f1 / "unity_x64.pdb"),
                             "--dia", str(args.dia), "--report", str(exe)], capture_output=True, timeout=10)
    assert result.returncode == 2 and b"unsafe_output_path" in result.stderr
    assert not list(args.out.glob("*.tmp.*"))
    (args.out / "test-summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("matched f1/f2, wrong/missing PDB rejection, input overwrite guard and atomic publication passed")


if __name__ == "__main__":
    main()
