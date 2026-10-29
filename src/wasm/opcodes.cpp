//
// Created by Dottik on 28/10/2026.
//

#include "wasm/opcodes.hpp"

#include <array>
#include <format>
#include <sstream>
#include <vector>

namespace wasm {

static std::vector<std::string_view> split(std::string_view text) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < text.size()) {
        const size_t j = text.find(' ', i);
        out.push_back(text.substr(i, j == std::string_view::npos ? text.size() - i : j - i));
        if (j == std::string_view::npos)
            break;
        i = j + 1;
    }
    return out;
}

static std::array<std::string, 256> buildNames() {
    std::array<std::string, 256> n;
    const std::pair<uint8_t, const char *> fixed[] = {
        {0x00, "unreachable"}, {0x01, "nop"}, {0x02, "block"}, {0x03, "loop"}, {0x04, "if"},
        {0x05, "else"}, {0x06, "try"}, {0x07, "catch"}, {0x08, "throw"}, {0x09, "rethrow"},
        {0x0A, "throw_ref"}, {0x0B, "end"}, {0x0C, "br"}, {0x0D, "br_if"}, {0x0E, "br_table"},
        {0x0F, "return"}, {0x10, "call"}, {0x11, "call_indirect"}, {0x12, "return_call"},
        {0x13, "return_call_indirect"}, {0x14, "call_ref"}, {0x15, "return_call_ref"},
        {0x18, "delegate"}, {0x19, "catch_all"}, {0x1A, "drop"}, {0x1B, "select"},
        {0x1C, "select"}, {0x1F, "try_table"}, {0x20, "local.get"}, {0x21, "local.set"},
        {0x22, "local.tee"}, {0x23, "global.get"}, {0x24, "global.set"}, {0x25, "table.get"},
        {0x26, "table.set"}, {0x3F, "memory.size"}, {0x40, "memory.grow"},
        {0x41, "i32.const"}, {0x42, "i64.const"}, {0x43, "f32.const"}, {0x44, "f64.const"},
        {0xD0, "ref.null"}, {0xD1, "ref.is_null"}, {0xD2, "ref.func"}, {0xD3, "ref.eq"},
        {0xD4, "ref.as_non_null"}, {0xD5, "br_on_null"}, {0xD6, "br_on_non_null"},
    };
    for (auto [op, name] : fixed)
        n[op] = name;

    auto fill = [&](size_t base, std::string_view list) {
        size_t i = base;
        for (auto word : split(list))
            n[i++] = std::string(word);
        return i;
    };
    fill(0x28, "i32.load i64.load f32.load f64.load i32.load8_s i32.load8_u i32.load16_s "
               "i32.load16_u i64.load8_s i64.load8_u i64.load16_s i64.load16_u i64.load32_s "
               "i64.load32_u i32.store i64.store f32.store f64.store i32.store8 i32.store16 "
               "i64.store8 i64.store16 i64.store32");

    const auto cmp = split("eqz eq ne lt_s lt_u gt_s gt_u le_s le_u ge_s ge_u");
    const auto fcmp = split("eq ne lt gt le ge");
    const auto iar = split("clz ctz popcnt add sub mul div_s div_u rem_s rem_u and or xor shl shr_s shr_u rotl rotr");
    const auto far = split("abs neg ceil floor trunc nearest sqrt add sub mul div min max copysign");
    size_t i = 0x45;
    for (auto [prefix, list] : {std::pair{"i32.", &cmp}, {"i64.", &cmp}, {"f32.", &fcmp}, {"f64.", &fcmp},
                                {"i32.", &iar}, {"i64.", &iar}, {"f32.", &far}, {"f64.", &far}})
        for (auto word : *list)
            n[i++] = std::string(prefix) + std::string(word);

    fill(0xA7, "i32.wrap_i64 i32.trunc_f32_s i32.trunc_f32_u i32.trunc_f64_s i32.trunc_f64_u "
               "i64.extend_i32_s i64.extend_i32_u i64.trunc_f32_s i64.trunc_f32_u i64.trunc_f64_s "
               "i64.trunc_f64_u f32.convert_i32_s f32.convert_i32_u f32.convert_i64_s "
               "f32.convert_i64_u f32.demote_f64 f64.convert_i32_s f64.convert_i32_u "
               "f64.convert_i64_s f64.convert_i64_u f64.promote_f32 i32.reinterpret_f32 "
               "i64.reinterpret_f64 f32.reinterpret_i32 f64.reinterpret_i64 i32.extend8_s "
               "i32.extend16_s i64.extend8_s i64.extend16_s i64.extend32_s");
    return n;
}

