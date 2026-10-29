//
// Created by Dottik on 28/10/2026.
//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bnwasm {

constexpr uint32_t kNLocals = 128, kNGlobals = 4, kNArgs = 8, kNResults = 2, kNStack = 32;

// Fake address spaces: code is mapped away from 0 so linear-memory constants don't resolve to code.
constexpr uint64_t kCodeBase = 0xC0000000;
constexpr uint64_t kExternBase = 0xF0000000;

// Lv / X hold whole v128 values (locals / stack slots).
// Ah / Rh carry the upper half of v128 arguments / results.
enum class Bank : uint32_t { Sp, L, Lv, G, A, R, S, X, Ld, Sd, Ah, Rh, Count };

struct RegDef {
    std::string name;
    uint32_t size;
    uint32_t full;    // full-width register id (itself for 8-byte regs)
    bool zeroExtend;  // 4-byte sub-register that zero-extends to `full`
};

uint32_t regId(Bank bank, uint32_t index = 0);
const std::vector<RegDef> &allRegs();  // indexed by (id - 1)

} // namespace bnwasm
