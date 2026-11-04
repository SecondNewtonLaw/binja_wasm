//
// wasm_lift_probe: headless lift-quality report for a WASM module.
//
//   wasm_lift_probe module.wasm [--json] [--check] [--limit N]
//                               [--dump <file-offset-in-hex>] [--quiet]
//
// It runs the same lifter as the Binary Ninja plugin against the partial IL in
// tests/support, so it can be used in CI and on real modules without a BN
// install or license.
//
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "il_interp.hpp"
#include "lift_runner.hpp"
#include "partial_il.hpp"

namespace {

    struct FunctionReport {
        uint64_t start = 0;
        uint64_t size = 0;
        size_t wasmInstrs = 0;
        size_t ilInstrs = 0;
        size_t unimplemented = 0;
        size_t nops = 0;
        size_t intrinsics = 0;
        size_t loads = 0;
        size_t stores = 0;
        size_t calls = 0;
        size_t spills = 0;
        size_t errors = 0;
        size_t nativeIntArith = 0;
        size_t nativeFloat = 0;
        size_t nativeCompare = 0;
        size_t unknownSets = 0;
        size_t unresolvedTables = 0;
        std::map<std::string, size_t> unimplementedOps;
        std::map<std::string, size_t> intrinsicCounts;
        std::vector<std::string> messages;
    };

