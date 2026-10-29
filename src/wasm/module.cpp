//
// Created by Dottik on 28/10/2026.
//

#include "wasm/module.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <map>

#include "wasm/decode.hpp"

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

static void limits(Reader &r) {
    const uint8_t flag = r.u8();
    r.uleb();
    if (flag & 1)
        r.uleb();
    if (flag & 8)
        r.uleb();
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
                case 2: limits(r); break;
                case 3: r.valtype(); r.u8(); break;
                case 4: r.u8(); r.uleb(); break;
                default: throw DecodeError(std::format("unknown import kind {}", kind));
                }
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
                    simd = std::find(p.begin(), p.end(), 0x7B) != p.end();
                }
                for (uint64_t g = 0; g < groups; ++g) {
                    const uint64_t c = r.uleb();
                    const uint8_t t = data[r.pos];
                    simd = simd || t == 0x7B;
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
                    if (op == 0x41)
                        memAddr = static_cast<uint32_t>(r.sleb());
                    else if (op == 0x42)
                        memAddr = static_cast<uint64_t>(r.sleb());
                    else
                        active = false;
                    while (r.u8() != 0x0B) {
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
    } catch (const DecodeError &) {
        return std::nullopt;
    }
}

} // namespace wasm
