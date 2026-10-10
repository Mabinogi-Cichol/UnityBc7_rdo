#include "rgbm_hook_bindings.h"
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <limits>
#include <delayimp.h>
#pragma comment(lib, "bcrypt.lib")

namespace rgbm {
std::string sha256(const uint8_t* data, size_t size) {
    if (!data || size > ULONG_MAX) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    unsigned char digest[32];
    auto status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(data),
        static_cast<ULONG>(size), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) return {};
    std::string result;
    for (auto byte : digest) { char hex[3]; sprintf_s(hex, "%02X", byte); result += hex; }
    return result;
}
namespace {
bool fits(size_t offset, size_t size, size_t total) {
    return offset <= total && size <= total - offset;
}
template<class T> bool read(const std::vector<uint8_t>& bytes, size_t offset, T& value) {
    if (!fits(offset, sizeof(T), bytes.size())) return false;
    memcpy(&value, bytes.data() + offset, sizeof(T));
    return true;
}
bool hash_bytes(const std::vector<uint8_t>& bytes, std::string& out) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;
    unsigned char digest[32];
    auto status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
                             static_cast<ULONG>(bytes.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) return false;
    out.clear();
    for (auto byte : digest) { char hex[3]; sprintf_s(hex, "%02X", byte); out += hex; }
    return true;
}
}

