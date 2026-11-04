//
// A small, self-contained stand-in for the subset of Binary Ninja's LowLevelIL
// API that the WASM lifter uses.
//
// The point is to run the *real* src/bn/lifter.cpp without Binary Ninja, so a
// test can inspect the emitted IL, evaluate it, and compare it against a WASM
// reference interpreter.  Only the API surface the lifter touches is modelled;
// the expression tree and the evaluation live in this repository.
//
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bn/intrinsics.hpp"
#include "bn/regs.hpp"

namespace BinaryNinja {

    using ExprId = size_t;

    enum class ILOp : uint16_t {
        Const,
        FloatConst,
        ConstPointer,
        Register,
        SetRegister,
        Load,
        Store,
        Add,
        Sub,
        Mult,
        DivSigned,
        DivUnsigned,
        ModSigned,
        ModUnsigned,
        And,
        Or,
        Xor,
        ShiftLeft,
        ArithShiftRight,
        LogicalShiftRight,
        RotateLeft,
        RotateRight,
        CompareEqual,
        CompareNotEqual,
        CompareSignedLessThan,
        CompareUnsignedLessThan,
        CompareSignedGreaterThan,
        CompareUnsignedGreaterThan,
        CompareSignedLessEqual,
        CompareUnsignedLessEqual,
        CompareSignedGreaterEqual,
        CompareUnsignedGreaterEqual,
        BoolToInt,
        SignExtend,
        ZeroExtend,
        LowPart,
        FloatAbs,
        FloatNeg,
        FloatTrunc,
        FloatSqrt,
        FloatAdd,
        FloatSub,
        FloatMult,
        FloatDiv,
        FloatCompareEqual,
        FloatCompareNotEqual,
        FloatCompareLessThan,
        FloatCompareGreaterThan,
        FloatCompareLessEqual,
        FloatCompareGreaterEqual,
        FloatToInt,
        IntToFloat,
        FloatConvert,
        Jump,
        Call,
        Goto,
        If,
        Return,
        NoReturn,
        Nop,
        Unimplemented,
        Intrinsic,
    };

    // Binary Ninja keeps these in lowlevelilinstruction.h; the lifter includes it.
    struct RegisterOrFlag {
        bool isFlag = false;
        uint32_t index = 0;

        RegisterOrFlag() = default;
        RegisterOrFlag(bool flag, uint32_t i) : isFlag(flag), index(i) {}

        bool IsRegister() const { return !isFlag; }
        bool IsFlag() const { return isFlag; }
        uint32_t GetRegister() const { return index; }
        uint32_t GetFlag() const { return index; }

        static RegisterOrFlag Register(uint32_t reg) { return {false, reg}; }
        static RegisterOrFlag Flag(uint32_t flag) { return {true, flag}; }

        bool operator==(const RegisterOrFlag &o) const { return isFlag == o.isFlag && index == o.index; }
        bool operator!=(const RegisterOrFlag &o) const { return !(*this == o); }
    };

    struct BNLowLevelILLabel {
        uint32_t id = 0;
    };

    inline std::atomic<uint32_t> &labelIdCounter() {
        static std::atomic<uint32_t> next{1};
        return next;
    }

    inline uint32_t nextLabelId() { return labelIdCounter().fetch_add(1, std::memory_order_relaxed); }

    // Golden tests want stable local-label names regardless of earlier tests.
    inline void resetLabelIds(uint32_t next = 1) { labelIdCounter().store(next, std::memory_order_relaxed); }

    struct LowLevelILLabel : BNLowLevelILLabel {
        LowLevelILLabel() { id = nextLabelId(); }
    };

    struct Expr {
        ILOp op = ILOp::Nop;
        size_t size = 0;  // result width (operand width for comparisons)
        uint64_t imm = 0; // constant, register id, intrinsic id
        uint32_t reg = 0; // register for Register/SetRegister
        std::vector<ExprId> kids;
        std::vector<RegisterOrFlag> outs; // intrinsic output registers
        std::vector<uint32_t> labels;     // goto/if label ids
    };

    struct Item {
        enum class Kind { Label, Instruction } kind = Kind::Instruction;
        ExprId expr = 0;
        uint32_t label = 0;
        uint64_t addr = 0; // file offset of the WASM instruction
    };

    class Architecture {};

    class LowLevelILFunction {
      public:
        // --- construction ---------------------------------------------------
        ExprId AddInstruction(ExprId expr) {
            items_.push_back({Item::Kind::Instruction, expr, 0, currentAddr_});
            return expr;
        }

