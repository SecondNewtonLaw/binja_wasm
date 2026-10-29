//
// Created by Dottik on 28/10/2026.
//

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace wasm {

struct DecodeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Bounds-checked byte cursor over the whole module image.
struct Reader {
    std::span<const uint8_t> data;
    size_t pos = 0;

    uint8_t u8();
    uint64_t uleb();
    int64_t sleb();
    std::string name();
    // Skips a value type, including the heap-type operand of ref types (0x63/0x64).
    uint8_t valtype();
    bool eof() const { return pos >= data.size(); }
};

// Display operand; only built when decoding with `full`.
struct Operand {
    enum Kind : char { Num = 'n', Imm = 'i', Func = 'f', Text = 't', Mem = 'm' };
    Kind kind;
    int64_t n = 0;     // Num/Imm/Func value, Mem offset
    std::string text;  // Text, Mem (offset/align summary)
};

// try_table clause: kind 0 catch, 1 catch_ref, 2 catch_all, 3 catch_all_ref. `tag` is unused for 2 and 3.
struct CatchSpec {
    uint8_t kind;
    uint64_t tag;
    uint64_t label;
};

struct Instr {
    uint32_t op = 0;    // plain opcode, or (prefix << 8) | sub
    uint32_t size = 0;
    // Index, label, const or memarg offset immediates, set without building strings.
    int64_t imm0 = 0;
    int64_t imm1 = 0;   // table index (call_indirect), lane (simd)
    std::array<uint8_t, 16> v128{};  // v128.const / i8x16.shuffle payload
    std::vector<Operand> ops;        // full decode only
    std::vector<CatchSpec> catches;  // try_table
};

// Decodes one instruction at `pos`. Throws DecodeError on truncated or unknown encodings.
Instr decode(std::span<const uint8_t> data, size_t pos, bool full = false);

// br_table labels followed by the default label.
std::vector<uint64_t> tableLabels(std::span<const uint8_t> data, size_t pos);

} // namespace wasm
