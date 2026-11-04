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

// Reads a constant expression for a global initializer.  The value is the raw
// bit pattern; expressions that refer to another global copy that global's
// initializer when it is already known.
static uint64_t globalInit(Reader &r, const std::vector<uint64_t> &known) {
    const uint8_t initOp = r.u8();
    uint64_t value = 0;
    switch (initOp) {
    case op::I32Const:
        value = static_cast<uint32_t>(r.sleb());
        break;
    case op::I64Const:
        value = static_cast<uint64_t>(r.sleb());
        break;
    case op::F32Const: {
        uint32_t bits = 0;
        for (int i = 0; i < 4; ++i)
            bits |= static_cast<uint32_t>(r.u8()) << (8 * i);
        value = bits;
        break;
    }
    case op::F64Const: {
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i)
            bits |= static_cast<uint64_t>(r.u8()) << (8 * i);
        value = bits;
        break;
    }
    case op::RefNull:
        r.sleb();  // heap type
        break;
    case op::RefFunc:
        r.uleb();
        break;
    case op::GlobalGet: {
        const uint64_t idx = r.uleb();
        value = idx < known.size() ? known[idx] : 0;
        break;
    }
    default:
        throw DecodeError(std::format("unsupported global initializer {:#x}", initOp));
    }
    if (r.u8() != op::End)
        throw DecodeError("global initializer missing end");
    return value;
}

// Reads a constant expression used by element/data offsets.  Returns the value
// when it is a simple integer constant (the common case), otherwise nullopt.
static std::optional<uint64_t> constOffset(Reader &r) {
    const uint8_t initOp = r.u8();
    std::optional<uint64_t> value;
    switch (initOp) {
    case op::I32Const:
        value = static_cast<uint32_t>(r.sleb());
        break;
    case op::I64Const:
        value = static_cast<uint64_t>(r.sleb());
        break;
    case op::GlobalGet:
        r.uleb();
        break;
    case op::RefNull:
        r.sleb();
        break;
    case op::RefFunc:
        r.uleb();
        break;
    default:
        throw DecodeError(std::format("unsupported const expression {:#x}", initOp));
    }
    if (r.u8() != op::End)
        throw DecodeError("const expression missing end");
    return value;
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
                m.globalInits.push_back(globalInit(r, m.globalInits));
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
                if (flags != 0 && flags != 2)
                    break;  // passive/declarative segments are not modelled
                const uint64_t tableIdx = flags == 2 ? r.uleb() : 0;
                const auto offset = constOffset(r);
                const uint64_t n = r.uleb();
                if (tableIdx != 0 || !offset) {
                    for (uint64_t k = 0; k < n; ++k)
                        r.uleb();
                    continue;
                }
                const uint64_t base = *offset;
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
                bool active = flag != 1;
                uint64_t memAddr = 0;
                if (active) {
                    const uint8_t op = r.u8();
                    if (op == op::I32Const)
                        memAddr = static_cast<uint32_t>(r.sleb());
                    else if (op == op::I64Const)
                        memAddr = static_cast<uint64_t>(r.sleb());
                    else
                        active = false;
                    while (r.u8() != op::End) {
                    }
                }
                const uint64_t sz = r.uleb();
                if (active && r.pos + sz <= data.size())
                    m.dataSegments.push_back({memAddr, r.pos, sz});
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
