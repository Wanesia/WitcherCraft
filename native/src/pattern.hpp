#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace wc::pattern
{
// IDA-style byte pattern: "48 8D 15 ?? ?? ?? ??" ('?' or '??' = wildcard).
struct Pattern
{
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask; // 1 = must match
    size_t size() const { return bytes.size(); }
};

Pattern Parse(std::string_view ida);
bool MatchAt(const uint8_t* p, const Pattern& pat);
std::vector<uint8_t*> FindAll(uint8_t* begin, size_t len, const Pattern& pat, size_t maxHits = SIZE_MAX);
uint8_t* FindFirst(uint8_t* begin, size_t len, const Pattern& pat);
std::vector<uint8_t*> FindBytes(uint8_t* begin, size_t len, const void* needle, size_t n, size_t maxHits = SIZE_MAX);

// Resolve a RIP-relative operand: insn + insnLen + int32 at (insn + dispOff).
inline uint8_t* Rel(uint8_t* insn, size_t dispOff, size_t insnLen)
{
    int32_t d;
    std::memcpy(&d, insn + dispOff, 4);
    return insn + insnLen + d;
}
} // namespace wc::pattern