        ExprId Const(size_t size, uint64_t val) { return make(ILOp::Const, size, val); }
        ExprId FloatConstRaw(size_t size, uint64_t val) { return make(ILOp::FloatConst, size, val); }
        ExprId ConstPointer(size_t size, uint64_t val) { return make(ILOp::ConstPointer, size, val); }

        ExprId Register(size_t size, uint32_t reg) { return make(ILOp::Register, size, 0, {}, reg); }
        ExprId SetRegister(size_t size, uint32_t reg, ExprId val) { return make(ILOp::SetRegister, size, 0, {val}, reg); }

        ExprId Load(size_t size, ExprId addr) { return make(ILOp::Load, size, 0, {addr}); }
        ExprId Store(size_t size, ExprId addr, ExprId val) { return make(ILOp::Store, size, 0, {addr, val}); }

        ExprId Add(size_t size, ExprId a, ExprId b) { return binary(ILOp::Add, size, a, b); }
        ExprId Sub(size_t size, ExprId a, ExprId b) { return binary(ILOp::Sub, size, a, b); }
        ExprId Mult(size_t size, ExprId a, ExprId b) { return binary(ILOp::Mult, size, a, b); }
        ExprId DivSigned(size_t size, ExprId a, ExprId b) { return binary(ILOp::DivSigned, size, a, b); }
        ExprId DivUnsigned(size_t size, ExprId a, ExprId b) { return binary(ILOp::DivUnsigned, size, a, b); }
        ExprId ModSigned(size_t size, ExprId a, ExprId b) { return binary(ILOp::ModSigned, size, a, b); }
        ExprId ModUnsigned(size_t size, ExprId a, ExprId b) { return binary(ILOp::ModUnsigned, size, a, b); }
        ExprId And(size_t size, ExprId a, ExprId b) { return binary(ILOp::And, size, a, b); }
        ExprId Or(size_t size, ExprId a, ExprId b) { return binary(ILOp::Or, size, a, b); }
        ExprId Xor(size_t size, ExprId a, ExprId b) { return binary(ILOp::Xor, size, a, b); }
        ExprId ShiftLeft(size_t size, ExprId a, ExprId b) { return binary(ILOp::ShiftLeft, size, a, b); }
        ExprId ArithShiftRight(size_t size, ExprId a, ExprId b) { return binary(ILOp::ArithShiftRight, size, a, b); }
        ExprId LogicalShiftRight(size_t size, ExprId a, ExprId b) { return binary(ILOp::LogicalShiftRight, size, a, b); }
        ExprId RotateLeft(size_t size, ExprId a, ExprId b) { return binary(ILOp::RotateLeft, size, a, b); }
        ExprId RotateRight(size_t size, ExprId a, ExprId b) { return binary(ILOp::RotateRight, size, a, b); }

        ExprId CompareEqual(size_t size, ExprId a, ExprId b) { return binary(ILOp::CompareEqual, size, a, b); }
        ExprId CompareNotEqual(size_t size, ExprId a, ExprId b) { return binary(ILOp::CompareNotEqual, size, a, b); }
        ExprId CompareSignedLessThan(size_t size, ExprId a, ExprId b) { return binary(ILOp::CompareSignedLessThan, size, a, b); }
        ExprId CompareUnsignedLessThan(size_t size, ExprId a, ExprId b) { return binary(ILOp::CompareUnsignedLessThan, size, a, b); }
        ExprId CompareSignedGreaterThan(size_t size, ExprId a, ExprId b) { return binary(ILOp::CompareSignedGreaterThan, size, a, b); }
        ExprId CompareUnsignedGreaterThan(size_t size, ExprId a, ExprId b) { return binary(ILOp::CompareUnsignedGreaterThan, size, a, b); }
        ExprId CompareSignedLessEqual(size_t size, ExprId a, ExprId b) { return binary(ILOp::CompareSignedLessEqual, size, a, b); }
        ExprId CompareUnsignedLessEqual(size_t size, ExprId a, ExprId b) { return binary(ILOp::CompareUnsignedLessEqual, size, a, b); }
        ExprId CompareSignedGreaterEqual(size_t size, ExprId a, ExprId b) { return binary(ILOp::CompareSignedGreaterEqual, size, a, b); }
        ExprId CompareUnsignedGreaterEqual(size_t size, ExprId a, ExprId b) { return binary(ILOp::CompareUnsignedGreaterEqual, size, a, b); }