static const std::array<std::string, 256> &names() {
    static const auto n = buildNames();
    return n;
}

static const std::vector<std::string_view> &fcNames() {
    static const auto n = split("i32.trunc_sat_f32_s i32.trunc_sat_f32_u i32.trunc_sat_f64_s i32.trunc_sat_f64_u "
                                "i64.trunc_sat_f32_s i64.trunc_sat_f32_u i64.trunc_sat_f64_s i64.trunc_sat_f64_u "
                                "memory.init data.drop memory.copy memory.fill table.init elem.drop table.copy "
                                "table.grow table.size table.fill");
    return n;
}

static std::array<std::optional<AtomicInfo>, 0x4F> buildAtomics() {
    std::array<std::optional<AtomicInfo>, 0x4F> t;
    t[0] = AtomicInfo{"memory.atomic.notify", 1, 2, 1};
    t[1] = AtomicInfo{"memory.atomic.wait32", 1, 3, 1};
    t[2] = AtomicInfo{"memory.atomic.wait64", 1, 3, 1};
    t[3] = AtomicInfo{"atomic.fence", 0, 0, 0};
    const char *loads[] = {"i32.atomic.load", "i64.atomic.load", "i32.atomic.load8_u", "i32.atomic.load16_u",
                           "i64.atomic.load8_u", "i64.atomic.load16_u", "i64.atomic.load32_u"};
    const char *stores[] = {"i32.atomic.store", "i64.atomic.store", "i32.atomic.store8", "i32.atomic.store16",
                            "i64.atomic.store8", "i64.atomic.store16", "i64.atomic.store32"};
    for (int i = 0; i < 7; ++i) {
        t[0x10 + i] = AtomicInfo{loads[i], 1, 1, 1};
        t[0x17 + i] = AtomicInfo{stores[i], 1, 2, 0};
    }
    const uint8_t bases[] = {0x1E, 0x25, 0x2C, 0x33, 0x3A, 0x41, 0x48};
    const char *ops[] = {"add", "sub", "and", "or", "xor", "xchg", "cmpxchg"};
    const char *shapes[] = {"i32.atomic.rmw.{}", "i64.atomic.rmw.{}", "i32.atomic.rmw8.{}_u",
                            "i32.atomic.rmw16.{}_u", "i64.atomic.rmw8.{}_u", "i64.atomic.rmw16.{}_u",
                            "i64.atomic.rmw32.{}_u"};
    for (int b = 0; b < 7; ++b)
        for (int i = 0; i < 7; ++i) {
            std::string name = shapes[i];
            name.replace(name.find("{}"), 2, ops[b]);
            t[bases[b] + i] = AtomicInfo{name, 1, static_cast<uint8_t>(b == 6 ? 3 : 2), 1};
        }
    return t;
}

const AtomicInfo *atomicInfo(uint32_t sub) {
    static const auto t = buildAtomics();
    return sub < t.size() && t[sub] ? &*t[sub] : nullptr;
}

