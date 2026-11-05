//
// Drives the production lifter one instruction at a time, exactly the way
// WasmArchitecture::GetInstructionLowLevelIL does in Binary Ninja, but into the
// headless partial IL.
//
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "bn/lifter.hpp"
#include "wasm/analysis.hpp"
#include "wasm/decode.hpp"
#include "wasm/module.hpp"

namespace lift_test {

    struct LiftedFunction {
        uint64_t start = 0;
        uint64_t size = 0;
        std::shared_ptr<const wasm::FunctionAnalysis> analysis;
        BinaryNinja::LowLevelILFunction il;
    };

    class LiftHarness {
      public:
        explicit LiftHarness(const wasm::Module &module) : module_(module) {}

        const wasm::Module &module() const { return module_; }

        const LiftedFunction &lift(uint64_t start) {
            const auto cached = cache_.find(start);
            if (cached != cache_.end())
                return *cached->second;

            const auto it = module_.addrIndex.find(start);
            if (it == module_.addrIndex.end())
                throw std::runtime_error("no function body at requested address");

            auto fn = std::make_unique<LiftedFunction>();
            fn->start = start;
            fn->size = module_.functions[it->second].size;
            fn->analysis = wasm::analyze(module_, start);
            if (!fn->analysis)
                throw std::runtime_error("function analysis failed");

            std::vector<uint64_t> addrs;
            addrs.reserve(fn->analysis->offsets.size());
            for (uint32_t off : fn->analysis->offsets)
                addrs.push_back(bnwasm::kCodeBase + start + off);
            fn->il.SetInstructionAddresses(addrs);

            for (uint32_t off : fn->analysis->offsets) {
                const uint64_t addr = start + off;
                const wasm::Instr ins = wasm::decode(module_.data, addr);
                fn->il.BeginInstruction(bnwasm::kCodeBase + addr, addr);
                bnwasm::lift(fn->il, &arch_, ins, addr, module_, fn->analysis.get());
            }

            auto &slot = cache_[start];
            slot = std::move(fn);
            return *slot;
        }

        const LiftedFunction *tryLift(uint64_t start) noexcept {
            try {
                return &lift(start);
            } catch (const std::exception &) {
                return nullptr;
            }
        }

        // Function index for a body address, or ~0.
        uint32_t functionIndex(uint64_t start) const {
            const auto it = module_.addrIndex.find(start);
            return it == module_.addrIndex.end() ? ~0u : it->second;
        }

      private:
        const wasm::Module &module_;
        BinaryNinja::Architecture arch_;
        std::unordered_map<uint64_t, std::unique_ptr<LiftedFunction>> cache_;
    };

} // namespace lift_test