        ExprId BoolToInt(size_t size, ExprId a) { return unary(ILOp::BoolToInt, size, a); }
        ExprId SignExtend(size_t size, ExprId a) { return unary(ILOp::SignExtend, size, a); }
        ExprId ZeroExtend(size_t size, ExprId a) { return unary(ILOp::ZeroExtend, size, a); }
        ExprId LowPart(size_t size, ExprId a) { return unary(ILOp::LowPart, size, a); }

        ExprId FloatAbs(size_t size, ExprId a) { return unary(ILOp::FloatAbs, size, a); }
        ExprId FloatNeg(size_t size, ExprId a) { return unary(ILOp::FloatNeg, size, a); }
        ExprId FloatTrunc(size_t size, ExprId a) { return unary(ILOp::FloatTrunc, size, a); }
        ExprId FloatSqrt(size_t size, ExprId a) { return unary(ILOp::FloatSqrt, size, a); }
        ExprId FloatAdd(size_t size, ExprId a, ExprId b) { return binary(ILOp::FloatAdd, size, a, b); }
        ExprId FloatSub(size_t size, ExprId a, ExprId b) { return binary(ILOp::FloatSub, size, a, b); }
        ExprId FloatMult(size_t size, ExprId a, ExprId b) { return binary(ILOp::FloatMult, size, a, b); }
        ExprId FloatDiv(size_t size, ExprId a, ExprId b) { return binary(ILOp::FloatDiv, size, a, b); }
        ExprId FloatCompareEqual(size_t size, ExprId a, ExprId b) { return binary(ILOp::FloatCompareEqual, size, a, b); }
        ExprId FloatCompareNotEqual(size_t size, ExprId a, ExprId b) { return binary(ILOp::FloatCompareNotEqual, size, a, b); }
        ExprId FloatCompareLessThan(size_t size, ExprId a, ExprId b) { return binary(ILOp::FloatCompareLessThan, size, a, b); }
        ExprId FloatCompareGreaterThan(size_t size, ExprId a, ExprId b) { return binary(ILOp::FloatCompareGreaterThan, size, a, b); }
        ExprId FloatCompareLessEqual(size_t size, ExprId a, ExprId b) { return binary(ILOp::FloatCompareLessEqual, size, a, b); }
        ExprId FloatCompareGreaterEqual(size_t size, ExprId a, ExprId b) { return binary(ILOp::FloatCompareGreaterEqual, size, a, b); }
        ExprId FloatToInt(size_t size, ExprId a) { return unary(ILOp::FloatToInt, size, a); }
        ExprId IntToFloat(size_t size, ExprId a) { return unary(ILOp::IntToFloat, size, a); }
        ExprId FloatConvert(size_t size, ExprId a) { return unary(ILOp::FloatConvert, size, a); }

        ExprId Jump(ExprId dest) { return unary(ILOp::Jump, 0, dest); }
        ExprId Call(ExprId dest) { return unary(ILOp::Call, 0, dest); }
        ExprId Goto(BNLowLevelILLabel &label) {
            ExprId e = make(ILOp::Goto, 0, 0);
            exprs_[e - 1].labels.push_back(label.id);
            return e;
        }
        ExprId If(ExprId cond, BNLowLevelILLabel &t, BNLowLevelILLabel &f) {
            ExprId e = make(ILOp::If, 0, 0, {cond});
            exprs_[e - 1].labels = {t.id, f.id};
            return e;
        }
        ExprId Return(ExprId dest) { return unary(ILOp::Return, 0, dest); }
        ExprId NoReturn() { return make(ILOp::NoReturn, 0, 0); }
        ExprId Nop() { return make(ILOp::Nop, 0, 0); }
        ExprId Unimplemented() { return make(ILOp::Unimplemented, 0, 0); }

        ExprId Intrinsic(const std::vector<RegisterOrFlag> &outputs, uint32_t intrinsic, const std::vector<ExprId> &params) {
            ExprId e = make(ILOp::Intrinsic, 0, intrinsic, params);
            exprs_[e - 1].outs = outputs;
            return e;
        }

        void MarkLabel(BNLowLevelILLabel &label) {
            if (!label.id)
                label.id = nextLabelId();
            if (labelPos_.count(label.id))
                return; // BN would reject a duplicate; tests are happy with idempotence
            Item item;
            item.kind = Item::Kind::Label;
            item.label = label.id;
            item.addr = currentAddr_;
            items_.push_back(item);
            labelPos_[label.id] = items_.size() - 1;
        }

        BNLowLevelILLabel *GetLabelForAddress(Architecture *, uint64_t addr) {
            if (!validAddrs_.empty() && !validAddrs_.count(addr))
                return nullptr;
            auto [it, inserted] = addrLabels_.try_emplace(addr);
            if (inserted) {
                it->second.id = nextLabelId();
                labelNames_[it->second.id] = "loc_" + hex(addr);
                labelAddr_[it->second.id] = addr;
            }
            return &it->second;
        }

