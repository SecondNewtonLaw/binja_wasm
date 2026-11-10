//
// Headless lift tests: builder -> parser -> production lifter -> partial IL,
// with the independent WASM interpreter as the semantic oracle.
//
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "il_interp.hpp"
#include "lift_coverage.hpp"
#include "lift_runner.hpp"
#include "partial_il.hpp"
#include "wasm_builder.hpp"
#include "wasm_interp.hpp"

namespace {

    struct TestFailure : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

#define CHECK(cond)                                                                                                                                            \
    do {                                                                                                                                                       \
        if (!(cond))                                                                                                                                           \
            throw TestFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #cond);                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                                                         \
    do {                                                                                                                                                       \
        const auto _a = (a);                                                                                                                                   \
        const auto _b = (b);                                                                                                                                   \
        if (!(_a == _b))                                                                                                                                       \
            throw TestFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #a + " == " + #b);                                               \
    } while (0)

    using wasmbuild::Code;
    using wasmbuild::ModuleBuilder;
    using wasmtest::v32;
    using wasmtest::v64;
    using wasmtest::Value;
    using wasmtest::vf32;
    using wasmtest::vf64;

    constexpr uint8_t I32 = wasm::kI32;
    constexpr uint8_t I64 = wasm::kI64;
    constexpr uint8_t F32 = wasm::kF32;
    constexpr uint8_t F64 = wasm::kF64;
    constexpr uint8_t V128 = wasm::kV128;

    // Value types are encoded as negative signed-LEB integers in block types:
    // i32 = 0x7F -> -1, i64 -> -2, ..., empty = 0x40 -> -64.
    int64_t blockType(uint8_t t) { return static_cast<int64_t>(t) - 0x80; }

    wasm::Module parseOrThrow(const ModuleBuilder &builder) {
        auto m = wasm::parseModule(builder.build());
        if (!m)
            throw TestFailure("parseModule failed");
        return std::move(*m);
    }

    std::string hexValue(uint64_t v, size_t bytes) {
        std::ostringstream os;
        os << "0x" << std::hex << std::setw(static_cast<int>(bytes * 2)) << std::setfill('0') << v;
        return os.str();
    }

    std::string fmtValue(const Value &v, uint8_t type) {
        switch (type) {
        case I32:
            return "i32 " + hexValue(static_cast<uint32_t>(v.lo), 4);
        case I64:
            return "i64 " + hexValue(v.lo, 8);
        case F32: {
            std::ostringstream os;
            os << "f32 " << std::bit_cast<float>(static_cast<uint32_t>(v.lo)) << " (" << hexValue(static_cast<uint32_t>(v.lo), 4) << ")";
            return os.str();
        }
        case F64: {
            std::ostringstream os;
            os << "f64 " << std::bit_cast<double>(v.lo) << " (" << hexValue(v.lo, 8) << ")";
            return os.str();
        }
        case V128:
            return "v128 " + hexValue(v.hi, 8) + hexValue(v.lo, 8);
        default:
            return "? " + hexValue(v.lo, 8);
        }
    }

    bool sameValue(const Value &a, const Value &b, uint8_t type) {
        switch (type) {
        case I32:
            return static_cast<uint32_t>(a.lo) == static_cast<uint32_t>(b.lo);
        case I64:
            return a.lo == b.lo;
        case F32: {
            const float x = std::bit_cast<float>(static_cast<uint32_t>(a.lo));
            const float y = std::bit_cast<float>(static_cast<uint32_t>(b.lo));
            if (std::isnan(x) && std::isnan(y))
                return true;
            return static_cast<uint32_t>(a.lo) == static_cast<uint32_t>(b.lo);
        }
        case F64: {
            const double x = std::bit_cast<double>(a.lo);
            const double y = std::bit_cast<double>(b.lo);
            if (std::isnan(x) && std::isnan(y))
                return true;
            return a.lo == b.lo;
        }
        case V128:
            return a.lo == b.lo && a.hi == b.hi;
        default:
            return a.lo == b.lo;
        }
    }

    struct Fixture {
        wasm::Module module;
        lift_test::LiftHarness lift;
        std::unordered_map<uint64_t, const lift_test::LiftedFunction *> funcs;

        explicit Fixture(const ModuleBuilder &builder) : module(parseOrThrow(builder)), lift(module) {
            for (uint64_t start : module.starts)
                funcs[start] = &lift.lift(start);
        }

        explicit Fixture(wasm::Module m) : module(std::move(m)), lift(module) {
            for (uint64_t start : module.starts)
                funcs[start] = &lift.lift(start);
        }

        uint32_t indexOf(const std::string &name) const {
            for (size_t i = 0; i < module.functions.size(); ++i)
                if (module.functions[i].name == name)
                    return static_cast<uint32_t>(i);
            throw TestFailure("function not found: " + name);
        }

        uint32_t tableIndexOf(const std::string &name) const {
            const uint32_t fn = indexOf(name);
            for (size_t i = 0; i < module.table.size(); ++i)
                if (module.table[i] == fn)
                    return static_cast<uint32_t>(i);
            throw TestFailure("function not in table: " + name);
        }

        struct Result {
            std::vector<Value> wasm;
            std::vector<Value> il;
            std::vector<uint8_t> wasmMemory;
            std::vector<uint8_t> ilMemory;
            std::vector<Value> wasmGlobals;
            std::vector<Value> ilGlobals;
        };

        Result run(uint32_t func, const std::vector<Value> &args) {
            wasmtest::WasmInterpreter wasm(module);
            auto wr = wasm.call(func, args);
            lift_test::IlInterpreter il(module, funcs);
            const uint64_t start = *module.functions[func].address;
            auto ir = il.call(start, args);
            return {std::move(wr), std::move(ir), wasm.memory(), il.memory, wasm.globals(), il.globals};
        }

        Result check(uint32_t func, const std::vector<Value> &args, const std::string &label) {
            Result r;
            try {
                r = run(func, args);
            } catch (const wasmtest::WasmTrap &) {
                // Expected domain error from the oracle; the caller decides to skip.
                throw;
            } catch (const std::exception &e) {
                dump(func, label + " threw: " + e.what());
                throw;
            }
            const wasm::FuncType *sig = module.funcSig(func);
            bool ok = sig && r.wasm.size() == r.il.size();
            std::string why;
            if (ok) {
                for (size_t k = 0; k < r.wasm.size(); ++k) {
                    if (!sameValue(r.wasm[k], r.il[k], sig->results[k])) {
                        ok = false;
                        why = "result " + std::to_string(k) + ": wasm=" + fmtValue(r.wasm[k], sig->results[k]) + " il=" + fmtValue(r.il[k], sig->results[k]);
                        break;
                    }
                }
            } else {
                why = "result count wasm=" + std::to_string(r.wasm.size()) + " il=" + std::to_string(r.il.size());
            }
            if (ok && r.wasmMemory != r.ilMemory) {
                ok = false;
                size_t at = 0;
                while (at < std::min(r.wasmMemory.size(), r.ilMemory.size()) && r.wasmMemory[at] == r.ilMemory[at])
                    ++at;
                why = "memory differs at 0x" + hexValue(at, 4);
            }
            if (ok && r.wasmGlobals.size() != r.ilGlobals.size()) {
                ok = false;
                why = "global count differs";
            }
            if (ok && sig) {
                for (size_t k = 0; k < r.wasmGlobals.size() && k < module.globalTypes.size(); ++k) {
                    if (!sameValue(r.wasmGlobals[k], r.ilGlobals[k], module.globalTypes[k])) {
                        ok = false;
                        why = "global " + std::to_string(k) + ": wasm=" + fmtValue(r.wasmGlobals[k], module.globalTypes[k]) +
                              " il=" + fmtValue(r.ilGlobals[k], module.globalTypes[k]);
                        break;
                    }
                }
            }
            if (!ok) {
                dump(func, label + ": " + why);
                throw TestFailure(label + ": " + why);
            }
            return r;
        }

        void dump(uint32_t func, const std::string &label) {
            const uint64_t start = *module.functions[func].address;
            const auto it = funcs.find(start);
            std::fprintf(stderr, "\n%s: function %u at 0x%llx\n", label.c_str(), func, (unsigned long long)start);
            if (it != funcs.end())
                std::fprintf(stderr, "%s\n", BinaryNinja::printIl(it->second->il).c_str());
        }
    };

    void emitOp(Code &c, uint32_t op) {
        if (op < 0x100) {
            c.op(static_cast<uint8_t>(op));
        } else {
            c.u8(static_cast<uint8_t>(op >> 8));
            c.uleb(op & 0xFF);
        }
    }

    std::vector<Value> i32Inputs() { return {v32(0), v32(1), v32(2), v32(0xFFFFFFFF), v32(0x7FFFFFFF), v32(0x80000000), v32(0x12345678), v32(0xFFFFFFFE)}; }
    std::vector<Value> i64Inputs() {
        return {
            v64(0), v64(1), v64(2), v64(~0ull), v64(0x7FFFFFFFFFFFFFFFull), v64(0x8000000000000000ull), v64(0x123456789ABCDEFull), v64(0xFFFFFFFFFFFFFFFEull)
        };
    }
    std::vector<Value> f32Inputs() {
        return {
            vf32(0.0f),
            vf32(-0.0f),
            vf32(1.0f),
            vf32(-1.0f),
            vf32(2.5f),
            vf32(-3.75f),
            vf32(1e30f),
            vf32(1e-30f),
            vf32(std::numeric_limits<float>::infinity()),
            vf32(-std::numeric_limits<float>::infinity()),
            vf32(std::numeric_limits<float>::quiet_NaN())
        };
    }
    std::vector<Value> f64Inputs() {
        return {
            vf64(0.0),
            vf64(-0.0),
            vf64(1.0),
            vf64(-1.0),
            vf64(2.5),
            vf64(-3.75),
            vf64(1e300),
            vf64(1e-300),
            vf64(std::numeric_limits<double>::infinity()),
            vf64(-std::numeric_limits<double>::infinity()),
            vf64(std::numeric_limits<double>::quiet_NaN())
        };
    }

    bool isDivOrRem(uint32_t op) { return (op >= 0x6D && op <= 0x70) || (op >= 0x7F && op <= 0x82); }

    void checkUnary(uint32_t op, uint8_t type, uint8_t result, const std::vector<Value> &inputs, const std::string &label) {
        ModuleBuilder b;
        Code c;
        c.localGet(0);
        emitOp(c, op);
        c.end();
        const uint32_t f = b.addTypedFunction({type}, {result}, c.b);
        Fixture fx(b);
        for (const Value &v : inputs) {
            try {
                fx.check(f, {v}, label + " " + fmtValue(v, type));
            } catch (const wasmtest::WasmTrap &) {
                // The oracle traps: the test input is outside the opcode domain.
            }
        }
    }

    void checkBinary(uint32_t op, uint8_t type, uint8_t result, const std::vector<Value> &inputs, const std::string &label) {
        ModuleBuilder b;
        Code c;
        c.localGet(0).localGet(1);
        emitOp(c, op);
        c.end();
        const uint32_t f = b.addTypedFunction({type, type}, {result}, c.b);
        Fixture fx(b);
        for (const Value &a : inputs) {
            for (const Value &bb : inputs) {
                try {
                    fx.check(f, {a, bb}, label + " " + fmtValue(a, type) + ", " + fmtValue(bb, type));
                } catch (const wasmtest::WasmTrap &) {
                    // Domain error in the oracle (div by zero, min/-1, NaN conversion).
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Numeric matrix
    // ---------------------------------------------------------------------------

    void testArithmeticSanity() {
        ModuleBuilder b;
        Code c;
        c.localGet(0).localGet(1).op(0x6A).end();
        const uint32_t f = b.addTypedFunction({I32, I32}, {I32}, c.b);
        Fixture fx(b);
        auto r = fx.check(f, {v32(7), v32(35)}, "add sanity");
        CHECK_EQ(static_cast<uint32_t>(r.wasm[0].lo), 42u);
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 42u);
    }

    void testNumericI32() {
        checkUnary(0x45, I32, I32, i32Inputs(), "i32.eqz");
        for (uint32_t op = 0x46; op <= 0x4F; ++op)
            checkBinary(op, I32, I32, i32Inputs(), "i32.cmp");
        for (uint32_t op = 0x67; op <= 0x69; ++op)
            checkUnary(op, I32, I32, i32Inputs(), "i32.unary");
        for (uint32_t op = 0x6A; op <= 0x78; ++op) {
            if (isDivOrRem(op)) {
                // Skip zero divisors and overflow in the oracle by filtering the input set.
                ModuleBuilder b;
                Code c;
                c.localGet(0).localGet(1);
                emitOp(c, op);
                c.end();
                const uint32_t f = b.addTypedFunction({I32, I32}, {I32}, c.b);
                Fixture fx(b);
                for (const Value &a : i32Inputs()) {
                    for (const Value &bb : i32Inputs()) {
                        if ((static_cast<uint32_t>(bb.lo) == 0) ||
                            (static_cast<int32_t>(a.lo) == std::numeric_limits<int32_t>::min() && static_cast<int32_t>(bb.lo) == -1))
                            continue;
                        fx.check(f, {a, bb}, "i32.bin " + fmtValue(a, I32) + ", " + fmtValue(bb, I32));
                    }
                }
            } else {
                checkBinary(op, I32, I32, i32Inputs(), "i32.bin");
            }
        }
    }

    void testNumericI64() {
        checkUnary(0x50, I64, I32, i64Inputs(), "i64.eqz");
        for (uint32_t op = 0x51; op <= 0x5A; ++op)
            checkBinary(op, I64, I32, i64Inputs(), "i64.cmp");
        for (uint32_t op = 0x79; op <= 0x7B; ++op)
            checkUnary(op, I64, I64, i64Inputs(), "i64.unary");
        for (uint32_t op = 0x7C; op <= 0x8A; ++op) {
            if (isDivOrRem(op)) {
                ModuleBuilder b;
                Code c;
                c.localGet(0).localGet(1);
                emitOp(c, op);
                c.end();
                const uint32_t f = b.addTypedFunction({I64, I64}, {I64}, c.b);
                Fixture fx(b);
                for (const Value &a : i64Inputs()) {
                    for (const Value &bb : i64Inputs()) {
                        if (bb.lo == 0 || (static_cast<int64_t>(a.lo) == std::numeric_limits<int64_t>::min() && static_cast<int64_t>(bb.lo) == -1))
                            continue;
                        fx.check(f, {a, bb}, "i64.bin " + fmtValue(a, I64) + ", " + fmtValue(bb, I64));
                    }
                }
            } else {
                checkBinary(op, I64, I64, i64Inputs(), "i64.bin");
            }
        }
    }

    void testNumericF32() {
        for (uint32_t op = 0x5B; op <= 0x60; ++op)
            checkBinary(op, F32, I32, f32Inputs(), "f32.cmp");
        for (uint32_t op = 0x8B; op <= 0x91; ++op)
            checkUnary(op, F32, F32, f32Inputs(), "f32.unary");
        for (uint32_t op = 0x92; op <= 0x98; ++op)
            checkBinary(op, F32, F32, f32Inputs(), "f32.bin");
    }

    void testNumericF64() {
        for (uint32_t op = 0x61; op <= 0x66; ++op)
            checkBinary(op, F64, I32, f64Inputs(), "f64.cmp");
        for (uint32_t op = 0x99; op <= 0x9F; ++op)
            checkUnary(op, F64, F64, f64Inputs(), "f64.unary");
        for (uint32_t op = 0xA0; op <= 0xA6; ++op)
            checkBinary(op, F64, F64, f64Inputs(), "f64.bin");
    }

    /// Pins the Wasm min/max signed-zero and NaN rules with explicit expected
    /// bits. The numeric matrix only requires the oracle and the IL emulation to
    /// agree, so it cannot catch a wrong result both sides share.
    void testMinMaxSemantics() {
        const float pzf = 0.0f, nzf = -0.0f;
        const double pzd = 0.0, nzd = -0.0;

        CHECK_EQ(std::bit_cast<uint32_t>(wasmtest::wasmMin(pzf, nzf)), 0x80000000u);
        CHECK_EQ(std::bit_cast<uint32_t>(wasmtest::wasmMin(nzf, pzf)), 0x80000000u);
        CHECK_EQ(std::bit_cast<uint32_t>(wasmtest::wasmMax(pzf, nzf)), 0x00000000u);
        CHECK_EQ(std::bit_cast<uint32_t>(wasmtest::wasmMax(nzf, pzf)), 0x00000000u);

        CHECK_EQ(std::bit_cast<uint64_t>(wasmtest::wasmMin(pzd, nzd)), 0x8000000000000000ull);
        CHECK_EQ(std::bit_cast<uint64_t>(wasmtest::wasmMin(nzd, pzd)), 0x8000000000000000ull);
        CHECK_EQ(std::bit_cast<uint64_t>(wasmtest::wasmMax(pzd, nzd)), 0x0000000000000000ull);
        CHECK_EQ(std::bit_cast<uint64_t>(wasmtest::wasmMax(nzd, pzd)), 0x0000000000000000ull);

        const float nanf = std::numeric_limits<float>::quiet_NaN();
        const double nand = std::numeric_limits<double>::quiet_NaN();
        CHECK(std::isnan(wasmtest::wasmMin(nanf, 1.0f)));
        CHECK(std::isnan(wasmtest::wasmMin(1.0f, nanf)));
        CHECK(std::isnan(wasmtest::wasmMax(nanf, 1.0f)));
        CHECK(std::isnan(wasmtest::wasmMax(1.0f, nanf)));
        CHECK(std::isnan(wasmtest::wasmMin(nand, 1.0)));
        CHECK(std::isnan(wasmtest::wasmMin(1.0, nand)));
        CHECK(std::isnan(wasmtest::wasmMax(nand, 1.0)));
        CHECK(std::isnan(wasmtest::wasmMax(1.0, nand)));

        CHECK_EQ(wasmtest::wasmMin(1.0f, 2.0f), 1.0f);
        CHECK_EQ(wasmtest::wasmMin(2.0f, 1.0f), 1.0f);
        CHECK_EQ(wasmtest::wasmMax(1.0f, 2.0f), 2.0f);
        CHECK_EQ(wasmtest::wasmMax(2.0f, 1.0f), 2.0f);
        CHECK_EQ(wasmtest::wasmMin(1.0, 2.0), 1.0);
        CHECK_EQ(wasmtest::wasmMin(2.0, 1.0), 1.0);
        CHECK_EQ(wasmtest::wasmMax(1.0, 2.0), 2.0);
        CHECK_EQ(wasmtest::wasmMax(2.0, 1.0), 2.0);
    }

    // ---------------------------------------------------------------------------
    // Conversions
    // ---------------------------------------------------------------------------

    void testConversions() {
        struct Case {
            uint32_t op;
            uint8_t src, dst;
            std::vector<Value> inputs;
        };
        std::vector<Value> smallF32 = {vf32(0.0f), vf32(1.5f), vf32(-2.5f), vf32(123.75f), vf32(65535.0f)};
        std::vector<Value> smallF64 = {vf64(0.0), vf64(1.5), vf64(-2.5), vf64(-2147483648.5), vf64(123.75), vf64(2048.5)};
        std::vector<Value> posF32 = {vf32(0.0f), vf32(-0.5f), vf32(1.5f), vf32(2.5f), vf32(123.75f), vf32(1e9f)};
        std::vector<Value> posF64 = {vf64(0.0), vf64(-0.5), vf64(1.5), vf64(2.5), vf64(123.75), vf64(1e18)};
        const std::vector<Case> cases = {
            {0xA7, I64, I32, i64Inputs()}, {0xA8, F32, I32, smallF32},    {0xA9, F32, I32, posF32},      {0xAA, F64, I32, smallF64},
            {0xAB, F64, I32, posF64},      {0xAC, I32, I64, i32Inputs()}, {0xAD, I32, I64, i32Inputs()}, {0xAE, F32, I64, smallF32},
            {0xAF, F32, I64, posF32},      {0xB0, F64, I64, smallF64},    {0xB1, F64, I64, posF64},      {0xB2, I32, F32, i32Inputs()},
            {0xB3, I32, F32, i32Inputs()}, {0xB4, I64, F32, i64Inputs()}, {0xB5, I64, F32, i64Inputs()}, {0xB6, F64, F32, f64Inputs()},
            {0xB7, I32, F64, i32Inputs()}, {0xB8, I32, F64, i32Inputs()}, {0xB9, I64, F64, i64Inputs()}, {0xBA, I64, F64, i64Inputs()},
            {0xBB, F32, F64, f32Inputs()}, {0xBC, F32, I32, f32Inputs()}, {0xBD, F64, I64, f64Inputs()}, {0xBE, I32, F32, i32Inputs()},
            {0xBF, I64, F64, i64Inputs()}, {0xC0, I32, I32, i32Inputs()}, {0xC1, I32, I32, i32Inputs()}, {0xC2, I64, I64, i64Inputs()},
            {0xC3, I64, I64, i64Inputs()}, {0xC4, I64, I64, i64Inputs()},
        };
        for (const Case &c : cases) {
            for (const Value &v : c.inputs) {
                try {
                    checkUnary(c.op, c.src, c.dst, {v}, "convert " + std::string(wasm::instrName(c.op)));
                } catch (const wasmtest::WasmTrap &) {
                }
            }
        }
    }

    void testTruncSat() {
        std::vector<Value> f32v = {vf32(0.0f),
                                   vf32(1.9f),
                                   vf32(-1.9f),
                                   vf32(1e30f),
                                   vf32(-1e30f),
                                   vf32(std::numeric_limits<float>::quiet_NaN()),
                                   vf32(std::numeric_limits<float>::infinity())};
        std::vector<Value> f64v = {vf64(0.0),
                                   vf64(1.9),
                                   vf64(-1.9),
                                   vf64(1e30),
                                   vf64(-1e30),
                                   vf64(std::numeric_limits<double>::quiet_NaN()),
                                   vf64(std::numeric_limits<double>::infinity())};
        for (uint32_t sub = 0; sub < 8; ++sub) {
            const bool dst32 = sub < 4;
            const bool src32 = (sub % 4) < 2;
            const uint8_t src = src32 ? F32 : F64;
            const uint8_t dst = dst32 ? I32 : I64;
            for (const Value &v : src32 ? f32v : f64v)
                checkUnary(wasm::kPrefixFC | sub, src, dst, {v}, "trunc_sat");
        }
    }

    // ---------------------------------------------------------------------------
    // Memory
    // ---------------------------------------------------------------------------

    std::vector<wasmbuild::bytes> memoryPatterns() {
        wasmbuild::bytes p(64);
        for (size_t i = 0; i < p.size(); ++i)
            p[i] = static_cast<uint8_t>(i * 7 + 0x80);
        p[0] = 0x00;
        p[1] = 0x01;
        p[2] = 0x7F;
        p[3] = 0x80;
        p[4] = 0xFF;
        return {p};
    }

    void testMemoryLoads() {
        struct LoadCase {
            uint8_t op;
            uint8_t type;
            const char *name;
        };
        const LoadCase cases[] = {
            {0x28, I32, "i32.load"},     {0x29, I64, "i64.load"},     {0x2A, F32, "f32.load"},     {0x2B, F64, "f64.load"},     {0x2C, I32, "i32.load8_s"},
            {0x2D, I32, "i32.load8_u"},  {0x2E, I32, "i32.load16_s"}, {0x2F, I32, "i32.load16_u"}, {0x30, I64, "i64.load8_s"},  {0x31, I64, "i64.load8_u"},
            {0x32, I64, "i64.load16_s"}, {0x33, I64, "i64.load16_u"}, {0x34, I64, "i64.load32_s"}, {0x35, I64, "i64.load32_u"},
        };
        ModuleBuilder b;
        b.setMemoryPages(1);
        b.addData(0, memoryPatterns()[0]);
        std::vector<uint32_t> funcs;
        for (const LoadCase &c : cases) {
            Code code;
            code.localGet(0).load(c.op, 4);
            code.end();
            funcs.push_back(b.addTypedFunction({I32}, {c.type}, code.b));
        }
        Fixture fx(b);
        for (size_t i = 0; i < std::size(cases); ++i) {
            for (uint32_t addr = 0; addr < 8; ++addr)
                fx.check(funcs[i], {v32(addr)}, std::string("load ") + cases[i].name);
        }
    }

    void testMemoryStores() {
        struct StoreCase {
            uint8_t store;
            uint8_t load;
            uint8_t type;
            const char *name;
        };
        const StoreCase cases[] = {
            {0x36, 0x28, I32, "i32.store"},  {0x37, 0x29, I64, "i64.store"},   {0x38, 0x2A, F32, "f32.store"},
            {0x39, 0x2B, F64, "f64.store"},  {0x3A, 0x2D, I32, "i32.store8"},  {0x3B, 0x2F, I32, "i32.store16"},
            {0x3C, 0x31, I64, "i64.store8"}, {0x3D, 0x33, I64, "i64.store16"}, {0x3E, 0x35, I64, "i64.store32"},
        };
        for (const StoreCase &c : cases) {
            ModuleBuilder b;
            b.setMemoryPages(1);
            b.addData(0, memoryPatterns()[0]);
            Code code;
            code.localGet(0);
            if (c.type == I32)
                code.localGet(1);
            else
                code.localGet(1);
            code.store(c.store, 4);
            code.localGet(0).load(c.load, 4);
            code.end();
            const uint32_t f = b.addTypedFunction({I32, c.type}, {c.type}, code.b);
            Fixture fx(b);
            std::vector<Value> values;
            if (c.type == I32)
                values = {v32(0), v32(1), v32(0xFFFFFFFF), v32(0x89ABCDEF)};
            else if (c.type == I64)
                values = {v64(0), v64(1), v64(~0ull), v64(0x0123456789ABCDEFull)};
            else if (c.type == F32)
                values = {vf32(0.0f), vf32(1.5f), vf32(-2.25f), vf32(1e30f)};
            else
                values = {vf64(0.0), vf64(1.5), vf64(-2.25), vf64(1e300)};
            for (uint32_t addr = 0; addr < 4; ++addr)
                for (const Value &v : values)
                    fx.check(f, {v32(addr), v}, std::string("store ") + c.name);
        }
    }

    void testBulkMemory() {
        ModuleBuilder b;
        b.setMemoryPages(1);
        b.addData(0x20, {1, 2, 3, 4});

        Code fill;
        fill.i32(0).i32(0xAB).i32(4).prefix(0xFC, 11).uleb(0);
        fill.i32(0).load(0x28).end();
        const uint32_t ffill = b.addTypedFunction({}, {I32}, fill.b);

        Code copy;
        copy.i32(0).i32(0xCD).i32(4).prefix(0xFC, 11).uleb(0);
        copy.i32(8).i32(0).i32(4).prefix(0xFC, 10).uleb(0).uleb(0);
        copy.i32(8).load(0x28).end();
        const uint32_t fcopy = b.addTypedFunction({}, {I32}, copy.b);

        Code init;
        init.i32(16).i32(0).i32(4).prefix(0xFC, 8).uleb(0).uleb(0);
        init.i32(16).load(0x28).end();
        const uint32_t finit = b.addTypedFunction({}, {I32}, init.b);

        Fixture fx(b);
        auto r = fx.check(ffill, {}, "memory.fill");
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 0xABABABABu);
        r = fx.check(fcopy, {}, "memory.copy");
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 0xCDCDCDCDu);
        r = fx.check(finit, {}, "memory.init");
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 0x04030201u);
    }

    void testMemorySizeGrow() {
        ModuleBuilder b;
        b.setMemoryPages(2);
        Code size;
        size.memorySize().end();
        const uint32_t fsize = b.addTypedFunction({}, {I32}, size.b);
        Code grow;
        grow.localGet(0).memoryGrow();
        grow.drop();
        grow.memorySize();
        grow.end();
        const uint32_t fgrow = b.addTypedFunction({I32}, {I32}, grow.b);
        Fixture fx(b);
        fx.check(fsize, {}, "memory.size");
        auto r = fx.check(fgrow, {v32(1)}, "memory.grow + size");
        CHECK_EQ(static_cast<uint32_t>(r.wasm[0].lo), 3u);
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 3u);
    }

