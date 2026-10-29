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

constexpr uint8_t kI32 = 0x7F;   // value type bytes
constexpr uint8_t kI64 = 0x7E;
constexpr uint8_t kF32 = 0x7D;
constexpr uint8_t kF64 = 0x7C;
constexpr uint8_t kV128 = 0x7B;

// Named ids for the opcodes the analyses and the lifter dispatch on.
namespace op {
constexpr uint32_t Unreachable = 0x00;
constexpr uint32_t Nop = 0x01;
constexpr uint32_t Block = 0x02;
constexpr uint32_t Loop = 0x03;
constexpr uint32_t If = 0x04;
constexpr uint32_t Else = 0x05;
constexpr uint32_t Try = 0x06;  // legacy exception handling
constexpr uint32_t Catch = 0x07;  // legacy
constexpr uint32_t Throw = 0x08;
constexpr uint32_t Rethrow = 0x09;
constexpr uint32_t ThrowRef = 0x0A;
constexpr uint32_t End = 0x0B;
constexpr uint32_t Br = 0x0C;
constexpr uint32_t BrIf = 0x0D;
constexpr uint32_t BrTable = 0x0E;
constexpr uint32_t Return = 0x0F;
constexpr uint32_t Call = 0x10;
constexpr uint32_t CallIndirect = 0x11;
constexpr uint32_t ReturnCall = 0x12;
constexpr uint32_t ReturnCallIndirect = 0x13;
constexpr uint32_t CallRef = 0x14;
constexpr uint32_t ReturnCallRef = 0x15;
constexpr uint32_t Delegate = 0x18;  // legacy
constexpr uint32_t CatchAll = 0x19;  // legacy
constexpr uint32_t Drop = 0x1A;
constexpr uint32_t Select = 0x1B;
constexpr uint32_t SelectT = 0x1C;
constexpr uint32_t TryTable = 0x1F;
constexpr uint32_t LocalGet = 0x20;
constexpr uint32_t LocalSet = 0x21;
constexpr uint32_t LocalTee = 0x22;
constexpr uint32_t GlobalGet = 0x23;
constexpr uint32_t GlobalSet = 0x24;
constexpr uint32_t TableGet = 0x25;
constexpr uint32_t TableSet = 0x26;
constexpr uint32_t LoadFirst = 0x28;
constexpr uint32_t LoadLast = 0x35;
constexpr uint32_t StoreFirst = 0x36;
constexpr uint32_t StoreLast = 0x3E;
constexpr uint32_t MemorySize = 0x3F;
constexpr uint32_t MemoryGrow = 0x40;
constexpr uint32_t I32Const = 0x41;
constexpr uint32_t I64Const = 0x42;
constexpr uint32_t F32Const = 0x43;
constexpr uint32_t F64Const = 0x44;
constexpr uint32_t I32Eqz = 0x45;
constexpr uint32_t I64Eqz = 0x50;
constexpr uint32_t NumericFirst = I32Eqz;
constexpr uint32_t NumericLast = 0xA6;   // f64.copysign
constexpr uint32_t I32Add = 0x6A;
constexpr uint32_t I32Sub = 0x6B;
constexpr uint32_t ConvertFirst = 0xA7;  // i32.wrap_i64
constexpr uint32_t ConvertLast = 0xC4;   // i64.extend32_s
constexpr uint32_t RefNull = 0xD0;
constexpr uint32_t RefIsNull = 0xD1;
constexpr uint32_t RefFunc = 0xD2;
constexpr uint32_t RefEq = 0xD3;
constexpr uint32_t RefAsNonNull = 0xD4;
constexpr uint32_t BrOnNull = 0xD5;
constexpr uint32_t BrOnNonNull = 0xD6;
constexpr uint32_t V128Load = kPrefixFD | 0x00;
constexpr uint32_t V128Store = kPrefixFD | 0x0B;
} // namespace op

constexpr bool isBlock(uint32_t o) {
    return o == op::Block || o == op::Loop || o == op::If || o == op::Try || o == op::TryTable;
}

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
