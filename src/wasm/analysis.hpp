//
// Created by Dottik on 28/10/2026.
//

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wasm/module.hpp"

namespace wasm {

constexpr uint64_t kFunctionEnd = ~0ull;      // branch target of the function-final `end` (return)
constexpr uint64_t kUnresolved = ~0ull - 1;   // br_table label with no enclosing frame

struct Move {
    uint32_t src, dst;  // operand stack slots
    bool vec = false;   // the value is a v128
};

struct CatchClause {
    uint8_t kind;        // CatchSpec::kind
    uint64_t tag;
    uint64_t target = kUnresolved;
    uint32_t entry = 0;  // first stack slot of the label's values
    uint32_t arity = 0;  // values the clause delivers to the label
};

// Everything the architecture needs about one function body. Addresses are file offsets.
struct FunctionAnalysis {
    uint64_t start = 0;
    uint64_t size = 0;
    std::vector<uint32_t> offsets;  // instruction offsets from `start`, ascending
    std::vector<uint32_t> heights;  // operand stack height before each instruction

    std::unordered_map<uint64_t, uint64_t> branch;                 // instruction -> target
    std::unordered_map<uint64_t, std::vector<uint64_t>> tables;    // br_table -> targets + default
    std::unordered_map<uint64_t, std::vector<Move>> moves;         // br/br_if/br_on_* -> value moves
    std::unordered_map<uint64_t, std::vector<std::vector<Move>>> tableMoves;  // br_table -> moves per label
    std::unordered_set<uint64_t> vecOps;                           // selects whose operands are v128

    std::unordered_map<uint64_t, std::vector<CatchClause>> catches;  // try_table -> clauses
    // call / throw inside try_tables -> the enclosing try_table addresses, innermost first
    std::unordered_map<uint64_t, std::vector<uint64_t>> tryChain;

    // Shadow-stack (global 0) bookkeeping, as adjustments relative to the tracked entry-relative delta:
    // setAdj   global.set 0 -> amount added to g0
    // edgeAdj  taken branch edge -> amount added to g0 so the target sees a single delta
    // fallAdj  fallthrough into a label -> same, applied after the instruction
    std::unordered_map<uint64_t, int64_t> setAdj, edgeAdj, fallAdj;
    // br_table -> per-label amount; throwing instruction -> per-clause amount over its tryChain
    std::unordered_map<uint64_t, std::vector<int64_t>> tableAdj, catchAdj;
    size_t unknownSets = 0;  // global.set 0 of a value that is not entry-relative

    static int64_t lookup(const std::unordered_map<uint64_t, int64_t> &m, uint64_t addr) {
        const auto it = m.find(addr);
        return it == m.end() ? 0 : it->second;
    }

    // Stack height before the instruction at `addr`, or 0 if unknown.
    uint32_t heightAt(uint64_t addr) const;
    const uint64_t *branchTarget(uint64_t addr) const;
    const std::vector<uint64_t> *table(uint64_t addr) const;
    const std::vector<Move> *movesAt(uint64_t addr) const;
};

// Returns nullptr when the body does not decode.
std::shared_ptr<const FunctionAnalysis> analyze(const Module &m, uint64_t start);

// Thread-safe, size-bounded cache of per-function analyses.
class AnalysisCache {
public:
    explicit AnalysisCache(size_t maxInstructions = 1'000'000) : budget_(maxInstructions) {}
    // Analysis of the function containing `addr`; nullptr if outside code or undecodable.
    std::shared_ptr<const FunctionAnalysis> get(const Module &m, uint64_t addr);
    void clear();

private:
    std::mutex mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<const FunctionAnalysis>> entries_;
    size_t budget_;
    size_t used_ = 0;
};

} // namespace wasm