    // ---------------------------------------------------------------------------
    // Locals and globals
    // ---------------------------------------------------------------------------

    void testLocalsAndTee() {
        ModuleBuilder b;
        Code c;
        c.localGet(0).localTee(1).localGet(1).op(0x6A).end(); // x + x
        const uint32_t f = b.addTypedFunction({I32}, {I32}, c.b, {I32});
        Fixture fx(b);
        for (const Value &v : i32Inputs())
            fx.check(f, {v}, "local.tee");
    }

    void testZeroInitLocals() {
        ModuleBuilder b;
        Code c;
        c.localGet(0).end();
        const uint32_t f = b.addTypedFunction({}, {I32}, c.b, {I32});
        Code d;
        d.localGet(0).end();
        const uint32_t g = b.addTypedFunction({}, {I64}, d.b, {I64});
        Fixture fx(b);
        auto r = fx.check(f, {}, "zero local i32");
        CHECK_EQ(static_cast<uint32_t>(r.wasm[0].lo), 0u);
        CHECK_EQ(r.il[0].lo, 0u);
        r = fx.check(g, {}, "zero local i64");
        CHECK_EQ(r.wasm[0].lo, 0u);
        CHECK_EQ(r.il[0].lo, 0u);
    }

    void testGlobals() {
        ModuleBuilder b;
        b.addGlobal(I32, 5, true); // g0: mutable counter (also the SP convention)
        b.addGlobal(I64, 7, true); // g1
        b.addGlobal(F32, 0, true); // g2
        b.addGlobal(F64, 0, true); // g3
        b.addGlobal(I32, 9, true); // g4: beyond the register bank, uses intrinsics
        Code get0;
        get0.globalGet(0).end();
        const uint32_t fget0 = b.addTypedFunction({}, {I32}, get0.b);
        Code bump0;
        bump0.globalGet(0).i32(1).op(0x6A).globalSet(0).globalGet(0).end();
        const uint32_t fbump0 = b.addTypedFunction({}, {I32}, bump0.b);
        Code set0;
        set0.i32(42).globalSet(0).globalGet(0).end();
        const uint32_t fset0 = b.addTypedFunction({}, {I32}, set0.b);
        Code set0Local;
        set0Local.i32(43).localSet(0).localGet(0).globalSet(0).globalGet(0).end();
        const uint32_t fset0Local = b.addTypedFunction({}, {I32}, set0Local.b, {I32});
        Code get4;
        get4.globalGet(4).end();
        const uint32_t fget4 = b.addTypedFunction({}, {I32}, get4.b);
        Code bump4;
        bump4.globalGet(4).i32(1).op(0x6A).globalSet(4).globalGet(4).end();
        const uint32_t fbump4 = b.addTypedFunction({}, {I32}, bump4.b);
        Code get1;
        get1.globalGet(1).end();
        const uint32_t fget1 = b.addTypedFunction({}, {I64}, get1.b);
        Code bump1;
        bump1.globalGet(1).i64(1).op(0x7C).globalSet(1).globalGet(1).end();
        const uint32_t fbump1 = b.addTypedFunction({}, {I64}, bump1.b);
        Fixture fx(b);
        fx.check(fget0, {}, "global.get 0");
        fx.check(fbump0, {}, "global.set 0");
        fx.check(fset0, {}, "global.set 0 constant");
        fx.check(fset0Local, {}, "global.set 0 via local");
        fx.check(fget4, {}, "global.get 4");
        fx.check(fbump4, {}, "global.set 4");
        fx.check(fget1, {}, "global.get 1");
        fx.check(fbump1, {}, "global.set 1");
    }

