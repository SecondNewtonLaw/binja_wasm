//
// Created by Dottik on 28/10/2026.
//

#include "wasm/analysis.hpp"

#include <algorithm>
#include <array>

#include "wasm/decode.hpp"
#include "wasm/opcodes.hpp"

namespace wasm {

namespace {

struct Frame {
    char kind;  // 'b' block, 'l' loop, 'i' if, 'e' else
    uint64_t begin;
    std::vector<std::pair<uint64_t, int>> pending;  // (branch address, br_table slot or -1)
    uint64_t elsePrev = 0;
};

void branchMap(const Module &m, FunctionAnalysis &fa) {
    std::span<const uint8_t> data = m.data;
    std::vector<Frame> stack;
    stack.push_back({'b', fa.start, {}, 0});
    uint64_t addr = fa.start, prev = fa.start;
    const uint64_t end = fa.start + fa.size;
    while (addr < end) {
        const Instr ins = decode(data, addr);
        const uint32_t op = ins.op;
        if (op == op::TryTable) {
            // catch labels count from outside the try_table
            auto &clauses = fa.catches[addr];
            for (size_t k = 0; k < ins.catches.size(); ++k) {
                const CatchSpec &c = ins.catches[k];
                clauses.push_back({c.kind, c.tag});
                if (c.label >= stack.size())
                    continue;
                Frame &f = stack[stack.size() - 1 - c.label];
                if (f.kind == 'l')
                    clauses.back().target = f.begin;
                else
                    f.pending.push_back({addr, -static_cast<int>(k) - 2});
            }
            stack.push_back({'b', addr, {}, 0});
        } else if (op == op::Block || op == op::Try) {
            stack.push_back({'b', addr, {}, 0});
        } else if (op == op::Loop) {
            stack.push_back({'l', addr, {}, 0});
        } else if (op == op::If) {
            stack.push_back({'i', addr, {}, 0});
        } else if (op == op::Else && stack.back().kind == 'i') {
            Frame &f = stack.back();
            fa.branch[f.begin] = addr;
            f.kind = 'e';
            f.elsePrev = prev;
        } else if (op == op::Br || op == op::BrIf || op == op::BrOnNull || op == op::BrOnNonNull) {
            const auto label = static_cast<uint64_t>(ins.imm0);
            if (label < stack.size()) {
                Frame &f = stack[stack.size() - 1 - label];
                if (f.kind == 'l')
                    fa.branch[addr] = f.begin;
                else
                    f.pending.push_back({addr, -1});
            }
        } else if (op == op::BrTable) {
            const auto labels = tableLabels(data, addr);
            auto &targets = fa.tables[addr];
            targets.assign(labels.size(), kUnresolved);
            for (size_t k = 0; k < labels.size(); ++k) {
                if (labels[k] >= stack.size())
                    continue;
                Frame &f = stack[stack.size() - 1 - labels[k]];
                if (f.kind == 'l')
                    targets[k] = f.begin;
                else
                    f.pending.push_back({addr, static_cast<int>(k)});
            }
        } else if (op == op::End || op == op::Delegate) {
            Frame f = std::move(stack.back());
            stack.pop_back();
            if (f.kind == 'i')
                fa.branch[f.begin] = addr + ins.size;
            else if (f.kind == 'e')
                fa.branch.emplace(f.elsePrev, addr + ins.size);
            for (const auto &[a, k] : f.pending) {
                if (k >= 0)
                    fa.tables[a][k] = addr;
                else if (k == -1)
                    fa.branch[a] = addr;
                else
                    fa.catches[a][-k - 2].target = addr;
            }
            if (stack.empty())
                fa.branch[addr] = kFunctionEnd;
        }
        prev = addr;
        addr += ins.size;
        if (stack.empty())
            break;
    }
}

Effect effectOf(const Module &m, const Instr &ins) {
    const uint32_t op = ins.op;
    if (op == op::Call || op == op::ReturnCall) {
        const FuncType *s = m.funcSig(static_cast<uint32_t>(ins.imm0));
        return s ? Effect{static_cast<int>(s->params.size()), static_cast<int>(s->results.size())} : Effect{0, 0};
    }
    if (op == op::CallIndirect || op == op::ReturnCallIndirect || op == op::CallRef || op == op::ReturnCallRef) {
        const FuncType *s = m.sig(static_cast<uint32_t>(ins.imm0));
        return s ? Effect{static_cast<int>(s->params.size()) + 1, static_cast<int>(s->results.size())} : Effect{1, 0};
    }
    if (op >= 0xFC00 && op < 0xFD00)
        return fixedEffect(op).value_or(Effect{1, 1});
    return fixedEffect(op).value_or(Effect{0, 0});
}

void heights(const Module &m, FunctionAnalysis &fa) {
    std::span<const uint8_t> data = m.data;
    const auto it = m.addrIndex.find(fa.start);
    const FuncType *sig = it != m.addrIndex.end() ? m.funcSig(it->second) : nullptr;
    const int rets = sig ? static_cast<int>(sig->results.size()) : 0;

    constexpr uint64_t kNoTry = ~0ull;
    struct HFrame {
        int entry, branchArity, endArity, params;
        std::vector<uint8_t> paramTy, resTy;  // 1 where the slot holds a v128
        uint64_t tryAddr;
    };
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
        const uint32_t op = ins.op;
        fa.offsets.push_back(static_cast<uint32_t>(addr - fa.start));
        fa.heights.push_back(static_cast<uint32_t>(h));
        const size_t slot = fa.heights.size() - 1;
        ty.resize(h, 0);

        if (op == op::Call || op == op::CallIndirect || op == op::CallRef || op == op::Throw || op == op::Rethrow ||
            op == op::ThrowRef) {
            std::vector<uint64_t> chain;
            for (size_t k = frames.size(); k-- > 0;)
                if (frames[k].tryAddr != kNoTry)
                    chain.push_back(frames[k].tryAddr);
            if (!chain.empty())
                fa.tryChain[addr] = std::move(chain);
        }

        if (isBlock(op)) {
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
            if (op == op::If) {
                h = std::max(h - 1, 0);
                ty.resize(h);
            }
            const int entry = std::max(h - p, 0);
            std::vector<uint8_t> paramTy;
            for (int i = 0; i < p; ++i)
                paramTy.push_back(static_cast<size_t>(entry + i) < ty.size() ? ty[entry + i] : 0);
            if (op == op::TryTable) {
                if (const auto c = fa.catches.find(addr); c != fa.catches.end()) {
                    for (size_t k = 0; k < ins.catches.size() && k < c->second.size(); ++k) {
                        if (const HFrame *f = frameAt(ins.catches[k].label)) {
                            c->second[k].entry = static_cast<uint32_t>(f->entry);
                            c->second[k].arity = static_cast<uint32_t>(f->branchArity);
                        }
                    }
                }
            }
            frames.push_back({entry, op == op::Loop ? p : r, r, p, std::move(paramTy), std::move(resTy),
                              op == op::TryTable ? addr : kNoTry});
        } else if (op == op::Else) {
            const HFrame &f = frames.back();
            h = f.entry + f.params;
            ty.resize(f.entry, 0);
            ty.insert(ty.end(), f.paramTy.begin(), f.paramTy.end());
        } else if (op == op::Catch || op == op::CatchAll) {
            h = frames.back().entry;
            ty.resize(h);
        } else if (op == op::End || op == op::Delegate) {
            HFrame f = frames.size() > 1 ? frames.back() : frames.front();
            if (frames.size() > 1)
                frames.pop_back();
            h = f.entry + f.endArity;
            ty.resize(f.entry, 0);
            ty.insert(ty.end(), f.resTy.begin(), f.resTy.end());
            fa.heights[slot] = static_cast<uint32_t>(h);
        } else if (op == op::Br || op == op::BrIf || op == op::BrOnNull || op == op::BrOnNonNull) {
            // br_on_null carries the values below the reference, br_on_non_null the reference too
            const int top = op == op::BrOnNull || op == op::BrIf ? h - 1 : h;
            if (const HFrame *f = frameAt(static_cast<uint64_t>(ins.imm0))) {
                auto mv = branchMoves(*f, std::max(top, 0));
                if (!mv.empty())
                    fa.moves[addr] = std::move(mv);
            }
            if (op == op::BrIf || op == op::BrOnNonNull) {
                h = std::max(h - 1, 0);
                ty.resize(h);
            }
        } else if (op == op::BrTable) {
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
        } else if (op == op::Select || op == op::SelectT) {
            const bool vec = h >= 3 && ty[h - 3];
            if (vec)
                fa.vecOps.insert(addr);
            h = std::max(h - 3, 0);
            ty.resize(h);
            ty.push_back(vec);
            ++h;
        } else if (op == op::LocalGet) {
            ty.push_back(vecLocal(ins.imm0));
            ++h;
        } else if (op == op::LocalTee) {
        } else {
            const Effect e = effectOf(m, ins);
            h = std::max(h - e.pops, 0);
            ty.resize(h);
            if (op >= 0xFD00 && op < 0xFE00) {
                if (const auto s = simdSig(op & 0xFF))
                    for (char c : s->pushes)
                        ty.push_back(c == 'v');
            } else if (op == op::Call || op == op::ReturnCall) {
                pushResults(m.funcSig(static_cast<uint32_t>(ins.imm0)));
            } else if (op == op::CallIndirect || op == op::ReturnCallIndirect || op == op::CallRef ||
                       op == op::ReturnCallRef) {
                pushResults(m.sig(static_cast<uint32_t>(ins.imm0)));
            }
            h += e.pushes;
        }
        addr += ins.size;
    }
}

// Tracks global 0 (shadow stack pointer) offset from its entry value; every change becomes `g0 += k`.
void spDeltas(const Module &m, FunctionAnalysis &fa) {
    struct Val {
        enum { Unknown, Sp, Const } kind = Unknown;
        int64_t v = 0;
    };
    std::span<const uint8_t> data = m.data;
    std::vector<Val> st;
    std::array<Val, kLocalLimit> loc{};
    std::unordered_map<uint64_t, int64_t> label;
    int64_t cur = 0;
    bool dead = false;
    uint64_t prev = fa.start;

    const auto pop = [&] {
        if (st.empty())
            return Val{};
        const Val v = st.back();
        st.pop_back();
        return v;
    };
    const auto edge = [&](uint64_t addr, uint64_t target) {
        if (target == kFunctionEnd || target == kUnresolved)
            return;
        const auto [it, fresh] = label.emplace(target, cur);
        (void)fresh;
        if (it->second != cur)
            fa.edgeAdj[addr] = it->second - cur;
    };

    for (size_t i = 0; i < fa.offsets.size(); ++i) {
        const uint64_t addr = fa.start + fa.offsets[i];
        const Instr ins = decode(data, addr);
        const uint32_t op = ins.op;
        st.resize(fa.heights[i]);

        if (const auto it = label.find(addr); it != label.end()) {
            if (!dead && cur != it->second)
                fa.fallAdj[prev] = it->second - cur;
            cur = it->second;
            dead = false;
        } else if (op == op::Loop) {
            label.emplace(addr, cur);
        }

        const uint64_t *target = nullptr;
        if (const auto b = fa.branch.find(addr); b != fa.branch.end())
            target = &b->second;

        if (op == op::If) {
            pop();
            if (target && *target != kFunctionEnd)
                label.emplace(*target, cur);
        } else if (op == op::BrIf) {
            pop();
            if (target)
                edge(addr, *target);
        } else if (op == op::Br) {
            if (target)
                edge(addr, *target);
            dead = true;
        } else if (op == op::BrOnNull || op == op::BrOnNonNull) {
            if (target)
                edge(addr, *target);
        } else if (op == op::BrTable) {
            if (const auto t = fa.tables.find(addr); t != fa.tables.end()) {
                std::vector<int64_t> adj(t->second.size(), 0);
                bool any = false;
                for (size_t k = 0; k < t->second.size(); ++k) {
                    const uint64_t x = t->second[k];
                    if (x == kFunctionEnd || x == kUnresolved)
                        continue;
                    const auto it = label.emplace(x, cur).first;
                    adj[k] = it->second - cur;
                    any = any || adj[k] != 0;
                }
                if (any)
                    fa.tableAdj[addr] = std::move(adj);
            }
            dead = true;
        } else if (op == op::Unreachable || op == op::Return || op == op::ReturnCall ||
                   op == op::ReturnCallIndirect || op == op::ReturnCallRef || op == op::Throw ||
                   op == op::Rethrow || op == op::ThrowRef) {
            dead = true;
        } else if (op == op::TryTable) {
            if (const auto c = fa.catches.find(addr); c != fa.catches.end())
                for (const CatchClause &cl : c->second)
                    if (cl.target != kUnresolved && cl.target != kFunctionEnd)
                        label.emplace(cl.target, cur);
        } else if (op == op::LocalGet) {
            st.push_back(static_cast<size_t>(ins.imm0) < loc.size() ? loc[ins.imm0] : Val{});
        } else if (op == op::LocalSet || op == op::LocalTee) {
            const Val v = pop();
            if (static_cast<size_t>(ins.imm0) < loc.size())
                loc[ins.imm0] = v;
            if (op == op::LocalTee)
                st.push_back(v);
        } else if (op == op::GlobalGet) {
            st.push_back(ins.imm0 == 0 ? Val{Val::Sp, cur} : Val{});
        } else if (op == op::GlobalSet) {
            const Val v = pop();
            if (ins.imm0 == 0 && v.kind == Val::Sp) {
                fa.setAdj[addr] = v.v - cur;
                cur = v.v;
            } else if (ins.imm0 == 0) {
                ++fa.unknownSets;
            }
        } else if (op == op::I32Const) {
            st.push_back({Val::Const, static_cast<int32_t>(ins.imm0)});
        } else if (op == op::I32Add || op == op::I32Sub) {
            const Val b = pop(), a = pop();
            Val r;
            if (a.kind == Val::Sp && b.kind == Val::Const)
                r = {Val::Sp, op == op::I32Add ? a.v + b.v : a.v - b.v};
            else if (op == op::I32Add && a.kind == Val::Const && b.kind == Val::Sp)
                r = {Val::Sp, a.v + b.v};
            st.push_back(r);
        } else if (op >= op::Call && !isBlock(op) && op != op::Else && op != op::End && op != op::Delegate &&
                   op != op::CatchAll && op != op::Catch) {
            const Effect e = effectOf(m, ins);
            for (int k = 0; k < e.pops; ++k)
                pop();
            for (int k = 0; k < e.pushes; ++k)
                st.push_back({});
        }

        if (target && op != op::If && op != op::BrIf && op != op::Br && op != op::BrTable && op != op::BrOnNull &&
            op != op::BrOnNonNull) {
            // else-terminating jump and function-final end
            edge(addr, *target);
            dead = true;
        }

        if (const auto tc = fa.tryChain.find(addr); tc != fa.tryChain.end()) {
            std::vector<int64_t> adj;
            bool any = false;
            for (uint64_t t : tc->second) {
                const auto c = fa.catches.find(t);
                if (c == fa.catches.end())
                    continue;
                for (const CatchClause &cl : c->second) {
                    int64_t d = 0;
                    if (cl.target != kUnresolved && cl.target != kFunctionEnd)
                        d = label.emplace(cl.target, cur).first->second - cur;
                    adj.push_back(d);
                    any = any || d != 0;
                }
            }
            if (any)
                fa.catchAdj[addr] = std::move(adj);
        }
        prev = addr;
    }
}

} // namespace

