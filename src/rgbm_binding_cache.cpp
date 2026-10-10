#include "rgbm_hook_contract.h"
#include <cstdio>
#include <sstream>
#include <cstring>
#include <algorithm>

namespace rgbm {
namespace {
bool hex64(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); });
}
bool equal_identity(const ImageIdentity& a, const ImageIdentity& b) {
    return a.sha256 == b.sha256 && a.timestamp == b.timestamp && a.image_size == b.image_size &&
        !memcmp(&a.pdb_guid, &b.pdb_guid, sizeof(GUID)) && a.pdb_age == b.pdb_age;
}
}
bool write_binding_cache(const std::wstring& path, const ContractBindings& bindings, std::string& error) {
    if (!hex64(bindings.identity.sha256)) { error = "uncertified_cache_identity"; return false; }
    std::ostringstream body;
    body << "BC7_RGBM_BINDINGS 1\n" << interface_id << '\n' << bindings.identity.sha256 << '\n'
        << bindings.identity.timestamp << ' ' << bindings.identity.image_size << ' ' << bindings.identity.pdb_age << '\n';
    const auto* guid = reinterpret_cast<const uint8_t*>(&bindings.identity.pdb_guid);
    for (size_t i = 0; i < sizeof(GUID); ++i) body << unsigned(guid[i]) << ' ';
    body << '\n' << role_count << '\n';
    for (size_t i = 0; i < role_count; ++i) {
        const auto& entry = bindings.entries[i];
        if (!entry.rva || !entry.length || !hex64(entry.entry_sha256)) { error = "uncertified_cache_entry"; return false; }
        body << role_name(static_cast<Role>(i)) << ' ' << entry.rva << ' ' << entry.length << ' ' << entry.entry_sha256 << '\n';
    }
    auto text = body.str();
    return publish_report(path, text + sha256(reinterpret_cast<const uint8_t*>(text.data()), text.size()) + '\n', error);
}
bool read_binding_cache(const std::wstring& path, const PeImage& image, ContractBindings& bindings, std::string& error) {
    bindings = {};
    auto fail = [&](const char* reason) { error = reason; return false; };
    FILE* file = _wfopen(path.c_str(), L"rb"); if (!file) return fail("cache_open_failed");
    std::string text(8193, '\0'); auto count = fread(text.data(), 1, text.size(), file); bool read_error = ferror(file) != 0; fclose(file);
    if (read_error || count >= text.size() || count < 66) return fail("cache_size_invalid");
    text.resize(count);
    if (text.back() != '\n' || text.find('\0') != std::string::npos) return fail("cache_format_invalid");
    auto body = text.substr(0, count - 65), checksum = text.substr(count - 65, 64);
    if (!hex64(checksum) || sha256(reinterpret_cast<const uint8_t*>(body.data()), body.size()) != checksum)
        return fail("cache_checksum_mismatch");
    ContractBindings pending; std::istringstream input(body); std::string magic, descriptor; unsigned schema = 0;
    if (!(input >> magic >> schema >> descriptor >> pending.identity.sha256) || magic != "BC7_RGBM_BINDINGS" ||
        schema != 1 || descriptor != interface_id || !hex64(pending.identity.sha256)) return fail("cache_schema_unsupported");
    uint64_t timestamp, size, age;
    if (!(input >> timestamp >> size >> age) || timestamp > UINT32_MAX || size > UINT32_MAX || age > UINT32_MAX)
        return fail("cache_identity_invalid");
    pending.identity.timestamp = uint32_t(timestamp); pending.identity.image_size = uint32_t(size); pending.identity.pdb_age = uint32_t(age);
    auto* guid = reinterpret_cast<uint8_t*>(&pending.identity.pdb_guid);
    for (size_t i = 0; i < sizeof(GUID); ++i) { unsigned value; if (!(input >> value) || value > 255) return fail("cache_guid_invalid"); guid[i] = uint8_t(value); }
    size_t entries; if (!(input >> entries) || entries != role_count) return fail("cache_roles_invalid");
    for (size_t i = 0; i < role_count; ++i) {
        std::string role; uint64_t rva, length;
        if (!(input >> role >> rva >> length >> pending.entries[i].entry_sha256) || role != role_name(static_cast<Role>(i)) ||
            !rva || rva > UINT32_MAX || !length || length > 16384 || !hex64(pending.entries[i].entry_sha256)) return fail("cache_entry_invalid");
        pending.entries[i].rva = uint32_t(rva); pending.entries[i].length = uint32_t(length);
    }
    std::string trailing; if (input >> trailing) return fail("cache_trailing_fields");
    if (!equal_identity(image.identity(), pending.identity)) return fail("cache_binary_identity_mismatch");
    // A checksum only detects corruption. Reconstruct the certified chain from
    // the current disk image before trusting even a deliberately rehashed cache.
    ContractBindings certified;
    const auto& e = pending.entries;
    if (!certify_contract(image, {e[0].rva, e[1].rva, e[5].rva}, certified, error)) return false;
    for (size_t i = 0; i < role_count; ++i) if (e[i].rva != certified.entries[i].rva || e[i].length != certified.entries[i].length ||
        e[i].entry_sha256 != certified.entries[i].entry_sha256) return fail("cache_contract_entry_mismatch");
    certified.source = "validated_cache"; bindings = std::move(certified); error.clear(); return true;
}
} // namespace rgbm