static std::array<std::optional<SimdSig>, 256> buildSimd() {
    std::array<std::optional<SimdSig>, 256> s;
    for (int o = 0; o < 0x0B; ++o)
        s[o] = SimdSig{"s", "v"};
    s[0x0B] = SimdSig{"sv", ""};
    s[0x0C] = SimdSig{"", "v"};
    s[0x0D] = s[0x0E] = SimdSig{"vv", "v"};
    for (int o = 0x0F; o < 0x15; ++o)
        s[o] = SimdSig{"s", "v"};
    for (int o : {0x15, 0x16, 0x18, 0x19, 0x1B, 0x1D, 0x1F, 0x21})
        s[o] = SimdSig{"v", "s"};
    for (int o : {0x17, 0x1A, 0x1C, 0x1E, 0x20, 0x22})
        s[o] = SimdSig{"vs", "v"};
    for (int o = 0x23; o < 0x4D; ++o)
        s[o] = SimdSig{"vv", "v"};
    s[0x4D] = SimdSig{"v", "v"};
    for (int o = 0x4E; o < 0x52; ++o)
        s[o] = SimdSig{"vv", "v"};
    s[0x52] = SimdSig{"vvv", "v"};
    s[0x53] = SimdSig{"v", "s"};
    for (int o = 0x54; o < 0x58; ++o)
        s[o] = SimdSig{"sv", "v"};
    for (int o = 0x58; o < 0x5C; ++o)
        s[o] = SimdSig{"sv", ""};
    s[0x5C] = s[0x5D] = SimdSig{"s", "v"};
    s[0x5E] = s[0x5F] = SimdSig{"v", "v"};

    std::array<bool, 256> unary{}, scalar{}, shifts{};
    for (int o : {0x60, 0x61, 0x62, 0x67, 0x68, 0x69, 0x6A, 0x74, 0x75, 0x7A, 0x7C, 0x7D, 0x7E, 0x7F,
                  0x80, 0x81, 0x87, 0x88, 0x89, 0x8A, 0x94, 0xA0, 0xA1, 0xA7, 0xA8, 0xA9, 0xAA, 0xC0,
                  0xC1, 0xC7, 0xC8, 0xC9, 0xCA, 0xE0, 0xE1, 0xE3, 0xEC, 0xED, 0xEF})
        unary[o] = true;
    for (int o = 0xF8; o < 0x100; ++o)
        unary[o] = true;
    for (int o : {0x63, 0x64, 0x83, 0x84, 0xA3, 0xA4, 0xC3, 0xC4})
        scalar[o] = true;
    for (int o : {0x6B, 0x6C, 0x6D, 0x8B, 0x8C, 0x8D, 0xAB, 0xAC, 0xAD, 0xCB, 0xCC, 0xCD})
        shifts[o] = true;
    for (int o = 0x60; o < 0x100; ++o)
        s[o] = unary[o] ? SimdSig{"v", "v"} : scalar[o] ? SimdSig{"v", "s"} : shifts[o] ? SimdSig{"vs", "v"} : SimdSig{"vv", "v"};
    return s;
}

std::optional<SimdSig> simdSig(uint32_t sub) {
    static const auto t = buildSimd();
    return sub < t.size() ? t[sub] : std::nullopt;
}

int simdImmCount(uint32_t sub) {
    if (sub == 0x0D || (sub >= 0x54 && sub <= 0x5B))
        return 2;
    if (sub <= 0x0B || sub == 0x5C || sub == 0x5D || (sub >= 0x15 && sub <= 0x22))
        return 1;
    return 0;
}

int fcUlebCount(uint32_t sub) {
    switch (sub) {
    case 8: case 10: case 12: case 14: return 2;
    case 9: case 11: case 13: case 15: case 16: case 17: return 1;
    default: return 0;
    }
}

std::string_view valtypeName(uint8_t type) {
    switch (type) {
    case 0x7F: return "i32";
    case 0x7E: return "i64";
    case 0x7D: return "f32";
    case 0x7C: return "f64";
    case 0x7B: return "v128";
    case 0x70: return "funcref";
    case 0x6F: return "externref";
    case 0x69: return "exnref";
    default: return "?";
    }
}