        // --- harness-only surface -------------------------------------------
        // Every instruction lifted for one WASM address is bracketed by
        // BeginInstruction so address labels land before the first IL instruction.
        void BeginInstruction(uint64_t bnAddr, uint64_t fileAddr) {
            currentAddr_ = fileAddr;
            if (!validAddrs_.empty() && !validAddrs_.count(bnAddr))
                return;
            auto [it, inserted] = addrLabels_.try_emplace(bnAddr);
            if (inserted) {
                it->second.id = nextLabelId();
                labelNames_[it->second.id] = "loc_" + hex(bnAddr);
                labelAddr_[it->second.id] = bnAddr;
            }
            if (!labelPos_.count(it->second.id)) {
                Item item;
                item.kind = Item::Kind::Label;
                item.label = it->second.id;
                item.addr = fileAddr;
                items_.push_back(item);
                labelPos_[it->second.id] = items_.size() - 1;
            }
        }

        void SetInstructionAddresses(const std::vector<uint64_t> &addrs) {
            validAddrs_.clear();
            validAddrs_.insert(addrs.begin(), addrs.end());
        }

        const std::vector<Expr> &exprs() const { return exprs_; }
        const std::vector<Item> &items() const { return items_; }
        const Expr &expr(ExprId id) const {
            if (id == 0 || id > exprs_.size())
                throw std::out_of_range("bad ExprId");
            return exprs_[id - 1];
        }

        uint32_t AddressLabelId(uint64_t addr) const {
            auto it = addrLabels_.find(addr);
            return it == addrLabels_.end() ? 0 : it->second.id;
        }
        bool HasLabel(uint32_t id) const { return labelPos_.count(id) != 0; }
        size_t LabelPosition(uint32_t id) const {
            auto it = labelPos_.find(id);
            if (it == labelPos_.end())
                throw std::out_of_range("unmarked label");
            return it->second;
        }
        uint64_t LabelAddress(uint32_t id) const {
            auto it = labelAddr_.find(id);
            return it == labelAddr_.end() ? ~0ull : it->second;
        }
        std::string LabelName(uint32_t id) const {
            auto it = labelNames_.find(id);
            if (it != labelNames_.end())
                return it->second;
            return "L" + std::to_string(id);
        }
        bool IsAddressLabel(uint32_t id) const { return labelAddr_.count(id) != 0; }

      private:
        ExprId make(ILOp op, size_t size, uint64_t imm, std::vector<ExprId> kids = {}, uint32_t reg = 0) {
            Expr e;
            e.op = op;
            e.size = size;
            e.imm = imm;
            e.reg = reg;
            e.kids = std::move(kids);
            exprs_.push_back(std::move(e));
            return exprs_.size();
        }

        ExprId unary(ILOp op, size_t size, ExprId a) { return make(op, size, 0, {a}); }
        ExprId binary(ILOp op, size_t size, ExprId a, ExprId b) { return make(op, size, 0, {a, b}); }

        static std::string hex(uint64_t v) {
            std::ostringstream os;
            os << std::hex << v;
            return os.str();
        }

        std::vector<Expr> exprs_;
        std::vector<Item> items_;
        std::map<uint64_t, BNLowLevelILLabel> addrLabels_;
        std::unordered_map<uint32_t, size_t> labelPos_;
        std::unordered_map<uint32_t, uint64_t> labelAddr_;
        std::unordered_map<uint32_t, std::string> labelNames_;
        std::set<uint64_t> validAddrs_;
        uint64_t currentAddr_ = 0;
    };

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

    inline const char *regName(uint32_t id) {
        static thread_local std::string scratch;
        const auto &regs = bnwasm::allRegs();
        if (id >= 1 && id <= regs.size())
            return regs[id - 1].name.c_str();
        if (id & 0x80000000u) {
            scratch = "t" + std::to_string(id & 0x7FFFFFFFu);
            return scratch.c_str();
        }
        scratch = "r" + std::to_string(id);
        return scratch.c_str();
    }

    inline std::string sizeSuffix(size_t size) {
        switch (size) {
        case 1:
            return "u8";
        case 2:
            return "u16";
        case 4:
            return "u32";
        case 8:
            return "u64";
        case 16:
            return "u128";
        default:
            return "u" + std::to_string(size * 8);
        }
    }

