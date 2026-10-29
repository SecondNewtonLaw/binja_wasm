//
// Created by Dottik on 28/10/2026.
//

#include "wasm/analysis.hpp"

#include <algorithm>

#include "wasm/decode.hpp"
#include "wasm/passes.hpp"

namespace wasm {

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
        pass::branchMap(m, *fa);
        pass::heights(m, *fa);
        pass::spDeltas(m, *fa);
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
