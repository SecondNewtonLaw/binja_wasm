//
// Interpreter for the headless partial IL.
//
// The goal is not decompiler-grade fidelity; it is to execute exactly the
// expression tree the production lifter emits so arithmetic, memory, calls and
// control flow can be compared against the independent WASM interpreter.
//
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "bn/intrinsics.hpp"
#include "bn/regs.hpp"
#include "lift_runner.hpp"
#include "wasm_interp.hpp"

namespace lift_test {

    using wasmtest::Value;

    struct IlTrap : std::runtime_error {
        using std::runtime_error::runtime_error;
    };
    struct IlUnsupported : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    class IlInterpreter {
      public:
        static constexpr uint64_t kSpillBase = 0xE0000000ull;
        // Spills live below the virtual stack pointer, so accept a window rather
        // than only addresses above the base.
        static constexpr uint64_t kSpillLow = 0xD0000000ull;

        IlInterpreter(const wasm::Module &module, const std::unordered_map<uint64_t, const LiftedFunction *> &functions) : m_(module), funcs_(functions) {
            init();
        }

        std::vector<Value> call(uint64_t start, const std::vector<Value> &args) {
            const auto it = funcs_.find(start);
            if (it == funcs_.end())
                throw IlTrap("function not lifted");
            const uint32_t index = m_.addrIndex.at(start);
            const wasm::FuncType *sig = m_.funcSig(index);
            if (!sig)
                throw IlTrap("missing signature");
            if (args.size() != sig->params.size())
                throw IlTrap("argument count");

            setArgs(sig, args);
            writeReg(bnwasm::regId(bnwasm::Bank::Sp), 4, Value{kSpillBase, 0});
            runFunction(*it->second, *sig, 0);
            syncGlobals();
            return readResults(*sig);
        }

        // Exposed for tests that want to inspect state after a call.
        std::vector<uint8_t> memory;
        std::vector<Value> globals;
        uint64_t pages = 1;

      private:
        void init() {
            uint64_t dataEnd = 0;
            for (const auto &s : m_.dataSegments)
                dataEnd = std::max(dataEnd, s.memAddr + s.size);
            pages = std::max<uint64_t>(m_.memoryPages ? m_.memoryPages : 1, (dataEnd + 65535) / 65536);
            memory.assign(pages * 65536, 0);
            for (const auto &s : m_.dataSegments) {
                if (s.fileOffset + s.size <= m_.data.size())
                    std::memcpy(memory.data() + s.memAddr, m_.data.data() + s.fileOffset, s.size);
            }
            globals.resize(m_.globalTypes.size());
            for (size_t i = 0; i < globals.size(); ++i) {
                globals[i] = Value{m_.globalInits[i], 0};
                if (i < bnwasm::kNGlobals) {
                    const uint8_t t = m_.globalTypes[i];
                    if (t == wasm::kV128) {
                        writeReg(bnwasm::regId(bnwasm::Bank::Gv, static_cast<uint32_t>(i)), 16, globals[i]);
                    } else {
                        const size_t width = (t == wasm::kI32 || t == wasm::kF32) ? 4 : 8;
                        writeReg(bnwasm::regId(bnwasm::Bank::G, static_cast<uint32_t>(i)), width, globals[i]);
                    }
                }
            }
        }

        // --- register file ---------------------------------------------------
        static const bnwasm::RegDef *regDef(uint32_t id) {
            const auto &regs = bnwasm::allRegs();
            return id >= 1 && id <= regs.size() ? &regs[id - 1] : nullptr;
        }

        static uint32_t fullReg(uint32_t id) {
            const auto *d = regDef(id);
            return d ? d->full : id;
        }

        Value readReg(uint32_t id, size_t size) const {
            const uint32_t full = fullReg(id);
            const auto it = regs_.find(full);
            Value v = it == regs_.end() ? Value{} : it->second;
            v.lo = mask(v.lo, size);
            if (size < 16)
                v.hi = 0;
            return v;
        }

        void writeReg(uint32_t id, size_t size, Value v) {
            const uint32_t full = fullReg(id);
            v.lo = mask(v.lo, size);
            if (size < 16)
                v.hi = 0;
            regs_[full] = v;
        }

        static uint64_t mask(uint64_t v, size_t size) {
            if (size >= 8)
                return v;
            return v & ((1ull << (size * 8)) - 1);
        }

        // --- memory ----------------------------------------------------------
        static bool isSpillAddress(uint64_t addr) { return addr >= kSpillLow && addr < kSpillBase + 0x10000000ull; }
        uint8_t read8(uint64_t addr) {
            if (isSpillAddress(addr))
                return spill_[addr];
            if (addr >= memory.size())
                throw IlTrap("out-of-bounds IL load");
            return memory[static_cast<size_t>(addr)];
        }
        void write8(uint64_t addr, uint8_t v) {
            if (isSpillAddress(addr)) {
                spill_[addr] = v;
                return;
            }
            if (addr >= memory.size())
                throw IlTrap("out-of-bounds IL store");
            memory[static_cast<size_t>(addr)] = v;
        }
        Value readMem(uint64_t addr, size_t size) {
            Value v;
            for (size_t i = 0; i < std::min<size_t>(size, 8); ++i)
                v.lo |= static_cast<uint64_t>(read8(addr + i)) << (8 * i);
            if (size > 8)
                for (size_t i = 8; i < size; ++i)
                    v.hi |= static_cast<uint64_t>(read8(addr + i)) << (8 * (i - 8));
            return v;
        }
        void writeMem(uint64_t addr, Value v, size_t size) {
            for (size_t i = 0; i < std::min<size_t>(size, 8); ++i)
                write8(addr + i, static_cast<uint8_t>(v.lo >> (8 * i)));
            if (size > 8)
                for (size_t i = 8; i < size; ++i)
                    write8(addr + i, static_cast<uint8_t>(v.hi >> (8 * (i - 8))));
        }

