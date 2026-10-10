#include "rgbm_hook_contract.h"
#include <array>
#include <cstring>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line=%d %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    // These RIP-relative pointers cannot be dereferenced from this disk buffer.
    // Detours' ordinary CopyFF would try to follow them, even for length-only use.
    for (auto modrm : {0x15, 0x25}) for (int32_t displacement : {INT32_MAX, INT32_MIN, int32_t(0)}) {
        std::array<uint8_t, 48> instruction{}; instruction[0] = 0xff; instruction[1] = uint8_t(modrm);
        memcpy(instruction.data() + 2, &displacement, 4); auto before = instruction;
        CHECK(rgbm::disk_instruction_length(instruction.data()) == 6); CHECK(instruction == before);
        std::array<uint8_t, 48> prefixed{}; prefixed[0] = 0x48; memcpy(prefixed.data() + 1, instruction.data(), 6);
        before = prefixed; CHECK(rgbm::disk_instruction_length(prefixed.data()) == 7); CHECK(prefixed == before);
    }
    std::array<uint8_t, 48> invalid{}; memset(invalid.data(), 0xf3, 16);
    CHECK(rgbm::disk_instruction_length(invalid.data()) == 0);
    puts("unreadable indirect pointers, prefixed encodings, byte preservation and excessive prefixes passed"); return 0;
}
