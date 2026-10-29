//
// Created by Dottik on 28/10/2026.
//

#undef NDEBUG
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <iterator>
#include <vector>

#include "wasm/analysis.hpp"
#include "wasm/decode.hpp"
#include "wasm/module.hpp"
#include "wasm/pointers.hpp"

static void handmade() {
    // type ()->(), one function: block; br 0; end; end
    const std::vector<uint8_t> bytes = {
        0x00, 'a', 's', 'm', 1, 0, 0, 0,
        1, 4, 1, 0x60, 0, 0,
        3, 2, 1, 0,
        10, 9, 1, 7, 0, 0x02, 0x40, 0x0C, 0x00, 0x0B, 0x0B,
    };
    auto m = wasm::parseModule(bytes);
    assert(m && m->functions.size() == 1 && m->functions[0].address == 23);
    assert(m->functions[0].name == "sub_17");

    wasm::AnalysisCache cache;
    const auto fa = cache.get(*m, 25);
    assert(fa);
    assert(*fa->branchTarget(25) == 27);
    assert(*fa->branchTarget(28) == wasm::kFunctionEnd);
    assert(fa->heightAt(25) == 0);
}

struct Fingerprint {
    uint64_t sum = 0;

    static uint64_t mix(uint64_t h, uint64_t v) {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h * 0xFF51AFD7ED558CCDull;
    }
    // Entry hashes are summed so unordered_map iteration order does not matter.
    template <typename Map, typename Fn> void map(uint64_t tag, const Map &m, Fn entry) {
        for (const auto &[k, v] : m)
            sum += entry(mix(tag, static_cast<uint64_t>(k)), v);
    }
    static uint64_t hashMoves(uint64_t h, const std::vector<wasm::Move> &mv) {
        for (const auto &x : mv)
            h = mix(mix(mix(h, x.src), x.dst), x.vec);
        return h;
    }

    void add(const wasm::FunctionAnalysis &fa) {
        sum += mix(mix(fa.start, fa.size), fa.unknownSets);
        for (size_t i = 0; i < fa.offsets.size(); ++i)
            sum += mix(mix(1, fa.offsets[i]), fa.heights[i]);
        map(2, fa.branch, [](uint64_t h, uint64_t v) { return mix(h, v); });
        map(3, fa.tables, [](uint64_t h, const std::vector<uint64_t> &v) {
            for (uint64_t x : v)
                h = mix(h, x);
            return h;
        });
        map(4, fa.moves, [](uint64_t h, const std::vector<wasm::Move> &v) { return hashMoves(h, v); });
        map(5, fa.tableMoves, [](uint64_t h, const std::vector<std::vector<wasm::Move>> &v) {
            for (const auto &mv : v)
                h = hashMoves(mix(h, 99), mv);
            return h;
        });
        for (uint64_t a : fa.vecOps)
            sum += mix(6, a);
        map(7, fa.catches, [](uint64_t h, const std::vector<wasm::CatchClause> &v) {
            for (const auto &c : v)
                h = mix(mix(mix(mix(mix(h, c.kind), c.tag), c.target), c.entry), c.arity);
            return h;
        });
        map(8, fa.tryChain, [](uint64_t h, const std::vector<uint64_t> &v) {
            for (uint64_t x : v)
                h = mix(h, x);
            return h;
        });
        const auto adj = [](uint64_t h, int64_t v) { return mix(h, static_cast<uint64_t>(v)); };
        map(9, fa.setAdj, adj);
        map(10, fa.edgeAdj, adj);
        map(11, fa.fallAdj, adj);
        const auto adjVec = [](uint64_t h, const std::vector<int64_t> &v) {
            for (int64_t x : v)
                h = mix(h, static_cast<uint64_t>(x));
            return h;
        };
        map(12, fa.tableAdj, adjVec);
        map(13, fa.catchAdj, adjVec);
    }
};