        // --- function calls --------------------------------------------------
        void setArgs(const wasm::FuncType *sig, const std::vector<Value> &args) {
            for (size_t k = 0; k < args.size() && k < bnwasm::kNArgs; ++k) {
                if (sig->params[k] == wasm::kV128) {
                    writeReg(bnwasm::regId(bnwasm::Bank::A, static_cast<uint32_t>(k)), 8, args[k]);
                    writeReg(bnwasm::regId(bnwasm::Bank::Ah, static_cast<uint32_t>(k)), 8, Value{args[k].hi, 0});
                } else {
                    writeReg(bnwasm::regId(bnwasm::Bank::A, static_cast<uint32_t>(k)), 8, args[k]);
                }
            }
        }

        void syncGlobals() {
            for (size_t i = 0; i < globals.size() && i < bnwasm::kNGlobals; ++i) {
                const uint8_t t = m_.globalTypes[i];
                if (t == wasm::kV128)
                    globals[i] = readReg(bnwasm::regId(bnwasm::Bank::Gv, static_cast<uint32_t>(i)), 16);
                else
                    globals[i] = readReg(bnwasm::regId(bnwasm::Bank::G, static_cast<uint32_t>(i)), (t == wasm::kI32 || t == wasm::kF32) ? 4 : 8);
            }
        }

        std::vector<Value> readResults(const wasm::FuncType &sig) {
            std::vector<Value> out;
            for (size_t k = 0; k < sig.results.size() && k < bnwasm::kNResults; ++k) {
                if (sig.results[k] == wasm::kV128) {
                    const Value lo = readReg(bnwasm::regId(bnwasm::Bank::R, static_cast<uint32_t>(k)), 8);
                    const Value hi = readReg(bnwasm::regId(bnwasm::Bank::Rh, static_cast<uint32_t>(k)), 8);
                    out.push_back({lo.lo, hi.lo});
                } else {
                    const size_t width = (sig.results[k] == wasm::kI32 || sig.results[k] == wasm::kF32) ? 4 : 8;
                    out.push_back(readReg(bnwasm::regId(bnwasm::Bank::R, static_cast<uint32_t>(k)), width));
                }
            }
            return out;
        }

        void callFunction(uint64_t dest, int depth) {
            if (depth > 256)
                throw IlTrap("IL call depth");
            if (dest >= bnwasm::kExternBase)
                throw IlUnsupported("import call");
            if (dest < bnwasm::kCodeBase)
                throw IlTrap("call target outside code");
            const uint64_t start = dest - bnwasm::kCodeBase;
            const auto it = funcs_.find(start);
            if (it == funcs_.end())
                throw IlTrap("call target not lifted");
            const uint32_t index = m_.addrIndex.at(start);
            const wasm::FuncType *sig = m_.funcSig(index);
            if (!sig)
                throw IlTrap("callee signature");

            std::vector<Value> args;
            for (size_t k = 0; k < sig->params.size() && k < bnwasm::kNArgs; ++k) {
                if (sig->params[k] == wasm::kV128) {
                    const Value lo = readReg(bnwasm::regId(bnwasm::Bank::A, static_cast<uint32_t>(k)), 8);
                    const Value hi = readReg(bnwasm::regId(bnwasm::Bank::Ah, static_cast<uint32_t>(k)), 8);
                    args.push_back({lo.lo, hi.lo});
                } else {
                    args.push_back(readReg(bnwasm::regId(bnwasm::Bank::A, static_cast<uint32_t>(k)), 8));
                }
            }

            const auto saved = saveNonInterface();
            const Value savedSp = readReg(bnwasm::regId(bnwasm::Bank::Sp), 4);
            writeReg(bnwasm::regId(bnwasm::Bank::Sp), 4, Value{(savedSp.lo - 0x1000ull * static_cast<uint64_t>(depth + 1)) & 0xFFFFFFFFull, 0});
            runFunction(*it->second, *sig, depth + 1);
            restoreNonInterface(saved);
            writeReg(bnwasm::regId(bnwasm::Bank::Sp), 4, savedSp);
            (void)args;
        }

        struct SavedRegs {
            std::vector<std::pair<uint32_t, Value>> regs;
        };
        SavedRegs saveNonInterface() const {
            SavedRegs s;
            const auto saveBank = [&](bnwasm::Bank b, uint32_t n) {
                for (uint32_t i = 0; i < n; ++i) {
                    const uint32_t id = bnwasm::regId(b, i);
                    auto it = regs_.find(fullReg(id));
                    s.regs.emplace_back(fullReg(id), it == regs_.end() ? Value{} : it->second);
                }
            };
            saveBank(bnwasm::Bank::L, bnwasm::kNLocals);
            saveBank(bnwasm::Bank::Lv, bnwasm::kNLocals);
            saveBank(bnwasm::Bank::S, bnwasm::kNStack);
            saveBank(bnwasm::Bank::X, bnwasm::kNStack);
            saveBank(bnwasm::Bank::Tv, bnwasm::kNSimdTemps);
            return s;
        }
        void restoreNonInterface(const SavedRegs &s) {
            for (const auto &[id, v] : s.regs)
                regs_[id] = v;
        }

