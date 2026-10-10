#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace rgbm {
// Disk-only reader. Never maps or executes the inspected executable.
struct CodeSection {
    uint32_t rva = 0, virtual_size = 0, raw_offset = 0, raw_size = 0;
    bool executable = false;
};
struct ImageIdentity {
    uint32_t timestamp = 0, image_size = 0;
    GUID pdb_guid{};
    uint32_t pdb_age = 0;
    std::string pdb_name, sha256;
};
struct FunctionRange { uint32_t begin = 0, end = 0; };
class PeImage {
public:
    bool load(const std::wstring& path, std::string& error);
    bool parse(std::vector<uint8_t> bytes, std::string& error);
    const uint8_t* at(uint32_t rva, size_t size, bool executable = false) const;
    FunctionRange function(uint32_t entry) const;
    std::string imported_function(uint32_t code_rva) const;
    const ImageIdentity& identity() const { return identity_; }
    const std::vector<CodeSection>& sections() const { return sections_; }
    const std::vector<FunctionRange>& functions() const { return functions_; }
private:
    std::vector<uint8_t> bytes_;
    std::vector<CodeSection> sections_;
    std::vector<FunctionRange> functions_;
    ImageIdentity identity_{};
    uint32_t headers_size_ = 0;
    IMAGE_DATA_DIRECTORY imports_{}, delay_imports_{};
};
std::string guid_string(const GUID& guid);
std::string json_string(const std::string& text);
// Publishes a complete report on the same volume; a failure preserves the old file.
bool publish_report(const std::wstring& path, const std::string& json, std::string& error);
std::string sha256(const uint8_t* data, size_t size);
} // namespace rgbm
