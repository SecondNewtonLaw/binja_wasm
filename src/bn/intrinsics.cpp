//
// Created by Dottik on 28/10/2026.
//

#include "bn/intrinsics.hpp"

#include <array>
#include <unordered_map>

#include "wasm/opcodes.hpp"

namespace bnwasm {

namespace {

struct Tables {
    std::vector<IntrinsicDef> defs;
    std::unordered_map<std::string, uint32_t> ids;
    std::array<Builtin, 256> plain{};
    std::array<bool, 256> plainSet{};
    std::unordered_map<uint32_t, Builtin> prefixed;
    std::array<uint32_t, 256> simd{};
    SpecialIntrinsics special{};

    uint32_t add(std::string name, uint32_t nin, uint32_t nout) {
        if (const auto it = ids.find(name); it != ids.end())
            return it->second;
        const auto id = static_cast<uint32_t>(defs.size());
        ids.emplace(name, id);
        defs.push_back({std::move(name), nin, nout});
        return id;
    }
};

struct Spec {
    const char *name;
    uint8_t imms, pops, outs;
};

Tables build() {
    Tables t;
    t.simd.fill(~0u);

    // Same set as lifter.py BUILTINS.
    std::unordered_map<std::string, Spec> wanted;
    const Spec base[] = {
        {"select", 0, 3, 1},       {"memory.size", 1, 0, 1}, {"memory.grow", 1, 1, 1}, {"memory.init", 1, 3, 0},
        {"memory.copy", 0, 3, 0},  {"memory.fill", 0, 3, 0}, {"data.drop", 1, 0, 0},   {"elem.drop", 1, 0, 0},
        {"table.get", 1, 1, 1},    {"table.set", 1, 2, 0},   {"table.size", 1, 0, 1},  {"table.grow", 1, 2, 1},
        {"table.fill", 1, 3, 0},   {"table.copy", 2, 3, 0},  {"table.init", 2, 3, 0},  {"throw", 1, 0, 0},
        {"rethrow", 1, 0, 0},      {"throw_ref", 0, 1, 0},
    };
    for (const auto &s : base)
        wanted[s.name] = s;
    static std::vector<std::string> owned;  // stable storage for generated names
    owned.reserve(128);
    auto gen = [&](std::string name, uint8_t imms, uint8_t pops, uint8_t outs) {
        owned.push_back(std::move(name));
        wanted[owned.back()] = {owned.back().c_str(), imms, pops, outs};
    };
    for (const char *ty : {"i32", "i64"})
        for (const char *o : {"clz", "ctz", "popcnt"})
            gen(std::string(ty) + "." + o, 0, 1, 1);
    for (const char *ty : {"f32", "f64"}) {
        for (const char *o : {"ceil", "floor", "nearest"})
            gen(std::string(ty) + "." + o, 0, 1, 1);
        for (const char *o : {"min", "max", "copysign"})
            gen(std::string(ty) + "." + o, 0, 2, 1);
    }
    for (const char *n : {"i32.trunc_f32_u", "i32.trunc_f64_u", "i64.trunc_f32_u", "i64.trunc_f64_u",
                          "f32.convert_i32_u", "f32.convert_i64_u", "f64.convert_i32_u", "f64.convert_i64_u",
                          "i32.trunc_sat_f32_s", "i32.trunc_sat_f32_u", "i32.trunc_sat_f64_s",
                          "i32.trunc_sat_f64_u", "i64.trunc_sat_f32_s", "i64.trunc_sat_f32_u",
                          "i64.trunc_sat_f64_s", "i64.trunc_sat_f64_u"})
        gen(n, 0, 1, 1);

    auto bind = [&](uint32_t op) {
        const auto it = wanted.find(wasm::instrName(op));
        if (it == wanted.end())
            return;
        const Spec &s = it->second;
        const uint32_t id = t.add(intrinsicName(s.name), s.imms + s.pops, s.outs);
        const Builtin b{id, s.imms, s.pops, s.outs, false};
        if (op < 0x100) {
            t.plain[op] = b;
            t.plainSet[op] = true;
        } else {
            t.prefixed[op] = b;
        }
    };
    for (uint32_t op = 0; op < 0x100; ++op)
        bind(op);
    for (uint32_t sub = 0; sub < 32; ++sub)
        bind(wasm::kPrefixFC | sub);

    // Atomics: load/store are lifted inline; the rest take a folded address.
    for (uint32_t sub = 0; sub < 0x4F; ++sub) {
        const wasm::AtomicInfo *a = wasm::atomicInfo(sub);
        if (!a || (sub >= 0x10 && sub <= 0x1D))
            continue;
        const uint8_t imms = 0;
        const bool fold = a->imms == 1;
        const uint32_t id = t.add(intrinsicName(a->name), imms + a->pops, a->pushes);
        t.prefixed[wasm::kPrefixFE | sub] = {id, imms, a->pops, a->pushes, fold};
    }

    // SIMD operands travel as two 8-byte halves; load/store/const are lifted inline.
    for (uint32_t sub = 0; sub < 256; ++sub) {
        const auto sig = wasm::simdSig(sub);
        if (!sig || sub == 0x00 || sub == 0x0B || sub == 0x0C)
            continue;
        const uint32_t imms = static_cast<uint32_t>(wasm::simdImmCount(sub));
        t.simd[sub] = t.add(intrinsicName(wasm::instrName(wasm::kPrefixFD | sub)),
                            imms + static_cast<uint32_t>(sig->pops.size()), sig->pushes.empty() ? 0 : 1);
        auto &d = t.defs[t.simd[sub]];
        d.vecIn = std::string(imms, 's').append(sig->pops);
        d.vecOut = !sig->pushes.empty() && sig->pushes[0] == 'v';
    }

    t.special.localGet = t.add("__builtin_local_get", 1, 1);
    t.special.localSet = t.add("__builtin_local_set", 2, 0);
    t.special.globalGet = t.add("__builtin_global_get", 1, 1);
    t.special.globalSet = t.add("__builtin_global_set", 2, 0);
    t.special.tableTarget = t.add("__builtin_table_target", 2, 1);
    t.special.catchMatch = t.add("__builtin_catch_match", 2, 1);
    t.special.catchValue = t.add("__builtin_catch_value", 3, 1);
    return t;
}

const Tables &tables() {
    static const Tables t = build();
    return t;
}

} // namespace

std::string intrinsicName(const std::string &wasmName) {
    std::string n = "__builtin_" + wasmName;
    for (char &c : n)
        if (c == '.')
            c = '_';
    return n;
}

const std::vector<IntrinsicDef> &intrinsics() { return tables().defs; }

uint32_t intrinsicId(const std::string &name) {
    const auto &t = tables();
    const auto it = t.ids.find(name);
    return it == t.ids.end() ? ~0u : it->second;
}

const Builtin *builtinFor(uint32_t op) {
    const auto &t = tables();
    if (op < 0x100)
        return t.plainSet[op] ? &t.plain[op] : nullptr;
    const auto it = t.prefixed.find(op);
    return it == t.prefixed.end() ? nullptr : &it->second;
}

uint32_t simdIntrinsic(uint32_t sub) { return sub < 256 ? tables().simd[sub] : ~0u; }

const SpecialIntrinsics &specials() { return tables().special; }

} // namespace bnwasm
