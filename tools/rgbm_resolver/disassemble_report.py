"""Read-only developer evidence: disassemble dynamically resolved function ranges.

Requires capstone in the selected Python environment. Never runs Unity or patches
code. JSON input must be a matched-PDB evidence_report from bc7_rgbm_resolver.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    report = json.loads(args.report.read_text(encoding="utf-8"))
    identity = report["binary_identity"]
    if report["artifact_kind"] != "evidence_report" or not identity["pdb_validated"]:
        raise ValueError("A matched-PDB evidence report is required")
    exe = Path(identity["exe_path"])
    if args.output.resolve() in (args.report.resolve(), exe.resolve()):
        raise ValueError("Output must not overwrite an input")
    data = exe.read_bytes()
    if hashlib.sha256(data).hexdigest().upper() != identity["sha256"]:
        raise ValueError("Executable identity has changed since resolution")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    sections = [struct.unpack_from("<III", data, pe + 24 + optional_size + i * 40 + 12)
                for i in range(count)]

    def code(rva, size):
        for address, raw_size, raw in sections:
            delta = rva - address
            if 0 <= delta and delta + size <= raw_size:
                return data[raw + delta:raw + delta + size]
        raise ValueError(f"Unmapped range 0x{rva:X}+0x{size:X}")

    roles = {}
    for kind in ("compress", "process", "semantic"):
        for candidate in report[kind + "_candidates"]:
            roles[candidate["rva"]] = (kind, candidate["unwind_end"])
    exception_rva, exception_size = struct.unpack_from("<II", data, pe + 24 + 112 + 3 * 8)
    ranges = {begin: end for begin, end, _ in struct.iter_unpack("<III", code(exception_rva, exception_size))}
    for candidate in report["callback_candidates"]:
        rva = candidate["rva"]
        roles[rva] = ("bc7_lambda", ranges[rva])
    for candidate in (report.get("process_dependencies", []) + report.get("callback_forwarders", []) +
                      report.get("fence_dependencies", [])):
        rva = candidate["rva"]
        end = candidate["unwind_end"] or rva + candidate["pdb_length"]
        if end > rva:
            roles[rva] = (candidate["name"], end)
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    lines = ["# Read-only RGBM interface evidence", f"exe={exe}", f"sha256={identity['sha256']}",
             f"pdb_guid={identity['pdb_guid']} age={identity['pdb_age']}"]
    for rva, (role, end) in roles.items():
        lines.append(f"\n## {role} [0x{rva:X},0x{end:X})")
        for instruction in decoder.disasm(code(rva, end - rva), rva):
            lines.append(f"{instruction.address:08X}  {instruction.bytes.hex():30s} "
                         f"{instruction.mnemonic} {instruction.op_str}")
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