    // ---------------------------------------------------------------------------
    // Structured control flow
    // ---------------------------------------------------------------------------

    void testIfElse() {
        ModuleBuilder b;
        Code c;
        c.localGet(0).if_(blockType(I32)).localGet(1).else_().localGet(2).end().end();
        const uint32_t f = b.addTypedFunction({I32, I32, I32}, {I32}, c.b);
        Fixture fx(b);
        for (int cond = 0; cond < 2; ++cond)
            for (int a = -3; a <= 3; ++a)
                for (int bb = -2; bb <= 2; ++bb)
                    fx.check(f, {v32(cond), v32(static_cast<uint32_t>(a)), v32(static_cast<uint32_t>(bb))}, "if/else");
    }

    void testIfElseBrIf() {
        // The true body ends in br_if; its false edge must still skip the else
        // body.  This is the case where the last true-body instruction already
        // has its own branch target.
        ModuleBuilder b;
        Code c;
        c.localGet(0).if_(blockType(I32));
        c.i32(1).localGet(1).brIf(0).drop().i32(3);
        c.else_();
        c.i32(2);
        c.end().end();
        const uint32_t f = b.addTypedFunction({I32, I32}, {I32}, c.b);
        Fixture fx(b);
        const uint32_t expected[2][2] = {{2, 2}, {3, 1}};
        for (int outer = 0; outer < 2; ++outer)
            for (int inner = 0; inner < 2; ++inner) {
                auto r = fx.check(f, {v32(outer), v32(inner)}, "if/else with br_if");
                CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), expected[outer][inner]);
            }
    }

    void testIfEmptyThen() {
        // Empty then-body: the true edge must skip the else body entirely.
        ModuleBuilder b;
        Code c;
        c.localGet(0).if_(-64);
        c.else_();
        c.i32(7).localSet(1);
        c.end();
        c.localGet(1).end();
        const uint32_t f = b.addTypedFunction({I32}, {I32}, c.b, {I32});
        Fixture fx(b);
        auto r = fx.check(f, {v32(1)}, "if empty then, cond true");
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 0u);
        r = fx.check(f, {v32(0)}, "if empty then, cond false");
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 7u);
    }

    void testIfElseBrIfLast() {
        // br_if is the last true-body instruction: its false edge leaves the
        // result on the stack and must skip the else body.
        ModuleBuilder b;
        Code c;
        c.localGet(0).if_(blockType(I32));
        c.i32(1).localGet(1).brIf(0);
        c.else_();
        c.i32(2);
        c.end().end();
        const uint32_t f = b.addTypedFunction({I32, I32}, {I32}, c.b);
        Fixture fx(b);
        const uint32_t expected[2][2] = {{2, 2}, {1, 1}};
        for (int outer = 0; outer < 2; ++outer)
            for (int inner = 0; inner < 2; ++inner) {
                auto r = fx.check(f, {v32(outer), v32(inner)}, "if/else with last br_if");
                CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), expected[outer][inner]);
            }
    }

    void testBlockParams() {
        ModuleBuilder b;
        const uint32_t blockTy = b.addType({I32}, {I32});
        const uint32_t fnTy = b.addType({I32}, {I32});
        Code c;
        c.localGet(0).block(blockTy).i32(1).op(0x6A).end().end();
        const uint32_t f = b.addFunction(fnTy, c.b);
        Fixture fx(b);
        for (uint32_t v : {0u, 1u, 0xFFFFFFFFu}) {
            auto r = fx.check(f, {v32(v)}, "block param");
            CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), v + 1);
        }
    }

    void testMultiValueBlock() {
        ModuleBuilder b;
        const uint32_t blockTy = b.addType({}, {I32, I32});
        const uint32_t fnTy = b.addType({}, {I32, I32});
        Code c;
        c.block(blockTy).i32(11).i32(22).end().end();
        const uint32_t f = b.addFunction(fnTy, c.b);
        Fixture fx(b);
        auto r = fx.check(f, {}, "multi-value block");
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 11u);
        CHECK_EQ(static_cast<uint32_t>(r.il[1].lo), 22u);
    }

    void testBlockBr() {
        ModuleBuilder b;
        Code c;
        c.block(blockType(I32)).i32(7).br(0).i32(9).end().end();
        const uint32_t f = b.addTypedFunction({}, {I32}, c.b);
        Fixture fx(b);
        auto r = fx.check(f, {}, "block/br");
        CHECK_EQ(static_cast<uint32_t>(r.wasm[0].lo), 7u);
    }

    void testBranchMoves() {
        // A branch to an outer block must move the branch argument from the
        // inner slot to the outer label's canonical slot.
        ModuleBuilder b;
        Code c;
        c.block(blockType(I32));       // outer entry 0
        c.i32(100);                    // slot 0
        c.block(blockType(I32));       // inner entry 1
        c.i32(7);                      // slot 1
        c.localGet(0).brIf(1);         // move slot 1 -> outer slot 0
        c.drop().i32(9);
        c.end();
        c.op(0x6A);                    // outer = 100 + inner
        c.end().end();
        const uint32_t f = b.addTypedFunction({I32}, {I32}, c.b);
        Fixture fx(b);
        auto r = fx.check(f, {v32(1)}, "branch moves taken");
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 7u);
        r = fx.check(f, {v32(0)}, "branch moves not taken");
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 109u);
    }

    void testBrIf() {
        ModuleBuilder b;
        Code c;
        c.block(blockType(I32)).i32(7).localGet(0).brIf(0).drop().i32(9).end().end();
        const uint32_t f = b.addTypedFunction({I32}, {I32}, c.b);
        Fixture fx(b);
        for (int cond = 0; cond < 2; ++cond) {
            auto r = fx.check(f, {v32(cond)}, "br_if");
            CHECK_EQ(static_cast<uint32_t>(r.wasm[0].lo), cond ? 7u : 9u);
        }
    }

    void testLoop() {
        ModuleBuilder b;
        Code c;
        c.block(-64);
        c.loop(-64);
        c.localGet(1).localGet(0).op(0x4E);             // i >= n
        c.brIf(1);                                      // exit the outer block
        c.localGet(2).localGet(1).op(0x6A).localSet(2); // s += i
        c.localGet(1).i32(1).op(0x6A).localSet(1);      // i++
        c.br(0);
        c.end();
        c.end();
        c.localGet(2).end();
        // locals: i, s
        const uint32_t f = b.addTypedFunction({I32}, {I32}, c.b, {I32, I32});
        Fixture fx(b);
        for (int n = 0; n <= 20; ++n) {
            auto r = fx.check(f, {v32(static_cast<uint32_t>(n))}, "loop sum");
            const uint32_t expect = static_cast<uint32_t>(n * (n - 1) / 2);
            CHECK_EQ(static_cast<uint32_t>(r.wasm[0].lo), expect);
            CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), expect);
        }
    }

    void testLoopAtStart() {
        // A loop as the first instruction of the body must not re-run the
        // function prologue on its back-edge.  A global counter keeps the
        // loop terminating even if a regression resets the local each time.
        ModuleBuilder b;
        b.addGlobal(I32, 0, true); // g0: shadow stack pointer convention
        b.addGlobal(I32, 0, true); // g1: loop counter
        Code c;
        c.loop(-64);
        c.globalGet(1).i32(1).op(0x6A).globalSet(1);
        c.localGet(0).i32(1).op(0x6A).localSet(0);
        c.globalGet(1).i32(3).op(0x49).brIf(0);
        c.end();
        c.localGet(0).end();
        const uint32_t f = b.addTypedFunction({}, {I32}, c.b, {I32});
        Fixture fx(b);
        auto r = fx.check(f, {}, "loop at function start");
        CHECK_EQ(static_cast<uint32_t>(r.wasm[0].lo), 3u);
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 3u);
    }

    void testBrTable() {
        ModuleBuilder b;
        Code c;
        c.block(-64); // exit
        c.block(-64); // case2
        c.block(-64); // case1
        c.block(-64); // case0
        c.localGet(0).brTable({0, 1, 2}, 3);
        c.end();
        c.i32(10).localSet(1).br(2);
        c.end();
        c.i32(20).localSet(1).br(1);
        c.end();
        c.i32(30).localSet(1).br(0);
        c.end();
        c.localGet(1).end();
        const uint32_t f = b.addTypedFunction({I32}, {I32}, c.b, {I32});
        Fixture fx(b);
        const uint32_t expected[] = {10, 20, 30, 0, 0, 0};
        for (uint32_t x = 0; x < std::size(expected); ++x) {
            auto r = fx.check(f, {v32(x)}, "br_table");
            CHECK_EQ(static_cast<uint32_t>(r.wasm[0].lo), expected[x]);
            CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), expected[x]);
        }
    }

    void testSelect() {
        ModuleBuilder b;
        Code c;
        c.localGet(0).localGet(1).localGet(2).select().end();
        const uint32_t f = b.addTypedFunction({I32, I32, I32}, {I32}, c.b);
        Fixture fx(b);
        for (int cond = 0; cond < 2; ++cond)
            for (int a = 0; a < 4; ++a)
                for (int bb = 0; bb < 4; ++bb)
                    fx.check(f, {v32(a), v32(bb), v32(cond)}, "select");
    }

    void testCall() {
        ModuleBuilder b;
        Code callee;
        callee.localGet(0).localGet(1).op(0x6A).end();
        const uint32_t fadd = b.addTypedFunction({I32, I32}, {I32}, callee.b);
        Code caller;
        caller.localGet(0).localGet(1).call(fadd).end();
        const uint32_t fcall = b.addTypedFunction({I32, I32}, {I32}, caller.b);
        Fixture fx(b);
        for (const Value &a : i32Inputs())
            for (const Value &c : i32Inputs())
                fx.check(fcall, {a, c}, "call add");
    }

    void testCallIndirect() {
        ModuleBuilder b;
        const uint32_t calleeTy = b.addType({I32}, {I32});
        Code f0;
        f0.localGet(0).i32(1).op(0x6A).end();
        const uint32_t f0i = b.addFunction(calleeTy, f0.b);
        Code f1;
        f1.localGet(0).i32(2).op(0x6C).end();
        const uint32_t f1i = b.addFunction(calleeTy, f1.b);
        b.addTable(2, {f0i, f1i});
        const uint32_t callerTy = b.addType({I32, I32}, {I32});
        Code caller;
        caller.localGet(1).localGet(0).callIndirect(calleeTy, 0).end();
        const uint32_t ci = b.addFunction(callerTy, caller.b);
        Code tail;
        tail.localGet(1).localGet(0).returnCallIndirect(calleeTy, 0).end();
        const uint32_t ti = b.addFunction(callerTy, tail.b);
        Fixture fx(b);
        for (uint32_t idx = 0; idx < 2; ++idx)
            for (uint32_t value : {0u, 1u, 9u, 0xFFFFFFFFu}) {
                const uint32_t expect = idx == 0 ? value + 1 : value * 2;
                auto r = fx.check(ci, {v32(idx), v32(value)}, "call_indirect");
                CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), expect);
                r = fx.check(ti, {v32(idx), v32(value)}, "return_call_indirect");
                CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), expect);
            }
    }

    void testCallRef() {
        ModuleBuilder b;
        const uint32_t ty = b.addType({I32}, {I32});
        Code callee;
        callee.localGet(0).i32(5).op(0x6A).end();
        const uint32_t ci = b.addFunction(ty, callee.b);
        Code caller;
        caller.localGet(0).refFunc(ci).op(0x14).uleb(ty).end();
        const uint32_t fi = b.addFunction(ty, caller.b);
        Code tail;
        tail.localGet(0).refFunc(ci).op(0x15).uleb(ty).end();
        const uint32_t ti = b.addFunction(ty, tail.b);
        Fixture fx(b);
        for (uint32_t v : {0u, 7u, 0xFFFFFFF0u}) {
            auto r = fx.check(fi, {v32(v)}, "call_ref");
            CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), v + 5);
            r = fx.check(ti, {v32(v)}, "return_call_ref");
            CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), v + 5);
        }
    }

    void testRecursion() {
        ModuleBuilder b;
        Code c;
        c.localGet(0).op(0x45); // eqz
        c.if_(blockType(I32));
        c.i32(1);
        c.else_();
        c.localGet(0);
        c.localGet(0).i32(1).op(0x6B); // n - 1
        c.call(0);
        c.op(0x6C); // mul
        c.end();
        c.end();
        const uint32_t f = b.addTypedFunction({I32}, {I32}, c.b);
        Fixture fx(b);
        uint32_t fact = 1;
        for (uint32_t n = 0; n <= 10; ++n) {
            if (n)
                fact *= n;
            auto r = fx.check(f, {v32(n)}, "factorial");
            CHECK_EQ(static_cast<uint32_t>(r.wasm[0].lo), fact);
            CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), fact);
        }
    }

    void testSignatureLimits() {
        ModuleBuilder b;
        std::vector<uint8_t> params(20, I32);
        Code c;
        c.localGet(19).end();
        const uint32_t manyParams = b.addTypedFunction(params, {I32}, c.b);
        Code d;
        for (int i = 0; i < 9; ++i)
            d.i32(i);
        d.end();
        const uint32_t manyResults = b.addTypedFunction({}, std::vector<uint8_t>(9, I32), d.b);
        Fixture fx(b);

        const auto stats = [&](uint32_t f) { return BinaryNinja::analyzeIl(fx.funcs.at(*fx.module.functions[f].address)->il); };
        // The lifter models 16 arguments and 8 results; beyond that it must say so
        // rather than silently dropping values.
        CHECK(stats(manyParams).unimplemented > 0);
        CHECK(stats(manyResults).unimplemented > 0);
    }

    void testMultiValue() {
        ModuleBuilder b;
        Code c;
        c.localGet(0).localGet(1).op(0x6A); // sum
        c.localGet(0).localGet(1).op(0x6B); // diff
        c.localGet(0).localGet(1).op(0x6C); // product
        c.end();
        const uint32_t f = b.addTypedFunction({I32, I32}, {I32, I32, I32}, c.b);
        Fixture fx(b);
        auto r = fx.check(f, {v32(7), v32(3)}, "multi-value results");
        CHECK_EQ(r.wasm.size(), 3u);
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 10u);
        CHECK_EQ(static_cast<uint32_t>(r.il[1].lo), 4u);
        CHECK_EQ(static_cast<uint32_t>(r.il[2].lo), 21u);
    }

    void testManyArgs() {
        ModuleBuilder b;
        Code c;
        for (uint32_t i = 0; i < 10; ++i)
            c.localGet(i);
        for (uint32_t i = 0; i < 9; ++i)
            c.op(0x6A);
        c.end();
        std::vector<uint8_t> params(10, I32);
        const uint32_t f = b.addTypedFunction(params, {I32}, c.b);
        Fixture fx(b);
        std::vector<Value> args;
        for (uint32_t i = 0; i < 10; ++i)
            args.push_back(v32(i + 1));
        auto r = fx.check(f, args, "ten arguments");
        CHECK_EQ(static_cast<uint32_t>(r.wasm[0].lo), 55u);
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 55u);
    }

    void testReturnCall() {
        ModuleBuilder b;
        Code callee;
        callee.localGet(0).localGet(1).op(0x6A).end();
        const uint32_t fadd = b.addTypedFunction({I32, I32}, {I32}, callee.b);
        Code caller;
        caller.localGet(0).localGet(1).returnCall(fadd).end();
        const uint32_t fcall = b.addTypedFunction({I32, I32}, {I32}, caller.b);
        Fixture fx(b);
        fx.check(fcall, {v32(11), v32(31)}, "return_call");
    }

    void testV128Transport() {
        ModuleBuilder b;
        Code c;
        c.localGet(0).end();
        const uint32_t f = b.addTypedFunction({V128}, {V128}, c.b);
        Fixture fx(b);
        const Value v{0x1122334455667788ull, 0x99AABBCCDDEEFF00ull};
        auto r = fx.check(f, {v}, "v128 param/result");
        CHECK_EQ(r.wasm[0].lo, v.lo);
        CHECK_EQ(r.wasm[0].hi, v.hi);
        CHECK_EQ(r.il[0].lo, v.lo);
        CHECK_EQ(r.il[0].hi, v.hi);
    }

    void testDeepStack() {
        ModuleBuilder b;
        Code c;
        for (int i = 0; i < 40; ++i)
            c.i32(1);
        for (int i = 0; i < 39; ++i)
            c.op(0x6A);
        c.end();
        const uint32_t f = b.addTypedFunction({}, {I32}, c.b);
        Fixture fx(b);
        auto r = fx.check(f, {}, "deep operand stack");
        CHECK_EQ(static_cast<uint32_t>(r.wasm[0].lo), 40u);
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 40u);
        const auto stats = BinaryNinja::analyzeIl(fx.funcs.at(*fx.module.functions[f].address)->il);
        CHECK(stats.spills > 0);
    }

    void testV128Select() {
        ModuleBuilder b;
        Code c;
        c.localGet(0).localGet(1).localGet(2).select().end();
        const uint32_t f = b.addTypedFunction({V128, V128, I32}, {V128}, c.b);
        Fixture fx(b);
        const Value a{0x1111111111111111ull, 0x2222222222222222ull};
        const Value bb{0x3333333333333333ull, 0x4444444444444444ull};
        for (int cond = 0; cond < 2; ++cond)
            fx.check(f, {a, bb, v32(cond)}, "v128 select");
    }

    void testV128Local() {
        ModuleBuilder b;
        Code c;
        c.localGet(0).localSet(1).localGet(1).end();
        const uint32_t f = b.addTypedFunction({V128}, {V128}, c.b, {V128});
        Fixture fx(b);
        const Value v{0xDEADBEEF01234567ull, 0xCAFEBABE89ABCDEFull};
        fx.check(f, {v}, "v128 local");
    }

    void testSimdSpillShape() {
        // Force both SIMD output paths through the spill area: a deep operand
        // stack means there is no free X/S register slot for the result.
        ModuleBuilder b;
        Code vecOut;
        for (int i = 0; i < 40; ++i)
            vecOut.i32(0);
        vecOut.v128Const(wasmbuild::bytes(16, 0)).v128Const(wasmbuild::bytes(16, 0));
        vecOut.simd(0xAE).drop();  // i32x4.add
        for (int i = 0; i < 40; ++i)
            vecOut.drop();
        vecOut.end();
        const uint32_t fvec = b.addTypedFunction({}, {}, vecOut.b);

        Code scalarOut;
        for (int i = 0; i < 40; ++i)
            scalarOut.i32(0);
        scalarOut.v128Const(wasmbuild::bytes(16, 0));
        scalarOut.simd(0xA3).drop();  // i32x4.all_true
        for (int i = 0; i < 40; ++i)
            scalarOut.drop();
        scalarOut.end();
        const uint32_t fscalar = b.addTypedFunction({}, {}, scalarOut.b);

        Fixture fx(b);
        const auto text = [&](uint32_t f) {
            return BinaryNinja::printIl(fx.funcs.at(*fx.module.functions[f].address)->il);
        };
        const std::string vt = text(fvec);
        CHECK(vt.find("tv0") != std::string::npos);
        CHECK(vt.find("store.u128") != std::string::npos);
        const std::string st = text(fscalar);
        CHECK(st.find("t8") != std::string::npos);
        CHECK(st.find("store.u64") != std::string::npos);
    }

    void testReadability() {
        ModuleBuilder b;
        Code addc;
        addc.localGet(0).localGet(1).op(0x6A).end();
        const uint32_t fadd = b.addTypedFunction({I32, I32}, {I32}, addc.b);
        Code clzc;
        clzc.localGet(0).op(0x67).end();
        const uint32_t fclz = b.addTypedFunction({I32}, {I32}, clzc.b);
        Code memc;
        memc.localGet(0).localGet(1).store(0x36, 4).localGet(0).load(0x28, 4).end();
        const uint32_t fmem = b.addTypedFunction({I32, I32}, {I32}, memc.b);
        Code vecc;
        vecc.localGet(0).end();
        const uint32_t fvec = b.addTypedFunction({V128}, {V128}, vecc.b);
        Code fc;
        fc.f32(1.5f).end();
        const uint32_t ffloat = b.addTypedFunction({}, {F32}, fc.b);
        Fixture fx(b);

        const auto text = [&](uint32_t f) { return BinaryNinja::printIl(fx.funcs.at(*fx.module.functions[f].address)->il); };
        const auto stats = [&](uint32_t f) { return BinaryNinja::analyzeIl(fx.funcs.at(*fx.module.functions[f].address)->il); };

        const std::string addText = text(fadd);
        CHECK(addText.find(" + ") != std::string::npos);
        CHECK(addText.find("__builtin") == std::string::npos);
        CHECK(addText.find("nop") == std::string::npos);
        CHECK_EQ(stats(fadd).intrinsics, 0u);
        CHECK_EQ(stats(fadd).spills, 0u);
        CHECK(stats(fadd).nativeIntArith >= 1);

        // A one-output intrinsic lands in the stack slot directly, with no temp move.
        const std::string clzText = text(fclz);
        CHECK(clzText.find("s0 = __builtin_i32_clz(s0)") != std::string::npos);
        CHECK(clzText.find("t8") == std::string::npos);
        CHECK_EQ(stats(fclz).intrinsics, 1u);

        const auto memStats = stats(fmem);
        CHECK(memStats.loads >= 1);
        CHECK(memStats.stores >= 1);
        CHECK(text(fmem).find("store.u32") != std::string::npos);

        const std::string vecText = text(fvec);
        CHECK(vecText.find("<< 64") != std::string::npos);
        CHECK(vecText.find(">>u 64") != std::string::npos);

        CHECK(text(ffloat).find("f32(1.5)") != std::string::npos);
    }

    void testBrOnNull() {
    ModuleBuilder b;
    Code c;
    c.block(blockType(I32)).i32(0).localGet(0).op(0xD5).uleb(0).drop().drop().i32(1).end().end();
    const uint32_t f = b.addTypedFunction({0x70}, {I32}, c.b);  // funcref param
    Fixture fx(b);
    auto r = fx.check(f, {Value{0, 0}}, "br_on_null null");
    CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 0u);
    r = fx.check(f, {Value{1, 0}}, "br_on_null non-null");
    CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 1u);
}

