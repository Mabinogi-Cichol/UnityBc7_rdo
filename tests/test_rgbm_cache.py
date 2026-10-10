"""Native cache/scan matrix against inspected binaries plus non-executed mutations."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM
from capstone.x86_const import X86_REG_RIP


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--resolver", type=Path, required=True)
    parser.add_argument("--bindings", type=Path, required=True)
    parser.add_argument("--dia", type=Path, required=True)
    parser.add_argument("--f1", type=Path, required=True)
    parser.add_argument("--f2", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    cases = []

    def run(name, mode, exe, cache, expected=0):
        report = args.out / f"{name}.json"
        result = subprocess.run([str(args.bindings), "--mode", mode, "--exe", str(exe), "--cache", str(cache),
                                 "--report", str(report)], capture_output=True, timeout=30)
        assert result.returncode == expected, (name, result.stdout, result.stderr)
        output = json.loads(report.read_text())
        assert output["validated"] == (expected == 0) and output["hook_installed"] is False
        assert len(output["entries"]) == (11 if expected == 0 else 0)
        cases.append({"case": name, "exit_code": result.returncode, "reason": output["reason"]})
        return output

    reports = []
    for version, editor in (("f1", args.f1), ("f2", args.f2)):
        cache = args.out / f"{version}.bindings"
        evidence = args.out / f"{version}-evidence.json"
        command = [str(args.resolver), "--exe", str(editor / "Unity.exe"), "--pdb", str(editor / "unity_x64.pdb"),
                   "--dia", str(args.dia), "--report", str(evidence), "--cache", str(cache)]
        result = subprocess.run(command, capture_output=True, timeout=60)
        assert result.returncode == 0, (version, result.stdout, result.stderr)
        report = json.loads(evidence.read_text())
        assert report["binary_identity"]["pdb_validated"] and report["contract_verified"] and report["cache_written"]
        reports.append(run(f"{version}-verify", "verify", editor / "Unity.exe", cache))
        scan = args.out / f"{version}-scan.bindings"
        run(f"{version}-scan", "scan", editor / "Unity.exe", scan)
        assert scan.read_bytes() == cache.read_bytes(), "Same binary must produce the same cache by both routes"
        wrong = command.copy(); wrong[4] = str(args.f2 / "unity_x64.pdb")
        if version == "f1":
            wrong[-1] = str(args.out / "wrong-pdb.bindings"); wrong[8] = str(args.out / "wrong-pdb-evidence.json")
            result = subprocess.run(wrong, capture_output=True, timeout=60)
            assert result.returncode == 3 and not Path(wrong[-1]).exists()
            cases.append({"case": "wrong-pdb-cache", "exit_code": 3})
    run("stale-f1-cache-on-f2", "verify", args.f2 / "Unity.exe", args.out / "f1.bindings", 3)
    original = (args.out / "f1.bindings").read_text()

    def tamper(name, transform, rehash=True):
        text = transform(original)
        if rehash:
            body = text[:text.rfind("\n", 0, len(text) - 1) + 1]
            text = body + hashlib.sha256(body.encode()).hexdigest().upper() + "\n"
        path = args.out / f"{name}.bindings"; path.write_text(text, newline="")
        run(name, "verify", args.f1 / "Unity.exe", path, 3)

    tamper("bad-checksum", lambda text: text.replace("compress", "xompress"), False)
    tamper("old-schema", lambda text: text.replace("BINDINGS 1", "BINDINGS 0"))
    tamper("unknown-interface", lambda text: text.replace("callback.v1", "callback.v9"))
    tamper("duplicate-role", lambda text: text.replace("process ", "compress "))
    tamper("wrong-role-rva", lambda text: text.replace("dispatcher " + str(reports[0]["entries"][2]["rva"]),
                                                     "dispatcher " + str(reports[0]["entries"][3]["rva"])))
    tamper("wrong-entry-hash", lambda text: text.replace(reports[0]["entries"][0]["entry_sha256"], "0" * 64))
    tamper("forged-length", lambda text: text.replace("compress " + str(reports[0]["entries"][0]["rva"]) + " 2914 ",
                                                     "compress " + str(reports[0]["entries"][0]["rva"]) + " 2913 "))
    truncated = args.out / "truncated.bindings"; truncated.write_text(original[:50])
    run("truncated", "verify", args.f1 / "Unity.exe", truncated, 3)
    binary = (args.f1 / "Unity.exe").read_bytes()
    pe = struct.unpack_from("<I", binary, 0x3c)[0]
    section_count = struct.unpack_from("<H", binary, pe + 6)[0]
    optional_size = struct.unpack_from("<H", binary, pe + 20)[0]
    sections = [struct.unpack_from("<III", binary, pe + 24 + optional_size + i * 40 + 12) for i in range(section_count)]

    def raw(rva):
        for address, size, offset in sections:
            if 0 <= rva - address < size:
                return offset + rva - address
        raise AssertionError("unmapped mutation")

    decoder = Cs(CS_ARCH_X86, CS_MODE_64); decoder.detail = True
    process = reports[0]["entries"][1]
    offset = raw(process["rva"])
    body = binary[offset:offset + process["length"]]
    # Add a fall-through NOP before the inspected process body. Rebase external
    # rel32/RIP references and extend only its disk function range. No execution.
    shifted = bytearray(b"\x90" + body)
    for instruction in decoder.disasm(body, process["rva"]):
        local = instruction.address - process["rva"] + 1
        if instruction.mnemonic == "call" and instruction.imm_size == 4:
            position = local + instruction.imm_offset
            delta = struct.unpack_from("<i", shifted, position)[0]
            struct.pack_into("<i", shifted, position, delta - 1)
        if any(op.type == CS_OP_MEM and op.mem.base == X86_REG_RIP for op in instruction.operands):
            position = local + instruction.disp_offset
            delta = struct.unpack_from("<i", shifted, position)[0]
            struct.pack_into("<i", shifted, position, delta - 1)
    exception_rva, exception_size = struct.unpack_from("<II", binary, pe + 24 + 112 + 3 * 8)
    exception_offset = raw(exception_rva)
    ranges = [struct.unpack_from("<III", binary, exception_offset + i) for i in range(0, exception_size, 12)]
    changed = bytearray(binary); changed[offset:offset + len(shifted)] = shifted
    index = next(i for i, (begin, _, _) in enumerate(ranges) if begin == process["rva"])
    assert ranges[index + 1][0] > process["rva"] + len(shifted)
    struct.pack_into("<I", changed, exception_offset + index * 12 + 4, process["rva"] + len(shifted))
    nop = args.out / "synthetic-leading-nop.exe"; nop.write_bytes(changed)
    run("leading-nop-accepted", "scan", nop, args.out / "leading-nop.bindings")
    run("old-cache-new-prologue-rejected", "verify", nop, args.out / "f1.bindings", 3)
    # Alter a real callback/rows transfer instruction without changing its size.
    bad = bytearray(binary); position = body.index(bytes.fromhex("41894be0"))
    bad[offset + position + 2] = 0x43
    unknown = args.out / "synthetic-unknown-abi.exe"; unknown.write_bytes(bad)
    run("unknown-abi", "scan", unknown, args.out / "unknown-abi.bindings", 3)
    bad = bytearray(binary); predicate = reports[0]["entries"][5]; begin = raw(predicate["rva"])
    position = binary[begin:begin + predicate["length"]].index(bytes.fromhex("b824020000"))
    bad[begin + position + 1] = 0x20
    unknown.write_bytes(bad)
    run("unknown-usage", "scan", unknown, args.out / "unknown-usage.bindings", 3)
    bad = bytearray(binary); wait = reports[0]["entries"][9]
    jump = raw(wait["rva"]) + wait["length"] - 4
    struct.pack_into("<i", bad, jump, process["rva"] - (wait["rva"] + wait["length"]))
    unknown.write_bytes(bad)
    run("wrong-wait-tail-target", "scan", unknown, args.out / "wrong-wait-target.bindings", 3)
    # A second root with the same core but irrelevant call destinations must be
    # rejected as ambiguous, never selected by historical address/proximity.
    compress = reports[0]["entries"][0]
    duplicate_index = next(i for i, (begin, end, _) in enumerate(ranges)
                           if end - begin > compress["length"] + 32 and begin != compress["rva"])
    destination = ranges[duplicate_index][0]
    bad = bytearray(binary)
    cloned = bytearray(binary[raw(compress["rva"]):raw(compress["rva"]) + compress["length"]])
    for instruction in decoder.disasm(cloned, compress["rva"]):
        local = instruction.address - compress["rva"]
        external_jump = (instruction.mnemonic == "jmp" and instruction.imm_size == 4 and
                         not compress["rva"] <= instruction.operands[0].imm < compress["rva"] + compress["length"])
        if (instruction.mnemonic == "call" and instruction.imm_size == 4) or external_jump:
            position = local + instruction.imm_offset
            delta = struct.unpack_from("<i", cloned, position)[0]
            struct.pack_into("<i", cloned, position, delta + compress["rva"] - destination)
        if any(op.type == CS_OP_MEM and op.mem.base == X86_REG_RIP for op in instruction.operands):
            position = local + instruction.disp_offset
            delta = struct.unpack_from("<i", cloned, position)[0]
            struct.pack_into("<i", cloned, position, delta + compress["rva"] - destination)
    bad[raw(destination):raw(destination) + compress["length"]] = cloned
    struct.pack_into("<I", bad, exception_offset + duplicate_index * 12 + 4, destination + compress["length"])
    unknown.write_bytes(bad)
    run("ambiguous-scan-root", "scan", unknown, args.out / "ambiguous.bindings", 3)
    overwrite = subprocess.run([str(args.bindings), "--mode", "scan", "--exe", str(args.f1 / "Unity.exe"),
                                "--cache", str(args.f1 / "Unity.exe"), "--report", str(args.out / "overwrite.json")],
                               capture_output=True, timeout=10)
    assert overwrite.returncode == 2
    (args.out / "summary.json").write_text(json.dumps({"passed": True, "cases": cases,
        "synthetic_binaries_executed": False, "unity_hook_installed": False}, indent=2) + "\n")
    print(f"Native cache and bounded-scan matrix passed: {len(cases)} cases")


if __name__ == "__main__":
    main()
