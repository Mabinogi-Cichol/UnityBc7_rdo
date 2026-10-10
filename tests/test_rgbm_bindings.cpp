#include "rgbm_hook_bindings.h"
#include <cstdio>
#include <cstring>
#include <limits>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); return 1; } } while (0)
template<class T> void put(std::vector<uint8_t>& bytes, size_t offset, const T& value) {
    memcpy(bytes.data() + offset, &value, sizeof(T));
}
std::vector<uint8_t> fixture(uint32_t function_rva = 0x1000, uint8_t first_instruction = 0x55) {
    std::vector<uint8_t> bytes(0x800, 0);
    IMAGE_DOS_HEADER dos{}; dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 0x80; put(bytes, 0, dos);
    put(bytes, 0x80, DWORD(IMAGE_NT_SIGNATURE));
    IMAGE_FILE_HEADER file{}; file.Machine = IMAGE_FILE_MACHINE_AMD64; file.NumberOfSections = 3;
    file.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64); file.TimeDateStamp = 0x12345678; put(bytes, 0x84, file);
    IMAGE_OPTIONAL_HEADER64 opt{}; opt.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    opt.SizeOfImage = 0x4000; opt.SizeOfHeaders = 0x200; opt.NumberOfRvaAndSizes = 16;
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG] = {0x2000, sizeof(IMAGE_DEBUG_DIRECTORY)};
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION] = {0x3000, sizeof(RUNTIME_FUNCTION)};
    put(bytes, 0x98, opt);
    for (unsigned i = 0; i < 3; ++i) {
        IMAGE_SECTION_HEADER section{}; section.VirtualAddress = 0x1000 * (i + 1); section.Misc.VirtualSize = 0x200;
        section.SizeOfRawData = 0x200; section.PointerToRawData = 0x200 * (i + 1);
        section.Characteristics = i ? IMAGE_SCN_MEM_READ : IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
        put(bytes, 0x188 + i * sizeof(section), section);
    }
    IMAGE_DEBUG_DIRECTORY debug{}; debug.Type = IMAGE_DEBUG_TYPE_CODEVIEW; debug.SizeOfData = 36;
    debug.AddressOfRawData = 0x2040; debug.PointerToRawData = 0x440; put(bytes, 0x400, debug);
    memcpy(bytes.data() + 0x440, "RSDS", 4);
    GUID guid{0x12345678, 0x1234, 0x5678, {0xab, 0xcd, 1, 2, 3, 4, 5, 6}};
    put(bytes, 0x444, guid); put(bytes, 0x454, DWORD(2)); memcpy(bytes.data() + 0x458, "fixture.pdb", 12);
    RUNTIME_FUNCTION range{}; range.BeginAddress = function_rva; range.EndAddress = function_rva + 32;
    range.UnwindData = 0x2090; put(bytes, 0x600, range); bytes[0x490] = 1;
    bytes[0x200 + function_rva - 0x1000] = first_instruction;
    return bytes;
}
int main() {
    rgbm::PeImage image; std::string error;
    CHECK(image.parse(fixture(), error)); CHECK(error.empty());
    CHECK(image.identity().pdb_age == 2); CHECK(image.identity().sha256.size() == 64);
    CHECK(image.identity().pdb_name == "fixture.pdb"); CHECK(image.function(0x1000).end == 0x1020);
    CHECK(image.at(0x1000, 32, true)); CHECK(!image.at(0x2000, 32, true));
    CHECK(!image.at(0x11ff, 2)); CHECK(!image.at(UINT32_MAX, 2));
    CHECK(!image.at(0x1000, SIZE_MAX)); CHECK(!image.at(0x1000, 0));
    auto original_hash = image.identity().sha256;
    CHECK(image.parse(fixture(0x1040, 0x48), error));
    CHECK(image.function(0x1040).begin == 0x1040); CHECK(!image.function(0x1000).begin);
    CHECK(image.identity().sha256 != original_hash);
    // Two import descriptors: the queried IAT belongs to the second. The first
    // descriptor's terminated name table must not be indexed beyond its end.
    auto imports = fixture();
    IMAGE_OPTIONAL_HEADER64 import_opt{}; memcpy(&import_opt, imports.data() + 0x98, sizeof(import_opt));
    import_opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {0x2100, 60}; put(imports, 0x98, import_opt);
    IMAGE_IMPORT_DESCRIPTOR first{}; first.OriginalFirstThunk = 0x2140; first.FirstThunk = 0x2160; first.Name = 0x2180;
    IMAGE_IMPORT_DESCRIPTOR second{}; second.OriginalFirstThunk = 0x2150; second.FirstThunk = 0x2170; second.Name = 0x2190;
    put(imports, 0x500, first); put(imports, 0x514, second);
    put(imports, 0x540, uint64_t(0x21a0)); put(imports, 0x550, uint64_t(0x21c0));
    memcpy(imports.data() + 0x580, "first.dll", 10); memcpy(imports.data() + 0x590, "compress_bc7e.dll", 17);
    memcpy(imports.data() + 0x5a2, "other", 6); memcpy(imports.data() + 0x5c2, "bc7elib_compress_blocks", 23);
    imports[0x230] = 0xff; imports[0x231] = 0x25;
    put(imports, 0x232, int32_t(0x2170 - 0x1036));
    CHECK(image.parse(imports, error));
    CHECK(image.imported_function(0x1030) == "compress_bc7e.dll!bc7elib_compress_blocks");
    imports[0x231] = 0x15; CHECK(image.parse(imports, error));
    CHECK(image.imported_function(0x1030) == "compress_bc7e.dll!bc7elib_compress_blocks");
    put(imports, 0x550, uint64_t(IMAGE_ORDINAL_FLAG64 | 123)); CHECK(image.parse(imports, error));
    CHECK(image.imported_function(0x1030).empty());
    unsigned rejected = 0;
    auto reject = [&](std::vector<uint8_t> bytes, const char* reason) {
        if (image.parse(std::move(bytes), error) || error != reason || image.at(0x1000, 1) || !image.identity().sha256.empty()) {
            fprintf(stderr, "expected=%s observed=%s\n", reason, error.c_str()); return false;
        }
        ++rejected; return true;
    };
    CHECK(reject(std::vector<uint8_t>(16), "invalid_dos_header"));
    auto bad = fixture(); put(bad, 0x3c, LONG(0x7fffffff)); CHECK(reject(bad, "invalid_pe_header"));
    bad = fixture(); put(bad, 0x84, WORD(IMAGE_FILE_MACHINE_I386)); CHECK(reject(bad, "unsupported_architecture"));
    bad = fixture(); put(bad, 0x98, WORD(IMAGE_NT_OPTIONAL_HDR32_MAGIC)); CHECK(reject(bad, "invalid_optional_header"));
    bad = fixture(); put(bad, 0x188 + 20, DWORD(0x7fffffff)); CHECK(reject(bad, "section_out_of_bounds"));
    bad = fixture(); put(bad, 0x1b0 + 12, DWORD(0x1000)); CHECK(reject(bad, "overlapping_sections"));
    bad = fixture(); put(bad, 0x1b0 + 20, DWORD(0x200)); CHECK(reject(bad, "overlapping_raw_sections"));
    bad = fixture(); put(bad, 0x400 + 24, DWORD(0x7fffffff)); CHECK(reject(bad, "codeview_out_of_bounds"));
    bad = fixture(); put(bad, 0x400 + 20, DWORD(0x2050)); CHECK(reject(bad, "codeview_address_mismatch"));
    bad = fixture(); bad[0x440] = 'N'; CHECK(reject(bad, "unsupported_codeview"));
    bad = fixture(); memset(bad.data() + 0x458, 'X', 12); CHECK(reject(bad, "invalid_codeview"));
    bad = fixture(); put(bad, 0x600, DWORD(0x2000)); put(bad, 0x604, DWORD(0x2020)); CHECK(reject(bad, "invalid_function_range"));
    CHECK(image.parse(fixture(), error)); CHECK(!image.load(L"does-not-exist-rgbm-fixture.exe", error));
    CHECK(image.identity().sha256.empty()); CHECK(!image.at(0x1000, 1));
    CHECK(rgbm::json_string("a\"\n\\") == "\"a\\\"\\u000a\\\\\"");
    puts("PE identity, moved entry/prologue, bounds, stale state, JSON escaping passed");
    printf("malformed_cases=%u\n", rejected); return 0;
}