        // --- execution -------------------------------------------------------
        void runFunction(const LiftedFunction &fn, const wasm::FuncType &sig, int depth) {
            const auto &items = fn.il.items();
            size_t pc = 0;
            uint64_t steps = 0;
            while (pc < items.size()) {
                if (++steps > 5'000'000)
                    throw IlTrap("IL step limit");
                const BinaryNinja::Item &item = items[pc];
                if (item.kind == BinaryNinja::Item::Kind::Label) {
                    ++pc;
                    continue;
                }
                const BinaryNinja::Expr &e = fn.il.expr(item.expr);
                switch (e.op) {
                case BinaryNinja::ILOp::SetRegister:
                    writeReg(e.reg, e.size, eval(fn, e.kids[0]));
                    ++pc;
                    break;
                case BinaryNinja::ILOp::Store:
                    writeMem(eval(fn, e.kids[0]).lo, eval(fn, e.kids[1]), e.size);
                    ++pc;
                    break;
                case BinaryNinja::ILOp::Call:
                    callFunction(eval(fn, e.kids[0]).lo, depth);
                    ++pc;
                    break;
                case BinaryNinja::ILOp::Jump: {
                    const uint64_t dest = eval(fn, e.kids[0]).lo;
                    const uint32_t id = fn.il.AddressLabelId(dest);
                    if (!id || !fn.il.HasLabel(id))
                        throw IlTrap("jump target has no label");
                    pc = fn.il.LabelPosition(id);
                    break;
                }
                case BinaryNinja::ILOp::Goto:
                    pc = fn.il.LabelPosition(e.labels[0]);
                    break;
                case BinaryNinja::ILOp::If: {
                    const bool cond = eval(fn, e.kids[0]).lo != 0;
                    pc = fn.il.LabelPosition(e.labels[cond ? 0 : 1]);
                    break;
                }
                case BinaryNinja::ILOp::Return:
                    return;
                case BinaryNinja::ILOp::NoReturn:
                    throw IlTrap("no_return");
                case BinaryNinja::ILOp::Nop:
                    ++pc;
                    break;
                case BinaryNinja::ILOp::Unimplemented:
                    throw IlTrap("unimplemented IL");
                case BinaryNinja::ILOp::Intrinsic:
                    executeIntrinsic(fn, e);
                    ++pc;
                    break;
                default:
                    throw IlTrap("bare expression in instruction stream");
                }
            }
            throw IlTrap("fell off the end of the IL function");
        }