    inline std::string fmtConst(uint64_t v) {
        std::ostringstream os;
        if (v <= 9)
            os << v;
        else if (v <= 0xFFF)
            os << v;
        else {
            os << "0x" << std::hex << v;
        }
        return os.str();
    }

    inline std::string fmtConst(uint64_t v, size_t size) {
        // i32/i64 immediates read better as signed values when the sign bit is set;
        // this matches the way the WASM source spells -1, -2, ...
        if (size == 4) {
            const int32_t s = static_cast<int32_t>(v);
            if (s < 0)
                return "-" + fmtConst(static_cast<uint64_t>(-static_cast<int64_t>(s)));
        } else if (size == 8) {
            const int64_t s = static_cast<int64_t>(v);
            if (s < 0)
                return "-" + fmtConst(~v + 1);
        }
        return fmtConst(v);
    }

    inline uint32_t fullRegId(uint32_t id) {
        const auto &regs = bnwasm::allRegs();
        return id >= 1 && id <= regs.size() ? regs[id - 1].full : id;
    }

    class Printer {
      public:
        explicit Printer(const LowLevelILFunction &il) : il_(il) {}

        std::string print() {
            for (size_t i = 0; i < il_.items().size(); ++i)
                printItem(i);
            return out_.str();
        }

        std::string printExprOnly(ExprId id) { return expr(id, 0); }

      private:
        void printItem(size_t index) {
            const Item &item = il_.items()[index];
            if (item.kind == Item::Kind::Label) {
                out_ << il_.LabelName(item.label) << ":\n";
                return;
            }
            const Expr &e = il_.expr(item.expr);
            out_ << "  ";
            switch (e.op) {
            case ILOp::SetRegister: {
                const Expr &rhs = il_.expr(e.kids[0]);
                if (e.size == 4 && rhs.op == ILOp::Register && rhs.reg == e.reg && rhs.size == 4 && fullRegId(e.reg) != e.reg) {
                    // 4-byte subregister write: make the zero-extension explicit
                    // instead of printing the confusing `sd1 = sd1`.
                    out_ << regName(e.reg) << " = low.u32(" << regName(fullRegId(e.reg)) << ")\n";
                } else {
                    out_ << regName(e.reg) << " = " << expr(e.kids[0], 0) << "\n";
                }
                break;
            }
            case ILOp::Store:
                out_ << "store." << sizeSuffix(e.size) << "(" << expr(e.kids[0], 0) << ", " << expr(e.kids[1], 0) << ")\n";
                break;
            case ILOp::Call:
                out_ << "call(" << expr(e.kids[0], 0) << ")\n";
                break;
            case ILOp::Jump:
                out_ << "jump(" << expr(e.kids[0], 0) << ")\n";
                break;
            case ILOp::Goto:
                out_ << "goto " << il_.LabelName(e.labels[0]) << "\n";
                break;
            case ILOp::If:
                out_ << "if (" << expr(e.kids[0], 0) << ") goto " << il_.LabelName(e.labels[0]) << " else " << il_.LabelName(e.labels[1]) << "\n";
                break;
            case ILOp::Return:
                out_ << "return " << expr(e.kids[0], 0) << "\n";
                break;
            case ILOp::NoReturn:
                out_ << "no_return\n";
                break;
            case ILOp::Nop:
                out_ << "nop\n";
                break;
            case ILOp::Unimplemented:
                out_ << "unimplemented\n";
                break;
            case ILOp::Intrinsic: {
                if (!e.outs.empty()) {
                    for (size_t k = 0; k < e.outs.size(); ++k) {
                        if (k)
                            out_ << ", ";
                        out_ << regName(e.outs[k].index);
                    }
                    out_ << " = ";
                }
                const auto &defs = bnwasm::intrinsics();
                out_ << (e.imm < defs.size() ? defs[e.imm].name : "intrinsic_" + std::to_string(e.imm)) << "(";
                for (size_t k = 0; k < e.kids.size(); ++k) {
                    if (k)
                        out_ << ", ";
                    out_ << expr(e.kids[k], 0);
                }
                out_ << ")\n";
                break;
            }
            default:
                // Expressions are normally consumed by a parent.  Print them anyway
                // so an accidental bare expression is visible.
                out_ << expr(item.expr, 0) << "\n";
                break;
            }
        }

