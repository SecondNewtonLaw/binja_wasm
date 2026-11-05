//
// Created by Dottik on 28/10/2026.
//

#include "wasm/module.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>

#include "wasm/decode.hpp"
#include "wasm/opcodes.hpp"

namespace wasm {

std::optional<uint64_t> readUleb(std::span<const uint8_t> data, size_t &pos) {
    uint64_t result = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (pos >= data.size())
            return std::nullopt;
        const uint8_t byte = data[pos++];
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80))
            return result;
    }
    return std::nullopt;
}

ptrdiff_t Module::startIndex(uint64_t addr) const {
    const auto it = std::upper_bound(starts.begin(), starts.end(), addr);
    return (it - starts.begin()) - 1;
}

static uint64_t limits(Reader &r) {
    const uint8_t flag = r.u8();
    const uint64_t min = r.uleb();
    if (flag & 1)
        r.uleb();
    if (flag & 8)
        r.uleb();
    return min;
}

// Upper bound for table allocations and element-segment writes.  Modules are
// untrusted input, and a structurally valid table minimum can otherwise ask
// for gigabytes before any opcode is lifted.
constexpr uint64_t kMaxTableEntries = 1u << 24;

struct ConstValue {
    uint64_t bits = 0;
    uint8_t type = kI32;
    bool known = false;
};

// Evaluates a constant expression.  Returns nullopt for a valid expression that
// depends on an unknown global or uses an operator not folded here; decoding
// errors still throw.
static std::optional<uint64_t> evalConstExpr(Reader &r, const std::vector<uint64_t> &known,
                                             const std::vector<uint8_t> &types) {
    std::vector<ConstValue> stack;
    const auto push = [&](ConstValue v) { stack.push_back(v); };
    const auto pop = [&]() -> ConstValue {
        if (stack.empty())
            return {};
        ConstValue v = stack.back();
        stack.pop_back();
        return v;
    };
    for (;;) {
        const uint8_t initOp = r.u8();
        if (initOp == op::End)
            break;
        switch (initOp) {
        case op::I32Const:
            push({static_cast<uint32_t>(r.sleb()), kI32, true});
            break;
        case op::I64Const:
            push({static_cast<uint64_t>(r.sleb()), kI64, true});
            break;
        case op::F32Const: {
            uint32_t bits = 0;
            for (int i = 0; i < 4; ++i)
                bits |= static_cast<uint32_t>(r.u8()) << (8 * i);
            push({bits, kF32, true});
            break;
        }
        case op::F64Const: {
            uint64_t bits = 0;
            for (int i = 0; i < 8; ++i)
                bits |= static_cast<uint64_t>(r.u8()) << (8 * i);
            push({bits, kF64, true});
            break;
        }
        case op::RefNull:
            r.sleb();  // heap type
            push({0, 0x70, true});
            break;
        case op::RefFunc:
            push({r.uleb(), 0x70, true});
            break;
        case op::GlobalGet: {
            const uint64_t idx = r.uleb();
            if (idx < known.size() && idx < types.size())
                push({known[idx], types[idx], true});
            else
                push({0, idx < types.size() ? types[idx] : kI32, false});
            break;
        }
        case op::I32Add:
        case op::I32Sub:
        case op::I32Mul:
        case op::I64Add:
        case op::I64Sub:
        case op::I64Mul: {
            const ConstValue b = pop();
            const ConstValue a = pop();
            const bool wide = initOp >= op::I64Add;
            const uint8_t type = wide ? kI64 : kI32;
            if (!a.known || !b.known || a.type != type || b.type != type) {
                push({0, type, false});
                break;
            }
            uint64_t value = 0;
            if (initOp == op::I32Add || initOp == op::I64Add)
                value = a.bits + b.bits;
            else if (initOp == op::I32Sub || initOp == op::I64Sub)
                value = a.bits - b.bits;
            else
                value = a.bits * b.bits;
            if (!wide)
                value = static_cast<uint32_t>(value);
            push({value, type, true});
            break;
        }
        default:
            // Extended-const operators we do not fold have no immediates.
            // Consume the opcode and poison the value instead of failing the
            // whole module; the expression is still consumed to its End.
            stack.clear();
            push({0, kI32, false});
            break;
        }
    }
    if (stack.empty() || !stack.back().known)
        return std::nullopt;
    return stack.back().bits;
}

// Reads a constant expression for a global initializer.  Unknown-but-valid
// expressions fall back to zero so an extended-const module still opens; the
// value is only used for global.get folding.
static uint64_t globalInit(Reader &r, const std::vector<uint64_t> &known,
                           const std::vector<uint8_t> &types) {
    return evalConstExpr(r, known, types).value_or(0);
}

// Reads a constant expression used by element/data offsets.  Returns the value
// when it is known, otherwise nullopt; the expression is always consumed.
static std::optional<uint64_t> constOffset(Reader &r, const std::vector<uint64_t> &known,
                                           const std::vector<uint8_t> &types) {
    return evalConstExpr(r, known, types);
}

