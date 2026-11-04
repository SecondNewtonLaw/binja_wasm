//
// Independent WebAssembly interpreter over the same decoded instruction stream.
//
// This is deliberately not a fast interpreter; it is the oracle for
// differential testing.  It supports the integer, float, memory, local/global,
// call and structured-control subset that the lift tests exercise.  SIMD,
// atomics and exceptions raise Unsupported so a test can skip them rather than
// silently comparing against the wrong semantics.
//
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "wasm/analysis.hpp"
#include "wasm/decode.hpp"
#include "wasm/module.hpp"
#include "wasm/opcodes.hpp"

namespace wasmtest {

    struct Value {
        uint64_t lo = 0;
        uint64_t hi = 0; // v128 upper half
    };

    inline Value v32(uint32_t x) { return {x, 0}; }
    inline Value v64(uint64_t x) { return {x, 0}; }
    inline Value vf32(float f) { return {std::bit_cast<uint32_t>(f), 0}; }
    inline Value vf64(double d) { return {std::bit_cast<uint64_t>(d), 0}; }
    inline uint32_t u32(const Value &v) { return static_cast<uint32_t>(v.lo); }
    inline uint64_t u64(const Value &v) { return v.lo; }
    inline float f32(const Value &v) { return std::bit_cast<float>(static_cast<uint32_t>(v.lo)); }
    inline double f64(const Value &v) { return std::bit_cast<double>(v.lo); }

    struct WasmTrap : std::runtime_error {
        using std::runtime_error::runtime_error;
    };
    struct Unsupported : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    class WasmInterpreter {
      public:
        explicit WasmInterpreter(const wasm::Module &module) : m_(module) { initMemory(); }

        const wasm::Module &module() const { return m_; }
        std::vector<uint8_t> &memory() { return mem_; }
        const std::vector<uint8_t> &memory() const { return mem_; }
        std::vector<Value> &globals() { return globals_; }
        const std::vector<Value> &globals() const { return globals_; }
        uint64_t pages() const { return pages_; }

        std::vector<Value> call(uint32_t funcIndex, const std::vector<Value> &args) { return runFunction(funcIndex, args, 0); }

        // Encode/decode helpers shared with tests.
        static uint64_t maskTo(size_t bytes) {
            if (bytes >= 8)
                return ~0ull;
            return (1ull << (bytes * 8)) - 1;
        }

      private:
        struct Frame {
            enum Kind { Function, Block, Loop, If, Try } kind = Block;
            uint64_t start = 0;
            uint64_t end = 0;
            uint64_t elseAddr = ~0ull;
            bool inElse = false;
        };

        struct BlockInfo {
            std::unordered_map<uint64_t, uint64_t> end;      // block start -> matching end
            std::unordered_map<uint64_t, uint64_t> elseAddr; // if start -> else address
        };

        void initMemory() {
            uint64_t dataEnd = 0;
            for (const auto &s : m_.dataSegments)
                dataEnd = std::max(dataEnd, s.memAddr + s.size);
            pages_ = std::max<uint64_t>(m_.memoryPages ? m_.memoryPages : 1, (dataEnd + 65535) / 65536);
            mem_.assign(pages_ * 65536, 0);
            for (const auto &s : m_.dataSegments) {
                if (s.memAddr + s.size > mem_.size())
                    mem_.resize(s.memAddr + s.size, 0);
                if (s.fileOffset + s.size <= m_.data.size())
                    std::memcpy(mem_.data() + s.memAddr, m_.data.data() + s.fileOffset, s.size);
            }
            globals_.resize(m_.globalTypes.size());
            for (size_t i = 0; i < globals_.size(); ++i)
                globals_[i] = maskValue(v64(m_.globalInits[i]), m_.globalTypes[i]);
        }

        static Value maskValue(Value v, uint8_t type) {
            switch (type) {
            case wasm::kI32:
            case wasm::kF32:
                v.lo &= 0xFFFFFFFFull;
                v.hi = 0;
                break;
            case wasm::kI64:
            case wasm::kF64:
                v.hi = 0;
                break;
            default:
                break;
            }
            return v;
        }

        uint8_t read8(uint64_t addr) {
            if (addr >= mem_.size())
                throw WasmTrap("out-of-bounds load");
            return mem_[addr];
        }
        void write8(uint64_t addr, uint8_t v) {
            if (addr >= mem_.size())
                throw WasmTrap("out-of-bounds store");
            mem_[addr] = v;
        }
        uint64_t readLE(uint64_t addr, size_t n) {
            uint64_t v = 0;
            for (size_t i = 0; i < n; ++i)
                v |= static_cast<uint64_t>(read8(addr + i)) << (8 * i);
            return v;
        }
        void writeLE(uint64_t addr, uint64_t v, size_t n) {
            for (size_t i = 0; i < n; ++i)
                write8(addr + i, static_cast<uint8_t>(v >> (8 * i)));
        }

        const wasm::FunctionAnalysis *analysis(uint64_t start) {
            auto it = analyses_.find(start);
            if (it != analyses_.end())
                return it->second.get();
            auto fa = wasm::analyze(m_, start);
            auto *raw = fa.get();
            analyses_[start] = std::move(fa);
            return raw;
        }