void testBrOnNonNull() {
    ModuleBuilder b;
    Code c;
    c.block(blockType(0x70)).localGet(0).op(0xD6).uleb(0).refNull().end().refIsNull().end();
    const uint32_t f = b.addTypedFunction({0x70}, {I32}, c.b);
    Fixture fx(b);
    auto r = fx.check(f, {Value{0, 0}}, "br_on_non_null null");
    CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 1u);
    r = fx.check(f, {Value{1, 0}}, "br_on_non_null non-null");
    CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 0u);
}

void testRefs() {
    ModuleBuilder b;
    Code nullc;
    nullc.refNull().refIsNull().end();
    const uint32_t fnull = b.addTypedFunction({}, {I32}, nullc.b);
    Code funcc;
    funcc.refFunc(0).refIsNull().end();
    const uint32_t ffunc = b.addTypedFunction({}, {I32}, funcc.b);
    Code eqc;
    eqc.refFunc(0).refFunc(0).refEq().end();
    const uint32_t feq = b.addTypedFunction({}, {I32}, eqc.b);
    Code neqc;
    neqc.refNull().refFunc(0).refEq().end();
    const uint32_t fneq = b.addTypedFunction({}, {I32}, neqc.b);
    Fixture fx(b);
    auto r = fx.check(fnull, {}, "ref.is_null null");
    CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 1u);
    r = fx.check(ffunc, {}, "ref.is_null func");
    CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 0u);
    r = fx.check(feq, {}, "ref.eq same");
    CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 1u);
    r = fx.check(fneq, {}, "ref.eq null/func");
    CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 0u);
}

