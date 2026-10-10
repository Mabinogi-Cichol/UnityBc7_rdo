"""Developer-only P0 static contract proof for the inspected interface shape.

Capstone is a development dependency. Output is NOT an installable cache and
must not be used to accept a different ABI merely because addresses were found.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM
from capstone.x86_const import X86_REG_RIP

KNOWN_BODY = {
    "process": "67114899dd5f1104c61693995f0864bb93c1f8681bee3963db8dd765fde7c273",
    "dispatcher": "486a2e9c09d4a126d35ac51e15ef9b7474b4a522435da5c1867dcd012dfefd0b",
    "forwarder": "4b1adc1c8a4679e12377356899ab88da54baf4d181de8f3e86a7c73d4a95771f",
}
EXPECTED_COMPRESS = "?CompressSingleImageTexture@@YAXAEAVImageReference@@W4TextureFormat@@W4TextureUsageMode@@H_N33QEAEQEBVTexture@@@Z"


def prove(path):
    report = json.loads(path.read_text(encoding="utf-8"))
    identity = report["binary_identity"]
    assert report["artifact_kind"] == "evidence_report" and identity["pdb_validated"]
    data = Path(identity["exe_path"]).read_bytes()
    assert hashlib.sha256(data).hexdigest().upper() == identity["sha256"], "binary changed"
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    sections = [struct.unpack_from("<III", data, pe + 24 + optional_size + i * 40 + 12) for i in range(count)]

    def code(candidate):
        rva = candidate["rva"]
        size = candidate["unwind_end"] - rva if candidate["unwind_end"] else candidate["pdb_length"]
        for address, raw_size, raw_offset in sections:
            delta = rva - address
            if 0 <= delta and delta + size <= raw_size:
                return data[raw_offset + delta:raw_offset + delta + size]
        raise ValueError("unmapped candidate")

    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    assert len(report["compress_candidates"]) == 1
    assert report["compress_candidates"][0]["name"] == EXPECTED_COMPRESS
    assert len(report["callback_forwarders"]) == 1 and len(report["semantic_candidates"]) == 1
    dispatcher_rva = [edge["target_rva"] for edge in report["process_edges"] if edge["kind"] == "dispatcher_rdx"]
    assert len(dispatcher_rva) == 1
    dispatcher = [candidate for candidate in report["process_dependencies"] if candidate["rva"] == dispatcher_rva[0]]
    assert len(dispatcher) == 1
    roles = {"process": report["process_candidates"][0], "dispatcher": dispatcher[0],
             "forwarder": report["callback_forwarders"][0]}
    body_evidence = {}
    for role, candidate in roles.items():
        raw = code(candidate)
        instructions = list(decoder.disasm(raw, candidate["rva"]))
        assert sum(instruction.size for instruction in instructions) == len(raw), "incomplete decode"
        normalized = bytearray(raw)
        for instruction in instructions:
            offset = instruction.address - candidate["rva"]
            if instruction.mnemonic in ("call", "jmp") and instruction.imm_size == 4:
                normalized[offset + instruction.imm_offset:offset + instruction.imm_offset + 4] = bytes(4)
            if any(op.type == CS_OP_MEM and op.mem.base == X86_REG_RIP for op in instruction.operands):
                begin = offset + instruction.disp_offset
                normalized[begin:begin + instruction.disp_size] = bytes(instruction.disp_size)
        digest = hashlib.sha256(normalized).hexdigest()
        assert digest == KNOWN_BODY[role], f"unrecognized {role} interface body"
        body_evidence[role] = {"rva": candidate["rva"], "normalized_sha256": digest, "length": len(raw)}
    predicate = report["semantic_candidates"][0]
    instructions = list(decoder.disasm(code(predicate), predicate["rva"]))
    matching = []
    for i in range(len(instructions) - 5):
        sequence = instructions[i:i + 6]
        if (sequence[0].mnemonic == "cmp" and sequence[0].op_str == "esi, 9" and
                sequence[1].mnemonic == "ja" and sequence[2].mnemonic == "mov" and
                sequence[2].op_str == "eax, 0x224" and sequence[3].mnemonic == "bt" and
                sequence[3].op_str == "eax, esi" and sequence[4].mnemonic == "jae" and
                sequence[5].mnemonic == "mov" and sequence[5].op_str == "dil, 1"):
            matching.append(sequence[0].address)
    assert len(matching) == 1, "native RGBM predicate not proved"
    call_names = [item["name"] for item in report["process_dependencies"]]
    expected = ["?ScheduleJobForEachInternal@@", "?CompleteFenceInternal@@", "?ClearFenceWithoutSync@@"]
    for name in expected:
        assert sum(item.startswith(name) for item in call_names) == 1, "missing/ambiguous lifecycle callee"
    wait = [item for item in report["fence_dependencies"]
            if item["name"].startswith("?WaitForJobGroupID@JobQueue@@")]
    assert len(wait) == 1, "native fence does not uniquely reach the named job-group wait"
    assert sum(edge["kind"] == "call" and edge["target_rva"] == wait[0]["rva"]
               for edge in report["fence_edges"]) == 1
    return {"exe_identity": identity, "interface_id": "unity.threaded-copy-for-each-callback.v1",
            "body_evidence": body_evidence, "native_rgbm_predicate_slice_rva": matching[0],
            "rgbm_usage_values": [2, 5, 9], "compress_signature": report["compress_candidates"][0]["signature"],
            "process_abi": "void(int rows, const void* source_job, void(*)(void* copied_job,int begin,int end))",
            "scope_end": "normal return after CompleteFenceInternal and ClearFenceWithoutSync",
            "context_method": "scoped precompiled callback identity; opaque JobData; no source-pixel field read",
            "native_wait": {"rva": wait[0]["rva"], "signature": wait[0]["signature"]},
            "pdb_private_types_available": False}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--f1", type=Path, required=True)
    parser.add_argument("--f2", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.out.resolve() in (args.f1.resolve(), args.f2.resolve()):
        raise ValueError("Output must not overwrite an evidence input")
    for path in (args.f1, args.f2):
        identity = json.loads(path.read_text(encoding="utf-8"))["binary_identity"]
        if args.out.resolve() == Path(identity["exe_path"]).resolve():
            raise ValueError("Output must not overwrite a binary input")
    versions = [prove(args.f1), prove(args.f2)]
    assert versions[0]["compress_signature"] == versions[1]["compress_signature"]
    assert all(versions[0]["body_evidence"][role]["normalized_sha256"] ==
               versions[1]["body_evidence"][role]["normalized_sha256"] for role in KNOWN_BODY)
    output = {"artifact_kind": "p0_static_interface_proof", "installable": False,
              "versions": versions, "runtime_validation_pending": True,
              "unknowns": ["production slot exhaustion/exception handling and Ready state transitions",
                           "Unity detour thread lifecycle coordination",
                           "P1 scanner/general ABI certification and runtime cache",
                           "P4 clean-machine deployment and P5 actual Unity encoding matrix"]}
    args.out.write_text(json.dumps(output, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("P0 matching signatures, process/dispatcher/forwarder body shapes and native RGBM predicate proved")


if __name__ == "__main__":
    main()