        const BlockInfo &blocks(uint64_t start) {
            auto it = blockInfo_.find(start);
            if (it != blockInfo_.end())
                return it->second;
            BlockInfo info;
            std::vector<uint64_t> stack;
            const auto fn = m_.addrIndex.find(start);
            if (fn == m_.addrIndex.end())
                throw WasmTrap("no function at block-scan start");
            const uint64_t end = start + m_.functions[fn->second].size;
            // The implicit function block is represented by the runtime Function
            // frame, not by an entry in this map: a real block can start at the
            // same address as the function body.
            uint64_t pc = start;
            while (pc < end) {
                const wasm::Instr ins = wasm::decode(m_.data, pc);
                if (wasm::isBlock(ins.op)) {
                    stack.push_back(pc);
                } else if (ins.op == wasm::op::Else) {
                    if (!stack.empty())
                        info.elseAddr[stack.back()] = pc;
                } else if (ins.op == wasm::op::End) {
                    if (stack.empty())
                        break;
                    const uint64_t s = stack.back();
                    stack.pop_back();
                    info.end[s] = pc;
                }
                pc += ins.size;
            }
            auto [slot, inserted] = blockInfo_.emplace(start, std::move(info));
            (void)inserted;
            return slot->second;
        }

        static bool isFloatCmp(uint32_t op) { return op >= 0x5B && op <= 0x66; }