void testMalformedInputs() {
    ModuleBuilder b;
    Code c;
    c.localGet(0).i32(1).op(0x6A).end();
    b.addTypedFunction({I32}, {I32}, c.b);
    const auto bytes = b.build();
    for (size_t cut = 0; cut <= bytes.size(); ++cut) {
        std::vector<uint8_t> part(bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(cut));
        (void)wasm::parseModule(std::move(part)); // must not throw or crash
    }
    auto corrupt = bytes;
    if (corrupt.size() > 11) {
        corrupt[8] = 0x0A; // code section id with a bogus size
        corrupt[9] = 0xFF;
        corrupt[10] = 0xFF;
        corrupt[11] = 0xFF;
        (void)wasm::parseModule(std::move(corrupt));
    }
}

void testConstDataPointer() {
        ModuleBuilder b;
        b.setMemoryPages(1);
        b.addData(0x10, {0xDE, 0xAD, 0xBE, 0xEF});
        Code c;
        c.i32(0x10).load(0x28).end();
        const uint32_t f = b.addTypedFunction({}, {I32}, c.b);
        Fixture fx(b);
        auto r = fx.check(f, {}, "data pointer constant");
        CHECK_EQ(static_cast<uint32_t>(r.wasm[0].lo), 0xEFBEADDEu);
    }

    void testCoverageMatrix() {
        const auto r = liftcov::run();
        std::printf("coverage: %zu cases, %zu missing, %zu structural errors\n", r.cases, r.missing.size(), r.errors.size());
        if (!r.missing.empty()) {
            std::fprintf(stderr, "coverage gaps:\n");
            for (const auto &m : r.missing)
                std::fprintf(stderr, "  %s\n", m.c_str());
        }
        for (const auto &e : r.errors)
            std::fprintf(stderr, "coverage error: %s\n", e.c_str());
        CHECK(r.missing.empty());
        CHECK(r.errors.empty());
    }

    // A small golden snapshot makes the emitted IL reviewable and catches
    // readability regressions that the semantic tests cannot express.
    void testGolden() {
        BinaryNinja::resetLabelIds();
        ModuleBuilder b;
        b.setMemoryPages(1);
        std::vector<std::pair<std::string, uint32_t>> funcs;

        {
            Code c;
            c.localGet(0).localGet(1).op(0x6A).end();
            funcs.emplace_back("add", b.addTypedFunction({I32, I32}, {I32}, c.b));
        }
        {
            Code c;
            c.localGet(0).op(0x45).if_(blockType(I32)).i32(1).else_();
            c.localGet(0).localGet(0).i32(1).op(0x6B).call(1).op(0x6C).end().end();
            funcs.emplace_back("factorial", b.addTypedFunction({I32}, {I32}, c.b));
        }
        {
            Code c;
            c.localGet(0).localGet(0).load(0x28, 4).store(0x36, 8);
            c.localGet(0).load(0x28, 8).end();
            funcs.emplace_back("memory", b.addTypedFunction({I32}, {I32}, c.b));
        }
        {
            Code c;
            c.localGet(0).end();
            funcs.emplace_back("v128", b.addTypedFunction({V128}, {V128}, c.b));
        }
        {
            Code c;
            c.localGet(0).op(0x67).end(); // i32.clz (intrinsic)
            funcs.emplace_back("clz", b.addTypedFunction({I32}, {I32}, c.b));
        }
        {
            Code c;
            c.block(blockType(I32)).i32(7).localGet(0).brIf(0).drop().i32(9).end().end();
            funcs.emplace_back("branch", b.addTypedFunction({I32}, {I32}, c.b));
        }
        {
            Code c;
            c.f32(1.5f).end();
            funcs.emplace_back("float", b.addTypedFunction({}, {F32}, c.b));
        }
        {
            Code c;
            c.localGet(0).localGet(1).simd(0xAE).end(); // i32x4.add
            funcs.emplace_back("simd_add", b.addTypedFunction({V128, V128}, {V128}, c.b));
        }
        {
            Code c;
            c.localGet(0).simdLane(0x1B, 0).end(); // i32x4.extract_lane
            funcs.emplace_back("simd_lane", b.addTypedFunction({V128}, {I32}, c.b));
        }

        auto module = parseOrThrow(b);
        lift_test::LiftHarness lift(module);
        std::ostringstream out;
        for (const auto &[name, index] : funcs) {
            const uint64_t start = *module.functions[index].address;
            const auto *fn = lift.tryLift(start);
            CHECK(fn != nullptr);
            out << "=== " << name << " @" << std::hex << start << std::dec << " ===\n";
            out << BinaryNinja::printIl(fn->il) << "\n";
        }

        const std::string path = std::string(WASM_TEST_SOURCE_DIR) + "/tests/golden/lift_snapshots.txt";
        const std::string actual = out.str();
        if (std::getenv("WASM_UPDATE_GOLDEN")) {
            // Text mode normalizes CRLF on Windows checkouts back to '\n'.
            std::ofstream f(path);
            CHECK(f);
            f << actual;
            std::printf("updated %s\n", path.c_str());
            return;
        }
        std::ifstream f(path);
        CHECK(f);
        std::string expected((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (expected == actual)
            return;
        std::istringstream ea(expected), aa(actual);
        std::string el, al;
        size_t line = 0;
        while (true) {
            const bool eok = static_cast<bool>(std::getline(ea, el));
            const bool aok = static_cast<bool>(std::getline(aa, al));
            ++line;
            if (!eok && !aok)
                break;
            if (eok != aok || el != al) {
                std::fprintf(
                    stderr, "golden mismatch at line %zu:\n  expected: %s\n  actual:   %s\n", line, eok ? el.c_str() : "<eof>", aok ? al.c_str() : "<eof>"
                );
                break;
            }
        }
        throw TestFailure("lift snapshot changed; review it and re-run with WASM_UPDATE_GOLDEN=1");
    }

    // Targeted regression fixture: tests/fixtures/lift_issues.wat.  It covers
    // the specific bugs fixed by this harness and the remaining readability
    // checks.  SIMD spilling is structural-only (the oracle does not model the
    // SIMD intrinsics).
    void testLiftIssuesSample() {
        const char *path = std::getenv("WASM_ISSUES_MODULE");
        if (!path)
            return;
        std::ifstream in(path, std::ios::binary);
        CHECK(in);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto m = wasm::parseModule(std::move(bytes));
        CHECK(m.has_value());
        Fixture fx(std::move(*m));

        const auto check = [&](const char *name, const std::vector<Value> &args) {
            return fx.check(fx.indexOf(name), args, std::string("issues ") + name);
        };

        auto r = check("empty_then", {v32(1)});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 0u);
        r = check("empty_then", {v32(0)});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 7u);

        for (int outer = 0; outer < 2; ++outer)
            for (int inner = 0; inner < 2; ++inner) {
                r = check("brif_else", {v32(outer), v32(inner)});
                CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), outer ? 1u : 2u);
            }

        r = check("uninit_local", {});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 0u);
        r = check("g0_set_const", {});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 42u);
        r = check("g0_via_local", {});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 43u);
        r = check("g1_bump", {});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 10u);
        r = check("g2_bump", {});
        CHECK_EQ(r.il[0].lo, 10000000001ull);

        for (const char *name : {"multi3", "call_multi"}) {
            r = check(name, {v32(7), v32(3)});
            CHECK_EQ(r.il.size(), 3u);
            CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 10u);
            CHECK_EQ(static_cast<uint32_t>(r.il[1].lo), 4u);
            CHECK_EQ(static_cast<uint32_t>(r.il[2].lo), 21u);
        }

        {
            std::vector<Value> args;
            for (uint32_t i = 1; i <= 10; ++i)
                args.push_back(v32(i));
            r = check("many_args", args);
            CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 55u);
        }
        r = check("deep_stack", {});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 40u);

        for (int c = 0; c < 2; ++c) {
            r = check("sel", {v32(c), v32(11), v32(22)});
            CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), c ? 11u : 22u);
        }

        r = check("apply_indirect", {v32(0), v32(5)});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 6u);
        r = check("apply_indirect", {v32(1), v32(5)});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 10u);
        r = check("tail_caller", {v32(5)});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 6u);

        const uint32_t addOne = fx.indexOf("add_one");
        r = check("apply_ref", {Value{static_cast<uint64_t>(addOne) + 1, 0}, v32(5)});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 6u);
        r = check("ref_test", {});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 0u);

        r = check("float32_const", {});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), std::bit_cast<uint32_t>(1.5f));
        r = check("float64_const", {});
        CHECK_EQ(r.il[0].lo, std::bit_cast<uint64_t>(3.141592653589793));
        r = check("wrap64", {v64(0x123456789ABCDEFull)});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 0x89ABCDEFu);

        for (uint32_t x = 0; x < 4; ++x) {
            r = check("br_table", {v32(x)});
            const uint32_t expect = x == 0 ? 10 : x == 1 ? 20 : x == 2 ? 30 : 0;
            CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), expect);
        }

        r = check("load_store", {v32(0x40), v32(123)});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 123u);
        r = check("bulk", {});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 0xABABABABu);
        r = check("init", {});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 0x04030201u);

        r = check("block_param", {v32(5)});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 6u);
        r = check("multi_block", {});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 11u);
        CHECK_EQ(static_cast<uint32_t>(r.il[1].lo), 22u);

        const Value vecA{0x1111111111111111ull, 0x2222222222222222ull};
        const Value vecB{0x3333333333333333ull, 0x4444444444444444ull};
        r = check("v128_id", {vecA});
        CHECK_EQ(r.il[0].lo, vecA.lo);
        CHECK_EQ(r.il[0].hi, vecA.hi);
        for (int c = 0; c < 2; ++c) {
            r = check("v128_select", {v32(c), vecA, vecB});
            const Value &expect = c ? vecA : vecB;
            CHECK_EQ(r.il[0].lo, expect.lo);
            CHECK_EQ(r.il[0].hi, expect.hi);
        }

        r = check("clz", {v32(0x80000000)});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 0u);
        r = check("popcnt", {v32(0xFFFFFFFF)});
        CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), 32u);
    }

    // Optional broader clang fixture: recursion, br_table, loops, 64-bit, floats,
    // memory, popcount/clz and select.
    void testComplexSample() {
        const char *path = std::getenv("WASM_COMPLEX_MODULE");
        if (!path)
            return;
        std::ifstream in(path, std::ios::binary);
        CHECK(in);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto m = wasm::parseModule(std::move(bytes));
        CHECK(m.has_value());
        Fixture fx(std::move(*m));

        const uint32_t fib = fx.indexOf("fib");
        for (int n = 0; n <= 12; ++n)
            fx.check(fib, {v32(static_cast<uint32_t>(n))}, "complex fib");

        const uint32_t switchy = fx.indexOf("switchy");
        for (int x : {0, 1, 2, 3, 4, 5, 6, 7, -1, 99})
            fx.check(switchy, {v32(static_cast<uint32_t>(x))}, "complex switchy");

        const uint32_t loops = fx.indexOf("loops");
        for (int n : {0, 1, 3, 4, 10, 21, 22, 40})
            fx.check(loops, {v32(static_cast<uint32_t>(n))}, "complex loops");

        const uint32_t rot = fx.indexOf("rot64");
        for (int n : {0, 1, 7, 31, 63})
            fx.check(rot, {v64(0x0123456789ABCDEFull), v32(static_cast<uint32_t>(n))}, "complex rot64");

        const uint32_t dmath = fx.indexOf("dmath");
        for (const auto &[a, b] : std::vector<std::pair<double, double>>{{1.5, 2.25}, {-3.0, 4.0}, {0.0, 0.5}})
            fx.check(dmath, {vf64(a), vf64(b)}, "complex dmath");

        const uint32_t fmath2 = fx.indexOf("fmath2");
        for (const auto &[a, b] : std::vector<std::pair<float, float>>{{1.5f, 2.0f}, {-3.0f, 0.5f}})
            fx.check(fmath2, {vf32(a), vf32(b)}, "complex fmath2");

        const uint32_t memsum = fx.indexOf("memsum");
        for (int n : {0, 1, 5})
            fx.check(memsum, {v32(0), v32(static_cast<uint32_t>(n))}, "complex memsum");

        const uint32_t bitcount = fx.indexOf("bitcount");
        for (uint32_t x : {0u, 1u, 0xFFFFFFFFu, 0xAAAAAAAAu})
            fx.check(bitcount, {v32(x)}, "complex bitcount");

        const uint32_t clz = fx.indexOf("clz");
        for (uint32_t x : {0u, 1u, 0x80000000u, 0x12345678u})
            fx.check(clz, {v32(x)}, "complex clz");

        const uint32_t mixed = fx.indexOf("mixed");
        fx.check(mixed, {v32(0xF0F0F0F0), v32(0x0F0F0F0F), v32(0x12345678)}, "complex mixed");

        const uint32_t sel = fx.indexOf("sel");
        for (int c = 0; c < 2; ++c)
            fx.check(sel, {v32(c), v32(11), v32(22)}, "complex sel");

        const uint32_t f2i = fx.indexOf("f2i");
        for (float f : {0.0f, 1.5f, -2.5f, 1e10f, -1e10f})
            fx.check(f2i, {vf32(f)}, "complex f2i");

        const uint32_t i2f = fx.indexOf("i2f");
        for (int32_t i : {0, 1, -1, 16777217, -16777217})
            fx.check(i2f, {v32(static_cast<uint32_t>(i))}, "complex i2f");

        const uint32_t udiv = fx.indexOf("udiv");
        for (const auto &[a, b] : std::vector<std::pair<uint32_t, uint32_t>>{{100, 7}, {0, 3}, {0xFFFFFFFFu, 1}})
            fx.check(udiv, {v32(a), v32(b)}, "complex udiv");

        const uint32_t bigcmp = fx.indexOf("bigcmp");
        for (const auto &[a, b] : std::vector<std::pair<int64_t, int64_t>>{{-1, 1}, {1, -1}, {5, 5}, {0, -100}})
            fx.check(bigcmp, {v64(static_cast<uint64_t>(a)), v64(static_cast<uint64_t>(b))}, "complex bigcmp");

        const uint32_t apply = fx.indexOf("apply");
        const uint32_t addOne = fx.tableIndexOf("add_one");
        const uint32_t doubleIt = fx.tableIndexOf("double_it");
        fx.check(apply, {v32(addOne), v32(5)}, "complex apply add_one");
        fx.check(apply, {v32(doubleIt), v32(5)}, "complex apply double_it");

        const uint32_t useApply = fx.indexOf("use_apply");
        for (uint32_t which = 0; which < 4; ++which)
            fx.check(useApply, {v32(which), v32(5)}, "complex use_apply");

        const uint32_t stacky = fx.indexOf("stacky");
        for (int n : {0, 1, 2, 5, 10}) {
            auto r = fx.check(stacky, {v32(static_cast<uint32_t>(n))}, "complex stacky");
            CHECK_EQ(static_cast<uint32_t>(r.il[0].lo), static_cast<uint32_t>(496 * n));
        }
    }

    // Optional real-module test.  Point WASM_TEST_MODULE at a clang-produced .wasm
    // with the sample functions (see tests/fixtures/sample.c).
    void testRealSample() {
        const char *path = std::getenv("WASM_TEST_MODULE");
        if (!path)
            return;
        std::ifstream in(path, std::ios::binary);
        CHECK(in);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto m = wasm::parseModule(std::move(bytes));
        CHECK(m.has_value());
        Fixture fx(std::move(*m));

        const uint32_t add = fx.indexOf("add");
        for (const auto &[a, b] : std::vector<std::pair<int, int>>{{0, 0}, {7, 35}, {-1, 1}, {100, -50}})
            fx.check(add, {v32(static_cast<uint32_t>(a)), v32(static_cast<uint32_t>(b))}, "sample add");

        const uint32_t sum = fx.indexOf("sum_to");
        for (int n : {0, 1, 2, 5, 20})
            fx.check(sum, {v32(static_cast<uint32_t>(n))}, "sample sum_to");

        const uint32_t branchy = fx.indexOf("branchy");
        for (int x : {0, 1, 2, 3, 4, -1})
            fx.check(branchy, {v32(static_cast<uint32_t>(x))}, "sample branchy");

        const uint32_t memtest = fx.indexOf("memtest");
        for (int n : {0, 1, 3})
            fx.check(memtest, {v32(0), v32(static_cast<uint32_t>(n))}, "sample memtest");

        const uint32_t big = fx.indexOf("big");
        for (const auto &[a, b] : std::vector<std::pair<int64_t, int64_t>>{{1, 2}, {100, 7}, {-5, 3}})
            fx.check(big, {v64(static_cast<uint64_t>(a)), v64(static_cast<uint64_t>(b))}, "sample big");

        const uint32_t fmath = fx.indexOf("fmath");
        for (const auto &[a, b] : std::vector<std::pair<float, float>>{{1.5f, 2.0f}, {-3.0f, 0.5f}})
            fx.check(fmath, {vf32(a), vf32(b)}, "sample fmath");

        const uint32_t ctors = fx.indexOf("__wasm_call_ctors");
        fx.check(ctors, {}, "sample ctors");
    }

} // namespace