static std::array<std::string, 256> buildSimdNames() {
    std::array<std::string, 256> n;
    const char *mem[] = {"v128.load", "v128.load8x8_s", "v128.load8x8_u", "v128.load16x4_s", "v128.load16x4_u",
                         "v128.load32x2_s", "v128.load32x2_u", "v128.load8_splat", "v128.load16_splat",
                         "v128.load32_splat", "v128.load64_splat", "v128.store", "v128.const", "i8x16.shuffle",
                         "i8x16.swizzle", "i8x16.splat", "i16x8.splat", "i32x4.splat", "i64x2.splat", "f32x4.splat",
                         "f64x2.splat"};
    for (size_t i = 0; i < std::size(mem); ++i)
        n[i] = mem[i];
    const char *lanes[] = {"i8x16.extract_lane_s", "i8x16.extract_lane_u", "i8x16.replace_lane",
                           "i16x8.extract_lane_s", "i16x8.extract_lane_u", "i16x8.replace_lane",
                           "i32x4.extract_lane",   "i32x4.replace_lane",   "i64x2.extract_lane",
                           "i64x2.replace_lane",   "f32x4.extract_lane",   "f32x4.replace_lane",
                           "f64x2.extract_lane",   "f64x2.replace_lane"};
    for (size_t i = 0; i < std::size(lanes); ++i)
        n[0x15 + i] = lanes[i];
    const char *cmpShape[] = {"i8x16", "i16x8", "i32x4"};
    const char *cmpOp[] = {"eq", "ne", "lt_s", "lt_u", "gt_s", "gt_u", "le_s", "le_u", "ge_s", "ge_u"};
    for (int s = 0; s < 3; ++s)
        for (int o = 0; o < 10; ++o)
            n[0x23 + s * 10 + o] = std::string(cmpShape[s]) + "." + cmpOp[o];
    const char *fcmp[] = {"eq", "ne", "lt", "gt", "le", "ge"};
    for (int o = 0; o < 6; ++o) {
        n[0x41 + o] = std::string("f32x4.") + fcmp[o];
        n[0x47 + o] = std::string("f64x2.") + fcmp[o];
    }
    const char *v[] = {"v128.not", "v128.and", "v128.andnot", "v128.or", "v128.xor", "v128.bitselect", "v128.any_true",
                       "v128.load8_lane", "v128.load16_lane", "v128.load32_lane", "v128.load64_lane",
                       "v128.store8_lane", "v128.store16_lane", "v128.store32_lane", "v128.store64_lane",
                       "v128.load32_zero", "v128.load64_zero", "f32x4.demote_f64x2_zero", "f64x2.promote_low_f32x4"};
    for (size_t i = 0; i < std::size(v); ++i)
        n[0x4D + i] = v[i];
    const struct {
        int base;
        const char *shape;
        const char *ops[32];
    } ints[] = {
        {0x60, "i8x16", {"abs", "neg", "popcnt", "all_true", "bitmask", "narrow_i16x8_s", "narrow_i16x8_u", "", "", "", "", "shl", "shr_s", "shr_u", "add", "add_sat_s", "add_sat_u", "sub", "sub_sat_s", "sub_sat_u", "", "", "min_s", "min_u", "max_s", "max_u", "", "avgr_u"}},
        {0x80, "i16x8", {"abs", "neg", "q15mulr_sat_s", "all_true", "bitmask", "narrow_i32x4_s", "narrow_i32x4_u", "extend_low_i8x16_s", "extend_high_i8x16_s", "extend_low_i8x16_u", "extend_high_i8x16_u", "shl", "shr_s", "shr_u", "add", "add_sat_s", "add_sat_u", "sub", "sub_sat_s", "sub_sat_u", "", "mul", "min_s", "min_u", "max_s", "max_u", "", "avgr_u", "extmul_low_i8x16_s", "extmul_high_i8x16_s", "extmul_low_i8x16_u", "extmul_high_i8x16_u"}},
        {0xA0, "i32x4", {"abs", "neg", "", "all_true", "bitmask", "", "", "extend_low_i16x8_s", "extend_high_i16x8_s", "extend_low_i16x8_u", "extend_high_i16x8_u", "shl", "shr_s", "shr_u", "add", "", "", "sub", "", "", "", "mul", "min_s", "min_u", "max_s", "max_u", "dot_i16x8_s", "", "extmul_low_i16x8_s", "extmul_high_i16x8_s", "extmul_low_i16x8_u", "extmul_high_i16x8_u"}},
        {0xC0, "i64x2", {"abs", "neg", "", "all_true", "bitmask", "", "", "extend_low_i32x4_s", "extend_high_i32x4_s", "extend_low_i32x4_u", "extend_high_i32x4_u", "shl", "shr_s", "shr_u", "add", "", "", "sub", "", "", "", "mul", "eq", "ne", "lt_s", "gt_s", "le_s", "ge_s", "extmul_low_i32x4_s", "extmul_high_i32x4_s", "extmul_low_i32x4_u", "extmul_high_i32x4_u"}},
    };
    for (const auto &g : ints)
        for (int i = 0; i < 32; ++i)
            if (g.ops[i] && *g.ops[i])
                n[g.base + i] = std::string(g.shape) + "." + g.ops[i];
    n[0x7C] = "i16x8.extadd_pairwise_i8x16_s"; n[0x7D] = "i16x8.extadd_pairwise_i8x16_u";
    n[0x7E] = "i32x4.extadd_pairwise_i16x8_s"; n[0x7F] = "i32x4.extadd_pairwise_i16x8_u";
    const char *fl[] = {"ceil", "floor", "trunc", "nearest"};
    for (int i = 0; i < 4; ++i)
        n[0x67 + i] = std::string("f32x4.") + fl[i];
    n[0x74] = "f64x2.ceil"; n[0x75] = "f64x2.floor"; n[0x7A] = "f64x2.trunc"; n[0x94] = "f64x2.nearest";
    const char *fo[] = {"abs", "neg", "", "sqrt", "add", "sub", "mul", "div", "min", "max", "pmin", "pmax"};
    for (int i = 0; i < 12; ++i)
        if (*fo[i]) {
            n[0xE0 + i] = std::string("f32x4.") + fo[i];
            n[0xEC + i] = std::string("f64x2.") + fo[i];
        }
    n[0xF8] = "i32x4.trunc_sat_f32x4_s"; n[0xF9] = "i32x4.trunc_sat_f32x4_u";
    n[0xFA] = "f32x4.convert_i32x4_s";   n[0xFB] = "f32x4.convert_i32x4_u";
    n[0xFC] = "i32x4.trunc_sat_f64x2_s_zero"; n[0xFD] = "i32x4.trunc_sat_f64x2_u_zero";
    n[0xFE] = "f64x2.convert_low_i32x4_s";    n[0xFF] = "f64x2.convert_low_i32x4_u";
    return n;
}

