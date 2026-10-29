//
// Created by Dottir on 28/10/2026.
//

#include "bn/lifter.hpp"

#include <algorithm>
#include <cstring>
#include <functional>

#include <lowlevelilinstruction.h>

#include "bn/intrinsics.hpp"
#include "bn/regs.hpp"
#include "wasm/opcodes.hpp"

namespace bnwasm {

using namespace BinaryNinja;
namespace op = wasm::op;

uint64_t funcAddress(const wasm::Module &module, uint32_t index) {
    const auto &f = module.functions[index];
    return f.address ? *f.address + kCodeBase : kExternBase + 8ull * index;
}

namespace {

struct MemOp {
    uint8_t bytes;
    bool sign;
    uint8_t result;
};

// Plain and atomic (0xFE10-0xFE16) loads.
bool loadInfo(uint32_t op, MemOp &out) {
    static constexpr MemOp plain[] = {
        {4, 0, 4}, {8, 0, 8}, {4, 0, 4}, {8, 0, 8}, {1, 1, 4}, {1, 0, 4}, {2, 1, 4},
        {2, 0, 4}, {1, 1, 8}, {1, 0, 8}, {2, 1, 8}, {2, 0, 8}, {4, 1, 8}, {4, 0, 8},
    };
    static constexpr MemOp atomic[] = {{4, 0, 4}, {8, 0, 8}, {1, 0, 4}, {2, 0, 4}, {1, 0, 8}, {2, 0, 8}, {4, 0, 8}};
    if (op >= op::LoadFirst && op <= op::LoadLast) {
        out = plain[op - op::LoadFirst];
        return true;
    }
    if (op >= 0xFE10 && op <= 0xFE16) {
        out = atomic[op - 0xFE10];
        return true;
    }
    return false;
}

// Plain and atomic (0xFE17-0xFE1D) stores: memory bytes, or 0.
uint8_t storeBytes(uint32_t op) {
    static constexpr uint8_t plain[] = {4, 8, 4, 8, 1, 2, 1, 2, 4};
    static constexpr uint8_t atomic[] = {4, 8, 1, 2, 1, 2, 4};
    if (op >= op::StoreFirst && op <= op::StoreLast)
        return plain[op - op::StoreFirst];
    if (op >= 0xFE17 && op <= 0xFE1D)
        return atomic[op - 0xFE17];
    return 0;
}

bool isNop(uint32_t op) {
    switch (op) {
    case op::Nop: case op::Block: case op::Loop: case op::Else: case op::Try: case op::Catch: case op::Delegate:
    case op::CatchAll: case op::TryTable: case op::RefAsNonNull:
        return true;
    default:
        return false;
    }
}

constexpr uint32_t temp(uint32_t n) { return 0x80000000u | n; }

class Lifter {
public:
    Lifter(LowLevelILFunction &il, Architecture *arch, const wasm::Module &m, const wasm::FunctionAnalysis *fa,
           uint64_t addr)
        : il_(il), arch_(arch), m_(m), fa_(fa), addr_(addr) {
        h_ = fa ? static_cast<int>(fa->heightAt(addr)) : 0;
        const ptrdiff_t i = m.startIndex(addr);
        if (i >= 0) {
            funcStart_ = m.starts[i];
            const auto it = m.addrIndex.find(funcStart_);
            if (it != m.addrIndex.end())
                sig_ = m.funcSig(it->second);
        }
    }

    void run(const wasm::Instr &ins);

private:
    LowLevelILFunction &il_;
    Architecture *arch_;
    const wasm::Module &m_;
    const wasm::FunctionAnalysis *fa_;
    uint64_t addr_;
    uint64_t funcStart_ = ~0ull;
    const wasm::FuncType *sig_ = nullptr;
    int h_ = 0;
    int v_[4] = {0, 0, 0, 0};

    void add(ExprId e) { il_.AddInstruction(e); }

    ExprId c4(uint64_t v) { return il_.Const(4, static_cast<uint32_t>(v)); }
    ExprId c8(uint64_t v) { return il_.Const(8, v); }

    ExprId spill(int d, int half) {
        const int64_t off = -8 * (2 * (static_cast<int64_t>(d) - static_cast<int64_t>(kNStack)) + half + 1);
        return il_.Add(4, il_.Register(4, regId(Bank::Sp)), c4(static_cast<uint64_t>(off)));
    }

    ExprId vreg(int d) {
        d = std::max(d, 0);
        if (static_cast<uint32_t>(d) >= kNStack)
            return il_.Load(16, spill(d, 1));
        return il_.Register(16, regId(Bank::X, d));
    }

    void vwrite(int d, ExprId expr) {
        d = std::max(d, 0);
        if (static_cast<uint32_t>(d) >= kNStack)
            add(il_.Store(16, spill(d, 1), expr));
        else
            add(il_.SetRegister(16, regId(Bank::X, d), expr));
    }

