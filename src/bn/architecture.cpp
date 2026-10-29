//
// Created by Dottik on 28/10/2026.
//

#include "bn/architecture.hpp"

#include <algorithm>
#include <format>

#include "bn/intrinsics.hpp"
#include "bn/lifter.hpp"
#include "bn/regs.hpp"
#include "bn/state.hpp"
#include "wasm/decode.hpp"
#include "wasm/opcodes.hpp"

namespace bnwasm {

using namespace BinaryNinja;

namespace {

class WasmCallingConvention : public CallingConvention {
public:
    explicit WasmCallingConvention(Architecture *arch) : CallingConvention(arch, "wasm") {}

    std::vector<uint32_t> GetCallerSavedRegisters() override {
        std::vector<uint32_t> regs;
        for (uint32_t i = 0; i < kNArgs; ++i)
            regs.push_back(regId(Bank::A, i));
        for (uint32_t i = 0; i < kNResults; ++i)
            regs.push_back(regId(Bank::R, i));
        return regs;
    }
    std::vector<uint32_t> GetIntegerArgumentRegisters() override {
        std::vector<uint32_t> regs;
        for (uint32_t i = 0; i < kNArgs; ++i)
            regs.push_back(regId(Bank::A, i));
        return regs;
    }
    uint32_t GetIntegerReturnValueRegister() override { return regId(Bank::R, 0); }
    uint32_t GetHighIntegerReturnValueRegister() override { return regId(Bank::R, 1); }
};

std::string hex(int64_t v) {
    if (v < 0)
        return std::format("-0x{:x}", static_cast<uint64_t>(-v));
    return v < 10 ? std::format("{}", v) : std::format("0x{:x}", v);
}

} // namespace

// Named "wasm-native" to coexist with the Python "WASM" plugin.
WasmArchitecture::WasmArchitecture() : Architecture("wasm-native") {}

BNEndianness WasmArchitecture::GetEndianness() const { return LittleEndian; }
size_t WasmArchitecture::GetAddressSize() const { return 4; }
size_t WasmArchitecture::GetDefaultIntegerSize() const { return 4; }
size_t WasmArchitecture::GetInstructionAlignment() const { return 1; }
// BN hard-caps this at 256.
size_t WasmArchitecture::GetMaxInstructionLength() const { return 256; }

bool WasmArchitecture::GetInstructionInfo(const uint8_t *, uint64_t addr, size_t maxLen, InstructionInfo &result) {
    const auto st = state();
    if (!st || maxLen == 0)
        return false;
    if (addr >= kExternBase) {
        result.length = 1;
        result.AddBranch(FunctionReturn);
        return true;
    }
    if (addr < kCodeBase)
        return false;
    const uint64_t off = addr - kCodeBase;
    const auto fa = st->cache.get(st->module, off);
    wasm::Instr ins;
    try {
        ins = wasm::decode(st->module.data, off);
    } catch (const wasm::DecodeError &) {
        return false;
    }
    // BN caps instruction length; an oversized br_table claims its first 256 bytes and never falls through.
    result.length = std::min<size_t>(ins.size, maxLen);

    const uint64_t *target = fa ? fa->branchTarget(off) : nullptr;
    const auto at = [&](uint64_t t) { return t + kCodeBase; };
    switch (ins.op) {
    case 0x10:
    case 0x12:
        if (static_cast<uint64_t>(ins.imm0) < st->module.functions.size()) {
            result.AddBranch(CallDestination, funcAddress(st->module, static_cast<uint32_t>(ins.imm0)));
            if (ins.op == 0x12)
                result.AddBranch(FunctionReturn);
        }
        break;
    case 0x04:
        if (target && *target != wasm::kFunctionEnd) {
            result.AddBranch(FalseBranch, at(*target));
            result.AddBranch(TrueBranch, addr + ins.size);
        }
        break;
    case 0xD5:
    case 0xD6:
    case 0x0D:
        if (target && *target != wasm::kFunctionEnd) {
            result.AddBranch(TrueBranch, at(*target));
            result.AddBranch(FalseBranch, addr + ins.size);
        }
        break;
    case 0x0E: {
        result.AddBranch(UnresolvedBranch);  // targets come from Function::SetAutoIndirectBranches
        break;
    }
    case 0x0F:
        result.AddBranch(FunctionReturn);
        break;
    case 0x00:
    case 0x08:
    case 0x09:
    case 0x0A:
        result.AddBranch(ExceptionBranch);
        break;
    case 0x0B:
        if (target && *target != wasm::kFunctionEnd)
            result.AddBranch(UnconditionalBranch, at(*target));
        else if (target)
            result.AddBranch(FunctionReturn);
        break;
    case 0x13:
    case 0x15:
        result.AddBranch(FunctionReturn);
        break;
    default:
        if (target && *target != wasm::kFunctionEnd)
            result.AddBranch(UnconditionalBranch, at(*target));
        break;
    }
    return true;
}

bool WasmArchitecture::GetInstructionText(const uint8_t *, uint64_t addr, size_t &len,
                                          std::vector<InstructionTextToken> &result) {
    const auto st = state();
    if (!st)
        return false;
    if (addr >= kExternBase) {
        len = 1;
        const uint64_t index = (addr - kExternBase) / 8;
        result.emplace_back(TextToken, index < st->module.functions.size() ? st->module.functions[index].name : "extern");
        return true;
    }
    if (addr < kCodeBase)
        return false;
    wasm::Instr ins;
    try {
        ins = wasm::decode(st->module.data, addr - kCodeBase, true);
    } catch (const wasm::DecodeError &) {
        return false;
    }
    len = std::min<size_t>(ins.size, 256);
    result.emplace_back(InstructionToken, wasm::instrName(ins.op));
    bool first = true;
    for (const auto &op : ins.ops) {
        if (op.kind == wasm::Operand::Mem && op.text.empty())
            continue;
        if (first)
            result.emplace_back(TextToken, " ");
        else
            result.emplace_back(OperandSeparatorToken, ", ");
        first = false;
        switch (op.kind) {
        case wasm::Operand::Num:
        case wasm::Operand::Imm:
            result.emplace_back(IntegerToken, hex(op.n), static_cast<uint64_t>(op.n));
            break;
        case wasm::Operand::Func: {
            const auto index = static_cast<uint64_t>(op.n);
            if (index < st->module.functions.size()) {
                const auto &f = st->module.functions[index];
                result.emplace_back(f.address ? PossibleAddressToken : TextToken, f.name,
                                    funcAddress(st->module, static_cast<uint32_t>(index)));
            } else {
                result.emplace_back(IntegerToken, hex(op.n), static_cast<uint64_t>(op.n));
            }
            break;
        }
        default:
            result.emplace_back(TextToken, op.text);
            break;
        }
    }
    return true;
}

bool WasmArchitecture::GetInstructionLowLevelIL(const uint8_t *, uint64_t addr, size_t &len, LowLevelILFunction &il) {
    const auto st = state();
    if (!st)
        return false;
    if (addr >= kExternBase) {
        len = 1;
        il.AddInstruction(il.Return(il.ConstPointer(4, 0)));
        return true;
    }
    if (addr < kCodeBase)
        return false;
    const uint64_t off = addr - kCodeBase;
    wasm::Instr ins;
    try {
        ins = wasm::decode(st->module.data, off);
    } catch (const wasm::DecodeError &) {
        return false;
    }
    len = std::min<size_t>(ins.size, 256);
    const auto fa = st->cache.get(st->module, off);
    lift(il, this, ins, off, st->module, fa.get());
    return true;
}

std::string WasmArchitecture::GetRegisterName(uint32_t reg) {
    const auto &regs = allRegs();
    return reg >= 1 && reg <= regs.size() ? regs[reg - 1].name : std::format("temp{}", reg & 0x7FFFFFFF);
}

BNRegisterInfo WasmArchitecture::GetRegisterInfo(uint32_t reg) {
    BNRegisterInfo info{};
    const auto &regs = allRegs();
    if (reg < 1 || reg > regs.size()) {
        info.fullWidthRegister = reg;
        info.size = 8;
        return info;
    }
    const RegDef &d = regs[reg - 1];
    info.fullWidthRegister = d.full;
    info.offset = 0;
    info.size = d.size;
    info.extend = d.zeroExtend ? ZeroExtendToFullWidth : NoExtend;
    return info;
}

// Global 0 is the shadow stack pointer in clang-produced modules.
uint32_t WasmArchitecture::GetStackPointerRegister() { return regId(Bank::G, 0); }

std::vector<uint32_t> WasmArchitecture::GetAllRegisters() {
    std::vector<uint32_t> out;
    for (uint32_t i = 1; i <= allRegs().size(); ++i)
        out.push_back(i);
    return out;
}

std::vector<uint32_t> WasmArchitecture::GetFullWidthRegisters() {
    std::vector<uint32_t> out;
    const auto &regs = allRegs();
    for (uint32_t i = 1; i <= regs.size(); ++i)
        if (regs[i - 1].full == i)
            out.push_back(i);
    return out;
}

std::vector<uint32_t> WasmArchitecture::GetGlobalRegisters() { return {}; }

std::string WasmArchitecture::GetIntrinsicName(uint32_t id) {
    const auto &defs = intrinsics();
    return id < defs.size() ? defs[id].name : "";
}

std::vector<uint32_t> WasmArchitecture::GetAllIntrinsics() {
    std::vector<uint32_t> out(intrinsics().size());
    for (uint32_t i = 0; i < out.size(); ++i)
        out[i] = i;
    return out;
}

std::vector<NameAndType> WasmArchitecture::GetIntrinsicInputs(uint32_t id) {
    const auto &defs = intrinsics();
    std::vector<NameAndType> out;
    if (id >= defs.size())
        return out;
    for (uint32_t i = 0; i < defs[id].nin; ++i) {
        const bool vec = i < defs[id].vecIn.size() && defs[id].vecIn[i] == 'v';
        out.emplace_back(std::format("p{}", i), Type::IntegerType(vec ? 16 : 8, false));
    }
    return out;
}

std::vector<Confidence<Ref<Type>>> WasmArchitecture::GetIntrinsicOutputs(uint32_t id) {
    const auto &defs = intrinsics();
    std::vector<Confidence<Ref<Type>>> out;
    if (id >= defs.size())
        return out;
    for (uint32_t i = 0; i < defs[id].nout; ++i)
        out.emplace_back(Type::IntegerType(defs[id].vecOut ? 16 : 8, false));
    return out;
}

void registerArchitecture() {
    auto *arch = new WasmArchitecture();
    Architecture::Register(arch);
    Ref<CallingConvention> cc = new WasmCallingConvention(arch);
    arch->RegisterCallingConvention(cc);
    arch->SetDefaultCallingConvention(cc);
    if (Ref<Platform> platform = arch->GetStandalonePlatform())
        platform->RegisterDefaultCallingConvention(cc);
}

} // namespace bnwasm