    uint64_t parseNumber(const std::string &s) {
        if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0)
            return std::strtoull(s.c_str() + 2, nullptr, 16);
        return std::strtoull(s.c_str(), nullptr, 16); // addresses default to hex
    }

    std::string jsonEscape(const std::string &s) {
        std::string out;
        for (char c : s) {
            switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
                break;
            }
        }
        return out;
    }

    void printJson(const std::vector<FunctionReport> &fns, size_t analyzed, size_t failed) {
        std::cout << "{\n";
        std::cout << "  \"functions\": " << fns.size() << ",\n";
        std::cout << "  \"analyzed\": " << analyzed << ",\n";
        std::cout << "  \"failed\": " << failed << ",\n";
        std::cout << "  \"reports\": [\n";
        for (size_t i = 0; i < fns.size(); ++i) {
            const auto &f = fns[i];
            std::cout << "    {\"start\": \"0x" << std::hex << f.start << std::dec << "\", \"size\": " << f.size << ", \"wasm_instrs\": " << f.wasmInstrs
                      << ", \"il_instrs\": " << f.ilInstrs << ", \"unimplemented\": " << f.unimplemented << ", \"nops\": " << f.nops
                      << ", \"intrinsics\": " << f.intrinsics << ", \"loads\": " << f.loads << ", \"stores\": " << f.stores << ", \"calls\": " << f.calls
                      << ", \"spills\": " << f.spills << ", \"errors\": " << f.errors << ", \"native_int_arith\": " << f.nativeIntArith
                      << ", \"native_float\": " << f.nativeFloat << ", \"native_compare\": " << f.nativeCompare << ", \"unknown_sets\": " << f.unknownSets
                      << ", \"unresolved_tables\": " << f.unresolvedTables << ", \"unimplemented_ops\": {";
            bool first = true;
            for (const auto &[k, v] : f.unimplementedOps) {
                if (!first)
                    std::cout << ", ";
                first = false;
                std::cout << "\"" << jsonEscape(k) << "\": " << v;
            }
            std::cout << "}, \"intrinsic_counts\": {";
            first = true;
            for (const auto &[k, v] : f.intrinsicCounts) {
                if (!first)
                    std::cout << ", ";
                first = false;
                std::cout << "\"" << jsonEscape(k) << "\": " << v;
            }
            std::cout << "}}";
            if (i + 1 != fns.size())
                std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "  ]\n}\n";
    }

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(
            stderr, "usage: wasm_lift_probe <module.wasm> [--json] [--check] [--limit N] "
                    "[--dump <hex>] [--quiet]\n"
        );
        return 2;
    }
    const std::string path = argv[1];
    bool json = false, check = false, quiet = false, list = false;
    size_t limit = 0;
    uint64_t dumpAddr = ~0ull;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json")
            json = true;
        else if (a == "--check")
            check = true;
        else if (a == "--quiet")
            quiet = true;
        else if (a == "--list")
            list = true;
        else if (a == "--limit" && i + 1 < argc)
            limit = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 0));
        else if (a == "--dump" && i + 1 < argc)
            dumpAddr = parseNumber(argv[++i]);
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return 2;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto module = wasm::parseModule(std::move(bytes));
    if (!module) {
        std::fprintf(stderr, "%s: not a WebAssembly module\n", path.c_str());
        return 2;
    }

    if (list) {
        for (uint64_t start : module->starts) {
            const auto &f = module->functions[module->addrIndex.at(start)];
            std::printf("%-24s start=0x%llx size=%llu\n", f.name.c_str(), (unsigned long long)start, (unsigned long long)f.size);
        }
        return 0;
    }

    lift_test::LiftHarness harness(*module);
    std::vector<FunctionReport> reports;
    size_t analyzed = 0, failed = 0;

    size_t n = 0;
    for (uint64_t start : module->starts) {
        if (limit && n++ >= limit)
            break;
        FunctionReport rep;
        rep.start = start;
        rep.size = module->functions[module->addrIndex.at(start)].size;
        const auto *fn = harness.tryLift(start);
        if (!fn) {
            ++failed;
            rep.messages.push_back("analysis or lift failed");
            reports.push_back(std::move(rep));
            continue;
        }
        ++analyzed;
        rep.wasmInstrs = fn->analysis ? fn->analysis->offsets.size() : 0;
        const auto stats = BinaryNinja::analyzeIl(fn->il);
        rep.ilInstrs = stats.instructions;
        rep.unimplemented = stats.unimplemented;
        rep.nops = stats.nops;
        rep.intrinsics = stats.intrinsics;
        rep.loads = stats.loads;
        rep.stores = stats.stores;
        rep.calls = stats.calls;
        rep.spills = stats.spills;
        rep.errors = stats.errors.size();
        rep.nativeIntArith = stats.nativeIntArith;
        rep.nativeFloat = stats.nativeFloat;
        rep.nativeCompare = stats.nativeCompare;
        rep.intrinsicCounts = stats.intrinsicCounts;
        if (fn->analysis) {
            rep.unknownSets = fn->analysis->unknownSets;
            for (const auto &[at, targets] : fn->analysis->tables) {
                (void)at;
                for (uint64_t t : targets)
                    if (t == wasm::kUnresolved)
                        ++rep.unresolvedTables;
            }
        }
        rep.messages = stats.errors;

        // Map each unimplemented IL instruction back to its source opcode.
        for (const auto &item : fn->il.items()) {
            if (item.kind != BinaryNinja::Item::Kind::Instruction)
                continue;
            const auto &e = fn->il.expr(item.expr);
            if (e.op != BinaryNinja::ILOp::Unimplemented)
                continue;
            try {
                const auto ins = wasm::decode(module->data, item.addr);
                ++rep.unimplementedOps[wasm::instrName(ins.op)];
            } catch (const std::exception &) {
                ++rep.unimplementedOps["<decode>"];
            }
        }

        if (!json && dumpAddr != ~0ull && dumpAddr >= start && dumpAddr < start + rep.size) {
            std::cout << "=== function 0x" << std::hex << start << " ===" << std::dec << "\n";
            std::cout << BinaryNinja::printIl(fn->il) << "\n";
        }
        reports.push_back(std::move(rep));
    }

    if (json) {
        printJson(reports, analyzed, failed);
        return 0;
    }

    size_t totalUnimpl = 0, totalErrors = 0, totalIl = 0, totalWasm = 0;
    size_t totalNativeInt = 0, totalNativeFloat = 0, totalNativeCmp = 0, totalSpills = 0;
    size_t totalUnknownSets = 0, totalUnresolvedTables = 0;
    std::map<std::string, size_t> unimplOps;
    std::map<std::string, size_t> intrinsicCounts;
    for (const auto &f : reports) {
        totalUnimpl += f.unimplemented;
        totalErrors += f.errors;
        totalIl += f.ilInstrs;
        totalWasm += f.wasmInstrs;
        totalNativeInt += f.nativeIntArith;
        totalNativeFloat += f.nativeFloat;
        totalNativeCmp += f.nativeCompare;
        totalSpills += f.spills;
        totalUnknownSets += f.unknownSets;
        totalUnresolvedTables += f.unresolvedTables;
        for (const auto &[k, v] : f.unimplementedOps)
            unimplOps[k] += v;
        for (const auto &[k, v] : f.intrinsicCounts)
            intrinsicCounts[k] += v;
    }

    if (!quiet) {
        std::printf("module: %s\n", path.c_str());
        std::printf("functions: %zu (lifted %zu, failed %zu)\n", reports.size(), analyzed, failed);
        std::printf("wasm instructions: %zu\n", totalWasm);
        std::printf("IL instructions: %zu  (%.2f per wasm insn)\n", totalIl, totalWasm ? static_cast<double>(totalIl) / totalWasm : 0.0);
        std::printf("unimplemented: %zu\n", totalUnimpl);
        std::printf("label/report errors: %zu\n", totalErrors);
        std::printf("spills: %zu\n", totalSpills);
        std::printf("unknown SP sets: %zu  unresolved br_table labels: %zu\n", totalUnknownSets, totalUnresolvedTables);
        std::printf("native arithmetic: %zu int, %zu compare, %zu float\n", totalNativeInt, totalNativeCmp, totalNativeFloat);
        size_t totalIntrinsicCalls = 0;
        for (const auto &[name, count] : intrinsicCounts)
            totalIntrinsicCalls += count;
        std::printf("intrinsic calls: %zu (%zu distinct)\n", totalIntrinsicCalls, intrinsicCounts.size());
        if (!unimplOps.empty()) {
            std::printf("unimplemented opcodes:\n");
            for (const auto &[op, count] : unimplOps)
                std::printf("  %-32s %zu\n", op.c_str(), count);
        }
        if (!intrinsicCounts.empty()) {
            std::vector<std::pair<std::string, size_t>> sorted(intrinsicCounts.begin(), intrinsicCounts.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
            std::printf("top intrinsics:\n");
            for (size_t i = 0; i < std::min<size_t>(sorted.size(), 20); ++i)
                std::printf("  %-44s %zu\n", sorted[i].first.c_str(), sorted[i].second);
        }
    }

    if (check && (totalUnimpl != 0 || totalErrors != 0 || failed != 0))
        return 1;
    return 0;
}
