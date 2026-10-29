//
// Created by Dottik on 28/10/2026.
//

#include <algorithm>

#include "wasm/decode.hpp"
#include "wasm/passes.hpp"

namespace wasm::pass {

constexpr uint64_t kNoTry = ~0ull;

struct HFrame {
    int entry, branchArity, endArity, params;
    std::vector<uint8_t> paramTy, resTy;  // 1 where the slot holds a v128
    uint64_t tryAddr;
};

void heights(const Module &m, FunctionAnalysis &fa) {
    std::span<const uint8_t> data = m.data;
    const auto it = m.addrIndex.find(fa.start);
    const FuncType *sig = it != m.addrIndex.end() ? m.funcSig(it->second) : nullptr;
    const int rets = sig ? static_cast<int>(sig->results.size()) : 0;

    std::vector<HFrame> frames;
    frames.push_back({0, rets, rets, 0, {}, {}, kNoTry});
    std::vector<uint8_t> ty;  // per operand slot, kept the same length as the height
    const auto lt = m.localTypes.find(fa.start);
    const auto vecLocal = [&](int64_t i) {
        return lt != m.localTypes.end() && i >= 0 && static_cast<size_t>(i) < lt->second.size() && lt->second[i] == kV128;
    };
    // Moves that carry the top `frame.branchArity` values below height `top` to the label's slots.
    const auto branchMoves = [&](const HFrame &f, int top) {
        std::vector<Move> mv;
        for (int i = 0; i < f.branchArity; ++i) {
            const int src = top - f.branchArity + i, dst = f.entry + i;
            if (src >= 0 && src != dst)
                mv.push_back({static_cast<uint32_t>(src), static_cast<uint32_t>(dst),
                              static_cast<size_t>(src) < ty.size() && ty[src]});
        }
        return mv;
    };
    const auto frameAt = [&](uint64_t label) -> const HFrame * {
        return label < frames.size() ? &frames[frames.size() - 1 - label] : nullptr;
    };
    const auto pushResults = [&](const FuncType *s) {
        if (s)
            for (uint8_t t : s->results)
                ty.push_back(t == kV128);
    };

    int h = 0;
    uint64_t addr = fa.start;
    const uint64_t end = fa.start + fa.size;
    while (addr < end) {
        const Instr ins = decode(data, addr);
        const uint32_t o = ins.op;
        fa.offsets.push_back(static_cast<uint32_t>(addr - fa.start));
        fa.heights.push_back(static_cast<uint32_t>(h));
        const size_t slot = fa.heights.size() - 1;
        ty.resize(h, 0);

        if (o == op::Call || o == op::CallIndirect || o == op::CallRef || o == op::Throw || o == op::Rethrow ||
            o == op::ThrowRef) {
            std::vector<uint64_t> chain;
            for (size_t k = frames.size(); k-- > 0;)
                if (frames[k].tryAddr != kNoTry)
                    chain.push_back(frames[k].tryAddr);
            if (!chain.empty())
                fa.tryChain[addr] = std::move(chain);
        }

        if (isBlock(o)) {
            const int64_t v = ins.imm0;
            int p = 0, r = 0;
            std::vector<uint8_t> resTy;
            if (v == -64) {
            } else if (v >= 0) {
                if (const FuncType *s = m.sig(static_cast<uint32_t>(v))) {
                    p = static_cast<int>(s->params.size()), r = static_cast<int>(s->results.size());
                    for (uint8_t t : s->results)
                        resTy.push_back(t == kV128);
                }
            } else {
                r = 1;
                resTy.push_back((v & 0x7F) == kV128);
            }
            if (o == op::If) {
                h = std::max(h - 1, 0);
                ty.resize(h);
            }
            const int entry = std::max(h - p, 0);
            std::vector<uint8_t> paramTy;
            for (int i = 0; i < p; ++i)
                paramTy.push_back(static_cast<size_t>(entry + i) < ty.size() ? ty[entry + i] : 0);
            if (o == op::TryTable) {
                if (const auto c = fa.catches.find(addr); c != fa.catches.end()) {
                    for (size_t k = 0; k < ins.catches.size() && k < c->second.size(); ++k) {
                        if (const HFrame *f = frameAt(ins.catches[k].label)) {
                            c->second[k].entry = static_cast<uint32_t>(f->entry);
                            c->second[k].arity = static_cast<uint32_t>(f->branchArity);
                        }
                    }
                }
            }
            frames.push_back({entry, o == op::Loop ? p : r, r, p, std::move(paramTy), std::move(resTy),
                              o == op::TryTable ? addr : kNoTry});
        } else if (o == op::Else) {
            const HFrame &f = frames.back();
            h = f.entry + f.params;
            ty.resize(f.entry, 0);
            ty.insert(ty.end(), f.paramTy.begin(), f.paramTy.end());
        } else if (o == op::Catch || o == op::CatchAll) {
            h = frames.back().entry;
            ty.resize(h);
        } else if (o == op::End || o == op::Delegate) {
            HFrame f = frames.size() > 1 ? frames.back() : frames.front();
            if (frames.size() > 1)
                frames.pop_back();
            h = f.entry + f.endArity;
            ty.resize(f.entry, 0);
            ty.insert(ty.end(), f.resTy.begin(), f.resTy.end());
            fa.heights[slot] = static_cast<uint32_t>(h);
        } else if (o == op::Br || o == op::BrIf || o == op::BrOnNull || o == op::BrOnNonNull) {
            // br_on_null carries the values below the reference, br_on_non_null the reference too
            const int top = o == op::BrOnNull || o == op::BrIf ? h - 1 : h;
            if (const HFrame *f = frameAt(static_cast<uint64_t>(ins.imm0))) {
                auto mv = branchMoves(*f, std::max(top, 0));
                if (!mv.empty())
                    fa.moves[addr] = std::move(mv);
            }
            if (o == op::BrIf || o == op::BrOnNonNull) {
                h = std::max(h - 1, 0);
                ty.resize(h);
            }
        } else if (o == op::BrTable) {
            h = std::max(h - 1, 0);
            ty.resize(h);
            const auto labels = tableLabels(data, addr);
            std::vector<std::vector<Move>> all(labels.size());
            bool any = false;
            for (size_t k = 0; k < labels.size(); ++k) {
                if (const HFrame *f = frameAt(labels[k])) {
                    all[k] = branchMoves(*f, h);
                    any = any || !all[k].empty();
                }
            }
            if (any)
                fa.tableMoves[addr] = std::move(all);
        } else if (o == op::Select || o == op::SelectT) {
            const bool vec = h >= 3 && ty[h - 3];
            if (vec)
                fa.vecOps.insert(addr);
            h = std::max(h - 3, 0);
            ty.resize(h);
            ty.push_back(vec);
            ++h;
        } else if (o == op::LocalGet) {
            ty.push_back(vecLocal(ins.imm0));
            ++h;
        } else if (o == op::LocalTee) {
        } else {
            const Effect e = effectOf(m, ins);
            h = std::max(h - e.pops, 0);
            ty.resize(h);
            if (o >= kPrefixFD && o < kPrefixFE) {
                if (const auto s = simdSig(o & 0xFF))
                    for (char c : s->pushes)
                        ty.push_back(c == 'v');
            } else if (o == op::Call || o == op::ReturnCall) {
                pushResults(m.funcSig(static_cast<uint32_t>(ins.imm0)));
            } else if (o == op::CallIndirect || o == op::ReturnCallIndirect || o == op::CallRef ||
                       o == op::ReturnCallRef) {
                pushResults(m.sig(static_cast<uint32_t>(ins.imm0)));
            }
            h += e.pushes;
        }
        addr += ins.size;
    }
}

} // namespace wasm::pass
