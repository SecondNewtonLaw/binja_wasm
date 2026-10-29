//
// Created by Dottik on 28/10/2026.
//

#pragma once

#include "wasm/analysis.hpp"
#include "wasm/opcodes.hpp"

// The passes behind analyze(), in dependency order: each reads what the previous ones filled in.
namespace wasm::pass {

// Fills branch, tables and the catch targets of catches.
void branchMap(const Module &m, FunctionAnalysis &fa);

// Fills offsets, heights, moves, tableMoves, vecOps, tryChain and the stack layout of catches.
void heights(const Module &m, FunctionAnalysis &fa);

// Fills setAdj, edgeAdj, fallAdj, tableAdj, catchAdj and unknownSets.
void spDeltas(const Module &m, FunctionAnalysis &fa);

inline Effect effectOf(const Module &m, const Instr &ins) {
    const uint32_t o = ins.op;
    if (o == op::Call || o == op::ReturnCall) {
        const FuncType *s = m.funcSig(static_cast<uint32_t>(ins.imm0));
        return s ? Effect{static_cast<int>(s->params.size()), static_cast<int>(s->results.size())} : Effect{0, 0};
    }
    if (o == op::CallIndirect || o == op::ReturnCallIndirect || o == op::CallRef || o == op::ReturnCallRef) {
        const FuncType *s = m.sig(static_cast<uint32_t>(ins.imm0));
        return s ? Effect{static_cast<int>(s->params.size()) + 1, static_cast<int>(s->results.size())} : Effect{1, 0};
    }
    if (o >= kPrefixFC && o < kPrefixFD)
        return fixedEffect(o).value_or(Effect{1, 1});
    return fixedEffect(o).value_or(Effect{0, 0});
}

} // namespace wasm::pass