        std::vector<Value> runFunction(uint32_t index, const std::vector<Value> &args, int depth) {
            if (depth > 256)
                throw WasmTrap("call depth");
            if (index >= m_.functions.size())
                throw WasmTrap("bad function index");
            const wasm::Function &func = m_.functions[index];
            if (!func.address)
                throw Unsupported("imported function");
            const wasm::FuncType *sig = m_.funcSig(index);
            if (!sig)
                throw WasmTrap("missing function type");
            if (args.size() != sig->params.size())
                throw WasmTrap("argument count mismatch");

            const uint64_t start = *func.address;
            const uint64_t end = start + func.size;
            const wasm::FunctionAnalysis *fa = analysis(start);
            if (!fa)
                throw WasmTrap("function analysis failed");
            const BlockInfo &info = blocks(start);

            std::vector<Value> locals;
            const auto ltIt = m_.localTypes.find(start);
            if (ltIt != m_.localTypes.end()) {
                locals.resize(ltIt->second.size());
                for (size_t i = 0; i < locals.size(); ++i)
                    locals[i] = Value{};
            } else {
                locals.resize(sig->params.size());
            }
            for (size_t i = 0; i < args.size() && i < locals.size(); ++i)
                locals[i] = maskValue(args[i], sig->params[i]);

            std::vector<Value> stack;
            std::vector<Frame> frames;
            frames.push_back({Frame::Function, start, end, ~0ull, false});

            const auto pop = [&]() -> Value {
                if (stack.empty())
                    throw WasmTrap("operand stack underflow");
                Value v = stack.back();
                stack.pop_back();
                return v;
            };
            const auto push = [&](Value v) { stack.push_back(v); };

            uint64_t pc = start;
            uint64_t steps = 0;
            const auto finish = [&](size_t nres) -> std::vector<Value> {
                if (stack.size() < nres)
                    throw WasmTrap("result stack underflow");
                return std::vector<Value>(stack.end() - static_cast<ptrdiff_t>(nres), stack.end());
            };

            const auto applyMoves = [&](const std::vector<wasm::Move> *mv, uint64_t target, std::vector<Value> &st) {
                const uint64_t rel = target - fa->start;
                const auto off = std::lower_bound(fa->offsets.begin(), fa->offsets.end(), rel);
                if (off == fa->offsets.end() || *off != rel)
                    throw WasmTrap("branch target is not an instruction");
                const size_t want = fa->heightAt(target);
                std::vector<Value> next(want);
                for (size_t i = 0; i < std::min(want, st.size()); ++i)
                    next[i] = st[i];
                if (mv) {
                    for (const wasm::Move &m : *mv)
                        if (m.src < st.size() && m.dst < next.size())
                            next[m.dst] = st[m.src];
                }
                st = std::move(next);
            };

            const auto popFramesAbove = [&](uint64_t target) {
                size_t i = frames.size();
                while (i-- > 0) {
                    if (frames[i].end == target || frames[i].start == target)
                        break;
                }
                if (i >= frames.size())
                    throw WasmTrap("branch target outside control stack");
                frames.resize(i + 1);
            };

            const auto branchTo = [&](uint64_t target, const std::vector<wasm::Move> *mv) {
                applyMoves(mv, target, stack);
                popFramesAbove(target);
                const wasm::Instr targetIns = wasm::decode(m_.data, target);
                // A loop label re-enters the body; the loop opcode itself is a no-op.
                pc = targetIns.op == wasm::op::Loop ? target + targetIns.size : target;
            };

            for (;;) {
                if (pc < start || pc >= end)
                    throw WasmTrap("pc out of function");
                if (++steps > 5'000'000)
                    throw WasmTrap("step limit");
                const wasm::Instr ins = wasm::decode(m_.data, pc);
                const uint32_t op = ins.op;
                const auto advance = [&] { pc += ins.size; };

                // Structured control.
                if (op == wasm::op::Block || op == wasm::op::Loop || op == wasm::op::Try || op == wasm::op::TryTable) {
                    const auto e = info.end.find(pc);
                    if (e == info.end.end())
                        throw WasmTrap("block without end");
                    frames.push_back({op == wasm::op::Loop ? Frame::Loop : Frame::Block, pc, e->second, ~0ull, false});
                    advance();
                    continue;
                }
                if (op == wasm::op::If) {
                    const Value cond = pop();
                    const auto e = info.end.find(pc);
                    if (e == info.end.end())
                        throw WasmTrap("if without end");
                    const auto el = info.elseAddr.find(pc);
                    const uint64_t elseAddr = el == info.elseAddr.end() ? ~0ull : el->second;
                    frames.push_back({Frame::If, pc, e->second, elseAddr, false});
                    if (cond.lo != 0)
                        advance();
                    else if (elseAddr != ~0ull) {
                        frames.back().inElse = true;
                        pc = elseAddr;
                    } else {
                        pc = e->second; // execute the matching end
                    }
                    continue;
                }
                if (op == wasm::op::Else) {
                    if (frames.empty() || frames.back().kind != Frame::If)
                        throw WasmTrap("else without if");
                    Frame &f = frames.back();
                    if (f.inElse) {
                        // Entered from the false edge: run the else body.
                        advance();
                    } else {
                        // Fell out of the then body: skip the else body and the end.
                        pc = f.end + ins.size;
                        frames.pop_back();
                    }
                    continue;
                }
                if (op == wasm::op::End) {
                    if (frames.size() == 1) {
                        return finish(sig->results.size());
                    }
                    frames.pop_back();
                    advance();
                    continue;
                }
                if (op == wasm::op::Br || op == wasm::op::BrIf || op == wasm::op::BrOnNull || op == wasm::op::BrOnNonNull) {
                    const uint64_t *target = fa->branchTarget(pc);
                    if (!target)
                        throw WasmTrap("unresolved branch");
                    if (op == wasm::op::Br) {
                        branchTo(*target, fa->movesAt(pc));
                        continue;
                    }
                    if (op == wasm::op::BrIf) {
                        const Value cond = pop();
                        if (cond.lo != 0)
                            branchTo(*target, fa->movesAt(pc));
                        else
                            advance();
                        continue;
                    }
                    const Value ref = pop();
                    const bool isNull = ref.lo == 0;
                    if ((op == wasm::op::BrOnNull && isNull) || (op == wasm::op::BrOnNonNull && !isNull)) {
                        branchTo(*target, fa->movesAt(pc));
                    } else {
                        if (op == wasm::op::BrOnNull)
                            push(ref);
                        advance();
                    }
                    continue;
                }
                if (op == wasm::op::BrTable) {
                    const Value idx = pop();
                    const auto t = fa->table(pc);
                    if (!t || t->empty())
                        throw WasmTrap("unresolved br_table");
                    const size_t k = static_cast<size_t>(u32(idx)) < t->size() - 1 ? u32(idx) : t->size() - 1;
                    if ((*t)[k] == wasm::kUnresolved || (*t)[k] == wasm::kFunctionEnd)
                        throw WasmTrap("bad br_table target");
                    const std::vector<wasm::Move> *mv = nullptr;
                    const auto tm = fa->tableMoves.find(pc);
                    if (tm != fa->tableMoves.end() && k < tm->second.size())
                        mv = &tm->second[k];
                    branchTo((*t)[k], mv);
                    continue;
                }
                if (op == wasm::op::Return) {
                    return finish(sig->results.size());
                }
                if (op == wasm::op::ReturnCall) {
                    const uint32_t callee = static_cast<uint32_t>(ins.imm0);
                    const wasm::FuncType *cs = m_.funcSig(callee);
                    if (!cs)
                        throw WasmTrap("bad callee type");
                    std::vector<Value> cargs(cs->params.size());
                    for (size_t i = cargs.size(); i-- > 0;)
                        cargs[i] = pop();
                    return runFunction(callee, cargs, depth + 1);
                }
                if (op == wasm::op::Call) {
                    const uint32_t callee = static_cast<uint32_t>(ins.imm0);
                    const wasm::FuncType *cs = m_.funcSig(callee);
                    if (!cs)
                        throw WasmTrap("bad callee type");
                    std::vector<Value> cargs(cs->params.size());
                    for (size_t i = cargs.size(); i-- > 0;)
                        cargs[i] = pop();
                    auto results = runFunction(callee, cargs, depth + 1);
                    for (const Value &v : results)
                        push(v);
                    advance();
                    continue;
                }
                if (op == wasm::op::CallRef || op == wasm::op::ReturnCallRef) {
                    const Value ref = pop();
                    if (ref.lo == 0 || ref.lo > m_.functions.size())
                        throw WasmTrap("call_ref null/out-of-range");
                    const uint32_t callee = static_cast<uint32_t>(ref.lo - 1);
                    if (m_.functions[callee].type != ins.imm0)
                        throw WasmTrap("call_ref type mismatch");
                    const wasm::FuncType *cs = m_.funcSig(callee);
                    if (!cs)
                        throw WasmTrap("call_ref missing type");
                    std::vector<Value> cargs(cs->params.size());
                    for (size_t i = cargs.size(); i-- > 0;)
                        cargs[i] = pop();
                    auto results = runFunction(callee, cargs, depth + 1);
                    if (op == wasm::op::ReturnCallRef)
                        return results;
                    for (const Value &v : results)
                        push(v);
                    advance();
                    continue;
                }
                if (op == wasm::op::CallIndirect || op == wasm::op::ReturnCallIndirect) {
                    const Value idx = pop();
                    const size_t elem = static_cast<size_t>(u32(idx));
                    if (elem >= m_.table.size() || m_.table[elem] == wasm::kNoFunc)
                        throw WasmTrap("call_indirect null/out-of-range");
                    const uint32_t callee = m_.table[elem];
                    if (callee >= m_.functions.size() || m_.functions[callee].type != ins.imm0)
                        throw WasmTrap("call_indirect type mismatch");
                    const wasm::FuncType *cs = m_.funcSig(callee);
                    if (!cs)
                        throw WasmTrap("call_indirect missing type");
                    std::vector<Value> cargs(cs->params.size());
                    for (size_t i = cargs.size(); i-- > 0;)
                        cargs[i] = pop();
                    auto results = runFunction(callee, cargs, depth + 1);
                    if (op == wasm::op::ReturnCallIndirect)
                        return results;
                    for (const Value &v : results)
                        push(v);
                    advance();
                    continue;
                }

                // Locals, globals, stack shuffling.
                if (op == wasm::op::LocalGet) {
                    const auto i = static_cast<size_t>(ins.imm0);
                    if (i >= locals.size())
                        throw WasmTrap("local index");
                    push(locals[i]);
                    advance();
                    continue;
                }
                if (op == wasm::op::LocalSet || op == wasm::op::LocalTee) {
                    const auto i = static_cast<size_t>(ins.imm0);
                    if (i >= locals.size())
                        throw WasmTrap("local index");
                    const Value v = pop();
                    locals[i] = v;
                    if (op == wasm::op::LocalTee)
                        push(v);
                    advance();
                    continue;
                }
                if (op == wasm::op::GlobalGet || op == wasm::op::GlobalSet) {
                    const auto i = static_cast<size_t>(ins.imm0);
                    if (i >= globals_.size())
                        throw WasmTrap("global index");
                    if (op == wasm::op::GlobalGet)
                        push(globals_[i]);
                    else
                        globals_[i] = maskValue(pop(), m_.globalTypes[i]);
                    advance();
                    continue;
                }
                if (op == wasm::op::Drop) {
                    pop();
                    advance();
                    continue;
                }
                if (op == wasm::op::Select || op == wasm::op::SelectT) {
                    const Value cond = pop();
                    const Value b = pop();
                    const Value a = pop();
                    push(cond.lo ? a : b);
                    advance();
                    continue;
                }
                if (op == wasm::op::Nop) {
                    advance();
                    continue;
                }
                if (op == wasm::op::Unreachable) {
                    throw WasmTrap("unreachable");
                }

                // Constants.
                if (op == wasm::op::I32Const) {
                    push(v32(static_cast<uint32_t>(ins.imm0)));
                    advance();
                    continue;
                }
                if (op == wasm::op::I64Const) {
                    push(v64(static_cast<uint64_t>(ins.imm0)));
                    advance();
                    continue;
                }
                if (op == wasm::op::F32Const) {
                    push(v32(static_cast<uint32_t>(ins.imm0)));
                    advance();
                    continue;
                }
                if (op == wasm::op::F64Const) {
                    push(v64(static_cast<uint64_t>(ins.imm0)));
                    advance();
                    continue;
                }
                if (op == wasm::op::RefNull) {
                    push(v64(0));
                    advance();
                    continue;
                }
                if (op == wasm::op::RefIsNull) {
                    const Value v = pop();
                    push(v32(v.lo == 0 ? 1 : 0));
                    advance();
                    continue;
                }
                if (op == wasm::op::RefFunc) {
                    // Keep function references distinct from null (0) so
                    // ref.is_null/ref.eq have the same observable behaviour as
                    // the lifter's ConstPointer-based representation.
                    push(v64(static_cast<uint32_t>(ins.imm0) + 1));
                    advance();
                    continue;
                }
                if (op == wasm::op::RefEq) {
                    const Value b = pop();
                    const Value a = pop();
                    push(v32(a.lo == b.lo ? 1 : 0));
                    advance();
                    continue;
                }

                // Memory.
                if (op >= wasm::op::LoadFirst && op <= wasm::op::LoadLast) {
                    const uint64_t addr = u32(pop());
                    const uint64_t off = static_cast<uint64_t>(ins.imm0);
                    Value out;
                    switch (op) {
                    case 0x28:
                        out = v32(static_cast<uint32_t>(readLE(addr + off, 4)));
                        break;
                    case 0x29:
                        out = v64(readLE(addr + off, 8));
                        break;
                    case 0x2A:
                        out = v32(static_cast<uint32_t>(readLE(addr + off, 4)));
                        break;
                    case 0x2B:
                        out = v64(readLE(addr + off, 8));
                        break;
                    case 0x2C:
                        out = v32(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(readLE(addr + off, 1)))));
                        break;
                    case 0x2D:
                        out = v32(static_cast<uint32_t>(readLE(addr + off, 1)));
                        break;
                    case 0x2E:
                        out = v32(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(readLE(addr + off, 2)))));
                        break;
                    case 0x2F:
                        out = v32(static_cast<uint32_t>(readLE(addr + off, 2)));
                        break;
                    case 0x30:
                        out = v64(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(readLE(addr + off, 1)))));
                        break;
                    case 0x31:
                        out = v64(readLE(addr + off, 1));
                        break;
                    case 0x32:
                        out = v64(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(readLE(addr + off, 2)))));
                        break;
                    case 0x33:
                        out = v64(readLE(addr + off, 2));
                        break;
                    case 0x34:
                        out = v64(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(readLE(addr + off, 4)))));
                        break;
                    case 0x35:
                        out = v64(readLE(addr + off, 4));
                        break;
                    default:
                        throw Unsupported("load");
                    }
                    push(out);
                    advance();
                    continue;
                }
                if (op >= wasm::op::StoreFirst && op <= wasm::op::StoreLast) {
                    const Value val = pop();
                    const uint64_t addr = u32(pop());
                    const uint64_t off = static_cast<uint64_t>(ins.imm0);
                    size_t n = 0;
                    switch (op) {
                    case 0x36:
                        n = 4;
                        break;
                    case 0x37:
                        n = 8;
                        break;
                    case 0x38:
                        n = 4;
                        break;
                    case 0x39:
                        n = 8;
                        break;
                    case 0x3A:
                        n = 1;
                        break;
                    case 0x3B:
                        n = 2;
                        break;
                    case 0x3C:
                        n = 1;
                        break;
                    case 0x3D:
                        n = 2;
                        break;
                    case 0x3E:
                        n = 4;
                        break;
                    default:
                        throw Unsupported("store");
                    }
                    writeLE(addr + off, val.lo, n);
                    advance();
                    continue;
                }
                if (op == wasm::op::MemorySize) {
                    push(v32(static_cast<uint32_t>(pages_)));
                    advance();
                    continue;
                }
                if (op == wasm::op::MemoryGrow) {
                    const uint32_t delta = u32(pop());
                    const uint64_t old = pages_;
                    pages_ += delta;
                    mem_.resize(pages_ * 65536, 0);
                    push(v32(static_cast<uint32_t>(old)));
                    advance();
                    continue;
                }

                // Prefixed numeric / bulk-memory instructions.
                if (op >= wasm::kPrefixFC && op < wasm::kPrefixFD) {
                    executeFc(ins, stack);
                    advance();
                    continue;
                }

                // Numeric and conversion instructions.
                if (op >= wasm::op::NumericFirst && op <= wasm::op::ConvertLast) {
                    executeNumeric(op, stack);
                    advance();
                    continue;
                }

                // Branches with reference values already handled.  Everything else
                // is either rare enough to be unsupported or a prefixed op.
                throw Unsupported("opcode " + wasm::instrName(op));
            }
        }

        void executeFc(const wasm::Instr &ins, std::vector<Value> &stack) {
            const uint32_t sub = ins.op & 0xFF;
            const auto pop = [&]() -> Value {
                if (stack.empty())
                    throw WasmTrap("operand stack underflow");
                Value v = stack.back();
                stack.pop_back();
                return v;
            };
            const auto push = [&](Value v) { stack.push_back(v); };

            if (sub < 8) {
                const bool dst32 = sub < 4;
                const bool src32 = (sub % 4) < 2;
                const bool unsig = (sub % 2) == 1;
                const Value a = pop();
                const double x = src32 ? static_cast<double>(f32(a)) : f64(a);
                const double t = std::trunc(x);
                if (dst32) {
                    uint32_t r;
                    if (unsig) {
                        r = std::isnan(t) || t <= 0 ? 0 : t >= 4294967295.0 ? 0xFFFFFFFFu : static_cast<uint32_t>(t);
                    } else {
                        r = std::isnan(t)        ? 0
                            : t <= -2147483648.0 ? 0x80000000u
                            : t >= 2147483647.0  ? 0x7FFFFFFFu
                                                 : static_cast<uint32_t>(static_cast<int32_t>(t));
                    }
                    push(v32(r));
                } else {
                    uint64_t r;
                    if (unsig) {
                        r = std::isnan(t) || t <= 0 ? 0 : t >= 18446744073709551615.0 ? ~0ull : static_cast<uint64_t>(t);
                    } else {
                        r = std::isnan(t)                 ? 0
                            : t <= -9223372036854775808.0 ? 0x8000000000000000ull
                            : t >= 9223372036854775807.0  ? 0x7FFFFFFFFFFFFFFFull
                                                          : static_cast<uint64_t>(static_cast<int64_t>(t));
                    }
                    push(v64(r));
                }
                return;
            }
            if (sub == 8) { // memory.init dataidx
                const uint64_t len = u32(pop());
                const uint64_t src = u32(pop());
                const uint64_t dst = u32(pop());
                const uint64_t dataIdx = static_cast<uint64_t>(ins.imm0);
                if (dataIdx >= m_.dataSegments.size())
                    throw WasmTrap("data segment index");
                const auto &seg = m_.dataSegments[dataIdx];
                if (src + len > seg.size || dst + len > mem_.size())
                    throw WasmTrap("memory.init range");
                std::memcpy(mem_.data() + dst, m_.data.data() + seg.fileOffset + src, len);
                return;
            }
            if (sub == 9) // data.drop
                return;
            if (sub == 10) { // memory.copy
                const uint64_t len = u32(pop());
                const uint64_t src = u32(pop());
                const uint64_t dst = u32(pop());
                if (src + len > mem_.size() || dst + len > mem_.size())
                    throw WasmTrap("memory.copy range");
                std::memmove(mem_.data() + dst, mem_.data() + src, len);
                return;
            }
            if (sub == 11) { // memory.fill
                const uint64_t len = u32(pop());
                const uint64_t val = u32(pop());
                const uint64_t dst = u32(pop());
                if (dst + len > mem_.size())
                    throw WasmTrap("memory.fill range");
                std::memset(mem_.data() + dst, static_cast<int>(val & 0xFF), len);
                return;
            }
            throw Unsupported("fc." + std::to_string(sub));
        }

        void executeNumeric(uint32_t op, std::vector<Value> &stack) {
            const auto pop = [&]() -> Value {
                if (stack.empty())
                    throw WasmTrap("operand stack underflow");
                Value v = stack.back();
                stack.pop_back();
                return v;
            };
            const auto push = [&](Value v) { stack.push_back(v); };

            if (op == 0x45) { // i32.eqz
                push(v32(u32(pop()) == 0 ? 1 : 0));
                return;
            }
            if (op == 0x50) { // i64.eqz
                push(v32(u64(pop()) == 0 ? 1 : 0));
                return;
            }
            if (op >= 0x46 && op <= 0x4F) {
                const uint32_t b = u32(pop()), a = u32(pop());
                const int32_t sa = static_cast<int32_t>(a), sb = static_cast<int32_t>(b);
                uint32_t r = 0;
                switch (op) {
                case 0x46:
                    r = a == b;
                    break;
                case 0x47:
                    r = a != b;
                    break;
                case 0x48:
                    r = sa < sb;
                    break;
                case 0x49:
                    r = a < b;
                    break;
                case 0x4A:
                    r = sa > sb;
                    break;
                case 0x4B:
                    r = a > b;
                    break;
                case 0x4C:
                    r = sa <= sb;
                    break;
                case 0x4D:
                    r = a <= b;
                    break;
                case 0x4E:
                    r = sa >= sb;
                    break;
                case 0x4F:
                    r = a >= b;
                    break;
                }
                push(v32(r));
                return;
            }
            if (op >= 0x51 && op <= 0x5A) {
                const uint64_t b = u64(pop()), a = u64(pop());
                const int64_t sa = static_cast<int64_t>(a), sb = static_cast<int64_t>(b);
                uint32_t r = 0;
                switch (op) {
                case 0x51:
                    r = a == b;
                    break;
                case 0x52:
                    r = a != b;
                    break;
                case 0x53:
                    r = sa < sb;
                    break;
                case 0x54:
                    r = a < b;
                    break;
                case 0x55:
                    r = sa > sb;
                    break;
                case 0x56:
                    r = a > b;
                    break;
                case 0x57:
                    r = sa <= sb;
                    break;
                case 0x58:
                    r = a <= b;
                    break;
                case 0x59:
                    r = sa >= sb;
                    break;
                case 0x5A:
                    r = a >= b;
                    break;
                }
                push(v32(r));
                return;
            }
            if (op >= 0x5B && op <= 0x66) {
                const bool is32 = op <= 0x60;
                const uint32_t idx = (op - 0x5B) % 6;
                const Value b = pop(), a = pop();
                bool r = false;
                if (is32) {
                    const float fa = f32(a), fb = f32(b);
                    switch (idx) {
                    case 0:
                        r = fa == fb;
                        break;
                    case 1:
                        r = fa != fb;
                        break;
                    case 2:
                        r = fa < fb;
                        break;
                    case 3:
                        r = fa > fb;
                        break;
                    case 4:
                        r = fa <= fb;
                        break;
                    case 5:
                        r = fa >= fb;
                        break;
                    }
                } else {
                    const double fa = f64(a), fb = f64(b);
                    switch (idx) {
                    case 0:
                        r = fa == fb;
                        break;
                    case 1:
                        r = fa != fb;
                        break;
                    case 2:
                        r = fa < fb;
                        break;
                    case 3:
                        r = fa > fb;
                        break;
                    case 4:
                        r = fa <= fb;
                        break;
                    case 5:
                        r = fa >= fb;
                        break;
                    }
                }
                push(v32(r ? 1 : 0));
                return;
            }
            if (op >= 0x67 && op <= 0x69) { // i32 clz/ctz/popcnt
                const uint32_t a = u32(pop());
                const uint32_t r = op == 0x67 ? std::countl_zero(a) : op == 0x68 ? std::countr_zero(a) : std::popcount(a);
                push(v32(r));
                return;
            }
            if (op >= 0x6A && op <= 0x78) {
                const uint32_t b = u32(pop()), a = u32(pop());
                const int32_t sa = static_cast<int32_t>(a), sb = static_cast<int32_t>(b);
                uint32_t r = 0;
                switch (op) {
                case 0x6A:
                    r = a + b;
                    break;
                case 0x6B:
                    r = a - b;
                    break;
                case 0x6C:
                    r = a * b;
                    break;
                case 0x6D:
                    if (!sb || (sa == std::numeric_limits<int32_t>::min() && sb == -1))
                        throw WasmTrap("i32.div_s");
                    r = static_cast<uint32_t>(sa / sb);
                    break;
                case 0x6E:
                    if (!b)
                        throw WasmTrap("i32.div_u");
                    r = a / b;
                    break;
                case 0x6F:
                    if (!sb || (sa == std::numeric_limits<int32_t>::min() && sb == -1))
                        throw WasmTrap("i32.rem_s");
                    r = static_cast<uint32_t>(sa % sb);
                    break;
                case 0x70:
                    if (!b)
                        throw WasmTrap("i32.rem_u");
                    r = a % b;
                    break;
                case 0x71:
                    r = a & b;
                    break;
                case 0x72:
                    r = a | b;
                    break;
                case 0x73:
                    r = a ^ b;
                    break;
                case 0x74:
                    r = a << (b & 31);
                    break;
                case 0x75:
                    r = static_cast<uint32_t>(sa >> (b & 31));
                    break;
                case 0x76:
                    r = a >> (b & 31);
                    break;
                case 0x77:
                    r = std::rotl(a, static_cast<int>(b & 31));
                    break;
                case 0x78:
                    r = std::rotr(a, static_cast<int>(b & 31));
                    break;
                }
                push(v32(r));
                return;
            }
            if (op >= 0x79 && op <= 0x7B) { // i64 clz/ctz/popcnt
                const uint64_t a = u64(pop());
                const uint64_t r = op == 0x79 ? std::countl_zero(a) : op == 0x7A ? std::countr_zero(a) : std::popcount(a);
                push(v64(r));
                return;
            }
            if (op >= 0x7C && op <= 0x8A) {
                const uint64_t b = u64(pop()), a = u64(pop());
                const int64_t sa = static_cast<int64_t>(a), sb = static_cast<int64_t>(b);
                uint64_t r = 0;
                switch (op) {
                case 0x7C:
                    r = a + b;
                    break;
                case 0x7D:
                    r = a - b;
                    break;
                case 0x7E:
                    r = a * b;
                    break;
                case 0x7F:
                    if (!sb || (sa == std::numeric_limits<int64_t>::min() && sb == -1))
                        throw WasmTrap("i64.div_s");
                    r = static_cast<uint64_t>(sa / sb);
                    break;
                case 0x80:
                    if (!b)
                        throw WasmTrap("i64.div_u");
                    r = a / b;
                    break;
                case 0x81:
                    if (!sb || (sa == std::numeric_limits<int64_t>::min() && sb == -1))
                        throw WasmTrap("i64.rem_s");
                    r = static_cast<uint64_t>(sa % sb);
                    break;
                case 0x82:
                    if (!b)
                        throw WasmTrap("i64.rem_u");
                    r = a % b;
                    break;
                case 0x83:
                    r = a & b;
                    break;
                case 0x84:
                    r = a | b;
                    break;
                case 0x85:
                    r = a ^ b;
                    break;
                case 0x86:
                    r = a << (b & 63);
                    break;
                case 0x87:
                    r = static_cast<uint64_t>(sa >> (b & 63));
                    break;
                case 0x88:
                    r = a >> (b & 63);
                    break;
                case 0x89:
                    r = std::rotl(a, static_cast<int>(b & 63));
                    break;
                case 0x8A:
                    r = std::rotr(a, static_cast<int>(b & 63));
                    break;
                }
                push(v64(r));
                return;
            }
            if ((op >= 0x8B && op <= 0x91) || (op >= 0x99 && op <= 0x9F)) {
                const bool is32 = op <= 0x91;
                const uint32_t idx = (op - (is32 ? 0x8B : 0x99)) % 7;
                const Value a = pop();
                if (is32) {
                    const float x = f32(a);
                    float r = x;
                    switch (idx) {
                    case 0:
                        r = std::fabs(x);
                        break;
                    case 1:
                        r = -x;
                        break;
                    case 2:
                        r = std::ceil(x);
                        break;
                    case 3:
                        r = std::floor(x);
                        break;
                    case 4:
                        r = std::trunc(x);
                        break;
                    case 5:
                        r = std::nearbyint(x);
                        break;
                    case 6:
                        r = std::sqrt(x);
                        break;
                    }
                    push(vf32(r));
                } else {
                    const double x = f64(a);
                    double r = x;
                    switch (idx) {
                    case 0:
                        r = std::fabs(x);
                        break;
                    case 1:
                        r = -x;
                        break;
                    case 2:
                        r = std::ceil(x);
                        break;
                    case 3:
                        r = std::floor(x);
                        break;
                    case 4:
                        r = std::trunc(x);
                        break;
                    case 5:
                        r = std::nearbyint(x);
                        break;
                    case 6:
                        r = std::sqrt(x);
                        break;
                    }
                    push(vf64(r));
                }
                return;
            }
            if ((op >= 0x92 && op <= 0x98) || (op >= 0xA0 && op <= 0xA6)) {
                const bool is32 = op <= 0x98;
                const uint32_t idx = (op - (is32 ? 0x92 : 0xA0)) % 7;
                const Value b = pop(), a = pop();
                if (is32) {
                    const float x = f32(a), y = f32(b);
                    float r = 0;
                    switch (idx) {
                    case 0:
                        r = x + y;
                        break;
                    case 1:
                        r = x - y;
                        break;
                    case 2:
                        r = x * y;
                        break;
                    case 3:
                        r = x / y;
                        break;
                    case 4:
                        r = std::isnan(x) || std::isnan(y) ? std::numeric_limits<float>::quiet_NaN() : std::fmin(x, y);
                        break;
                    case 5:
                        r = std::isnan(x) || std::isnan(y) ? std::numeric_limits<float>::quiet_NaN() : std::fmax(x, y);
                        break;
                    case 6:
                        r = std::copysign(x, y);
                        break;
                    }
                    push(vf32(r));
                } else {
                    const double x = f64(a), y = f64(b);
                    double r = 0;
                    switch (idx) {
                    case 0:
                        r = x + y;
                        break;
                    case 1:
                        r = x - y;
                        break;
                    case 2:
                        r = x * y;
                        break;
                    case 3:
                        r = x / y;
                        break;
                    case 4:
                        r = std::isnan(x) || std::isnan(y) ? std::numeric_limits<double>::quiet_NaN() : std::fmin(x, y);
                        break;
                    case 5:
                        r = std::isnan(x) || std::isnan(y) ? std::numeric_limits<double>::quiet_NaN() : std::fmax(x, y);
                        break;
                    case 6:
                        r = std::copysign(x, y);
                        break;
                    }
                    push(vf64(r));
                }
                return;
            }
            if (op >= 0xA7 && op <= 0xC4) {
                executeConvert(op, stack);
                return;
            }
            throw Unsupported("numeric " + wasm::instrName(op));
        }

        void executeConvert(uint32_t op, std::vector<Value> &stack) {
            const auto pop = [&]() -> Value {
                if (stack.empty())
                    throw WasmTrap("operand stack underflow");
                Value v = stack.back();
                stack.pop_back();
                return v;
            };
            const auto push = [&](Value v) { stack.push_back(v); };
            const Value a = pop();
            const auto truncChecked = [](double x, double lo, double hi) {
                if (std::isnan(x) || x < lo || x >= hi)
                    throw WasmTrap("float-to-int out of range");
                return std::trunc(x);
            };
            const double i32Lo = -std::ldexp(1.0, 31);
            const double i32Hi = std::ldexp(1.0, 31);
            const double u32Hi = std::ldexp(1.0, 32);
            const double i64Lo = -std::ldexp(1.0, 63);
            const double i64Hi = std::ldexp(1.0, 63);
            const double u64Hi = std::ldexp(1.0, 64);
            switch (op) {
            case 0xA7:
                push(v32(static_cast<uint32_t>(a.lo)));
                break; // i32.wrap_i64
            case 0xA8:
                push(v32(static_cast<uint32_t>(static_cast<int32_t>(truncChecked(f32(a), i32Lo, i32Hi)))));
                break;
            case 0xA9:
                push(v32(static_cast<uint32_t>(truncChecked(f32(a), 0.0, u32Hi))));
                break;
            case 0xAA:
                push(v32(static_cast<uint32_t>(static_cast<int32_t>(truncChecked(f64(a), i32Lo, i32Hi)))));
                break;
            case 0xAB:
                push(v32(static_cast<uint32_t>(truncChecked(f64(a), 0.0, u32Hi))));
                break;
            case 0xAC:
                push(v64(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a.lo)))));
                break;
            case 0xAD:
                push(v64(static_cast<uint32_t>(a.lo)));
                break;
            case 0xAE:
                push(v64(static_cast<uint64_t>(static_cast<int64_t>(truncChecked(f32(a), i64Lo, i64Hi)))));
                break;
            case 0xAF:
                push(v64(static_cast<uint64_t>(truncChecked(f32(a), 0.0, u64Hi))));
                break;
            case 0xB0:
                push(v64(static_cast<uint64_t>(static_cast<int64_t>(truncChecked(f64(a), i64Lo, i64Hi)))));
                break;
            case 0xB1:
                push(v64(static_cast<uint64_t>(truncChecked(f64(a), 0.0, u64Hi))));
                break;
            case 0xB2:
                push(vf32(static_cast<float>(static_cast<int32_t>(a.lo))));
                break;
            case 0xB3:
                push(vf32(static_cast<float>(static_cast<uint32_t>(a.lo))));
                break;
            case 0xB4:
                push(vf32(static_cast<float>(static_cast<int64_t>(a.lo))));
                break;
            case 0xB5:
                push(vf32(static_cast<float>(a.lo)));
                break;
            case 0xB6:
                push(vf32(static_cast<float>(f64(a))));
                break;
            case 0xB7:
                push(vf64(static_cast<double>(static_cast<int32_t>(a.lo))));
                break;
            case 0xB8:
                push(vf64(static_cast<double>(static_cast<uint32_t>(a.lo))));
                break;
            case 0xB9:
                push(vf64(static_cast<double>(static_cast<int64_t>(a.lo))));
                break;
            case 0xBA:
                push(vf64(static_cast<double>(a.lo)));
                break;
            case 0xBB:
                push(vf64(static_cast<double>(f32(a))));
                break;
            case 0xBC:
                push(v32(static_cast<uint32_t>(a.lo)));
                break;
            case 0xBD:
                push(v64(a.lo));
                break;
            case 0xBE:
                push(v32(static_cast<uint32_t>(a.lo)));
                break;
            case 0xBF:
                push(v64(a.lo));
                break;
            case 0xC0:
                push(v32(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(a.lo)))));
                break;
            case 0xC1:
                push(v32(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(a.lo)))));
                break;
            case 0xC2:
                push(v64(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(a.lo)))));
                break;
            case 0xC3:
                push(v64(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(a.lo)))));
                break;
            case 0xC4:
                push(v64(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a.lo)))));
                break;
            default:
                throw Unsupported("convert");
            }
        }

        const wasm::Module &m_;
        std::vector<uint8_t> mem_;
        std::vector<Value> globals_;
        uint64_t pages_ = 1;
        std::unordered_map<uint64_t, std::shared_ptr<const wasm::FunctionAnalysis>> analyses_;
        std::unordered_map<uint64_t, BlockInfo> blockInfo_;
    };

} // namespace wasmtest