        std::string expr(ExprId id, int depth) {
            if (depth > 96)
                return "...";
            const Expr &e = il_.expr(id);
            const auto kid = [&](size_t n) { return expr(e.kids[n], depth + 1); };
            switch (e.op) {
            case ILOp::Const:
                return fmtConst(e.imm, e.size);
            case ILOp::ConstPointer:
                return "ptr(" + fmtConst(e.imm) + ")";
            case ILOp::FloatConst: {
                std::ostringstream os;
                if (e.size == 4) {
                    const float f = std::bit_cast<float>(static_cast<uint32_t>(e.imm));
                    os << "f32(" << f << ")";
                } else if (e.size == 8) {
                    const double d = std::bit_cast<double>(e.imm);
                    os << "f64(" << d << ")";
                } else {
                    os << "float(" << fmtConst(e.imm) << ")";
                }
                return os.str();
            }
            case ILOp::Register:
                return regName(e.reg);
            case ILOp::SetRegister:
                return std::string("(") + regName(e.reg) + " = " + kid(0) + ")";
            case ILOp::Load:
                return "load." + sizeSuffix(e.size) + "(" + kid(0) + ")";
            case ILOp::Store:
                return "store." + sizeSuffix(e.size) + "(" + kid(0) + ", " + kid(1) + ")";
            case ILOp::Add: {
                // `sp + 0xfffffff8` reads much better as `sp - 8`.
                const Expr &rhs = il_.expr(e.kids[1]);
                if (rhs.op == ILOp::Const) {
                    const int64_t sv = e.size == 4 ? static_cast<int32_t>(rhs.imm) : static_cast<int64_t>(rhs.imm);
                    if (sv < 0)
                        return "(" + kid(0) + " - " + fmtConst(static_cast<uint64_t>(-sv)) + ")";
                }
                return "(" + kid(0) + " + " + kid(1) + ")";
            }
            case ILOp::Sub: {
                const Expr &rhs = il_.expr(e.kids[1]);
                if (rhs.op == ILOp::Const) {
                    const int64_t sv = e.size == 4 ? static_cast<int32_t>(rhs.imm) : static_cast<int64_t>(rhs.imm);
                    if (sv < 0)
                        return "(" + kid(0) + " + " + fmtConst(static_cast<uint64_t>(-sv)) + ")";
                }
                return "(" + kid(0) + " - " + kid(1) + ")";
            }
            case ILOp::Mult:
                return "(" + kid(0) + " * " + kid(1) + ")";
            case ILOp::DivSigned:
                return "(" + kid(0) + " /s " + kid(1) + ")";
            case ILOp::DivUnsigned:
                return "(" + kid(0) + " /u " + kid(1) + ")";
            case ILOp::ModSigned:
                return "(" + kid(0) + " %s " + kid(1) + ")";
            case ILOp::ModUnsigned:
                return "(" + kid(0) + " %u " + kid(1) + ")";
            case ILOp::And:
                return "(" + kid(0) + " & " + kid(1) + ")";
            case ILOp::Or:
                return "(" + kid(0) + " | " + kid(1) + ")";
            case ILOp::Xor:
                return "(" + kid(0) + " ^ " + kid(1) + ")";
            case ILOp::ShiftLeft:
                return "(" + kid(0) + " << " + kid(1) + ")";
            case ILOp::ArithShiftRight:
                return "(" + kid(0) + " >>s " + kid(1) + ")";
            case ILOp::LogicalShiftRight:
                return "(" + kid(0) + " >>u " + kid(1) + ")";
            case ILOp::RotateLeft:
                return "rotl(" + kid(0) + ", " + kid(1) + ")";
            case ILOp::RotateRight:
                return "rotr(" + kid(0) + ", " + kid(1) + ")";
            case ILOp::CompareEqual:
                return "(" + kid(0) + " == " + kid(1) + ")";
            case ILOp::CompareNotEqual:
                return "(" + kid(0) + " != " + kid(1) + ")";
            case ILOp::CompareSignedLessThan:
                return "(" + kid(0) + " s< " + kid(1) + ")";
            case ILOp::CompareUnsignedLessThan:
                return "(" + kid(0) + " u< " + kid(1) + ")";
            case ILOp::CompareSignedGreaterThan:
                return "(" + kid(0) + " s> " + kid(1) + ")";
            case ILOp::CompareUnsignedGreaterThan:
                return "(" + kid(0) + " u> " + kid(1) + ")";
            case ILOp::CompareSignedLessEqual:
                return "(" + kid(0) + " s<= " + kid(1) + ")";
            case ILOp::CompareUnsignedLessEqual:
                return "(" + kid(0) + " u<= " + kid(1) + ")";
            case ILOp::CompareSignedGreaterEqual:
                return "(" + kid(0) + " s>= " + kid(1) + ")";
            case ILOp::CompareUnsignedGreaterEqual:
                return "(" + kid(0) + " u>= " + kid(1) + ")";
            case ILOp::BoolToInt:
                return "bool_to_int(" + kid(0) + ")";
            case ILOp::SignExtend:
                return "sext." + sizeSuffix(e.size) + "(" + kid(0) + ")";
            case ILOp::ZeroExtend:
                return "zext." + sizeSuffix(e.size) + "(" + kid(0) + ")";
            case ILOp::LowPart:
                return "low." + sizeSuffix(e.size) + "(" + kid(0) + ")";
            case ILOp::FloatAbs:
                return "fabs(" + kid(0) + ")";
            case ILOp::FloatNeg:
                return "fneg(" + kid(0) + ")";
            case ILOp::FloatTrunc:
                return "ftrunc(" + kid(0) + ")";
            case ILOp::FloatSqrt:
                return "fsqrt(" + kid(0) + ")";
            case ILOp::FloatAdd:
                return "(" + kid(0) + " +f " + kid(1) + ")";
            case ILOp::FloatSub:
                return "(" + kid(0) + " -f " + kid(1) + ")";
            case ILOp::FloatMult:
                return "(" + kid(0) + " *f " + kid(1) + ")";
            case ILOp::FloatDiv:
                return "(" + kid(0) + " /f " + kid(1) + ")";
            case ILOp::FloatCompareEqual:
                return "(" + kid(0) + " ==f " + kid(1) + ")";
            case ILOp::FloatCompareNotEqual:
                return "(" + kid(0) + " !=f " + kid(1) + ")";
            case ILOp::FloatCompareLessThan:
                return "(" + kid(0) + " <f " + kid(1) + ")";
            case ILOp::FloatCompareGreaterThan:
                return "(" + kid(0) + " >f " + kid(1) + ")";
            case ILOp::FloatCompareLessEqual:
                return "(" + kid(0) + " <=f " + kid(1) + ")";
            case ILOp::FloatCompareGreaterEqual:
                return "(" + kid(0) + " >=f " + kid(1) + ")";
            case ILOp::FloatToInt:
                return "float_to_int." + sizeSuffix(e.size) + "(" + kid(0) + ")";
            case ILOp::IntToFloat:
                return "int_to_float." + sizeSuffix(e.size) + "(" + kid(0) + ")";
            case ILOp::FloatConvert:
                return "float_convert." + sizeSuffix(e.size) + "(" + kid(0) + ")";
            case ILOp::Jump:
                return "jump(" + kid(0) + ")";
            case ILOp::Call:
                return "call(" + kid(0) + ")";
            case ILOp::Goto:
                return "goto " + il_.LabelName(e.labels[0]);
            case ILOp::If:
                return "if (" + kid(0) + ") goto " + il_.LabelName(e.labels[0]) + " else " + il_.LabelName(e.labels[1]);
            case ILOp::Return:
                return "return " + kid(0);
            case ILOp::NoReturn:
                return "no_return";
            case ILOp::Nop:
                return "nop";
            case ILOp::Unimplemented:
                return "unimplemented";
            case ILOp::Intrinsic: {
                const auto &defs = bnwasm::intrinsics();
                std::string s = e.imm < defs.size() ? defs[e.imm].name : "intrinsic";
                s += "(";
                for (size_t k = 0; k < e.kids.size(); ++k) {
                    if (k)
                        s += ", ";
                    s += kid(k);
                }
                return s + ")";
            }
            }
            return "?";
        }

