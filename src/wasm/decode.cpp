//
// Created by Dottik on 28/10/2026.
//

#include "wasm/decode.hpp"

#include <cstring>
#include <format>

#include "wasm/opcodes.hpp"

namespace wasm {

uint8_t Reader::u8() {
    if (pos >= data.size())
        throw DecodeError("truncated");
    return data[pos++];
}

uint64_t Reader::uleb() {
    uint64_t result = 0;
    for (unsigned shift = 0;; shift += 7) {
        const uint8_t b = u8();
        if (shift < 64)
            result |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80))
            return result;
        if (shift > 70)
            throw DecodeError("leb128 too long");
    }
}

int64_t Reader::sleb() {
    int64_t result = 0;
    unsigned shift = 0;
    for (;;) {
        const uint8_t b = u8();
        if (shift < 64)
            result |= static_cast<int64_t>(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if ((b & 0x40) && shift < 64)
                result |= -(static_cast<int64_t>(1) << shift);
            return result;
        }
        if (shift > 70)
            throw DecodeError("leb128 too long");
    }
}

std::string Reader::name() {
    const uint64_t n = uleb();
    if (n > data.size() - std::min(pos, data.size()))
        throw DecodeError("truncated name");
    std::string s(reinterpret_cast<const char *>(data.data() + pos), n);
    pos += n;
    return s;
}

uint8_t Reader::valtype() {
    const uint8_t b = u8();
    if (b == 0x63 || b == 0x64)
        sleb();
    return b;
}

static Operand memarg(Reader &r, Instr &out, bool full) {
    const uint64_t align = r.uleb();
    if (align & 0x40)
        r.uleb();
    const uint64_t off = r.uleb();
    out.imm0 = static_cast<int64_t>(off);
    Operand o{Operand::Mem, static_cast<int64_t>(off), {}};
    if (full) {
        if (off)
            o.text = std::format("offset={}", off);
        if (align & 0x3F)
            o.text += std::format("{}align={}", o.text.empty() ? "" : " ", align & 0x3F);
    }
    return o;
}

static bool hasIdx1(uint8_t op) {
    switch (op) {
    case 0x08: case 0x09: case 0x0C: case 0x0D: case 0x10: case 0x12: case 0x14: case 0x15: case 0x18:
    case 0x07: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0xD2:
    case 0xD5: case 0xD6: case 0x3F: case 0x40:
        return true;
    default:
        return false;
    }
}