uint32_t FunctionAnalysis::heightAt(uint64_t addr) const {
    if (addr < start)
        return 0;
    const auto rel = static_cast<uint32_t>(addr - start);
    const auto it = std::lower_bound(offsets.begin(), offsets.end(), rel);
    return it != offsets.end() && *it == rel ? heights[it - offsets.begin()] : 0;
}

const uint64_t *FunctionAnalysis::branchTarget(uint64_t addr) const {
    const auto it = branch.find(addr);
    return it == branch.end() ? nullptr : &it->second;
}

const std::vector<uint64_t> *FunctionAnalysis::table(uint64_t addr) const {
    const auto it = tables.find(addr);
    return it == tables.end() ? nullptr : &it->second;
}

const std::vector<Move> *FunctionAnalysis::movesAt(uint64_t addr) const {
    const auto it = moves.find(addr);
    return it == moves.end() ? nullptr : &it->second;
}

std::shared_ptr<const FunctionAnalysis> analyze(const Module &m, uint64_t start) {
    const auto it = m.addrIndex.find(start);
    if (it == m.addrIndex.end())
        return nullptr;
    auto fa = std::make_shared<FunctionAnalysis>();
    fa->start = start;
    fa->size = m.functions[it->second].size;
    try {
        branchMap(m, *fa);
        heights(m, *fa);
        spDeltas(m, *fa);
    } catch (const DecodeError &) {
        return nullptr;
    }
    return fa;
}

std::shared_ptr<const FunctionAnalysis> AnalysisCache::get(const Module &m, uint64_t addr) {
    const ptrdiff_t i = m.startIndex(addr);
    if (i < 0)
        return nullptr;
    const uint64_t start = m.starts[i];
    {
        std::lock_guard lock(mutex_);
        if (const auto it = entries_.find(start); it != entries_.end())
            return it->second;
    }
    auto fa = analyze(m, start);
    std::lock_guard lock(mutex_);
    if (used_ > budget_) {
        entries_.clear();
        used_ = 0;
    }
    // Failed analyses are cached too, so undecodable bodies are not retried per instruction.
    used_ += fa ? fa->offsets.size() : 1;
    entries_[start] = fa;
    return fa;
}

void AnalysisCache::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    used_ = 0;
}

} // namespace wasm
