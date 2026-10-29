//
// Created by Dottik on 28/10/2026.
//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bnwasm {

struct IntrinsicDef {
    std::string name;
    uint32_t nin;
    uint32_t nout;
    std::string vecIn;   // chars of 'v' mark 16-byte inputs; shorter than nin means the rest are 8 bytes
    bool vecOut = false; // first output is 16 bytes
};

// Intrinsic ids are indices into this table.
const std::vector<IntrinsicDef> &intrinsics();
uint32_t intrinsicId(const std::string &name);

// Instructions lifted as `__builtin_*` calls.
struct Builtin {
    uint32_t intrinsic;
    uint8_t imms;      // leading constant immediates
    uint8_t pops;
    uint8_t outs;
    bool foldAddress;  // atomic: memarg offset is added to the address operand instead of passed
};
// nullptr when `op` (plain or prefixed instruction id) has no builtin form.
const Builtin *builtinFor(uint32_t op);

// Intrinsic of a SIMD sub opcode (those not lifted inline), or ~0u.
uint32_t simdIntrinsic(uint32_t sub);

struct SpecialIntrinsics {
    uint32_t localGet, localSet, globalGet, globalSet, tableTarget, catchMatch, catchValue;
};
const SpecialIntrinsics &specials();

std::string intrinsicName(const std::string &wasmName);

} // namespace bnwasm