bool PeImage::load(const std::wstring& path, std::string& error) {
    bytes_.clear(); sections_.clear(); functions_.clear(); identity_ = {}; headers_size_ = 0;
    FILE* file = _wfopen(path.c_str(), L"rb");
    if (!file) { error = "exe_open_failed"; return false; }
    _fseeki64(file, 0, SEEK_END);
    const auto length = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
    if (length <= 0 || length > 1024LL * 1024 * 1024) {
        fclose(file); error = "exe_size_invalid"; return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    const auto count = fread(bytes.data(), 1, bytes.size(), file);
    fclose(file);
    if (count != bytes.size()) { error = "exe_read_failed"; return false; }
    return parse(std::move(bytes), error);
}

bool PeImage::parse(std::vector<uint8_t> bytes, std::string& error) {
    bytes_.clear(); sections_.clear(); functions_.clear(); identity_ = {}; headers_size_ = 0;
    auto fail = [&](const char* reason) {
        bytes_.clear(); sections_.clear(); functions_.clear(); identity_ = {}; headers_size_ = 0;
        error = reason; return false;
    };
    IMAGE_DOS_HEADER dos{};
    if (!read(bytes, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
        return fail("invalid_dos_header");
    const size_t pe = static_cast<size_t>(dos.e_lfanew);
    DWORD signature = 0; IMAGE_FILE_HEADER file{}; IMAGE_OPTIONAL_HEADER64 opt{};
    if (!read(bytes, pe, signature) || signature != IMAGE_NT_SIGNATURE ||
        !read(bytes, pe + 4, file)) return fail("invalid_pe_header");
    if (file.Machine != IMAGE_FILE_MACHINE_AMD64) return fail("unsupported_architecture");
    if (file.SizeOfOptionalHeader < sizeof(opt) || !read(bytes, pe + 24, opt) ||
        opt.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || opt.NumberOfRvaAndSizes < 16 ||
        !opt.SizeOfImage || !opt.SizeOfHeaders || opt.SizeOfHeaders > bytes.size() ||
        opt.SizeOfHeaders > opt.SizeOfImage) return fail("invalid_optional_header");
    const auto table = pe + 24 + file.SizeOfOptionalHeader;
    if (!file.NumberOfSections || file.NumberOfSections > 96 ||
        !fits(table, size_t(file.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER), bytes.size()) ||
        table + size_t(file.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER) > opt.SizeOfHeaders)
        return fail("invalid_section_table");
    identity_.timestamp = file.TimeDateStamp; identity_.image_size = opt.SizeOfImage;
    headers_size_ = opt.SizeOfHeaders;
    for (unsigned i = 0; i < file.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{}; read(bytes, table + i * sizeof(section), section);
        const auto extent = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
        if (!fits(section.PointerToRawData, section.SizeOfRawData, bytes.size()) ||
            section.VirtualAddress < headers_size_ ||
            !fits(section.VirtualAddress, extent, opt.SizeOfImage)) return fail("section_out_of_bounds");
        for (const auto& old : sections_) {
            const uint64_t old_extent = std::max(old.virtual_size, old.raw_size);
            if (section.VirtualAddress < uint64_t(old.rva) + old_extent &&
                old.rva < uint64_t(section.VirtualAddress) + extent) return fail("overlapping_sections");
            if (section.SizeOfRawData && old.raw_size &&
                section.PointerToRawData < uint64_t(old.raw_offset) + old.raw_size &&
                old.raw_offset < uint64_t(section.PointerToRawData) + section.SizeOfRawData)
                return fail("overlapping_raw_sections");
        }
        sections_.push_back({section.VirtualAddress, section.Misc.VirtualSize,
            section.PointerToRawData, section.SizeOfRawData,
            (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0});
    }
    bytes_ = std::move(bytes);
    imports_ = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    delay_imports_ = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!hash_bytes(bytes_, identity_.sha256)) return fail("sha256_failed");
    const auto& debug = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!debug.Size || debug.Size % sizeof(IMAGE_DEBUG_DIRECTORY)) return fail("missing_codeview");
    const uint8_t* directories = at(debug.VirtualAddress, debug.Size);
    if (!directories) return fail("debug_directory_out_of_bounds");
    unsigned codeviews = 0;
    for (size_t i = 0; i < debug.Size; i += sizeof(IMAGE_DEBUG_DIRECTORY)) {
        IMAGE_DEBUG_DIRECTORY entry{}; memcpy(&entry, directories + i, sizeof(entry));
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        if (++codeviews != 1) return fail("ambiguous_codeview");
        if (entry.SizeOfData < 25 || !fits(entry.PointerToRawData, entry.SizeOfData, bytes_.size()))
            return fail("codeview_out_of_bounds");
        const auto* cv = bytes_.data() + entry.PointerToRawData;
        if (entry.AddressOfRawData && at(entry.AddressOfRawData, entry.SizeOfData) != cv)
            return fail("codeview_address_mismatch");
        if (memcmp(cv, "RSDS", 4)) return fail("unsupported_codeview");
        memcpy(&identity_.pdb_guid, cv + 4, sizeof(GUID));
        memcpy(&identity_.pdb_age, cv + 20, 4);
        const char* name = reinterpret_cast<const char*>(cv + 24);
        const auto* terminator = static_cast<const char*>(memchr(name, 0, entry.SizeOfData - 24));
        if (!terminator || terminator == name || !identity_.pdb_age) return fail("invalid_codeview");
        identity_.pdb_name.assign(name, terminator);
    }
    if (!codeviews) return fail("missing_codeview");
    const auto& exception = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!exception.Size || exception.Size % sizeof(RUNTIME_FUNCTION)) return fail("missing_function_ranges");
    const auto* ranges = at(exception.VirtualAddress, exception.Size);
    if (!ranges) return fail("function_ranges_out_of_bounds");
    for (size_t i = 0; i < exception.Size; i += sizeof(RUNTIME_FUNCTION)) {
        RUNTIME_FUNCTION range{}; memcpy(&range, ranges + i, sizeof(range));
        if (range.BeginAddress >= range.EndAddress ||
            !at(range.BeginAddress, range.EndAddress - range.BeginAddress, true) ||
            !at(range.UnwindData & ~1u, 4) ||
            (!functions_.empty() && range.BeginAddress < functions_.back().end))
            return fail("invalid_function_range");
        functions_.push_back({range.BeginAddress, range.EndAddress});
    }
    error.clear(); return true;
}

const uint8_t* PeImage::at(uint32_t rva, size_t size, bool executable) const {
    if (!size || !fits(rva, size, identity_.image_size)) return nullptr;
    if (!executable && fits(rva, size, headers_size_) && fits(rva, size, bytes_.size()))
        return bytes_.data() + rva;
    for (const auto& section : sections_) {
        if (executable && !section.executable) continue;
        if (rva < section.rva) continue;
        const size_t delta = size_t(rva) - section.rva;
        if (fits(delta, size, section.raw_size)) return bytes_.data() + section.raw_offset + delta;
    }
    return nullptr;
}

FunctionRange PeImage::function(uint32_t entry) const {
    auto found = std::lower_bound(functions_.begin(), functions_.end(), entry,
        [](const FunctionRange& range, uint32_t value) { return range.begin < value; });
    return found != functions_.end() && found->begin == entry ? *found : FunctionRange{};
}

std::string PeImage::imported_function(uint32_t code_rva) const {
    const auto* instruction = at(code_rva, 6, true);
    if (!instruction || instruction[0] != 0xff || (instruction[1] != 0x25 && instruction[1] != 0x15)) return {};
    int32_t displacement = 0; memcpy(&displacement, instruction + 2, 4);
    const int64_t iat = int64_t(code_rva) + 6 + displacement;
    if (iat < 0 || iat > UINT32_MAX) return {};
    auto lookup = [&](uint32_t names, uint32_t slots, uint32_t module) -> std::string {
        if (iat < slots || (iat - slots) % 8) return {};
        const uint64_t index = (iat - slots) / 8;
        if (!names || index >= 4096) return {};
        // An IAT address belonging to a later descriptor must not be interpreted
        // as an arbitrary distant entry of this descriptor's name table.
        for (uint64_t i = 0; i <= index; ++i) {
            const uint64_t rva = uint64_t(names) + i * 8;
            const auto* prior = rva <= UINT32_MAX ? at(uint32_t(rva), 8) : nullptr;
            uint64_t value = 0;
            if (!prior) return {};
            memcpy(&value, prior, 8);
            if (!value) return {};
        }
        const uint64_t entry = uint64_t(names) + (iat - slots);
        if (entry > UINT32_MAX) return {};
        const auto* thunk = at(uint32_t(entry), 8);
        if (!thunk) return {};
        uint64_t name_rva = 0; memcpy(&name_rva, thunk, 8);
        if (!name_rva || (name_rva & IMAGE_ORDINAL_FLAG64) || name_rva > UINT32_MAX - 2) return {};
        auto text = [&](uint32_t rva) -> std::string {
            std::string result;
            for (unsigned i = 0; i < 1024 && uint64_t(rva) + i <= UINT32_MAX; ++i) {
                const auto* ch = at(rva + i, 1);
                if (!ch) return {};
                if (!*ch) return result;
                result += char(*ch);
            }
            return {};
        };
        auto dll = text(module), name = text(uint32_t(name_rva) + 2);
        return dll.empty() || name.empty() ? "" : dll + "!" + name;
    };
    for (uint64_t offset = 0; offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= imports_.Size; offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        const uint64_t rva = uint64_t(imports_.VirtualAddress) + offset;
        const auto* data = rva <= UINT32_MAX ? at(uint32_t(rva), sizeof(IMAGE_IMPORT_DESCRIPTOR)) : nullptr;
        if (!data) return {};
        IMAGE_IMPORT_DESCRIPTOR descriptor{}; memcpy(&descriptor, data, sizeof(descriptor));
        if (!descriptor.Name) break;
        auto name = lookup(descriptor.OriginalFirstThunk, descriptor.FirstThunk, descriptor.Name);
        if (!name.empty()) return name;
    }
    for (uint64_t offset = 0; offset + sizeof(ImgDelayDescr) <= delay_imports_.Size; offset += sizeof(ImgDelayDescr)) {
        const uint64_t rva = uint64_t(delay_imports_.VirtualAddress) + offset;
        const auto* data = rva <= UINT32_MAX ? at(uint32_t(rva), sizeof(ImgDelayDescr)) : nullptr;
        if (!data) return {};
        ImgDelayDescr descriptor{}; memcpy(&descriptor, data, sizeof(descriptor));
        if (!descriptor.rvaDLLName) break;
        if (descriptor.grAttrs != dlattrRva) continue;
        auto name = lookup(descriptor.rvaINT, descriptor.rvaIAT, descriptor.rvaDLLName);
        if (!name.empty()) return name;
    }
    return {};
}

std::string guid_string(const GUID& guid) {
    char text[40];
    sprintf_s(text, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
        guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return text;
}

std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (unsigned char ch : text) {
        if (ch == '\\' || ch == '"') { out += '\\'; out += char(ch); }
        else if (ch < 0x20) { char hex[7]; sprintf_s(hex, "\\u%04x", unsigned(ch)); out += hex; }
        else out += char(ch);
    }
    return out + '"';
}

bool publish_report(const std::wstring& path, const std::string& json, std::string& error) {
    const auto temp = path + L".tmp." + std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = "report_create_failed"; return false; }
    DWORD written = 0;
    const bool ok = json.size() <= MAXDWORD && WriteFile(file, json.data(),
        static_cast<DWORD>(json.size()), &written, nullptr) && written == json.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str()); error = "report_publish_failed"; return false;
    }
    error.clear(); return true;
}
} // namespace rgbm