        const LowLevelILFunction &il_;
        std::ostringstream out_;
    };

    inline std::string printIl(const LowLevelILFunction &il) { return Printer(il).print(); }

    // ---------------------------------------------------------------------------
    // Structural checks and statistics
    // ---------------------------------------------------------------------------

    struct IlStats {
        size_t instructions = 0;
        size_t expressions = 0;
        size_t unimplemented = 0;
        size_t nops = 0;
        size_t intrinsics = 0;
        size_t loads = 0;
        size_t stores = 0;
        size_t calls = 0;
        size_t returns = 0;
        size_t directJumps = 0;
        size_t gotos = 0;
        size_t ifs = 0;
        size_t setRegisters = 0;
        size_t registerReads = 0;
        size_t spills = 0;
        size_t v128Ops = 0;
        size_t nativeIntArith = 0;
        size_t nativeFloat = 0;
        size_t nativeCompare = 0;
        std::map<std::string, size_t> intrinsicCounts;
        std::vector<std::string> errors;
    };

    inline bool containsRegister(const LowLevelILFunction &il, ExprId id, uint32_t reg) {
        const Expr &e = il.expr(id);
        if (e.op == ILOp::Register && e.reg == reg)
            return true;
        for (ExprId k : e.kids)
            if (containsRegister(il, k, reg))
                return true;
        return false;
    }