    ExprId reg(int d, size_t size = 8) {
        d = std::max(d, 0);
        if (static_cast<uint32_t>(d) >= kNStack)
            return il_.Load(size, spill(d, 0));
        if (size == 4)
            return il_.Register(4, regId(Bank::Sd, d));
        const ExprId r = il_.Register(8, regId(Bank::S, d));
        return size == 8 ? r : il_.LowPart(size, r);
    }

    void write(int d, ExprId expr, size_t size = 8) {
        d = std::max(d, 0);
        if (static_cast<uint32_t>(d) >= kNStack)
            add(il_.Store(size, spill(d, 0), expr));
        else if (size == 4)
            add(il_.SetRegister(4, regId(Bank::Sd, d), expr));
        else
            add(il_.SetRegister(8, regId(Bank::S, d), expr));
    }

    void pop(int n) {
        h_ = std::max(h_ - 1, 0);
        v_[n] = h_;
    }
    ExprId val(int n, size_t size = 8) { return reg(v_[n], size); }
    void push(ExprId e, size_t size = 8) {
        write(h_, e, size);
        ++h_;
    }

    void jumpTo(uint64_t target) { add(il_.Jump(il_.ConstPointer(4, target))); }

    void gotoAddr(uint64_t target) {
        target += kCodeBase;
        if (BNLowLevelILLabel *label = il_.GetLabelForAddress(arch_, target))
            add(il_.Goto(*label));
        else
            jumpTo(target);
    }

    void branchIf(ExprId cond, uint64_t taken, uint64_t other, const std::function<void()> &pre = {},
                  const std::function<void()> &falsePre = {}) {
        taken += kCodeBase;
        other += kCodeBase;
        BNLowLevelILLabel *t = il_.GetLabelForAddress(arch_, taken);
        BNLowLevelILLabel *f = il_.GetLabelForAddress(arch_, other);
        LowLevelILLabel tNew, fNew;
        BNLowLevelILLabel &tDst = (!t || pre) ? static_cast<BNLowLevelILLabel &>(tNew) : *t;
        BNLowLevelILLabel &fDst = (!f || falsePre) ? static_cast<BNLowLevelILLabel &>(fNew) : *f;
        add(il_.If(cond, tDst, fDst));
        if (&tDst != t) {
            il_.MarkLabel(tDst);
            if (pre)
                pre();
            if (t)
                add(il_.Goto(*t));
            else
                jumpTo(taken);
        }
        if (&fDst != f) {
            il_.MarkLabel(fDst);
            if (falsePre)
                falsePre();
            if (f)
                add(il_.Goto(*f));
            else
                jumpTo(other);
        }
    }

    void intrinsic(uint32_t id, const std::vector<ExprId> &params, uint32_t outs) {
        std::vector<RegisterOrFlag> outputs;
        for (uint32_t k = 0; k < outs; ++k)
            outputs.push_back(RegisterOrFlag::Register(temp(8 + k)));
        add(il_.Intrinsic(outputs, id, params));
        for (uint32_t k = 0; k < outs; ++k)
            push(il_.Register(8, temp(8 + k)));
    }

    void builtin(const Builtin &b, const wasm::Instr &ins) {
        for (int k = b.pops - 1; k >= 0; --k)
            pop(k);
        std::vector<ExprId> params;
        const int64_t imms[2] = {ins.imm0, ins.imm1};
        for (int k = 0; k < b.imms; ++k)
            params.push_back(c8(static_cast<uint64_t>(imms[k])));
        for (int k = 0; k < b.pops; ++k) {
            if (k == 0 && b.foldAddress)
                params.push_back(effectiveAddress(0, ins.imm0));
            else
                params.push_back(val(k));
        }
        intrinsic(b.intrinsic, params, b.outs);
    }

    void epilogue() {
        const int nres = sig_ ? static_cast<int>(sig_->results.size()) : 0;
        for (int k = 0; k < std::min<int>(nres, kNResults); ++k) {
            if (sig_->results[k] == wasm::kV128) {
                const ExprId v = vreg(h_ - nres + k);
                add(il_.SetRegister(8, regId(Bank::R, k), il_.LowPart(8, v)));
                add(il_.SetRegister(8, regId(Bank::Rh, k), il_.LowPart(8, il_.LogicalShiftRight(16, vreg(h_ - nres + k), il_.Const(1, 64)))));
            } else {
                add(il_.SetRegister(8, regId(Bank::R, k), reg(h_ - nres + k)));
            }
        }
        add(il_.Return(il_.ConstPointer(4, 0)));
    }