        Value eval(const LiftedFunction &fn, BinaryNinja::ExprId id) {
            const auto &il = fn.il;
            const BinaryNinja::Expr &e = il.expr(id);
            const auto kid = [&](size_t n) { return eval(fn, e.kids[n]); };
            const auto kidSize = [&](size_t n) { return il.expr(e.kids[n]).size; };
            switch (e.op) {
            case BinaryNinja::ILOp::Const:
            case BinaryNinja::ILOp::FloatConst:
            case BinaryNinja::ILOp::ConstPointer:
                return {e.imm, 0};
            case BinaryNinja::ILOp::Register:
                return readReg(e.reg, e.size);
            case BinaryNinja::ILOp::SetRegister: {
                const Value v = kid(0);
                writeReg(e.reg, e.size, v);
                return v;
            }
            case BinaryNinja::ILOp::Load: {
                const Value addr = kid(0);
                return readMem(addr.lo, e.size);
            }
            case BinaryNinja::ILOp::Store: {
                const Value addr = kid(0);
                const Value v = kid(1);
                writeMem(addr.lo, v, e.size);
                return {};
            }
            case BinaryNinja::ILOp::Add:
                return bin(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::Sub:
                return bin(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::Mult:
                return bin(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::DivSigned:
                return bin(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::DivUnsigned:
                return bin(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::ModSigned:
                return bin(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::ModUnsigned:
                return bin(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::And:
                return bin(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::Or:
                return bin(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::Xor:
                return bin(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::ShiftLeft:
                return bin(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::ArithShiftRight:
                return bin(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::LogicalShiftRight:
                return bin(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::RotateLeft:
                return bin(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::RotateRight:
                return bin(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::CompareEqual:
                return cmp(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::CompareNotEqual:
                return cmp(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::CompareSignedLessThan:
                return cmp(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::CompareUnsignedLessThan:
                return cmp(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::CompareSignedGreaterThan:
                return cmp(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::CompareUnsignedGreaterThan:
                return cmp(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::CompareSignedLessEqual:
                return cmp(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::CompareUnsignedLessEqual:
                return cmp(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::CompareSignedGreaterEqual:
                return cmp(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::CompareUnsignedGreaterEqual:
                return cmp(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::BoolToInt:
                return Value{kid(0).lo != 0 ? 1ull : 0ull, 0};
            case BinaryNinja::ILOp::SignExtend: {
                const Value v = kid(0);
                return signExtend(v, kidSize(0), e.size);
            }
            case BinaryNinja::ILOp::ZeroExtend: {
                const Value v = kid(0);
                return zeroExtend(v, kidSize(0), e.size);
            }
            case BinaryNinja::ILOp::LowPart: {
                const Value v = kid(0);
                Value r = v;
                r.lo = mask(v.lo, e.size);
                if (e.size < 8)
                    r.hi = 0;
                return r;
            }
            case BinaryNinja::ILOp::FloatAbs:
            case BinaryNinja::ILOp::FloatNeg:
            case BinaryNinja::ILOp::FloatTrunc:
            case BinaryNinja::ILOp::FloatSqrt:
                return floatUnary(e.size, kid(0), e.op);
            case BinaryNinja::ILOp::FloatAdd:
            case BinaryNinja::ILOp::FloatSub:
            case BinaryNinja::ILOp::FloatMult:
            case BinaryNinja::ILOp::FloatDiv:
                return floatBinary(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::FloatCompareEqual:
            case BinaryNinja::ILOp::FloatCompareNotEqual:
            case BinaryNinja::ILOp::FloatCompareLessThan:
            case BinaryNinja::ILOp::FloatCompareGreaterThan:
            case BinaryNinja::ILOp::FloatCompareLessEqual:
            case BinaryNinja::ILOp::FloatCompareGreaterEqual:
                return floatCompare(e.size, kid(0), kid(1), e.op);
            case BinaryNinja::ILOp::FloatToInt:
                return floatToInt(kid(0), kidSize(0), e.size);
            case BinaryNinja::ILOp::IntToFloat:
                return intToFloat(kid(0), kidSize(0), e.size);
            case BinaryNinja::ILOp::FloatConvert:
                return floatConvert(kid(0), kidSize(0), e.size);
            default:
                throw IlTrap("expression cannot be evaluated: op " + std::to_string(static_cast<int>(e.op)));
            }
        }

        Value bin(size_t size, Value a, Value b, BinaryNinja::ILOp op) {
            const uint64_t m = mask(~0ull, size);
            uint64_t r = 0;
            if (size == 16) {
                // Only the joins/splits emitted for v128 values use 16-byte operators.
                if (op == BinaryNinja::ILOp::Or) {
                    return {a.lo | b.lo, a.hi | b.hi};
                }
                if (op == BinaryNinja::ILOp::And) {
                    return {a.lo & b.lo, a.hi & b.hi};
                }
                if (op == BinaryNinja::ILOp::Xor) {
                    return {a.lo ^ b.lo, a.hi ^ b.hi};
                }
                const unsigned n = static_cast<unsigned>(b.lo & 127);
                if (op == BinaryNinja::ILOp::ShiftLeft) {
                    if (n >= 64)
                        return {0, (a.lo << (n - 64)) & ~0ull};
                    return {(a.lo << n) & ~0ull, ((a.hi << n) | (n ? (a.lo >> (64 - n)) : 0)) & ~0ull};
                }
                if (op == BinaryNinja::ILOp::LogicalShiftRight) {
                    if (n >= 64)
                        return {(a.hi >> (n - 64)) & ~0ull, 0};
                    return {((a.lo >> n) | (n ? (a.hi << (64 - n)) : 0)) & ~0ull, (a.hi >> n) & ~0ull};
                }
                throw IlUnsupported("16-byte arithmetic");
            }
            switch (op) {
            case BinaryNinja::ILOp::Add:
                r = a.lo + b.lo;
                break;
            case BinaryNinja::ILOp::Sub:
                r = a.lo - b.lo;
                break;
            case BinaryNinja::ILOp::Mult:
                r = a.lo * b.lo;
                break;
            case BinaryNinja::ILOp::DivSigned: {
                if (size == 4) {
                    const int32_t x = static_cast<int32_t>(a.lo), y = static_cast<int32_t>(b.lo);
                    if (!y || (x == std::numeric_limits<int32_t>::min() && y == -1))
                        throw IlTrap("IL i32.div_s");
                    r = static_cast<uint32_t>(x / y);
                } else {
                    const int64_t x = static_cast<int64_t>(a.lo), y = static_cast<int64_t>(b.lo);
                    if (!y || (x == std::numeric_limits<int64_t>::min() && y == -1))
                        throw IlTrap("IL i64.div_s");
                    r = static_cast<uint64_t>(x / y);
                }
                break;
            }
            case BinaryNinja::ILOp::DivUnsigned:
                if (!b.lo)
                    throw IlTrap("IL div by zero");
                r = a.lo / b.lo;
                break;
            case BinaryNinja::ILOp::ModSigned: {
                if (size == 4) {
                    const int32_t x = static_cast<int32_t>(a.lo), y = static_cast<int32_t>(b.lo);
                    if (!y || (x == std::numeric_limits<int32_t>::min() && y == -1))
                        throw IlTrap("IL i32.rem_s");
                    r = static_cast<uint32_t>(x % y);
                } else {
                    const int64_t x = static_cast<int64_t>(a.lo), y = static_cast<int64_t>(b.lo);
                    if (!y || (x == std::numeric_limits<int64_t>::min() && y == -1))
                        throw IlTrap("IL i64.rem_s");
                    r = static_cast<uint64_t>(x % y);
                }
                break;
            }
            case BinaryNinja::ILOp::ModUnsigned:
                if (!b.lo)
                    throw IlTrap("IL rem by zero");
                r = a.lo % b.lo;
                break;
            case BinaryNinja::ILOp::And:
                r = a.lo & b.lo;
                break;
            case BinaryNinja::ILOp::Or:
                r = a.lo | b.lo;
                break;
            case BinaryNinja::ILOp::Xor:
                r = a.lo ^ b.lo;
                break;
            case BinaryNinja::ILOp::ShiftLeft:
                r = a.lo << (b.lo & (size * 8 - 1));
                break;
            case BinaryNinja::ILOp::ArithShiftRight:
                if (size == 4)
                    r = static_cast<uint32_t>(static_cast<int32_t>(a.lo) >> (b.lo & 31));
                else
                    r = static_cast<uint64_t>(static_cast<int64_t>(a.lo) >> (b.lo & 63));
                break;
            case BinaryNinja::ILOp::LogicalShiftRight:
                r = a.lo >> (b.lo & (size * 8 - 1));
                break;
            case BinaryNinja::ILOp::RotateLeft:
                r = size == 4 ? std::rotl(static_cast<uint32_t>(a.lo), static_cast<int>(b.lo & 31)) : std::rotl(a.lo, static_cast<int>(b.lo & 63));
                break;
            case BinaryNinja::ILOp::RotateRight:
                r = size == 4 ? std::rotr(static_cast<uint32_t>(a.lo), static_cast<int>(b.lo & 31)) : std::rotr(a.lo, static_cast<int>(b.lo & 63));
                break;
            default:
                throw IlTrap("bad binary op");
            }
            return {r & m, 0};
        }

        static Value cmp(size_t size, Value a, Value b, BinaryNinja::ILOp op) {
            bool r = false;
            if (size == 4) {
                const uint32_t x = static_cast<uint32_t>(a.lo), y = static_cast<uint32_t>(b.lo);
                const int32_t sx = static_cast<int32_t>(x), sy = static_cast<int32_t>(y);
                switch (op) {
                case BinaryNinja::ILOp::CompareEqual:
                    r = x == y;
                    break;
                case BinaryNinja::ILOp::CompareNotEqual:
                    r = x != y;
                    break;
                case BinaryNinja::ILOp::CompareSignedLessThan:
                    r = sx < sy;
                    break;
                case BinaryNinja::ILOp::CompareUnsignedLessThan:
                    r = x < y;
                    break;
                case BinaryNinja::ILOp::CompareSignedGreaterThan:
                    r = sx > sy;
                    break;
                case BinaryNinja::ILOp::CompareUnsignedGreaterThan:
                    r = x > y;
                    break;
                case BinaryNinja::ILOp::CompareSignedLessEqual:
                    r = sx <= sy;
                    break;
                case BinaryNinja::ILOp::CompareUnsignedLessEqual:
                    r = x <= y;
                    break;
                case BinaryNinja::ILOp::CompareSignedGreaterEqual:
                    r = sx >= sy;
                    break;
                case BinaryNinja::ILOp::CompareUnsignedGreaterEqual:
                    r = x >= y;
                    break;
                default:
                    throw IlTrap("bad compare");
                }
            } else {
                const uint64_t x = a.lo, y = b.lo;
                const int64_t sx = static_cast<int64_t>(x), sy = static_cast<int64_t>(y);
                switch (op) {
                case BinaryNinja::ILOp::CompareEqual:
                    r = x == y;
                    break;
                case BinaryNinja::ILOp::CompareNotEqual:
                    r = x != y;
                    break;
                case BinaryNinja::ILOp::CompareSignedLessThan:
                    r = sx < sy;
                    break;
                case BinaryNinja::ILOp::CompareUnsignedLessThan:
                    r = x < y;
                    break;
                case BinaryNinja::ILOp::CompareSignedGreaterThan:
                    r = sx > sy;
                    break;
                case BinaryNinja::ILOp::CompareUnsignedGreaterThan:
                    r = x > y;
                    break;
                case BinaryNinja::ILOp::CompareSignedLessEqual:
                    r = sx <= sy;
                    break;
                case BinaryNinja::ILOp::CompareUnsignedLessEqual:
                    r = x <= y;
                    break;
                case BinaryNinja::ILOp::CompareSignedGreaterEqual:
                    r = sx >= sy;
                    break;
                case BinaryNinja::ILOp::CompareUnsignedGreaterEqual:
                    r = x >= y;
                    break;
                default:
                    throw IlTrap("bad compare");
                }
            }
            return {r ? 1ull : 0ull, 0};
        }

        static Value signExtend(Value v, size_t from, size_t to) {
            const uint64_t x = mask(v.lo, from);
            const bool neg = from < 8 && (x & (1ull << (from * 8 - 1)));
            Value r{x, 0};
            if (neg && to > from) {
                r.lo = x | (~0ull << (from * 8));
            }
            r.lo = mask(r.lo, to);
            return r;
        }
        static Value zeroExtend(Value v, size_t from, size_t to) {
            Value r = v;
            r.lo = mask(v.lo, from);
            if (to < 16 || from <= 8)
                r.hi = 0;
            return r;
        }

        static float asF32(Value v) { return std::bit_cast<float>(static_cast<uint32_t>(v.lo)); }
        static double asF64(Value v) { return std::bit_cast<double>(v.lo); }

        static Value floatUnary(size_t size, Value a, BinaryNinja::ILOp op) {
            if (size == 4) {
                const float x = asF32(a);
                float r = x;
                switch (op) {
                case BinaryNinja::ILOp::FloatAbs:
                    r = std::fabs(x);
                    break;
                case BinaryNinja::ILOp::FloatNeg:
                    r = -x;
                    break;
                case BinaryNinja::ILOp::FloatTrunc:
                    r = std::trunc(x);
                    break;
                case BinaryNinja::ILOp::FloatSqrt:
                    r = std::sqrt(x);
                    break;
                default:
                    throw IlTrap("bad float unary");
                }
                return {std::bit_cast<uint32_t>(r), 0};
            }
            const double x = asF64(a);
            double r = x;
            switch (op) {
            case BinaryNinja::ILOp::FloatAbs:
                r = std::fabs(x);
                break;
            case BinaryNinja::ILOp::FloatNeg:
                r = -x;
                break;
            case BinaryNinja::ILOp::FloatTrunc:
                r = std::trunc(x);
                break;
            case BinaryNinja::ILOp::FloatSqrt:
                r = std::sqrt(x);
                break;
            default:
                throw IlTrap("bad float unary");
            }
            return {std::bit_cast<uint64_t>(r), 0};
        }

        static Value floatBinary(size_t size, Value a, Value b, BinaryNinja::ILOp op) {
            if (size == 4) {
                const float x = asF32(a), y = asF32(b);
                float r = 0;
                switch (op) {
                case BinaryNinja::ILOp::FloatAdd:
                    r = x + y;
                    break;
                case BinaryNinja::ILOp::FloatSub:
                    r = x - y;
                    break;
                case BinaryNinja::ILOp::FloatMult:
                    r = x * y;
                    break;
                case BinaryNinja::ILOp::FloatDiv:
                    r = x / y;
                    break;
                default:
                    throw IlTrap("bad float binary");
                }
                return {std::bit_cast<uint32_t>(r), 0};
            }
            const double x = asF64(a), y = asF64(b);
            double r = 0;
            switch (op) {
            case BinaryNinja::ILOp::FloatAdd:
                r = x + y;
                break;
            case BinaryNinja::ILOp::FloatSub:
                r = x - y;
                break;
            case BinaryNinja::ILOp::FloatMult:
                r = x * y;
                break;
            case BinaryNinja::ILOp::FloatDiv:
                r = x / y;
                break;
            default:
                throw IlTrap("bad float binary");
            }
            return {std::bit_cast<uint64_t>(r), 0};
        }

        static Value floatCompare(size_t size, Value a, Value b, BinaryNinja::ILOp op) {
            bool r = false;
            if (size == 4) {
                const float x = asF32(a), y = asF32(b);
                switch (op) {
                case BinaryNinja::ILOp::FloatCompareEqual:
                    r = x == y;
                    break;
                case BinaryNinja::ILOp::FloatCompareNotEqual:
                    r = x != y;
                    break;
                case BinaryNinja::ILOp::FloatCompareLessThan:
                    r = x < y;
                    break;
                case BinaryNinja::ILOp::FloatCompareGreaterThan:
                    r = x > y;
                    break;
                case BinaryNinja::ILOp::FloatCompareLessEqual:
                    r = x <= y;
                    break;
                case BinaryNinja::ILOp::FloatCompareGreaterEqual:
                    r = x >= y;
                    break;
                default:
                    throw IlTrap("bad float compare");
                }
            } else {
                const double x = asF64(a), y = asF64(b);
                switch (op) {
                case BinaryNinja::ILOp::FloatCompareEqual:
                    r = x == y;
                    break;
                case BinaryNinja::ILOp::FloatCompareNotEqual:
                    r = x != y;
                    break;
                case BinaryNinja::ILOp::FloatCompareLessThan:
                    r = x < y;
                    break;
                case BinaryNinja::ILOp::FloatCompareGreaterThan:
                    r = x > y;
                    break;
                case BinaryNinja::ILOp::FloatCompareLessEqual:
                    r = x <= y;
                    break;
                case BinaryNinja::ILOp::FloatCompareGreaterEqual:
                    r = x >= y;
                    break;
                default:
                    throw IlTrap("bad float compare");
                }
            }
            return {r ? 1ull : 0ull, 0};
        }

        static Value floatToInt(Value v, size_t from, size_t to) {
            const bool src32 = from == 4;
            int64_t r = 0;
            if (src32)
                r = static_cast<int64_t>(std::trunc(asF32(v)));
            else
                r = static_cast<int64_t>(std::trunc(asF64(v)));
            return {to == 4 ? static_cast<uint32_t>(static_cast<int32_t>(r)) : static_cast<uint64_t>(r), 0};
        }

        static Value intToFloat(Value v, size_t from, size_t to) {
            if (to == 4) {
                const float f = from == 4 ? static_cast<float>(static_cast<int32_t>(v.lo)) : static_cast<float>(static_cast<int64_t>(v.lo));
                return {std::bit_cast<uint32_t>(f), 0};
            }
            const double d = from == 4 ? static_cast<double>(static_cast<int32_t>(v.lo)) : static_cast<double>(static_cast<int64_t>(v.lo));
            return {std::bit_cast<uint64_t>(d), 0};
        }

        static Value floatConvert(Value v, size_t from, size_t to) {
            if (from == 4 && to == 8)
                return {std::bit_cast<uint64_t>(static_cast<double>(asF32(v))), 0};
            if (from == 8 && to == 4)
                return {std::bit_cast<uint32_t>(static_cast<float>(asF64(v))), 0};
            return v;
        }

        // --- intrinsics ------------------------------------------------------
        void executeIntrinsic(const LiftedFunction &fn, const BinaryNinja::Expr &e) {
            const auto &defs = bnwasm::intrinsics();
            if (e.imm >= defs.size())
                throw IlTrap("unknown intrinsic");
            const std::string &name = defs[e.imm].name;
            std::vector<Value> p;
            p.reserve(e.kids.size());
            for (BinaryNinja::ExprId k : e.kids)
                p.push_back(eval(fn, k));
            const auto out = [&](size_t k, const Value &v) {
                if (k >= e.outs.size())
                    throw IlTrap("intrinsic output count");
                const size_t size = (k == 0 && defs[e.imm].vecOut) ? 16 : 8;
                writeReg(e.outs[k].index, size, v);
            };
            const auto need = [&](size_t n) {
                if (p.size() < n)
                    throw IlTrap("intrinsic operand count");
            };

            if (name == "__builtin_memory_size") {
                out(0, {pages, 0});
                return;
            }
            if (name == "__builtin_memory_grow") {
                // params are [memory index, delta]
                need(2);
                const uint64_t old = pages;
                pages += p[1].lo;
                memory.resize(pages * 65536, 0);
                out(0, {old, 0});
                return;
            }
            if (name == "__builtin_memory_copy") {
                need(3);
                std::memmove(memory.data() + p[0].lo, memory.data() + p[1].lo, p[2].lo);
                return;
            }
            if (name == "__builtin_memory_fill") {
                need(3);
                std::memset(memory.data() + p[0].lo, static_cast<int>(p[1].lo), p[2].lo);
                return;
            }
            if (name == "__builtin_memory_init") {
                need(4);
                const uint64_t dataIdx = p[0].lo;
                if (dataIdx >= m_.dataSegments.size())
                    throw IlTrap("data segment index");
                const auto &seg = m_.dataSegments[dataIdx];
                if (p[2].lo + p[3].lo > seg.size)
                    throw IlTrap("memory.init range");
                std::memcpy(memory.data() + p[1].lo, m_.data.data() + seg.fileOffset + p[2].lo, p[3].lo);
                return;
            }
            if (name == "__builtin_data_drop" || name == "__builtin_elem_drop")
                return;
            if (name == "__builtin_i32_clz") {
                need(1);
                out(0, {static_cast<uint64_t>(std::countl_zero(static_cast<uint32_t>(p[0].lo))), 0});
                return;
            }
            if (name == "__builtin_i32_ctz") {
                need(1);
                out(0, {static_cast<uint64_t>(std::countr_zero(static_cast<uint32_t>(p[0].lo))), 0});
                return;
            }
            if (name == "__builtin_i32_popcnt") {
                need(1);
                out(0, {static_cast<uint64_t>(std::popcount(static_cast<uint32_t>(p[0].lo))), 0});
                return;
            }
            if (name == "__builtin_i64_clz") {
                need(1);
                out(0, {static_cast<uint64_t>(std::countl_zero(p[0].lo)), 0});
                return;
            }
            if (name == "__builtin_i64_ctz") {
                need(1);
                out(0, {static_cast<uint64_t>(std::countr_zero(p[0].lo)), 0});
                return;
            }
            if (name == "__builtin_i64_popcnt") {
                need(1);
                out(0, {static_cast<uint64_t>(std::popcount(p[0].lo)), 0});
                return;
            }
            if (name == "__builtin_f32_ceil" || name == "__builtin_f32_floor" || name == "__builtin_f32_nearest") {
                need(1);
                const float x = asF32(p[0]);
                const float r = name.ends_with("ceil") ? std::ceil(x) : name.ends_with("floor") ? std::floor(x) : std::nearbyint(x);
                out(0, {std::bit_cast<uint32_t>(r), 0});
                return;
            }
            if (name == "__builtin_f64_ceil" || name == "__builtin_f64_floor" || name == "__builtin_f64_nearest") {
                need(1);
                const double x = asF64(p[0]);
                const double r = name.ends_with("ceil") ? std::ceil(x) : name.ends_with("floor") ? std::floor(x) : std::nearbyint(x);
                out(0, {std::bit_cast<uint64_t>(r), 0});
                return;
            }
            if (name == "__builtin_f32_min" || name == "__builtin_f32_max" || name == "__builtin_f32_copysign") {
                need(2);
                const float x = asF32(p[0]), y = asF32(p[1]);
                const float r = name.ends_with("min")   ? (std::isnan(x) || std::isnan(y) ? std::numeric_limits<float>::quiet_NaN() : std::fmin(x, y))
                                : name.ends_with("max") ? (std::isnan(x) || std::isnan(y) ? std::numeric_limits<float>::quiet_NaN() : std::fmax(x, y))
                                                        : std::copysign(x, y);
                out(0, {std::bit_cast<uint32_t>(r), 0});
                return;
            }
            if (name == "__builtin_f64_min" || name == "__builtin_f64_max" || name == "__builtin_f64_copysign") {
                need(2);
                const double x = asF64(p[0]), y = asF64(p[1]);
                const double r = name.ends_with("min")   ? (std::isnan(x) || std::isnan(y) ? std::numeric_limits<double>::quiet_NaN() : std::fmin(x, y))
                                 : name.ends_with("max") ? (std::isnan(x) || std::isnan(y) ? std::numeric_limits<double>::quiet_NaN() : std::fmax(x, y))
                                                         : std::copysign(x, y);
                out(0, {std::bit_cast<uint64_t>(r), 0});
                return;
            }
            if (name == "__builtin_i32_trunc_f32_u" || name == "__builtin_i32_trunc_f64_u") {
                need(1);
                const double x = name.ends_with("f32_u") ? static_cast<double>(asF32(p[0])) : asF64(p[0]);
                out(0, {static_cast<uint32_t>(std::trunc(x)), 0});
                return;
            }
            if (name == "__builtin_i64_trunc_f32_u" || name == "__builtin_i64_trunc_f64_u") {
                need(1);
                const double x = name.ends_with("f32_u") ? static_cast<double>(asF32(p[0])) : asF64(p[0]);
                out(0, {static_cast<uint64_t>(std::trunc(x)), 0});
                return;
            }
            if (name == "__builtin_f32_convert_i32_u" || name == "__builtin_f32_convert_i64_u") {
                need(1);
                const float f = name.ends_with("i32_u") ? static_cast<float>(static_cast<uint32_t>(p[0].lo)) : static_cast<float>(p[0].lo);
                out(0, {std::bit_cast<uint32_t>(f), 0});
                return;
            }
            if (name == "__builtin_f64_convert_i32_u" || name == "__builtin_f64_convert_i64_u") {
                need(1);
                const double d = name.ends_with("i32_u") ? static_cast<double>(static_cast<uint32_t>(p[0].lo)) : static_cast<double>(p[0].lo);
                out(0, {std::bit_cast<uint64_t>(d), 0});
                return;
            }
            // trunc_sat: saturating float-to-int conversions.
            if (name.rfind("__builtin_i32_trunc_sat_", 0) == 0 || name.rfind("__builtin_i64_trunc_sat_", 0) == 0) {
                need(1);
                const bool dst32 = name.rfind("__builtin_i32_", 0) == 0;
                const bool src32 = name.find("f32") != std::string::npos;
                const bool unsig = name.find("_u") != std::string::npos;
                const double x = src32 ? static_cast<double>(asF32(p[0])) : asF64(p[0]);
                if (dst32) {
                    int64_t r;
                    if (unsig) {
                        const double t = std::trunc(x);
                        r = std::isnan(t) || t <= 0 ? 0 : t >= 4294967295.0 ? 4294967295ll : static_cast<int64_t>(t);
                    } else {
                        const double t = std::trunc(x);
                        r = std::isnan(t)        ? 0
                            : t <= -2147483648.0 ? std::numeric_limits<int32_t>::min()
                            : t >= 2147483647.0  ? std::numeric_limits<int32_t>::max()
                                                 : static_cast<int64_t>(t);
                    }
                    out(0, {static_cast<uint32_t>(r), 0});
                } else {
                    int64_t r;
                    if (unsig) {
                        const double t = std::trunc(x);
                        r = std::isnan(t) || t <= 0 ? 0 : t >= 18446744073709551615.0 ? -1ll : static_cast<int64_t>(t);
                    } else {
                        const double t = std::trunc(x);
                        r = std::isnan(t)                 ? 0
                            : t <= -9223372036854775808.0 ? std::numeric_limits<int64_t>::min()
                            : t >= 9223372036854775807.0  ? std::numeric_limits<int64_t>::max()
                                                          : static_cast<int64_t>(t);
                    }
                    out(0, {static_cast<uint64_t>(r), 0});
                }
                return;
            }
            if (name == "__builtin_table_target") {
                need(2);
                const uint64_t tableIdx = p[0].lo;
                const uint64_t elem = p[1].lo;
                if (tableIdx != 0 || elem >= m_.table.size() || m_.table[elem] == wasm::kNoFunc)
                    throw IlTrap("table_target null/out-of-range");
                out(0, {bnwasm::funcAddress(m_, m_.table[elem]), 0});
                return;
            }
            if (name == "__builtin_global_get" || name == "__builtin_global_set") {
                need(1);
                const size_t i = static_cast<size_t>(p[0].lo);
                if (i >= globals.size())
                    throw IlTrap("global index");
                if (name.ends_with("_get")) {
                    out(0, globals[i]);
                } else {
                    need(2);
                    globals[i] = p[1];
                }
                return;
            }
            throw IlUnsupported("intrinsic " + name);
        }

        const wasm::Module &m_;
        const std::unordered_map<uint64_t, const LiftedFunction *> &funcs_;
        std::unordered_map<uint32_t, Value> regs_;
        std::unordered_map<uint64_t, uint8_t> spill_;
    };

} // namespace lift_test