int main(int argc, char **argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    struct Case {
        const char *name;
        std::function<void()> fn;
    };
    const std::vector<Case> cases = {
        {"arithmetic_sanity", testArithmeticSanity},
        {"numeric_i32", testNumericI32},
        {"numeric_i64", testNumericI64},
        {"numeric_f32", testNumericF32},
        {"numeric_f64", testNumericF64},
        {"minmax_semantics", testMinMaxSemantics},
        {"conversions", testConversions},
        {"trunc_sat", testTruncSat},
        {"memory_loads", testMemoryLoads},
        {"memory_stores", testMemoryStores},
        {"bulk_memory", testBulkMemory},
        {"memory_size_grow", testMemorySizeGrow},
        {"locals_tee", testLocalsAndTee},
        {"zero_init_locals", testZeroInitLocals},
        {"globals", testGlobals},
        {"if_else", testIfElse},
        {"if_empty_then", testIfEmptyThen},
        {"if_else_br_if", testIfElseBrIf},
        {"if_else_br_if_last", testIfElseBrIfLast},
        {"block_params", testBlockParams},
        {"multi_value_block", testMultiValueBlock},
        {"block_br", testBlockBr},
        {"branch_moves", testBranchMoves},
        {"br_if", testBrIf},
        {"loop", testLoop},
        {"loop_at_start", testLoopAtStart},
        {"br_table", testBrTable},
        {"select", testSelect},
        {"call", testCall},
        {"call_indirect", testCallIndirect},
        {"call_ref", testCallRef},
        {"recursion", testRecursion},
        {"return_call", testReturnCall},
        {"signature_limits", testSignatureLimits},
        {"multi_value", testMultiValue},
        {"many_args", testManyArgs},
        {"v128_transport", testV128Transport},
        {"v128_select", testV128Select},
        {"v128_local", testV128Local},
        {"deep_stack", testDeepStack},
        {"simd_spill_shape", testSimdSpillShape},
        {"readability", testReadability},
        {"br_on_null", testBrOnNull},
        {"br_on_non_null", testBrOnNonNull},
        {"refs", testRefs},
        {"malformed_inputs", testMalformedInputs},
        {"const_data_pointer", testConstDataPointer},
        {"coverage", testCoverageMatrix},
        {"golden", testGolden},
        {"real_sample", testRealSample},
        {"issues_sample", testLiftIssuesSample},
        {"complex_sample", testComplexSample},
    };
    int failed = 0;
    for (const auto &c : cases) {
        if (!filter.empty() && std::strstr(c.name, filter.c_str()) == nullptr)
            continue;
        try {
            c.fn();
            std::printf("PASS %s\n", c.name);
        } catch (const std::exception &e) {
            std::printf("FAIL %s: %s\n", c.name, e.what());
            ++failed;
        }
    }
    std::printf("%d failed\n", failed);
    return failed == 0 ? 0 : 1;
}