Instr decode(std::span<const uint8_t> data, size_t pos, bool full) {
    Reader r{data, pos};
    Instr out;
    const uint8_t op = r.u8();
    out.op = op;

    if (isBlock(op)) {
        const int64_t v = r.sleb();
        out.imm0 = v;
        if (full && v != -64)
            out.ops.push_back({Operand::Text, 0, v >= 0 ? std::format("(type {})", v)
                                                          : std::format("(result {})", valtypeName(v & 0x7F))});
        if (op == 0x1F) {
            const uint64_t cnt = r.uleb();
            for (uint64_t i = 0; i < cnt; ++i) {
                CatchSpec c{r.u8(), 0, 0};
                if (c.kind < 2)
                    c.tag = r.uleb();
                c.label = r.uleb();
                out.catches.push_back(c);
                if (full) {
                    if (c.kind < 2)
                        out.ops.push_back({Operand::Num, static_cast<int64_t>(c.tag), {}});
                    out.ops.push_back({Operand::Num, static_cast<int64_t>(c.label), {}});
                }
            }
        }
    } else if (hasIdx1(op)) {
        const uint64_t v = r.uleb();
        out.imm0 = static_cast<int64_t>(v);
        if (full)
            out.ops.push_back({op == 0x10 || op == 0x12 ? Operand::Func : Operand::Num, static_cast<int64_t>(v), {}});
    } else if (op == 0x0E) {
        const uint64_t cnt = r.uleb();
        out.imm0 = static_cast<int64_t>(cnt);
        std::string text = "[";
        for (uint64_t i = 0; i < cnt; ++i) {
            const uint64_t v = r.uleb();
            if (full)
                text += std::format("{}{}", i ? ", " : "", v);
        }
        const uint64_t def = r.uleb();
        out.imm1 = static_cast<int64_t>(def);
        if (full) {
            out.ops.push_back({Operand::Text, 0, text + "]"});
            out.ops.push_back({Operand::Num, static_cast<int64_t>(def), {}});
        }
    } else if (op == 0x11 || op == 0x13) {
        out.imm0 = static_cast<int64_t>(r.uleb());
        out.imm1 = static_cast<int64_t>(r.uleb());
        if (full) {
            out.ops.push_back({Operand::Num, out.imm0, {}});
            out.ops.push_back({Operand::Num, out.imm1, {}});
        }
    } else if (op == 0x1C) {
        const uint64_t cnt = r.uleb();
        for (uint64_t i = 0; i < cnt; ++i)
            r.valtype();
    } else if (op >= 0x28 && op <= 0x3E) {
        Operand m = memarg(r, out, full);
        if (full)
            out.ops.push_back(std::move(m));
    } else if (op == 0x41 || op == 0x42) {
        out.imm0 = r.sleb();
        if (full)
            out.ops.push_back({Operand::Imm, out.imm0, {}});
    } else if (op == 0x43) {
        uint32_t bits = 0;
        for (int i = 0; i < 4; ++i)
            bits |= static_cast<uint32_t>(r.u8()) << (8 * i);
        out.imm0 = bits;
        if (full) {
            float f;
            std::memcpy(&f, &bits, 4);
            out.ops.push_back({Operand::Text, 0, std::format("{}", f)});
        }
    } else if (op == 0x44) {
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i)
            bits |= static_cast<uint64_t>(r.u8()) << (8 * i);
        out.imm0 = static_cast<int64_t>(bits);
        if (full) {
            double d;
            std::memcpy(&d, &bits, 8);
            out.ops.push_back({Operand::Text, 0, std::format("{}", d)});
        }
    } else if (op == 0xD0) {
        r.sleb();
    } else if (op == 0xFC) {
        const uint64_t sub = r.uleb();
        if (sub > 17)
            throw DecodeError("unknown 0xFC sub opcode");
        for (int i = 0, n = fcUlebCount(sub); i < n; ++i) {
            const uint64_t v = r.uleb();
            if (i == 0)
                out.imm0 = static_cast<int64_t>(v);
            else
                out.imm1 = static_cast<int64_t>(v);
            if (full)
                out.ops.push_back({Operand::Num, static_cast<int64_t>(v), {}});
        }
        out.op = kPrefixFC | static_cast<uint32_t>(sub);
    } else if (op == 0xFD) {
        const uint64_t sub = r.uleb();
        if (sub <= 11 || sub == 0x5C || sub == 0x5D || (sub >= 0x54 && sub <= 0x5B)) {
            Operand m = memarg(r, out, full);
            if (full)
                out.ops.push_back(std::move(m));
        }
        if (sub == 0x0C || sub == 0x0D) {
            for (auto &b : out.v128)
                b = r.u8();
            if (full) {
                std::string hex;
                for (uint8_t b : out.v128)
                    hex += std::format("{:02x}", b);
                out.ops.push_back({Operand::Text, 0, hex});
            }
        } else if ((sub >= 0x15 && sub <= 0x22) || (sub >= 0x54 && sub <= 0x5B)) {
            out.imm1 = r.u8();
            if (full)
                out.ops.push_back({Operand::Num, out.imm1, {}});
        }
        out.op = kPrefixFD | static_cast<uint32_t>(sub);
    } else if (op == 0xFE) {
        const uint64_t sub = r.uleb();
        if (sub == 3) {
            r.u8();
        } else {
            Operand m = memarg(r, out, full);
            if (full)
                out.ops.push_back(std::move(m));
        }
        out.op = kPrefixFE | static_cast<uint32_t>(sub);
    } else if (instrName(op).empty()) {
        throw DecodeError(std::format("unknown opcode {:#x}", op));
    }
    out.size = static_cast<uint32_t>(r.pos - pos);
    return out;
}

std::vector<uint64_t> tableLabels(std::span<const uint8_t> data, size_t pos) {
    Reader r{data, pos + 1};
    const uint64_t cnt = r.uleb();
    std::vector<uint64_t> labels;
    labels.reserve(cnt + 1);
    for (uint64_t i = 0; i <= cnt; ++i)
        labels.push_back(r.uleb());
    return labels;
}

} // namespace wasm
