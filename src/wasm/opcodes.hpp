//
// Created by Dottik on 28/10/2026.
//

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wasm {

// Instruction ids: plain opcodes are 0x00-0xFF; prefixed ones are (prefix << 8) | sub.
constexpr uint32_t kPrefixFC = 0xFC00;
constexpr uint32_t kPrefixFD = 0xFD00;
constexpr uint32_t kPrefixFE = 0xFE00;

constexpr bool isBlock(uint32_t op) { return op == 0x02 || op == 0x03 || op == 0x04 || op == 0x06 || op == 0x1F; }

// Display name for any instruction id; unknown SIMD/atomic subs get a generic name.
std::string instrName(uint32_t op);

// Name of a value type byte ("i32", ...) or "?".
std::string_view valtypeName(uint8_t type);

struct AtomicInfo {
    std::string name;
    uint8_t imms;  // memarg count (0 for fence)
    uint8_t pops;
    uint8_t pushes;
};
const AtomicInfo *atomicInfo(uint32_t sub);

// Operand kinds for SIMD stack effects: 'v' = v128, 's' = scalar.
struct SimdSig {
    std::string_view pops;
    std::string_view pushes;
};
std::optional<SimdSig> simdSig(uint32_t sub);

// Number of 8-byte immediates a SIMD op carries (offsets, lane indices, shuffle mask).
int simdImmCount(uint32_t sub);

// Plain/FC stack effect (pops, pushes) for ops with a fixed effect. nullopt = needs context (calls) or unknown.
struct Effect {
    int pops;
    int pushes;
};
std::optional<Effect> fixedEffect(uint32_t op);

// Number of uleb immediates after a 0xFC sub opcode.
int fcUlebCount(uint32_t sub);

} // namespace wasm