    void call(ExprId dest, const wasm::FuncType *sig) {
        const int nparams = sig ? static_cast<int>(sig->params.size()) : 0;
        const int nresults = sig ? static_cast<int>(sig->results.size()) : 0;
        const int base = std::max(h_ - nparams, 0);
        for (int k = 0; k < std::min<int>(nparams, kNArgs); ++k) {
            if (sig->params[k] == wasm::kV128) {
                add(il_.SetRegister(8, regId(Bank::A, k), il_.LowPart(8, vreg(base + k))));
                add(il_.SetRegister(8, regId(Bank::Ah, k),
                                    il_.LowPart(8, il_.LogicalShiftRight(16, vreg(base + k), il_.Const(1, 64)))));
            } else {
                add(il_.SetRegister(8, regId(Bank::A, k), reg(base + k)));
            }
        }
        add(il_.Call(dest));
        h_ = base;
        for (int k = 0; k < std::min<int>(nresults, kNResults); ++k) {
            if (sig->results[k] == wasm::kV128) {
                vwrite(h_, joinHalves(regId(Bank::R, k), regId(Bank::Rh, k)));
                ++h_;
            } else {
                push(il_.Register(8, regId(Bank::R, k)));
            }
        }
        h_ = base + nresults;
    }

    ExprId joinHalves(uint32_t lo, uint32_t hi) {
        return il_.Or(16, il_.ZeroExtend(16, il_.Register(8, lo)),
                      il_.ShiftLeft(16, il_.ZeroExtend(16, il_.Register(8, hi)), il_.Const(1, 64)));
    }

    ExprId effectiveAddress(int baseN, int64_t offset, int64_t extra = 0) {
        const ExprId base = val(baseN, 4);
        const uint64_t total = static_cast<uint64_t>(offset + extra);
        return total ? il_.Add(4, base, c4(total)) : base;
    }

    bool isVecLocal(uint32_t i) const {
        const auto it = m_.localTypes.find(funcStart_);
        return it != m_.localTypes.end() && i < it->second.size() && it->second[i] == wasm::kV128;
    }

    size_t localSize(uint32_t i) const {
        const auto it = m_.localTypes.find(funcStart_);
        if (it == m_.localTypes.end() || i >= it->second.size())
            return 8;
        const uint8_t t = it->second[i];
        return t == wasm::kI32 || t == wasm::kF32 ? 4 : 8;
    }

    void moves(const std::vector<wasm::Move> *mv) {
        if (!mv)
            return;
        for (const auto &m : *mv) {
            if (m.vec)
                vwrite(static_cast<int>(m.dst), vreg(static_cast<int>(m.src)));
            else
                write(static_cast<int>(m.dst), reg(static_cast<int>(m.src)));
        }
    }

    // After a call or throw inside try_tables: leave through the first clause that matches the exception.
    void dispatchCatches(bool throws) {
        if (fa_ && fa_->tryChain.count(addr_)) {
            const auto chain = fa_->tryChain.find(addr_);
            const auto adj = fa_->catchAdj.find(addr_);
            size_t idx = 0;
            for (uint64_t t : chain->second) {
                const auto cs = fa_->catches.find(t);
                if (cs == fa_->catches.end())
                    continue;
                for (const wasm::CatchClause &cl : cs->second) {
                    const size_t k = idx++;
                    if (cl.target == wasm::kUnresolved || cl.target == wasm::kFunctionEnd)
                        continue;
                    const auto &sp = specials();
                    add(il_.Intrinsic({RegisterOrFlag::Register(temp(2))}, sp.catchMatch, {c8(cl.kind), c8(cl.tag)}));
                    LowLevelILLabel stub, cont;
                    add(il_.If(il_.CompareNotEqual(8, il_.Register(8, temp(2)), c8(0)), stub, cont));
                    il_.MarkLabel(stub);
                    for (uint32_t i = 0; i < cl.arity; ++i) {
                        add(il_.Intrinsic({RegisterOrFlag::Register(temp(3))}, sp.catchValue,
                                          {c8(cl.kind), c8(cl.tag), c8(i)}));
                        write(static_cast<int>(cl.entry + i), il_.Register(8, temp(3)));
                    }
                    if (adj != fa_->catchAdj.end() && k < adj->second.size() && adj->second[k]) {
                        const uint32_t g0 = regId(Bank::G, 0);
                        add(il_.SetRegister(4, g0, il_.Add(4, il_.Register(4, g0), c4(static_cast<uint64_t>(adj->second[k])))));
                    }
                    gotoAddr(cl.target);
                    il_.MarkLabel(cont);
                    add(il_.Nop());
                }
            }
        }
        if (throws)
            add(il_.NoReturn());
    }

    void adjustSp(const std::unordered_map<uint64_t, int64_t> &table, uint64_t addr) {
        const int64_t d = wasm::FunctionAnalysis::lookup(table, addr);
        if (!d)
            return;
        const uint32_t g0 = regId(Bank::G, 0);
        add(il_.SetRegister(4, g0, il_.Add(4, il_.Register(4, g0), c4(static_cast<uint64_t>(d)))));
    }

