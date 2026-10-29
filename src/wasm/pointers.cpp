//
// Created by Dottik on 28/10/2026.
//

#include "wasm/pointers.hpp"

#include <algorithm>
#include <array>

#include "wasm/analysis.hpp"
#include "wasm/decode.hpp"
#include "wasm/opcodes.hpp"

namespace wasm {

namespace {

struct Graph {
    std::vector<uint32_t> paramBase;  // node id of each function's first param
    uint32_t retBase = 0;             // node id of function 0's result
    std::vector<char> ptr;
    std::vector<std::pair<uint32_t, uint32_t>> flows;  // value flows; propagated in both directions

    uint32_t param(uint32_t f, uint32_t i) const { return paramBase[f] + i; }
    uint32_t ret(uint32_t f) const { return retBase + f; }
};

void scan(const Module &m, Graph &g, uint32_t f) {
    const auto fa = analyze(m, *m.functions[f].address);
    if (!fa)
        return;
    const FuncType *sig = m.funcSig(f);
    constexpr int32_t kNone = -1;
    std::vector<int32_t> st;
    std::array<int32_t, kLocalLimit> loc;
    loc.fill(kNone);
    if (sig)
        for (size_t i = 0; i < sig->params.size() && i < loc.size(); ++i)
            if (sig->params[i] == kI32)
                loc[i] = static_cast<int32_t>(g.param(f, static_cast<uint32_t>(i)));
    const bool retI32 = sig && !sig->results.empty() && sig->results[0] == kI32;

    const auto pop = [&] {
        if (st.empty())
            return kNone;
        const int32_t v = st.back();
        st.pop_back();
        return v;
    };
    const auto use = [&](int32_t node) {
        if (node >= 0)
            g.ptr[node] = 1;
    };
    const auto flow = [&](int32_t src, uint32_t dst) {
        if (src >= 0)
            g.flows.emplace_back(static_cast<uint32_t>(src), dst);
    };

    for (size_t i = 0; i < fa->offsets.size(); ++i) {
        const uint64_t addr = fa->start + fa->offsets[i];
        const Instr ins = decode(m.data, addr);
        const uint32_t op = ins.op;
        st.resize(fa->heights[i], kNone);

        if (op >= op::LoadFirst && op <= op::LoadLast) {
            use(pop());
            st.push_back(kNone);
        } else if (op >= op::StoreFirst && op <= op::StoreLast) {
            pop();
            use(pop());
        } else if (op == op::V128Load) {
            use(pop());
            st.push_back(kNone);
        } else if (op == op::V128Store) {
            pop();
            use(pop());
        } else if (op == op::LocalGet) {
            st.push_back(static_cast<size_t>(ins.imm0) < loc.size() ? loc[ins.imm0] : kNone);
        } else if (op == op::LocalSet || op == op::LocalTee) {
            const int32_t v = pop();
            if (static_cast<size_t>(ins.imm0) < loc.size())
                loc[ins.imm0] = v;
            if (op == op::LocalTee)
                st.push_back(v);
        } else if (op == op::I32Add || op == op::I32Sub) {
            const int32_t b = pop(), a = pop();
            st.push_back(a >= 0 ? a : (op == op::I32Add ? b : kNone));
        } else if (op == op::Call || op == op::ReturnCall) {
            const auto callee = static_cast<uint32_t>(ins.imm0);
            const FuncType *cs = m.funcSig(callee);
            if (cs) {
                std::vector<int32_t> args(cs->params.size(), kNone);
                for (size_t k = args.size(); k-- > 0;)
                    args[k] = pop();
                for (size_t k = 0; k < args.size(); ++k)
                    if (cs->params[k] == kI32)
                        flow(args[k], g.param(callee, static_cast<uint32_t>(k)));
                for (size_t k = 0; k < cs->results.size(); ++k)
                    st.push_back(k == 0 && cs->results[0] == kI32 ? static_cast<int32_t>(g.ret(callee)) : kNone);
            }
            if (op == op::ReturnCall && retI32 && !st.empty())
                flow(st.back(), g.ret(f));
        } else if (op == op::Return) {
            if (retI32 && !st.empty())
                flow(st.back(), g.ret(f));
        } else if (op == op::End) {
            const auto b = fa->branch.find(addr);
            if (b != fa->branch.end() && b->second == kFunctionEnd && retI32 && !st.empty())
                flow(st.back(), g.ret(f));
        } else if (op == op::If || op == op::BrIf || op == op::BrTable) {
            pop();
        } else if (op >= op::Call && !isBlock(op) && op != op::Else && op != op::Catch && op != op::Delegate &&
                   op != op::CatchAll && op != op::CallIndirect && op != op::ReturnCallIndirect) {
            const Effect e = fixedEffect(op).value_or(Effect{0, 0});
            for (int k = 0; k < e.pops; ++k)
                pop();
            for (int k = 0; k < e.pushes; ++k)
                st.push_back(kNone);
        } else if (op == op::CallIndirect || op == op::ReturnCallIndirect) {
            const FuncType *cs = m.sig(static_cast<uint32_t>(ins.imm0));
            pop();
            if (cs) {
                for (size_t k = 0; k < cs->params.size(); ++k)
                    pop();
                for (size_t k = 0; k < cs->results.size(); ++k)
                    st.push_back(kNone);
            }
        }
    }
}

} // namespace

PtrInfo inferPointers(const Module &m) {
    Graph g;
    uint32_t nodes = 0;
    g.paramBase.reserve(m.functions.size());
    for (uint32_t f = 0; f < m.functions.size(); ++f) {
        g.paramBase.push_back(nodes);
        const FuncType *s = m.funcSig(f);
        nodes += s ? static_cast<uint32_t>(s->params.size()) : 0;
    }
    g.retBase = nodes;
    nodes += static_cast<uint32_t>(m.functions.size());
    g.ptr.assign(nodes, 0);

    for (uint32_t f = 0; f < m.functions.size(); ++f)
        if (m.functions[f].address)
            scan(m, g, f);

    for (bool changed = true; changed;) {
        changed = false;
        for (const auto &[a, b] : g.flows)
            if (g.ptr[a] != g.ptr[b]) {
                g.ptr[a] = g.ptr[b] = 1;
                changed = true;
            }
    }

    PtrInfo out;
    out.param.resize(m.functions.size());
    out.ret.resize(m.functions.size());
    for (uint32_t f = 0; f < m.functions.size(); ++f) {
        const FuncType *s = m.funcSig(f);
        const size_t n = s ? s->params.size() : 0;
        for (size_t i = 0; i < n; ++i)
            out.param[f].push_back(g.ptr[g.param(f, static_cast<uint32_t>(i))]);
        out.ret[f] = g.ptr[g.ret(f)];
    }
    return out;
}

} // namespace wasm