static std::optional<Module> parse(std::vector<uint8_t> bytes) {
    Module m;
    m.data = std::move(bytes);
    const std::span<const uint8_t> data = m.data;

    std::map<uint32_t, std::string> names;
    std::vector<std::string> imported;
    std::vector<uint32_t> importedTypes, definedTypes;
    struct Body {
        uint64_t addr, size;
    };
    std::vector<Body> bodies;

    Reader sec{data, 8};
    while (sec.pos < data.size()) {
        const uint8_t id = sec.u8();
        const uint64_t size = sec.uleb();
        const size_t q = sec.pos;
        const size_t end = q + size;
        if (end > data.size())
            throw DecodeError("section overruns file");
        Reader r{data, q};
        switch (id) {
        case 1: {
            const uint64_t cnt = r.uleb();
            for (uint64_t i = 0; i < cnt; ++i) {
                if (data[r.pos] != 0x60)
                    break;
                ++r.pos;
                FuncType t;
                for (auto *group : {&t.params, &t.results}) {
                    const uint64_t c = r.uleb();
                    for (uint64_t k = 0; k < c; ++k) {
                        group->push_back(data[r.pos]);
                        r.valtype();
                    }
                }
                m.types.push_back(std::move(t));
            }
            break;
        }
        case 3: {
            const uint64_t cnt = r.uleb();
            for (uint64_t i = 0; i < cnt; ++i)
                definedTypes.push_back(static_cast<uint32_t>(r.uleb()));
            break;
        }
        case 2: {
            const uint64_t cnt = r.uleb();
            for (uint64_t i = 0; i < cnt; ++i) {
                const std::string mod = r.name();
                const std::string nm = r.name();
                const uint8_t kind = r.u8();
                switch (kind) {
                case 0:
                    imported.push_back(mod + "." + nm);
                    importedTypes.push_back(static_cast<uint32_t>(r.uleb()));
                    break;
                case 1: r.valtype(); limits(r); break;
                case 2: m.memoryPages = std::max<uint64_t>(m.memoryPages, limits(r)); break;
                case 3: {
                    const uint8_t t = r.valtype();
                    r.u8();  // mutability
                    m.globalTypes.push_back(t);
                    m.globalInits.push_back(0);
                    break;
                }
                case 4: r.u8(); r.uleb(); break;
                default: throw DecodeError(std::format("unknown import kind {}", kind));
                }
            }
            break;
        }
        case 4: {
            const uint64_t cnt = r.uleb();
            for (uint64_t i = 0; i < cnt; ++i) {
                r.valtype();  // reftype
                const uint64_t min = limits(r);
                if (min > kMaxTableEntries)
                    throw DecodeError("table minimum too large");
                if (m.table.empty())
                    m.table.assign(min, kNoFunc);
            }
            break;
        }
        case 5: {
            const uint64_t cnt = r.uleb();
            for (uint64_t i = 0; i < cnt; ++i)
                m.memoryPages = std::max<uint64_t>(m.memoryPages, limits(r));
            break;
        }
        case 6: {
            const uint64_t cnt = r.uleb();
            for (uint64_t i = 0; i < cnt; ++i) {
                const uint8_t t = r.valtype();
                r.u8();  // mutability
                m.globalTypes.push_back(t);
                m.globalInits.push_back(globalInit(r, m.globalInits, m.globalTypes));
            }
            break;
        }
        case 7: {
            const uint64_t cnt = r.uleb();
            for (uint64_t i = 0; i < cnt; ++i) {
                const std::string nm = r.name();
                const uint8_t kind = r.u8();
                const auto idx = static_cast<uint32_t>(r.uleb());
                if (kind == 0) {
                    names[idx] = nm;
                    m.entries.push_back(idx);
                }
            }
            break;
        }
        case 8:
            m.entries.push_back(static_cast<uint32_t>(r.uleb()));
            break;
        case 9: {
            const uint64_t cnt = r.uleb();
            for (uint64_t i = 0; i < cnt; ++i) {
                const uint64_t flags = r.uleb();
                if (flags == 1 || flags == 3) {
                    r.u8();  // elemkind
                    const uint64_t count = r.uleb();
                    if (r.pos > end || count > end - r.pos)
                        throw DecodeError("element count exceeds section");
                    for (uint64_t k = 0; k < count; ++k)
                        r.uleb();
                    continue;
                }
                if (flags != 0 && flags != 2)
                    break;  // expression/declarative forms are not modelled
                const uint64_t tableIdx = flags == 2 ? r.uleb() : 0;
                const auto offset = constOffset(r, m.globalInits, m.globalTypes);
                if (flags == 2)
                    r.u8();  // elemkind
                const uint64_t n = r.uleb();
                if (r.pos > end || n > end - r.pos)
                    throw DecodeError("element count exceeds section");
                if (tableIdx != 0 || !offset) {
                    for (uint64_t k = 0; k < n; ++k)
                        r.uleb();
                    continue;
                }
                const uint64_t base = *offset;
                if (base > kMaxTableEntries || n > kMaxTableEntries - base)
                    throw DecodeError("element segment exceeds table limit");
                if (base + n > m.table.size())
                    m.table.resize(base + n, kNoFunc);
                for (uint64_t k = 0; k < n; ++k)
                    m.table[base + k] = static_cast<uint32_t>(r.uleb());
            }
            break;
        }
        case 10: {
            const uint64_t cnt = r.uleb();
            for (uint64_t j = 0; j < cnt; ++j) {
                const uint64_t bsize = r.uleb();
                const size_t bodyEnd = r.pos + bsize;
                const uint64_t groups = r.uleb();
                const bool typed = j < definedTypes.size() && definedTypes[j] < m.types.size();
                std::vector<uint8_t> ltypes;
                bool simd = false;
                if (typed) {
                    const auto &p = m.types[definedTypes[j]].params;
                    ltypes.assign(p.begin(), p.begin() + std::min(p.size(), kLocalLimit));
                    simd = std::find(p.begin(), p.end(), kV128) != p.end();
                }
                for (uint64_t g = 0; g < groups; ++g) {
                    const uint64_t c = r.uleb();
                    const uint8_t t = data[r.pos];
                    simd = simd || t == kV128;
                    ltypes.insert(ltypes.end(), std::min<uint64_t>(c, kLocalLimit - std::min(ltypes.size(), kLocalLimit)), t);
                    r.valtype();
                }
                m.localTypes[r.pos] = std::move(ltypes);
                if (simd)
                    m.simdFuncs.insert(r.pos);
                bodies.push_back({r.pos, bodyEnd - r.pos});
                r.pos = bodyEnd;
            }
            break;
        }
        case 11: {
            const uint64_t cnt = r.uleb();
            for (uint64_t i = 0; i < cnt; ++i) {
                const uint64_t flag = r.uleb();
                if (flag == 2)
                    r.uleb();
                std::optional<uint64_t> memAddr;
                if (flag != 1)
                    memAddr = evalConstExpr(r, m.globalInits, m.globalTypes);
                const uint64_t sz = r.uleb();
                if (r.pos > data.size() || sz > data.size() - r.pos)
                    break;
                if (memAddr)
                    m.dataSegments.push_back({*memAddr, r.pos, sz});
                r.pos += sz;
            }
            break;
        }
        case 0: {
            if (r.name() != "name")
                break;
            while (r.pos < end) {
                const uint8_t sub = r.u8();
                const uint64_t sz = r.uleb();
                const size_t next = r.pos + sz;
                if (sub == 1) {
                    const uint64_t cnt = r.uleb();
                    for (uint64_t i = 0; i < cnt; ++i) {
                        const auto idx = static_cast<uint32_t>(r.uleb());
                        names[idx] = r.name();
                    }
                }
                r.pos = next;
            }
            break;
        }
        default:
            break;
        }
        sec.pos = end;
    }

    const auto nameOf = [&](uint32_t i, std::string fallback) {
        const auto it = names.find(i);
        return it != names.end() ? it->second : std::move(fallback);
    };
    m.functions.reserve(imported.size() + bodies.size());
    for (uint32_t i = 0; i < imported.size(); ++i)
        m.functions.push_back({nameOf(i, imported[i]), std::nullopt, 0, importedTypes[i]});
    for (size_t j = 0; j < bodies.size(); ++j) {
        const auto i = static_cast<uint32_t>(imported.size() + j);
        const uint32_t t = j < definedTypes.size() ? definedTypes[j] : kNoType;
        m.functions.push_back({nameOf(i, std::format("sub_{:x}", bodies[j].addr)), bodies[j].addr, bodies[j].size, t});
        m.addrIndex[bodies[j].addr] = i;
        m.starts.push_back(bodies[j].addr);
    }
    std::sort(m.starts.begin(), m.starts.end());
    return m;
}

std::optional<Module> parseModule(std::vector<uint8_t> data) {
    static constexpr std::array<uint8_t, 8> kHeader = {0x00, 'a', 's', 'm', 0x01, 0x00, 0x00, 0x00};
    if (data.size() < kHeader.size() || std::memcmp(data.data(), kHeader.data(), kHeader.size()) != 0)
        return std::nullopt;
    try {
        return parse(std::move(data));
    } catch (const DecodeError &e) {
        if (std::getenv("WASM_PARSE_DEBUG"))
            std::fprintf(stderr, "wasm parse: %s\n", e.what());
        return std::nullopt;
    } catch (const std::exception &e) {
        if (std::getenv("WASM_PARSE_DEBUG"))
            std::fprintf(stderr, "wasm parse: %s\n", e.what());
        return std::nullopt;
    }
}

} // namespace wasm