static void realModule(const char *path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto m = wasm::parseModule(std::move(bytes));
    assert(m);
    size_t instrs = 0, failed = 0, sets = 0, unknownSets = 0, edges = 0, reconciled = 0;
    size_t tblMoves = 0, tblAdj = 0, vecSel = 0, tries = 0, throwing = 0;
    Fingerprint fp;
    for (const uint64_t start : m->starts) {
        const auto fa = wasm::analyze(*m, start);
        if (!fa) {
            ++failed;
            continue;
        }
        fp.add(*fa);
        instrs += fa->offsets.size();
        sets += fa->setAdj.size();
        unknownSets += fa->unknownSets;
        edges += fa->edgeAdj.size() + fa->fallAdj.size();
        reconciled += !fa->edgeAdj.empty() || !fa->fallAdj.empty();
        tblMoves += fa->tableMoves.size();
        tblAdj += fa->tableAdj.size();
        vecSel += fa->vecOps.size();
        tries += fa->catches.size();
        throwing += fa->tryChain.size();
    }
    if (const char *dbg = std::getenv("WASM_DBG")) {
        const uint64_t at = std::strtoull(dbg, nullptr, 16);
        for (const uint64_t start : m->starts) {
            const auto fa = wasm::analyze(*m, start);
            if (!fa || at < fa->start || at >= fa->start + fa->size)
                continue;
            std::printf("func %llx size=%llu unknownSets=%zu\n", (unsigned long long)fa->start,
                        (unsigned long long)fa->size, fa->unknownSets);
            for (size_t i = 0; i < fa->offsets.size(); ++i) {
                const uint64_t a = fa->start + fa->offsets[i];
                const auto ins = wasm::decode(m->data, a);
                if ((ins.op == 0x24 || ins.op == 0x23) && ins.imm0 == 0)
                    std::printf(" g%s0 at %llx h=%u\n", ins.op == 0x24 ? "set" : "get", (unsigned long long)a,
                                fa->heights[i]);
            }
            if (std::getenv("WASM_OPS")) {
                std::map<uint32_t, size_t> hist;
                for (size_t i = 0; i < fa->offsets.size(); ++i)
                    ++hist[wasm::decode(m->data, fa->start + fa->offsets[i]).op];
                for (auto &[op, n] : hist) std::printf(" op %x x%zu\n", op, n);
            }
            for (auto &[a, k] : fa->setAdj) std::printf(" set %llx %+lld\n", (unsigned long long)a, (long long)k);
            for (auto &[a, k] : fa->edgeAdj) std::printf(" edge %llx %+lld\n", (unsigned long long)a, (long long)k);
            for (auto &[a, k] : fa->fallAdj) std::printf(" fall %llx %+lld\n", (unsigned long long)a, (long long)k);
            for (auto &[a, t] : fa->tables) std::printf(" table %llx n=%zu\n", (unsigned long long)a, t.size());
            for (auto &[a, t] : fa->branch)
                if (t == at) std::printf(" branch->%llx from %llx\n", (unsigned long long)t, (unsigned long long)a);
        }
    }
    std::printf("functions=%zu entries=%zu instrs=%zu failed=%zu\n", m->functions.size(), m->entries.size(), instrs,
                failed);
    const auto ptrs = wasm::inferPointers(*m);
    size_t pp = 0, pr = 0, params = 0;
    for (size_t f = 0; f < ptrs.param.size(); ++f) {
        for (char c : ptrs.param[f])
            pp += c, ++params;
        pr += ptrs.ret[f];
    }
    std::printf("ptr: params=%zu/%zu rets=%zu/%zu\n", pp, params, pr, ptrs.ret.size());
    for (size_t f = 0; f < ptrs.param.size(); ++f) {
        for (char c : ptrs.param[f])
            fp.sum += Fingerprint::mix(f, c);
        fp.sum += Fingerprint::mix(~f, ptrs.ret[f]);
    }
    std::printf("fingerprint=%016llx\n", static_cast<unsigned long long>(fp.sum));
    std::printf("sp: tracked_sets=%zu unknown_sets=%zu reconcile_edges=%zu functions_reconciled=%zu\n", sets,
                unknownSets, edges, reconciled);
    std::printf("br_table: moves=%zu spadj=%zu  vec_select=%zu  try_table=%zu throwing=%zu\n", tblMoves, tblAdj,
                vecSel, tries, throwing);
}

int main(int argc, char **argv) {
    const std::array<uint8_t, 3> leb = {0xE5, 0x8E, 0x26}; // 624485
    size_t pos = 0;
    assert(wasm::readUleb(leb, pos) == 624485 && pos == 3);

    const std::array<uint8_t, 1> truncated = {0x80};
    pos = 0;
    assert(!wasm::readUleb(truncated, pos));

    const std::vector<uint8_t> bad = {0x7F, 'E', 'L', 'F', 1, 0, 0, 0};
    assert(!wasm::parseModule(bad));

    handmade();
    if (argc > 1)
        realModule(argv[1]);
    return 0;
}
