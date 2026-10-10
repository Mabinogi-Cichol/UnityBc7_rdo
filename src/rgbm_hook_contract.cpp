#include "rgbm_hook_contract.h"
#include "rgbm_hook_profile.h"
#include <detours.h>
#include <cstring>
#include <set>
#include <algorithm>

namespace rgbm {
size_t disk_instruction_length(uint8_t* source) {
    size_t prefix = 0;
    while (prefix < 15 && (source[prefix] == 0x66 || source[prefix] == 0x67 || source[prefix] == 0xf0 ||
        source[prefix] == 0xf2 || source[prefix] == 0xf3 || source[prefix] == 0x2e || source[prefix] == 0x36 ||
        source[prefix] == 0x3e || source[prefix] == 0x26 || source[prefix] == 0x64 || source[prefix] == 0x65 ||
        (source[prefix] & 0xf0) == 0x40)) ++prefix;
    if (prefix == 15) return 0;
    // Detours CopyFF follows FF /2,/4 RIP memory pointers, which are not valid
    // pointers in a disk buffer. FF /0 has exactly the same addressing length
    // and no pointer dereference. Restore the byte before returning. We never
    // use its relocated output or inferred target, only instruction length.
    bool indirect = source[prefix] == 0xff && (source[prefix + 1] == 0x15 || source[prefix + 1] == 0x25);
    uint8_t original = source[prefix + 1]; if (indirect) source[prefix + 1] = 0x05;
    size_t length = 0;
    __try {
        auto* next = static_cast<uint8_t*>(DetourCopyInstruction(nullptr, nullptr, source, nullptr, nullptr));
        if (next > source && next - source <= 15) length = size_t(next - source);
    } __except(EXCEPTION_EXECUTE_HANDLER) { length = 0; }
    if (indirect) source[prefix + 1] = original;
    return length;
}
namespace {
struct Instruction { uint32_t offset, length; std::vector<uint8_t> bytes; uint32_t call = 0, lea_r8 = 0, lea_rdx = 0, jump = 0; };
bool decode(const PeImage& image, uint32_t rva, uint32_t length, std::vector<Instruction>& result) {
    result.clear();
    if (!length || length > 16384) return false;
    const auto* data = image.at(rva, length, true); if (!data) return false;
    std::vector<uint8_t> copy(data, data + length); copy.resize(length + 32, 0);
    for (uint32_t offset = 0; offset < length;) {
        auto* p = copy.data() + offset;
        auto size = uint32_t(disk_instruction_length(p));
        if (!size || size > length - offset) return false;
        Instruction instruction{offset, size, {p, p + size}};
        auto target = [&](unsigned displacement) -> uint32_t {
            int32_t value; memcpy(&value, p + displacement, 4);
            int64_t address = int64_t(rva) + offset + size + value;
            return address > 0 && address <= UINT32_MAX ? uint32_t(address) : 0;
        };
        if (size == 5 && p[0] == 0xe8) instruction.call = target(1);
        if (size == 5 && p[0] == 0xe9) instruction.jump = target(1);
        if (size == 7 && p[0] == 0x4c && p[1] == 0x8d && p[2] == 0x05) instruction.lea_r8 = target(3);
        if (size == 7 && p[0] == 0x48 && p[1] == 0x8d && p[2] == 0x15) instruction.lea_rdx = target(3);
        result.push_back(std::move(instruction)); offset += size;
    }
    return true;
}
// This recognizer only normalizes the legacy encodings used by the first
// profile. VEX/EVEX and unfamiliar opcode maps retain their bytes and therefore
// cannot accidentally inherit the accepted profile. Instruction lengths are
// always obtained from the decoder, never by scanning E8 inside immediates.
unsigned rip_displacement(const std::vector<uint8_t>& b) {
    size_t i = 0;
    while (i < b.size() && (b[i] == 0x66 || b[i] == 0xf2 || b[i] == 0xf3 ||
        b[i] == 0x2e || b[i] == 0x3e || b[i] == 0x64 || b[i] == 0x65 || (b[i] & 0xf0) == 0x40)) ++i;
    if (i >= b.size()) return 0;
    uint8_t op = b[i++];
    bool modrm = (op >= 0x80 && op <= 0x8f) || op == 0x01 || op == 0x03 || op == 0x21 || op == 0x23 ||
        op == 0x29 || op == 0x2b || op == 0x31 || op == 0x33 || op == 0x39 || op == 0x3b ||
        op == 0xc6 || op == 0xc7 || op == 0xf6 || op == 0xf7 || op == 0xfe || op == 0xff;
    if (op == 0x0f) {
        if (i >= b.size()) return 0;
        op = b[i++];
        modrm = (op >= 0x10 && op <= 0x17) || (op >= 0x28 && op <= 0x2f) ||
            (op >= 0x40 && op <= 0x7f) || (op >= 0x90 && op <= 0x9f) || op == 0xa3 ||
            op == 0xaf || op == 0xb6 || op == 0xb7 || op == 0xbe || op == 0xbf;
    }
    return modrm && i + 5 <= b.size() && (b[i] & 0xc7) == 0x05 ? unsigned(i + 1) : 0;
}
uint32_t function_length(const PeImage& image, uint32_t rva) {
    auto range = image.function(rva); return range.begin ? range.end - range.begin : 0;
}
uint32_t leaf_length(const PeImage& image, uint32_t rva, uint32_t nominal) {
    uint32_t nops = 0;
    for (; nops < 16; ++nops) { auto p = image.at(rva + nops, 1, true); if (!p || *p != 0x90) break; }
    return image.at(rva, nops + nominal, true) ? nops + nominal : 0;
}
bool matches(const PeImage& image, Role role, uint32_t rva, uint32_t length) {
    auto expected = profile_lengths[static_cast<size_t>(role)];
    if (!expected || !profile_hashes[static_cast<size_t>(role)] || length < expected || length > expected + 16) return false;
    return canonical_body_hash(image, rva, length) == profile_hashes[static_cast<size_t>(role)];
}
bool install_entry(const PeImage& image, ContractBindings& bindings, Role role, uint32_t rva, uint32_t length) {
    if (!rva || !length || !matches(image, role, rva, length)) return false;
    const auto* data = image.at(rva, std::max<uint32_t>(length, 32), true);
    if (!data) return false;
    bindings.entries[static_cast<size_t>(role)] = {rva, length, sha256(data, 32)}; return true;
}
uint32_t tail_jump(const PeImage& image, uint32_t rva, uint32_t length) {
    std::vector<Instruction> instructions;
    return decode(image, rva, length, instructions) && !instructions.empty() ? instructions.back().jump : 0;
}
}
const char* role_name(Role role) {
    static const char* names[] = {"compress", "process", "dispatcher", "forwarder", "lambda", "predicate",
        "schedule", "complete", "clear", "wait", "wait_target"};
    return static_cast<size_t>(role) < role_count ? names[static_cast<size_t>(role)] : "invalid";
}
std::string canonical_body_hash(const PeImage& image, uint32_t rva, uint32_t length) {
    std::vector<Instruction> instructions; if (!decode(image, rva, length, instructions)) return {};
    std::vector<uint8_t> normalized; bool started = false;
    for (const auto& instruction : instructions) {
        auto b = instruction.bytes;
        if (!started && b.size() == 1 && b[0] == 0x90) continue;
        started = true;
        if (instruction.call || (instruction.jump && (instruction.jump < rva || instruction.jump >= uint64_t(rva) + length)))
            memset(b.data() + 1, 0, 4);
        auto displacement = rip_displacement(b);
        if (displacement) memset(b.data() + displacement, 0, 4);
        normalized.insert(normalized.end(), b.begin(), b.end());
    }
    return normalized.empty() ? "" : sha256(normalized.data(), normalized.size());
}
bool certify_contract(const PeImage& image, const ContractRoots& roots, ContractBindings& bindings, std::string& error) {
    bindings = {}; ContractBindings pending; pending.identity = image.identity();
    auto fail = [&](const char* reason) { error = reason; return false; };
    if (!install_entry(image, pending, Role::Compress, roots.compress, function_length(image, roots.compress)) ||
        !install_entry(image, pending, Role::Predicate, roots.predicate, function_length(image, roots.predicate)))
        return fail("compress_or_usage_contract_unknown");
    std::vector<Instruction> compress; if (!decode(image, roots.compress, function_length(image, roots.compress), compress)) return fail("compress_decode_failed");
    // The actual BC7 path must carry a three-argument forwarder in R8 into the
    // uniquely certified process, and that forwarder must reach the BC7 import.
    std::set<std::pair<uint32_t, uint32_t>> edges;
    for (size_t i = 0; i < compress.size(); ++i) if (compress[i].call) {
        const auto process = compress[i].call;
        if (roots.process && roots.process != process) continue;
        if (!matches(image, Role::Process, process, function_length(image, process))) continue;
        for (size_t j = i; j-- > 0;) {
            if (compress[i].offset - compress[j].offset > 32) break;
            if (!compress[j].lea_r8) continue;
            const auto forwarder = compress[j].lea_r8;
            auto length = leaf_length(image, forwarder, 16);
            auto lambda = tail_jump(image, forwarder, length);
            if (!matches(image, Role::Forwarder, forwarder, length) ||
                !matches(image, Role::Lambda, lambda, function_length(image, lambda))) continue;
            std::vector<Instruction> callback;
            if (!decode(image, lambda, function_length(image, lambda), callback)) continue;
            bool import = false;
            for (const auto& instruction : callback)
                if (instruction.call && image.imported_function(instruction.call) == "compress_bc7e.dll!bc7elib_compress_blocks") import = true;
            if (import) edges.emplace(process, forwarder);
        }
    }
    if (edges.size() != 1) return fail("bc7_call_chain_missing_or_ambiguous");
    auto process = edges.begin()->first, forwarder = edges.begin()->second;
    auto lambda = tail_jump(image, forwarder, leaf_length(image, forwarder, 16));
    if (!install_entry(image, pending, Role::Process, process, function_length(image, process)) ||
        !install_entry(image, pending, Role::Forwarder, forwarder, leaf_length(image, forwarder, 16)) ||
        !install_entry(image, pending, Role::Lambda, lambda, function_length(image, lambda))) return fail("bc7_abi_unknown");
    std::vector<Instruction> body; if (!decode(image, process, function_length(image, process), body)) return fail("process_decode_failed");
    std::vector<uint32_t> calls, dispatchers;
    for (const auto& instruction : body) { if (instruction.call) calls.push_back(instruction.call); if (instruction.lea_rdx) dispatchers.push_back(instruction.lea_rdx); }
    if (calls.size() != 3 || dispatchers.size() != 1 ||
        !install_entry(image, pending, Role::Dispatcher, dispatchers[0], leaf_length(image, dispatchers[0], 31)) ||
        !install_entry(image, pending, Role::Schedule, calls[0], function_length(image, calls[0])) ||
        !install_entry(image, pending, Role::Complete, calls[1], function_length(image, calls[1])) ||
        !install_entry(image, pending, Role::Clear, calls[2], leaf_length(image, calls[2], 9))) return fail("copy_dispatch_join_contract_unknown");
    std::vector<Instruction> complete; decode(image, calls[1], function_length(image, calls[1]), complete);
    std::set<uint32_t> waits;
    for (const auto& instruction : complete) if (instruction.call &&
        matches(image, Role::Wait, instruction.call, leaf_length(image, instruction.call, 24))) waits.insert(instruction.call);
    if (waits.size() != 1 || !install_entry(image, pending, Role::Wait, *waits.begin(), leaf_length(image, *waits.begin(), 24)))
        return fail("job_group_wait_unknown");
    auto wait_target = tail_jump(image, *waits.begin(), leaf_length(image, *waits.begin(), 24));
    if (!install_entry(image, pending, Role::WaitTarget, wait_target, function_length(image, wait_target)))
        return fail("job_group_wait_target_unknown");
    std::set<uint32_t> distinct;
    for (const auto& entry : pending.entries) if (!distinct.insert(entry.rva).second) return fail("entry_role_alias");
    pending.source = "explicit_roots_with_native_contract"; bindings = std::move(pending); error.clear(); return true;
}
bool scan_contract(const PeImage& image, ContractBindings& bindings, std::string& error) {
    bindings = {}; std::vector<uint32_t> compress, predicate;
    // Bounded by the validated .pdata function ranges and executable sections.
    // Filter length before decoding. No historical RVA or lambda name is used.
    for (const auto& range : image.functions()) {
        auto length = range.end - range.begin;
        if (length >= profile_lengths[0] && length <= profile_lengths[0] + 16 &&
            matches(image, Role::Compress, range.begin, length)) compress.push_back(range.begin);
        auto p = static_cast<size_t>(Role::Predicate);
        if (length >= profile_lengths[p] && length <= profile_lengths[p] + 16 &&
            matches(image, Role::Predicate, range.begin, length)) predicate.push_back(range.begin);
    }
    if (compress.size() != 1 || predicate.size() != 1) { error = "scan_roots_missing_or_ambiguous"; return false; }
    if (!certify_contract(image, {compress[0], 0, predicate[0]}, bindings, error)) return false;
    bindings.source = "bounded_scan_with_native_contract"; return true;
}
bool validate_loaded_entries(HMODULE module, const PeImage& image, const ContractBindings& bindings, std::string& error) {
    if (!module || bindings.identity.sha256 != image.identity().sha256) { error = "loaded_identity_invalid"; return false; }
    auto base = reinterpret_cast<uintptr_t>(module);
    auto readable = [&](uintptr_t address, size_t size, bool code) {
        while (size) {
            MEMORY_BASIC_INFORMATION memory{};
            if (!VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) || memory.AllocationBase != module ||
                memory.State != MEM_COMMIT || (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
            DWORD protection = memory.Protect & 0xff;
            if (code ? protection != PAGE_EXECUTE_READ :
                (protection != PAGE_READONLY && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
                 protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)) return false;
            size_t available = memory.RegionSize - (address - reinterpret_cast<uintptr_t>(memory.BaseAddress));
            if (!available) return false;
            auto part = std::min(size, available); address += part; size -= part;
        }
        return true;
    };
    if (!readable(base, sizeof(IMAGE_DOS_HEADER), false)) { error = "loaded_header_unreadable"; return false; }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 1048576 ||
        !readable(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64), false)) { error = "loaded_header_invalid"; return false; }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt->FileHeader.TimeDateStamp != bindings.identity.timestamp ||
        nt->OptionalHeader.SizeOfImage != bindings.identity.image_size ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG) { error = "loaded_image_identity_mismatch"; return false; }
    const auto& debug = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!debug.Size || debug.Size > 4096 || debug.Size % sizeof(IMAGE_DEBUG_DIRECTORY) ||
        uint64_t(debug.VirtualAddress) + debug.Size > bindings.identity.image_size ||
        !readable(base + debug.VirtualAddress, debug.Size, false)) { error = "loaded_codeview_invalid"; return false; }
    unsigned codeviews = 0;
    for (size_t i = 0; i < debug.Size; i += sizeof(IMAGE_DEBUG_DIRECTORY)) {
        IMAGE_DEBUG_DIRECTORY directory{}; memcpy(&directory, reinterpret_cast<const void*>(base + debug.VirtualAddress + i), sizeof(directory));
        if (directory.Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        const auto address = base + directory.AddressOfRawData;
        if (++codeviews != 1 || directory.SizeOfData < 24 || !directory.AddressOfRawData ||
            uint64_t(directory.AddressOfRawData) + directory.SizeOfData > bindings.identity.image_size ||
            !readable(address, 24, false) || memcmp(reinterpret_cast<const void*>(address), "RSDS", 4) ||
            memcmp(reinterpret_cast<const void*>(address + 4), &bindings.identity.pdb_guid, sizeof(GUID)) ||
            memcmp(reinterpret_cast<const void*>(address + 20), &bindings.identity.pdb_age, 4)) {
            error = "loaded_codeview_identity_mismatch"; return false;
        }
    }
    if (codeviews != 1) { error = "loaded_codeview_missing"; return false; }
    for (const auto& entry : bindings.entries) {
        auto address = base + entry.rva;
        auto length = std::max<uint32_t>(entry.length, 32);
        if (!readable(address, length, true) || !image.at(entry.rva, length, true) ||
            memcmp(reinterpret_cast<const void*>(address), image.at(entry.rva, length, true), length) ||
            sha256(reinterpret_cast<const uint8_t*>(address), 32) != entry.entry_sha256) {
            error = "loaded_entry_modified_or_unreadable"; return false;
        }
    }
    error.clear(); return true;
}
} // namespace rgbm
