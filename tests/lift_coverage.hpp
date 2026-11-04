//
// Opcode coverage matrix: build a module with one function per opcode family,
// run the production lifter over it, and report every function that produced
// LLIL_UNIMPLEMENTED or a structural error.
//
// Stack operands are pushed as i32/v128 zeros; lifting only needs the correct
// stack *height*, not the value types.  This is intentional: it lets the matrix
// cover the whole decoder without needing a full typed generator.
//
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "partial_il.hpp"
#include "wasm/analysis.hpp"
#include "wasm/decode.hpp"
#include "wasm/module.hpp"
#include "wasm/opcodes.hpp"
#include "wasm_builder.hpp"

namespace liftcov {

    struct Result {
        size_t cases = 0;
        std::vector<std::string> missing;
        std::vector<std::string> errors;
    };

    namespace detail {

        inline void pushOperands(wasmbuild::Code &c, std::string_view pops) {
            for (char p : pops) {
                if (p == 'v')
                    c.v128Const(wasmbuild::bytes(16, 0));
                else
                    c.i32(0);
            }
        }

        inline void dropResults(wasmbuild::Code &c, std::string_view pushes) {
            for (size_t i = 0; i < pushes.size(); ++i)
                c.drop();
        }

        inline void plainCase(wasmbuild::Code &c, uint32_t op) {
            const auto eff = wasm::fixedEffect(op);
            if (!eff) {
                c.u8(static_cast<uint8_t>(op));
                return;
            }
            for (int i = 0; i < eff->pops; ++i)
                c.i32(0);
            c.u8(static_cast<uint8_t>(op));
            for (int i = 0; i < eff->pushes; ++i)
                c.drop();
        }

    } // namespace detail

