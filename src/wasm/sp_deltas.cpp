//
// Created by Dottik on 28/10/2026.
//

#include <array>

#include "wasm/decode.hpp"
#include "wasm/passes.hpp"

namespace wasm::pass {

struct SpVal {
    enum Kind { Unknown, Sp, Const } kind = Unknown;
    int64_t v = 0;
};

// Tracks global 0 (shadow stack pointer) offset from its entry value; every change becomes `g0 += k`.
void spDeltas(const Module &m, FunctionAnalysis &fa) {
    std::span<const uint8_t> data = m.data;
    std::vector<SpVal> st;
    std::array<SpVal, kLocalLimit> loc{};
    std::unordered_map<uint64_t, int64_t> label;
    int64_t cur = 0;
    bool dead = false;
    uint64_t prev = fa.start;

    const auto pop = [&] {
        if (st.empty())
            return SpVal{};
        const SpVal v = st.back();
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
        const uint32_t o = ins.op;
        st.resize(fa.heights[i]);

        if (const auto it = label.find(addr); it != label.end()) {
            if (!dead && cur != it->second)
                fa.fallAdj[prev] = it->second - cur;
            cur = it->second;
            dead = false;
        } else if (o == op::Loop) {
            label.emplace(addr, cur);
        }

        const uint64_t *target = nullptr;
        if (const auto b = fa.branch.find(addr); b != fa.branch.end())
            target = &b->second;

        if (o == op::If) {
            pop();
            if (target && *target != kFunctionEnd)
                label.emplace(*target, cur);
        } else if (o == op::BrIf) {
            pop();
            if (target)
                edge(addr, *target);
        } else if (o == op::Br) {
            if (target)
                edge(addr, *target);
            dead = true;
        } else if (o == op::BrOnNull || o == op::BrOnNonNull) {
            if (target)
                edge(addr, *target);
        } else if (o == op::BrTable) {
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
        } else if (o == op::Unreachable || o == op::Return || o == op::ReturnCall || o == op::ReturnCallIndirect ||
                   o == op::ReturnCallRef || o == op::Throw || o == op::Rethrow || o == op::ThrowRef) {
            dead = true;
        } else if (o == op::TryTable) {
            if (const auto c = fa.catches.find(addr); c != fa.catches.end())
                for (const CatchClause &cl : c->second)
                    if (cl.target != kUnresolved && cl.target != kFunctionEnd)
                        label.emplace(cl.target, cur);
        } else if (o == op::LocalGet) {
            st.push_back(static_cast<size_t>(ins.imm0) < loc.size() ? loc[ins.imm0] : SpVal{});
        } else if (o == op::LocalSet || o == op::LocalTee) {
            const SpVal v = pop();
            if (static_cast<size_t>(ins.imm0) < loc.size())
                loc[ins.imm0] = v;
            if (o == op::LocalTee)
                st.push_back(v);
        } else if (o == op::GlobalGet) {
            st.push_back(ins.imm0 == 0 ? SpVal{SpVal::Sp, cur} : SpVal{});
        } else if (o == op::GlobalSet) {
            const SpVal v = pop();
            if (ins.imm0 == 0 && v.kind == SpVal::Sp) {
                fa.setAdj[addr] = v.v - cur;
                cur = v.v;
            } else if (ins.imm0 == 0) {
                ++fa.unknownSets;
            }
        } else if (o == op::I32Const) {
            st.push_back({SpVal::Const, static_cast<int32_t>(ins.imm0)});
        } else if (o == op::I32Add || o == op::I32Sub) {
            const SpVal b = pop(), a = pop();
            SpVal r;
            if (a.kind == SpVal::Sp && b.kind == SpVal::Const)
                r = {SpVal::Sp, o == op::I32Add ? a.v + b.v : a.v - b.v};
            else if (o == op::I32Add && a.kind == SpVal::Const && b.kind == SpVal::Sp)
                r = {SpVal::Sp, a.v + b.v};
            st.push_back(r);
        } else if (o >= op::Call && !isBlock(o) && o != op::Else && o != op::End && o != op::Delegate &&
                   o != op::CatchAll && o != op::Catch) {
            const Effect e = effectOf(m, ins);
            for (int k = 0; k < e.pops; ++k)
                pop();
            for (int k = 0; k < e.pushes; ++k)
                st.push_back({});
        }

        if (target && o != op::If && o != op::BrIf && o != op::Br && o != op::BrTable && o != op::BrOnNull &&
            o != op::BrOnNonNull) {
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

} // namespace wasm::pass