    void brTable(const wasm::Instr &ins);
    void localOrGlobal(const wasm::Instr &ins);
    void simd(const wasm::Instr &ins);
    void numeric(uint32_t op);
    void convert(uint32_t op);
};

void Lifter::brTable(const wasm::Instr &) {
    pop(0);
    const auto *targets = fa_ ? fa_->table(addr_) : nullptr;
    const bool resolved = targets && std::none_of(targets->begin(), targets->end(),
                                                  [](uint64_t t) { return t == wasm::kUnresolved || t == wasm::kFunctionEnd; });
    if (!resolved) {
        add(il_.Unimplemented());
        return;
    }
    const auto mv = fa_->tableMoves.find(addr_);
    const auto adj = fa_->tableAdj.find(addr_);
    // moves and SP fixup of label k, then the jump
    const auto leave = [&](size_t k) {
        if (mv != fa_->tableMoves.end() && k < mv->second.size())
            moves(&mv->second[k]);
        if (adj != fa_->tableAdj.end() && k < adj->second.size() && adj->second[k]) {
            const uint32_t g0 = regId(Bank::G, 0);
            add(il_.SetRegister(4, g0, il_.Add(4, il_.Register(4, g0), c4(static_cast<uint64_t>(adj->second[k])))));
        }
        gotoAddr((*targets)[k]);
    };
    const size_t n = targets->size() - 1;
    const auto same = [&](size_t a, size_t b) {
        if ((*targets)[a] != (*targets)[b])
            return false;
        const auto d = [&](size_t k) { return adj != fa_->tableAdj.end() && k < adj->second.size() ? adj->second[k] : 0; };
        const bool mvOk = mv == fa_->tableMoves.end() || a >= mv->second.size() || b >= mv->second.size() ||
                          mv->second[a].size() == mv->second[b].size();
        return d(a) == d(b) && mvOk;
    };
    std::vector<size_t> seen;
    for (size_t k = 0; k < n; ++k) {
        if (same(k, n) ||
            std::any_of(seen.begin(), seen.end(), [&](size_t s) { return same(s, k); }))
            continue;
        seen.push_back(k);
        ExprId cond = il_.CompareEqual(4, val(0, 4), c4(k));
        for (size_t k2 = k + 1; k2 < n; ++k2)
            if (same(k, k2))
                cond = il_.Or(4, cond, il_.CompareEqual(4, val(0, 4), c4(k2)));
        LowLevelILLabel nxt, taken;
        add(il_.If(cond, taken, nxt));
        il_.MarkLabel(taken);
        leave(k);
        il_.MarkLabel(nxt);
    }
    leave(n);
}

void Lifter::localOrGlobal(const wasm::Instr &ins) {
    const uint32_t op = ins.op;
    const auto i = static_cast<uint32_t>(ins.imm0);
    const bool local = op <= op::LocalTee;
    const uint32_t limit = local ? kNLocals : kNGlobals;
    const size_t width = local ? localSize(i) : (i == 0 ? 4 : 8);
    const uint32_t regid = local ? regId(width == 4 ? Bank::Ld : Bank::L, i) : regId(Bank::G, i);
    const bool vec = local && i < limit && isVecLocal(i);
    const auto &sp = specials();
    if (op == op::LocalGet || op == op::GlobalGet) {
        if (vec) {
            vwrite(h_, il_.Register(16, regId(Bank::Lv, i)));
            ++h_;
        } else if (i < limit) {
            push(il_.Register(width, regid), width);
        } else {
            intrinsic(local ? sp.localGet : sp.globalGet, {c8(i)}, 1);
        }
        return;
    }
    pop(0);
    if (op == op::GlobalSet && i == 0 && fa_ && fa_->setAdj.count(addr_)) {
        adjustSp(fa_->setAdj, addr_);
    } else if (op == op::GlobalSet && i == 0 && fa_) {
        // dynamic stack pointer (aligned/variable alloca): BN can't track it, keep g0 on the static frame
        add(il_.Nop());
    } else if (vec) {
        add(il_.SetRegister(16, regId(Bank::Lv, i), vreg(v_[0])));
    } else if (i < limit) {
        add(il_.SetRegister(width, regid, val(0, width)));
    } else {
        add(il_.Intrinsic({}, local ? sp.localSet : sp.globalSet, {c8(i), val(0)}));
    }
    if (op == op::LocalTee)
        ++h_;
}

std::vector<uint64_t> simdImms(const wasm::Instr &ins) {
    const uint32_t sub = ins.op & 0xFF;
    std::vector<uint64_t> out;
    if (sub <= 11 || sub == 0x5C || sub == 0x5D || (sub >= 0x54 && sub <= 0x5B))
        out.push_back(static_cast<uint64_t>(ins.imm0));
    if (sub == 0x0C || sub == 0x0D) {
        uint64_t lo, hi;
        std::memcpy(&lo, ins.v128.data(), 8);
        std::memcpy(&hi, ins.v128.data() + 8, 8);
        out.push_back(lo);
        out.push_back(hi);
    } else if ((sub >= 0x15 && sub <= 0x22) || (sub >= 0x54 && sub <= 0x5B)) {
        out.push_back(static_cast<uint64_t>(ins.imm1));
    }
    return out;
}

void Lifter::simd(const wasm::Instr &ins) {
    const uint32_t sub = ins.op & 0xFF;
    const auto sig = wasm::simdSig(sub);
    if (!sig) {
        add(il_.Unimplemented());
        return;
    }
    const int h = h_;
    if (sub == 0x0C) {
        uint64_t lo, hi;
        std::memcpy(&lo, ins.v128.data(), 8);
        std::memcpy(&hi, ins.v128.data() + 8, 8);
        vwrite(h, il_.Or(16, il_.ZeroExtend(16, c8(lo)), il_.ShiftLeft(16, il_.ZeroExtend(16, c8(hi)), il_.Const(1, 64))));
        ++h_;
    } else if (sub == 0x00) {
        pop(0);
        vwrite(h_, il_.Load(16, effectiveAddress(0, ins.imm0)));
        ++h_;
    } else if (sub == 0x0B) {
        pop(1);
        pop(0);
        add(il_.Store(16, effectiveAddress(0, ins.imm0), vreg(v_[1])));
    } else {
        std::vector<ExprId> args;
        for (auto it = sig->pops.rbegin(); it != sig->pops.rend(); ++it) {
            pop(0);
            args.push_back(*it == 'v' ? vreg(v_[0]) : reg(v_[0]));
        }
        std::vector<ExprId> params;
        for (uint64_t imm : simdImms(ins))
            params.push_back(c8(imm));
        params.insert(params.end(), args.rbegin(), args.rend());
        const bool vecOut = !sig->pushes.empty() && sig->pushes[0] == 'v';
        const bool direct = vecOut && static_cast<uint32_t>(h_) < kNStack;
        std::vector<RegisterOrFlag> outputs;
        if (direct)
            outputs.push_back(RegisterOrFlag::Register(regId(Bank::X, h_)));
        else if (!sig->pushes.empty())
            outputs.push_back(RegisterOrFlag::Register(temp(8)));
        add(il_.Intrinsic(outputs, simdIntrinsic(sub), params));
        if (vecOut) {
            if (!direct)
                add(il_.Store(16, spill(h_, 1), il_.Register(8, temp(8))));
            ++h_;
        } else if (!sig->pushes.empty()) {
            push(il_.Register(8, temp(8)));
        }
    }
}

void Lifter::numeric(uint32_t op) {
    // size, float, group within the type's block
    const auto done = [&](size_t size, ExprId e, size_t outSize) { push(e, outSize); (void)size; };
    size_t size;
    bool isFloat = false;
    uint32_t idx;
    enum { Eqz, ICmp, FCmp, IBin, FUn, FBin } kind;
    if (op == op::I32Eqz || op == op::I64Eqz) {
        kind = Eqz, size = op == op::I32Eqz ? 4 : 8, idx = 0;
    } else if (op >= 0x46 && op <= 0x4F) {
        kind = ICmp, size = 4, idx = op - 0x46;
    } else if (op >= 0x51 && op <= 0x5A) {
        kind = ICmp, size = 8, idx = op - 0x51;
    } else if (op >= 0x5B && op <= 0x66) {
        kind = FCmp, isFloat = true, size = op <= 0x60 ? 4 : 8, idx = (op - 0x5B) % 6;
    } else if (op >= 0x6A && op <= 0x78) {
        kind = IBin, size = 4, idx = op - 0x6A;
    } else if (op >= 0x7C && op <= 0x8A) {
        kind = IBin, size = 8, idx = op - 0x7C;
    } else if ((op >= 0x8B && op <= 0x91) || (op >= 0x99 && op <= 0x9F)) {
        kind = FUn, isFloat = true, size = op <= 0x91 ? 4 : 8, idx = (op - 0x8B) % 14;
    } else if ((op >= 0x92 && op <= 0x98) || (op >= 0xA0 && op <= 0xA6)) {
        kind = FBin, isFloat = true, size = op <= 0x98 ? 4 : 8, idx = (op - 0x92) % 14;
    } else {
        add(il_.Unimplemented());
        return;
    }
    (void)isFloat;

    if (kind == Eqz) {
        pop(0);
        done(size, il_.BoolToInt(4, il_.CompareEqual(size, val(0, size), il_.Const(size, 0))), 4);
        return;
    }
    if (kind == FUn) {
        pop(0);
        ExprId a = val(0, size);
        ExprId e;
        switch (idx % 7) {  // abs neg ceil floor trunc nearest sqrt
        case 0: e = il_.FloatAbs(size, a); break;
        case 1: e = il_.FloatNeg(size, a); break;
        case 4: e = il_.FloatTrunc(size, a); break;
        case 6: e = il_.FloatSqrt(size, a); break;
        default: add(il_.Unimplemented()); return;
        }
        done(size, e, size);
        return;
    }
    pop(1);
    pop(0);
    const ExprId a = val(0, size), b = val(1, size);
    ExprId e;
    switch (kind) {
    case ICmp:
        switch (idx) {  // eq ne lt_s lt_u gt_s gt_u le_s le_u ge_s ge_u
        case 0: e = il_.CompareEqual(size, a, b); break;
        case 1: e = il_.CompareNotEqual(size, a, b); break;
        case 2: e = il_.CompareSignedLessThan(size, a, b); break;
        case 3: e = il_.CompareUnsignedLessThan(size, a, b); break;
        case 4: e = il_.CompareSignedGreaterThan(size, a, b); break;
        case 5: e = il_.CompareUnsignedGreaterThan(size, a, b); break;
        case 6: e = il_.CompareSignedLessEqual(size, a, b); break;
        case 7: e = il_.CompareUnsignedLessEqual(size, a, b); break;
        case 8: e = il_.CompareSignedGreaterEqual(size, a, b); break;
        default: e = il_.CompareUnsignedGreaterEqual(size, a, b); break;
        }
        done(size, il_.BoolToInt(4, e), 4);
        return;
    case FCmp:
        switch (idx) {  // eq ne lt gt le ge
        case 0: e = il_.FloatCompareEqual(size, a, b); break;
        case 1: e = il_.FloatCompareNotEqual(size, a, b); break;
        case 2: e = il_.FloatCompareLessThan(size, a, b); break;
        case 3: e = il_.FloatCompareGreaterThan(size, a, b); break;
        case 4: e = il_.FloatCompareLessEqual(size, a, b); break;
        default: e = il_.FloatCompareGreaterEqual(size, a, b); break;
        }
        done(size, il_.BoolToInt(4, e), 4);
        return;
    case IBin:
        switch (idx) {  // clz ctz popcnt add sub mul div_s div_u rem_s rem_u and or xor shl shr_s shr_u rotl rotr
        case 0: e = il_.Add(size, a, b); break;
        case 1: e = il_.Sub(size, a, b); break;
        case 2: e = il_.Mult(size, a, b); break;
        case 3: e = il_.DivSigned(size, a, b); break;
        case 4: e = il_.DivUnsigned(size, a, b); break;
        case 5: e = il_.ModSigned(size, a, b); break;
        case 6: e = il_.ModUnsigned(size, a, b); break;
        case 7: e = il_.And(size, a, b); break;
        case 8: e = il_.Or(size, a, b); break;
        case 9: e = il_.Xor(size, a, b); break;
        case 10: e = il_.ShiftLeft(size, a, b); break;
        case 11: e = il_.ArithShiftRight(size, a, b); break;
        case 12: e = il_.LogicalShiftRight(size, a, b); break;
        case 13: e = il_.RotateLeft(size, a, b); break;
        default: e = il_.RotateRight(size, a, b); break;
        }
        done(size, e, size);
        return;
    default:  // FBin: add sub mul div min max copysign
        switch (idx % 7) {
        case 0: e = il_.FloatAdd(size, a, b); break;
        case 1: e = il_.FloatSub(size, a, b); break;
        case 2: e = il_.FloatMult(size, a, b); break;
        case 3: e = il_.FloatDiv(size, a, b); break;
        default: add(il_.Unimplemented()); return;
        }
        done(size, e, size);
        return;
    }
}

void Lifter::convert(uint32_t op) {
    pop(0);
    switch (op) {
    case 0xA8: push(il_.FloatToInt(4, val(0, 4)), 4); break;
    case 0xAA: push(il_.FloatToInt(4, val(0, 8)), 4); break;
    case 0xAE: push(il_.FloatToInt(8, val(0, 4)), 8); break;
    case 0xB0: push(il_.FloatToInt(8, val(0, 8)), 8); break;
    case 0xA7: case 0xBC: case 0xBE: push(val(0, 4), 4); break;
    case 0xAC: push(il_.SignExtend(8, val(0, 4))); break;
    case 0xAD: push(il_.ZeroExtend(8, val(0, 4))); break;
    case 0xB2: push(il_.IntToFloat(4, val(0, 4)), 4); break;
    case 0xB4: push(il_.IntToFloat(4, val(0, 8)), 4); break;
    case 0xB7: push(il_.IntToFloat(8, val(0, 4))); break;
    case 0xB9: push(il_.IntToFloat(8, val(0, 8))); break;
    case 0xB6: push(il_.FloatConvert(4, val(0, 8)), 4); break;
    case 0xBB: push(il_.FloatConvert(8, val(0, 4))); break;
    case 0xBD: case 0xBF: push(val(0)); break;
    case 0xC0: push(il_.SignExtend(4, val(0, 1)), 4); break;
    case 0xC1: push(il_.SignExtend(4, val(0, 2)), 4); break;
    case 0xC2: push(il_.SignExtend(8, val(0, 1))); break;
    case 0xC3: push(il_.SignExtend(8, val(0, 2))); break;
    case 0xC4: push(il_.SignExtend(8, val(0, 4))); break;
    default: add(il_.Unimplemented()); break;
    }
}

void Lifter::run(const wasm::Instr &ins) {
    const uint32_t op = ins.op;
    bool terminal = false;
    const uint64_t *target = fa_ ? fa_->branchTarget(addr_) : nullptr;

    if (const auto it = m_.addrIndex.find(addr_); it != m_.addrIndex.end()) {
        if (const wasm::FuncType *sig = m_.funcSig(it->second)) {
            const size_t n = std::min<size_t>({sig->params.size(), kNArgs, kNLocals});
            for (uint32_t i = 0; i < n; ++i) {
                if (sig->params[i] == wasm::kV128)
                    add(il_.SetRegister(16, regId(Bank::Lv, i), joinHalves(regId(Bank::A, i), regId(Bank::Ah, i))));
                else if (localSize(i) == 4)
                    add(il_.SetRegister(4, regId(Bank::Ld, i), il_.LowPart(4, il_.Register(8, regId(Bank::A, i)))));
                else
                    add(il_.SetRegister(8, regId(Bank::L, i), il_.Register(8, regId(Bank::A, i))));
            }
        }
    }

    MemOp mem;
    if (isNop(op)) {
    } else if (op == op::Unreachable) {
        add(il_.NoReturn());
        terminal = true;
    } else if (op == op::If) {
        pop(0);
        if (target) {
            branchIf(il_.CompareNotEqual(4, val(0, 4), c4(0)), addr_ + ins.size, *target);
            terminal = true;
        }
    } else if (op == op::BrIf) {
        pop(0);
        if (target) {
            const auto *mv = fa_->movesAt(addr_);
            const ExprId cond = il_.CompareNotEqual(4, val(0, 4), c4(0));
            const bool adj = wasm::FunctionAnalysis::lookup(fa_->edgeAdj, addr_) != 0;
            branchIf(cond, *target, addr_ + ins.size,
                     mv || adj ? std::function<void()>([&] {
                         moves(mv);
                         adjustSp(fa_->edgeAdj, addr_);
                     })
                               : std::function<void()>(),
                     wasm::FunctionAnalysis::lookup(fa_->fallAdj, addr_) != 0
                         ? std::function<void()>([&] { adjustSp(fa_->fallAdj, addr_); })
                         : std::function<void()>());
            terminal = true;
        }
    } else if (op == op::End) {
        if (target && *target == wasm::kFunctionEnd) {
            epilogue();
            terminal = true;
        }
    } else if (op == op::Return) {
        epilogue();
        terminal = true;
    } else if (op == op::Call || op == op::ReturnCall) {
        const auto index = static_cast<uint32_t>(ins.imm0);
        if (index >= m_.functions.size()) {
            add(il_.Unimplemented());
            terminal = true;
        } else {
            call(il_.ConstPointer(4, funcAddress(m_, index)), m_.funcSig(index));
            if (op == op::ReturnCall) {
                epilogue();
                terminal = true;
            } else {
                dispatchCatches(false);
            }
        }
    } else if (op == op::CallIndirect || op == op::ReturnCallIndirect) {
        pop(0);
        add(il_.Intrinsic({RegisterOrFlag::Register(temp(1))}, specials().tableTarget, {c8(static_cast<uint64_t>(ins.imm1)), val(0)}));
        call(il_.LowPart(4, il_.Register(8, temp(1))), m_.sig(static_cast<uint32_t>(ins.imm0)));
        if (op == op::ReturnCallIndirect) {
            epilogue();
            terminal = true;
        } else {
            dispatchCatches(false);
        }
    } else if (op == op::CallRef || op == op::ReturnCallRef) {
        pop(0);
        call(il_.LowPart(4, val(0)), m_.sig(static_cast<uint32_t>(ins.imm0)));
        if (op == op::ReturnCallRef) {
            epilogue();
            terminal = true;
        } else {
            dispatchCatches(false);
        }
    } else if (op == op::BrOnNull || op == op::BrOnNonNull) {
        pop(0);
        if (target) {
            const auto *mv = fa_->movesAt(addr_);
            const bool null = op == op::BrOnNull;
            const ExprId cond = null ? il_.CompareEqual(8, val(0), c8(0)) : il_.CompareNotEqual(8, val(0), c8(0));
            branchIf(cond, *target, addr_ + ins.size,
                     [&] {
                         moves(mv);
                         adjustSp(fa_->edgeAdj, addr_);
                     },
                     [&] { adjustSp(fa_->fallAdj, addr_); });
            terminal = true;
        }
    } else if (op == op::Throw || op == op::Rethrow || op == op::ThrowRef) {
        dispatchCatches(true);
        terminal = true;
    } else if (op == op::BrTable) {
        brTable(ins);
        terminal = true;
    } else if (op == op::Br) {
        moves(fa_ ? fa_->movesAt(addr_) : nullptr);
    } else if (op == op::Drop) {
        pop(0);
    } else if (op >= op::LocalGet && op <= op::GlobalSet) {
        localOrGlobal(ins);
    } else if (loadInfo(op, mem)) {
        pop(0);
        ExprId value = il_.Load(mem.bytes, effectiveAddress(0, ins.imm0));
        if (mem.bytes < mem.result)
            value = mem.sign ? il_.SignExtend(mem.result, value) : il_.ZeroExtend(mem.result, value);
        push(value, mem.result);
    } else if (const uint8_t sb = storeBytes(op)) {
        pop(1);
        pop(0);
        add(il_.Store(sb, effectiveAddress(0, ins.imm0), val(1, sb)));
    } else if (op >= wasm::kPrefixFD && op < wasm::kPrefixFE) {
        simd(ins);
    } else if (op == op::I32Const) {
        const auto v = static_cast<uint32_t>(ins.imm0);
        const bool inData = std::any_of(m_.dataSegments.begin(), m_.dataSegments.end(), [&](const wasm::DataSegment &s) {
            return v >= s.memAddr && v < s.memAddr + s.size;
        });
        push(inData ? il_.ConstPointer(4, v) : c4(v), 4);
    } else if (op == op::I64Const) {
        push(c8(static_cast<uint64_t>(ins.imm0)));
    } else if (op == op::F32Const) {
        push(c4(static_cast<uint64_t>(ins.imm0)), 4);
    } else if (op == op::F64Const) {
        push(c8(static_cast<uint64_t>(ins.imm0)));
    } else if (op == op::RefNull) {
        push(c8(0));
    } else if (op == op::RefIsNull) {
        pop(0);
        push(il_.BoolToInt(4, il_.CompareEqual(8, val(0), c8(0))), 4);
    } else if (op == op::RefFunc) {
        const auto index = static_cast<uint32_t>(ins.imm0);
        push(il_.ConstPointer(4, index < m_.functions.size() ? funcAddress(m_, index) : 0), 4);
    } else if (op == op::RefEq) {
        pop(1);
        pop(0);
        push(il_.BoolToInt(4, il_.CompareEqual(8, val(0), val(1))), 4);
    } else if (op == op::Select || op == op::SelectT) {
        pop(2);
        pop(1);
        pop(0);
        const int dst = v_[0];
        LowLevelILLabel taken, other, done;
        add(il_.If(il_.CompareNotEqual(4, val(2, 4), c4(0)), taken, other));
        il_.MarkLabel(taken);
        const bool vec = fa_ && fa_->vecOps.count(addr_);
        if (vec)
            vwrite(dst, vreg(v_[0]));
        else
            write(dst, reg(v_[0]));
        add(il_.Goto(done));
        il_.MarkLabel(other);
        if (vec)
            vwrite(dst, vreg(v_[1]));
        else
            write(dst, reg(v_[1]));
        il_.MarkLabel(done);
        add(il_.Nop());
        h_ = dst + 1;
    } else if (const Builtin *b = builtinFor(op)) {
        builtin(*b, ins);
    } else if (op >= op::NumericFirst && op <= op::NumericLast) {
        numeric(op);
    } else if (op >= op::ConvertFirst && op <= op::ConvertLast) {
        convert(op);
    } else {
        add(il_.Unimplemented());
        terminal = true;
    }

    if (!terminal && target && *target != wasm::kFunctionEnd) {
        adjustSp(fa_->edgeAdj, addr_);
        gotoAddr(*target);
    } else if (!terminal && fa_) {
        adjustSp(fa_->fallAdj, addr_);
    }
}

} // namespace

void lift(LowLevelILFunction &il, Architecture *arch, const wasm::Instr &ins, uint64_t addr,
          const wasm::Module &module, const wasm::FunctionAnalysis *fa) {
    Lifter(il, arch, module, fa, addr).run(ins);
}

} // namespace bnwasm