    inline IlStats analyzeIl(const LowLevelILFunction &il, bool checkLabels = true) {
        IlStats st;
        std::set<ExprId> seen;
        const auto visit = [&](auto &&self, ExprId id) -> void {
            if (!seen.insert(id).second)
                return;
            const Expr &e = il.expr(id);
            ++st.expressions;
            switch (e.op) {
            case ILOp::Unimplemented:
                ++st.unimplemented;
                break;
            case ILOp::Nop:
                ++st.nops;
                break;
            case ILOp::Intrinsic: {
                ++st.intrinsics;
                const auto &defs = bnwasm::intrinsics();
                st.intrinsicCounts[e.imm < defs.size() ? defs[e.imm].name : "unknown"]++;
                break;
            }
            case ILOp::Load:
                ++st.loads;
                break;
            case ILOp::Store:
                ++st.stores;
                break;
            case ILOp::Call:
                ++st.calls;
                break;
            case ILOp::Return:
                ++st.returns;
                break;
            case ILOp::Jump:
                ++st.directJumps;
                break;
            case ILOp::Goto:
                ++st.gotos;
                break;
            case ILOp::If:
                ++st.ifs;
                break;
            case ILOp::SetRegister:
                ++st.setRegisters;
                break;
            case ILOp::Register:
                ++st.registerReads;
                break;
            case ILOp::Add:
            case ILOp::Sub:
            case ILOp::Mult:
            case ILOp::DivSigned:
            case ILOp::DivUnsigned:
            case ILOp::ModSigned:
            case ILOp::ModUnsigned:
            case ILOp::And:
            case ILOp::Or:
            case ILOp::Xor:
            case ILOp::ShiftLeft:
            case ILOp::ArithShiftRight:
            case ILOp::LogicalShiftRight:
            case ILOp::RotateLeft:
            case ILOp::RotateRight:
                ++st.nativeIntArith;
                break;
            case ILOp::CompareEqual:
            case ILOp::CompareNotEqual:
            case ILOp::CompareSignedLessThan:
            case ILOp::CompareUnsignedLessThan:
            case ILOp::CompareSignedGreaterThan:
            case ILOp::CompareUnsignedGreaterThan:
            case ILOp::CompareSignedLessEqual:
            case ILOp::CompareUnsignedLessEqual:
            case ILOp::CompareSignedGreaterEqual:
            case ILOp::CompareUnsignedGreaterEqual:
                ++st.nativeCompare;
                break;
            case ILOp::FloatAbs:
            case ILOp::FloatNeg:
            case ILOp::FloatTrunc:
            case ILOp::FloatSqrt:
            case ILOp::FloatAdd:
            case ILOp::FloatSub:
            case ILOp::FloatMult:
            case ILOp::FloatDiv:
            case ILOp::FloatCompareEqual:
            case ILOp::FloatCompareNotEqual:
            case ILOp::FloatCompareLessThan:
            case ILOp::FloatCompareGreaterThan:
            case ILOp::FloatCompareLessEqual:
            case ILOp::FloatCompareGreaterEqual:
                ++st.nativeFloat;
                break;
            default:
                break;
            }
            if ((e.op == ILOp::Register || e.op == ILOp::SetRegister) && e.size == 16)
                ++st.v128Ops;
            for (ExprId k : e.kids)
                self(self, k);
        };

        const uint32_t sp = bnwasm::regId(bnwasm::Bank::Sp);
        for (const Item &item : il.items()) {
            if (item.kind == Item::Kind::Label)
                continue;
            ++st.instructions;
            visit(visit, item.expr);
            const Expr &e = il.expr(item.expr);
            if (e.op == ILOp::Load || e.op == ILOp::Store) {
                if (containsRegister(il, e.kids[0], sp))
                    ++st.spills;
            }
            if (checkLabels) {
                if (e.op == ILOp::Goto) {
                    if (!il.HasLabel(e.labels[0]))
                        st.errors.push_back("unmarked goto label " + il.LabelName(e.labels[0]));
                } else if (e.op == ILOp::If) {
                    if (!il.HasLabel(e.labels[0]))
                        st.errors.push_back("unmarked if-true label " + il.LabelName(e.labels[0]));
                    if (!il.HasLabel(e.labels[1]))
                        st.errors.push_back("unmarked if-false label " + il.LabelName(e.labels[1]));
                }
            }
        }
        return st;
    }

} // namespace BinaryNinja