    inline Result run() {
        using wasmbuild::Code;
        using wasmbuild::ModuleBuilder;

        Result out;
        ModuleBuilder b;
        b.setMemoryPages(1);
        b.addData(0, {1, 2, 3, 4});
        b.addGlobal(wasm::kI32, 0, true);
        b.addGlobal(wasm::kI64, 0, true);
        b.addGlobal(wasm::kF32, 0, true);
        b.addGlobal(wasm::kF64, 0, true);

        // Function 0 is a no-op callee used by the call coverage cases.
        const uint32_t voidType = b.addType({}, {});
        Code callee;
        callee.end();
        const uint32_t calleeIndex = b.addFunction(voidType, callee.b);
        b.addTable(1, {calleeIndex});

        std::vector<std::pair<std::string, uint32_t>> cases;

        const auto add = [&](std::string label, Code code) {
            const uint32_t fn = b.addFunction(voidType, std::move(code.b), {wasm::kI32, wasm::kI64, wasm::kF32, wasm::kF64, wasm::kV128});
            cases.emplace_back(std::move(label), fn);
        };

        // --- control / misc -------------------------------------------------
        {
            Code c;
            c.op(0x00).end();
            add("unreachable", std::move(c));
            c = Code{};
            c.op(0x01).end();
            add("nop", std::move(c));
            c = Code{};
            c.op(0x0F).end();
            add("return", std::move(c));
            c = Code{};
            c.i32(0).drop().op(0x1A).end();
            add("drop", std::move(c));
            c = Code{};
            c.i32(0).i32(0).i32(0).select().drop().end();
            add("select", std::move(c));
            c = Code{};
            c.i32(0).i32(0).i32(0).selectT({wasm::kI32}).drop().end();
            add("select_t", std::move(c));
            c = Code{};
            c.call(calleeIndex).end();
            add("call", std::move(c));
            c = Code{};
            c.returnCall(calleeIndex).end();
            add("return_call", std::move(c));
            c = Code{};
            c.i32(0).callIndirect(voidType, 0).end();
            add("call_indirect", std::move(c));
            c = Code{};
            c.i32(0).returnCallIndirect(voidType, 0).end();
            add("return_call_indirect", std::move(c));
            c = Code{};
            c.refFunc(calleeIndex).op(0x14).uleb(voidType).end();
            add("call_ref", std::move(c));
            c = Code{};
            c.refFunc(calleeIndex).op(0x15).uleb(voidType).end();
            add("return_call_ref", std::move(c));
            c = Code{};
            c.op(0x06).sleb(-64).op(0x07).uleb(0).op(0x0B).op(0x0B).end();
            add("try/catch", std::move(c));
            c = Code{};
            c.op(0x06).sleb(-64).op(0x19).op(0x0B).op(0x0B).end();
            add("try/catch_all", std::move(c));
            c = Code{};
            c.block(-1);  // outer block, result i32
            c.op(0x1F).sleb(-1).uleb(1).u8(0).uleb(0).uleb(0);  // try_table (result i32) catch 0 0
            c.i32(1);
            c.end();
            c.end();
            c.end();
            add("try_table", std::move(c));
            c = Code{};
            c.block(-64);  // catch target
            c.op(0x1F).sleb(-64).uleb(1).u8(0).uleb(0).uleb(0);
            c.op(0x08).uleb(0);  // throw 0 -> dispatchCatches
            c.end();
            c.end();
            c.end();
            add("try_table_throw", std::move(c));
            c = Code{};
            c.op(0x08).uleb(0).end();
            add("throw", std::move(c));
            c = Code{};
            c.op(0x09).uleb(0).end();
            add("rethrow", std::move(c));
            c = Code{};
            c.refNull().op(0x0A).end();
            add("throw_ref", std::move(c));
        }

        // --- locals / globals / tables --------------------------------------
        {
            Code c;
            c.localGet(0).drop().end();
            add("local.get", std::move(c));
            c = Code{};
            c.i32(0).localSet(0).end();
            add("local.set", std::move(c));
            c = Code{};
            c.i32(0).localTee(0).drop().end();
            add("local.tee", std::move(c));
            c = Code{};
            c.globalGet(0).drop().end();
            add("global.get", std::move(c));
            c = Code{};
            c.i32(0).globalSet(0).end();
            add("global.set", std::move(c));
            c = Code{};
            c.i32(0).tableGet(0).drop().end();
            add("table.get", std::move(c));
            c = Code{};
            c.i32(0).refNull().tableSet(0).end();
            add("table.set", std::move(c));
        }

        // --- memory ---------------------------------------------------------
        for (uint32_t op = 0x28; op <= 0x35; ++op) {
            Code c;
            c.i32(0);
            c.load(static_cast<uint8_t>(op), 0);
            c.drop().end();
            add("load." + std::to_string(op), std::move(c));
        }
        for (uint32_t op = 0x36; op <= 0x3E; ++op) {
            Code c;
            c.i32(0).i64(0);
            c.store(static_cast<uint8_t>(op), 0);
            c.end();
            add("store." + std::to_string(op), std::move(c));
        }
        {
            Code c;
            c.memorySize().drop().end();
            add("memory.size", std::move(c));
            c = Code{};
            c.i32(0).memoryGrow().drop().end();
            add("memory.grow", std::move(c));
        }

        // --- constants ------------------------------------------------------
        {
            Code c;
            c.i32(0).drop().end();
            add("i32.const", std::move(c));
            c = Code{};
            c.i64(0).drop().end();
            add("i64.const", std::move(c));
            c = Code{};
            c.f32(0).drop().end();
            add("f32.const", std::move(c));
            c = Code{};
            c.f64(0).drop().end();
            add("f64.const", std::move(c));
        }

        // --- numeric / conversion ------------------------------------------
        for (uint32_t op = wasm::op::NumericFirst; op <= wasm::op::ConvertLast; ++op) {
            Code c;
            detail::plainCase(c, op);
            c.end();
            add("num." + std::to_string(op), std::move(c));
        }

        // --- references -----------------------------------------------------
        {
            Code c;
            c.refNull().drop().end();
            add("ref.null", std::move(c));
            c = Code{};
            c.refNull().refIsNull().drop().end();
            add("ref.is_null", std::move(c));
            c = Code{};
            c.refFunc(calleeIndex).drop().end();
            add("ref.func", std::move(c));
            c = Code{};
            c.refNull().refNull().refEq().drop().end();
            add("ref.eq", std::move(c));
            c = Code{};
            c.refNull().op(0xD4).drop().end();
            add("ref.as_non_null", std::move(c));
            c = Code{};
            c.block(-64).refNull().op(0xD5).uleb(0).drop().end().end();
            add("br_on_null", std::move(c));
            c = Code{};
            c.block(-64).refNull().op(0xD6).uleb(0).end().end();
            add("br_on_non_null", std::move(c));
        }

        // --- 0xFC: saturating conversions and bulk memory -------------------
        for (uint32_t sub = 0; sub < 18; ++sub) {
            Code c;
            switch (sub) {
            case 0:
            case 1:
            case 2:
            case 3:
                c.i32(0).prefix(0xFC, sub).drop();
                break;
            case 4:
            case 5:
            case 6:
            case 7:
                c.i64(0).prefix(0xFC, sub).drop();
                break;
            case 8:
                c.i32(0).i32(0).i32(0).prefix(0xFC, sub).uleb(0).uleb(0);
                break;
            case 9:
                c.prefix(0xFC, sub).uleb(0);
                break;
            case 10:
                c.i32(0).i32(0).i32(0).prefix(0xFC, sub).uleb(0).uleb(0);
                break;
            case 11:
                c.i32(0).i32(0).i32(0).prefix(0xFC, sub).uleb(0);
                break;
            case 12:
                c.i32(0).i32(0).i32(0).prefix(0xFC, sub).uleb(0).uleb(0);
                break;
            case 13:
                c.prefix(0xFC, sub).uleb(0);
                break;
            case 14:
                c.i32(0).i32(0).i32(0).prefix(0xFC, sub).uleb(0).uleb(0);
                break;
            case 15:
                c.refNull().i32(0).prefix(0xFC, sub).uleb(0).drop();
                break;
            case 16:
                c.prefix(0xFC, sub).uleb(0).drop();
                break;
            case 17:
                c.i32(0).refNull().i32(0).prefix(0xFC, sub).uleb(0);
                break;
            }
            c.end();
            add("fc." + std::to_string(sub), std::move(c));
        }

        // --- 0xFD: SIMD -----------------------------------------------------
        for (uint32_t sub = 0; sub < 256; ++sub) {
            const auto sig = wasm::simdSig(sub);
            if (!sig)
                continue;
            Code c;
            if (sub == 0x0C) {
                c.v128Const(wasmbuild::bytes(16, 0)).drop().end();
                add("simd.0c", std::move(c));
                continue;
            }
            if (sub <= 0x0A) {
                c.i32(0).simdMem(sub, 0).drop().end();
                add("simd." + std::to_string(sub), std::move(c));
                continue;
            }
            if (sub == 0x0B) {
                c.i32(0).v128Const(wasmbuild::bytes(16, 0)).simdMem(sub, 0).end();
                add("simd.0b", std::move(c));
                continue;
            }
            if (sub == 0x0D) {
                c.v128Const(wasmbuild::bytes(16, 0)).v128Const(wasmbuild::bytes(16, 0));
                c.shuffle(wasmbuild::bytes(16, 0)).drop().end();
                add("simd.0d", std::move(c));
                continue;
            }
            if (sub >= 0x0F && sub <= 0x14) {
                c.i32(0).simdMem(sub, 0).drop().end();
                add("simd." + std::to_string(sub), std::move(c));
                continue;
            }
            if (sub >= 0x15 && sub <= 0x22) {
                detail::pushOperands(c, sig->pops);
                c.simdLane(sub, 0);
                detail::dropResults(c, sig->pushes);
                c.end();
                add("simd." + std::to_string(sub), std::move(c));
                continue;
            }
            if (sub >= 0x54 && sub <= 0x5B) {
                detail::pushOperands(c, sig->pops);
                c.simdMemLane(sub, 0, 0);
                detail::dropResults(c, sig->pushes);
                c.end();
                add("simd." + std::to_string(sub), std::move(c));
                continue;
            }
            if (sub == 0x5C || sub == 0x5D) {
                c.i32(0).simdMem(sub, 0).drop().end();
                add("simd." + std::to_string(sub), std::move(c));
                continue;
            }
            detail::pushOperands(c, sig->pops);
            c.simd(sub);
            detail::dropResults(c, sig->pushes);
            c.end();
            add("simd." + std::to_string(sub), std::move(c));
        }

        // --- 0xFE: atomics ---------------------------------------------------
        for (uint32_t sub = 0; sub < 0x4F; ++sub) {
            const wasm::AtomicInfo *info = wasm::atomicInfo(sub);
            if (!info)
                continue;
            Code c;
            for (int i = 0; i < info->pops; ++i)
                c.i32(0);
            if (sub == 3)
                c.atomicFence();
            else
                c.atomicMem(sub, 0);
            for (int i = 0; i < info->pushes; ++i)
                c.drop();
            c.end();
            add("atomic." + std::to_string(sub), std::move(c));
        }

        auto module = wasm::parseModule(b.build());
        if (!module) {
            out.errors.push_back("coverage module did not parse");
            return out;
        }
        out.cases = cases.size();

        lift_test::LiftHarness harness(*module);
        for (const auto &[label, fn] : cases) {
            const auto it = module->addrIndex.find(module->functions[fn].address.value_or(0));
            if (it == module->addrIndex.end()) {
                out.missing.push_back(label + " (no body)");
                continue;
            }
            const uint64_t start = *module->functions[fn].address;
            const auto *lifted = harness.tryLift(start);
            if (!lifted) {
                out.missing.push_back(label + " (lift failed)");
                continue;
            }
            const auto stats = BinaryNinja::analyzeIl(lifted->il);
            if (stats.unimplemented)
                out.missing.push_back(label);
            for (const auto &e : stats.errors)
                out.errors.push_back(label + ": " + e);
        }
        return out;
    }

} // namespace liftcov
