"""Developer-only first-profile generation, gated by the existing P0 proof.

Never run on an unknown cache. This script does not ship with the plugin and
cannot silently extend compatibility: both inspected reports must pass P0.
"""
import argparse
import json
from pathlib import Path
import subprocess
import struct
from verify_p0_contract import prove


def entries(report):
    process = report["process_candidates"][0]
    forwarder = report["callback_forwarders"][0]
    dispatcher_rva = next(e["target_rva"] for e in report["process_edges"] if e["kind"] == "dispatcher_rdx")
    dependencies = report["process_dependencies"]
    dispatcher = next(e for e in dependencies if e["rva"] == dispatcher_rva)
    roles = [report["compress_candidates"][0], process, dispatcher, forwarder,
             report["callback_candidates"][0], report["semantic_candidates"][0]]
    for prefix in ("?ScheduleJobForEachInternal@@", "?CompleteFenceInternal@@", "?ClearFenceWithoutSync@@"):
        roles.append(next(e for e in dependencies if e["name"].startswith(prefix)))
    roles.append(next(e for e in report["fence_dependencies"] if e["name"].startswith("?WaitForJobGroupID@JobQueue@@")))
    output = [(e["rva"], e["unwind_end"] - e["rva"] if e["unwind_end"] else e["pdb_length"]) for e in roles]
    data = Path(report["binary_identity"]["exe_path"]).read_bytes()
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    size = struct.unpack_from("<H", data, pe + 20)[0]
    sections = [struct.unpack_from("<III", data, pe + 24 + size + i * 40 + 12) for i in range(count)]
    raw = lambda rva: next(offset + rva - address for address, length, offset in sections if 0 <= rva - address < length)
    wait, length = output[-1]
    body = data[raw(wait):raw(wait) + length]
    assert body[-5] == 0xe9  # The complete P0 wait forwarding body was independently inspected.
    target = wait + length + struct.unpack_from("<i", body, length - 4)[0]
    exception, size = struct.unpack_from("<II", data, pe + 24 + 112 + 24)
    ranges = list(struct.iter_unpack("<III", data[raw(exception):raw(exception) + size]))
    matches = [end - begin for begin, end, _ in ranges if begin == target]
    assert len(matches) == 1
    output.append((target, matches[0]))
    return output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--f1", type=Path, required=True)
    parser.add_argument("--f2", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    observations = []
    for path in (args.f1, args.f2):
        prove(path)
        report = json.loads(path.read_text())
        fingerprints = []
        lengths = []
        for rva, size in entries(report):
            result = subprocess.run([str(args.probe), report["binary_identity"]["exe_path"], f"{rva:X}", str(size)],
                                    capture_output=True, timeout=15, check=True)
            digest = result.stdout.decode().strip()
            assert len(digest) == 64 and all(c in "0123456789ABCDEF" for c in digest)
            fingerprints.append(digest)
            lengths.append(size)
        observations.append((fingerprints, lengths))
    assert observations[0] == observations[1], "Profiles differ; do not extend acceptance automatically"
    hashes, lengths = observations[0]
    source = ('#pragma once\n// Evidence-derived first interface profile; no historical RVA or version allowlist.\n'
              '// Generated only after both matched-PDB reports pass the independent P0 proof.\n'
              'namespace rgbm {\ninline const char* const profile_hashes[role_count] = {\n' +
              ''.join(f'    "{h}",\n' for h in hashes) + '};\n' +
              'inline constexpr uint32_t profile_lengths[role_count] = {' + ','.join(map(str, lengths)) + '};\n}\n')
    args.out.write_text(source, encoding="utf-8")
    print("Both certified P0 shapes produced the same first interface profile")


if __name__ == "__main__":
    main()