std::string instrName(uint32_t op) {
    if (op < 0x100)
        return names()[op];
    const uint32_t prefix = op & 0xFF00, sub = op & 0xFF;
    if (prefix == kPrefixFC)
        return sub < fcNames().size() ? std::string(fcNames()[sub]) : std::format("misc.0x{:02x}", sub);
    if (prefix == kPrefixFD) {
        static const auto names = buildSimdNames();
        return names[sub].empty() ? std::format("simd.0x{:02x}", sub) : names[sub];
    }
    if (const auto *a = atomicInfo(sub))
        return a->name;
    return std::format("atomic.0x{:02x}", sub);
}

static std::array<std::optional<Effect>, 256> buildEffects() {
    std::array<std::optional<Effect>, 256> e;
    const std::pair<uint8_t, Effect> fixed[] = {
        {0x1A, {1, 0}}, {0x1B, {3, 1}}, {0x1C, {3, 1}}, {0x20, {0, 1}}, {0x21, {1, 0}}, {0x22, {1, 1}},
        {0x23, {0, 1}}, {0x24, {1, 0}}, {0x25, {1, 1}}, {0x26, {2, 0}}, {0x3F, {0, 1}}, {0x40, {1, 1}},
        {0xD0, {0, 1}}, {0xD1, {1, 1}}, {0xD2, {0, 1}}, {0xD3, {2, 1}}, {0xD4, {1, 1}}, {0x0E, {1, 0}},
    };
    for (auto [o, eff] : fixed)
        e[o] = eff;
    struct Range {
        int lo, hi;
        Effect eff;
    };
    const Range ranges[] = {
        {0x28, 0x35, {1, 1}}, {0x36, 0x3E, {2, 0}}, {0x41, 0x44, {0, 1}}, {0x46, 0x4F, {2, 1}},
        {0x51, 0x66, {2, 1}}, {0x67, 0x69, {1, 1}}, {0x6A, 0x78, {2, 1}}, {0x79, 0x7B, {1, 1}},
        {0x7C, 0x8A, {2, 1}}, {0x8B, 0x91, {1, 1}}, {0x92, 0x98, {2, 1}}, {0x99, 0x9F, {1, 1}},
        {0xA0, 0xA6, {2, 1}}, {0xA7, 0xC4, {1, 1}},
    };
    for (const auto &r : ranges)
        for (int o = r.lo; o <= r.hi; ++o)
            e[o] = r.eff;
    e[0x45] = e[0x50] = Effect{1, 1};
    return e;
}

std::optional<Effect> fixedEffect(uint32_t op) {
    static const auto t = buildEffects();
    if (op >= 0xFE00) {
        const auto *a = atomicInfo(op & 0xFF);
        return a ? Effect{a->pops, a->pushes} : Effect{0, 0};
    }
    if (op >= 0xFD00) {
        const auto s = simdSig(op & 0xFF);
        return s ? Effect{static_cast<int>(s->pops.size()), static_cast<int>(s->pushes.size())} : Effect{0, 0};
    }
    if (op >= 0xFC00) {
        const auto name = instrName(op);
        const std::pair<const char *, Effect> fc[] = {
            {"memory.init", {3, 0}}, {"memory.copy", {3, 0}}, {"memory.fill", {3, 0}}, {"table.init", {3, 0}},
            {"table.copy", {3, 0}}, {"table.fill", {3, 0}}, {"table.grow", {2, 1}}, {"table.size", {0, 1}},
            {"data.drop", {0, 0}}, {"elem.drop", {0, 0}},
        };
        for (const auto &[n, eff] : fc)
            if (name == n)
                return eff;
        return Effect{1, 1};
    }
    if (op == 0x10 || op == 0x12 || op == 0x11 || op == 0x13)
        return std::nullopt;
    return t[op] ? *t[op] : Effect{0, 0};
}

} // namespace wasm
