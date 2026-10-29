//
// Created by Dottik on 28/10/2026.
//

#include "wasm/decode.hpp"
#include "wasm/passes.hpp"

namespace wasm::pass {

enum class FrameKind { Block, Loop, If, Else };

struct Frame {
    FrameKind kind;
    uint64_t begin;
    std::vector<std::pair<uint64_t, int>> pending;  // (branch address, br_table slot or -1; <= -2 = catch clause)
    uint64_t elsePrev = 0;
};

constexpr int kBranchSlot = -1;
constexpr int catchSlot(size_t clause) { return -static_cast<int>(clause) - 2; }
constexpr size_t catchClause(int slot) { return static_cast<size_t>(-slot - 2); }

// Branches to a loop resolve at once to its start; to any other frame, when it ends.
void branchMap(const Module &m, FunctionAnalysis &fa) {
    std::span<const uint8_t> data = m.data;
    std::vector<Frame> stack;
    stack.push_back({FrameKind::Block, fa.start, {}, 0});
    uint64_t addr = fa.start, prev = fa.start;
    const uint64_t end = fa.start + fa.size;
    const auto frameAt = [&](uint64_t label) -> Frame * {
        return label < stack.size() ? &stack[stack.size() - 1 - label] : nullptr;
    };

    while (addr < end) {
        const Instr ins = decode(data, addr);
        const uint32_t o = ins.op;
        if (o == op::TryTable) {
            // catch labels count from outside the try_table
            auto &clauses = fa.catches[addr];
            for (size_t k = 0; k < ins.catches.size(); ++k) {
                const CatchSpec &c = ins.catches[k];
                clauses.push_back({c.kind, c.tag});
                if (Frame *f = frameAt(c.label)) {
                    if (f->kind == FrameKind::Loop)
                        clauses.back().target = f->begin;
                    else
                        f->pending.push_back({addr, catchSlot(k)});
                }
            }
            stack.push_back({FrameKind::Block, addr, {}, 0});
        } else if (o == op::Block || o == op::Try) {
            stack.push_back({FrameKind::Block, addr, {}, 0});
        } else if (o == op::Loop) {
            stack.push_back({FrameKind::Loop, addr, {}, 0});
        } else if (o == op::If) {
            stack.push_back({FrameKind::If, addr, {}, 0});
        } else if (o == op::Else && stack.back().kind == FrameKind::If) {
            Frame &f = stack.back();
            fa.branch[f.begin] = addr;
            f.kind = FrameKind::Else;
            f.elsePrev = prev;
        } else if (o == op::Br || o == op::BrIf || o == op::BrOnNull || o == op::BrOnNonNull) {
            if (Frame *f = frameAt(static_cast<uint64_t>(ins.imm0))) {
                if (f->kind == FrameKind::Loop)
                    fa.branch[addr] = f->begin;
                else
                    f->pending.push_back({addr, kBranchSlot});
            }
        } else if (o == op::BrTable) {
            const auto labels = tableLabels(data, addr);
            auto &targets = fa.tables[addr];
            targets.assign(labels.size(), kUnresolved);
            for (size_t k = 0; k < labels.size(); ++k) {
                if (Frame *f = frameAt(labels[k])) {
                    if (f->kind == FrameKind::Loop)
                        targets[k] = f->begin;
                    else
                        f->pending.push_back({addr, static_cast<int>(k)});
                }
            }
        } else if (o == op::End || o == op::Delegate) {
            Frame f = std::move(stack.back());
            stack.pop_back();
            if (f.kind == FrameKind::If)
                fa.branch[f.begin] = addr + ins.size;
            else if (f.kind == FrameKind::Else)
                fa.branch.emplace(f.elsePrev, addr + ins.size);
            for (const auto &[a, slot] : f.pending) {
                if (slot >= 0)
                    fa.tables[a][slot] = addr;
                else if (slot == kBranchSlot)
                    fa.branch[a] = addr;
                else
                    fa.catches[a][catchClause(slot)].target = addr;
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

} // namespace wasm::pass
