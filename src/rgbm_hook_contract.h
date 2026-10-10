#pragma once
#include "rgbm_hook_bindings.h"
#include <array>

namespace rgbm {
constexpr const char* interface_id = "unity.threaded-copy-for-each-callback.v1";
enum class Role : size_t { Compress, Process, Dispatcher, Forwarder, Lambda, Predicate,
                          Schedule, Complete, Clear, Wait, WaitTarget, Count };
constexpr size_t role_count = static_cast<size_t>(Role::Count);
const char* role_name(Role role);
struct ContractEntry { uint32_t rva = 0, length = 0; std::string entry_sha256; };
struct ContractBindings {
    ImageIdentity identity;
    std::array<ContractEntry, role_count> entries{};
    std::string source;
};
struct ContractRoots { uint32_t compress = 0, process = 0, predicate = 0; };
// Limited, evidence-derived first interface profile, NOT a generic x64 ABI solver.
// Canonical bodies and semantic edges are both checked. Leading single-byte
// NOPs are allowed; arbitrary compiler/prologue changes require a new proof.
std::string canonical_body_hash(const PeImage& image, uint32_t rva, uint32_t length);
// Source must provide at least 32 bytes of readable padding. Length only;
// imported branch pointers from a disk image are never dereferenced.
size_t disk_instruction_length(uint8_t* padded_source);
bool certify_contract(const PeImage& image, const ContractRoots& roots,
                      ContractBindings& bindings, std::string& error);
// No PDB names or historical RVA are consulted by this bounded scan.
bool scan_contract(const PeImage& image, ContractBindings& bindings, std::string& error);
bool write_binding_cache(const std::wstring& path, const ContractBindings& bindings, std::string& error);
bool read_binding_cache(const std::wstring& path, const PeImage& image,
                        ContractBindings& bindings, std::string& error);
// Validate only; never modifies protections/bytes or installs a hook.
bool validate_loaded_entries(HMODULE module, const PeImage& image,
                             const ContractBindings& bindings, std::string& error);
} // namespace rgbm
