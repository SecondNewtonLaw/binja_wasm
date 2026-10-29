//
// Created by Dottik on 28/10/2026.
//

#include "bn/view.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <optional>
#include <vector>

#include "bn/lifter.hpp"
#include "bn/regs.hpp"
#include "bn/state.hpp"
#include "wasm/decode.hpp"
#include "wasm/module.hpp"
#include "wasm/opcodes.hpp"

namespace bnwasm {

using namespace BinaryNinja;

// br_table targets are reachable only through the table, so BN needs them registered as indirect
// branches to create their blocks (otherwise the lifted compare chain falls back to `jump(addr)`).
class TableResolver : public BinaryDataNotification {
public:
    explicit TableResolver(Ref<Architecture> arch) : BinaryDataNotification(FunctionAdded), arch_(std::move(arch)) {}

    void OnAnalysisFunctionAdded(BinaryView *, Function *func) override {
        const auto st = state();
        if (!st || func->GetStart() < kCodeBase || func->GetStart() >= kExternBase)
            return;
        const auto fa = st->cache.get(st->module, func->GetStart() - kCodeBase);
        if (!fa)
            return;
        for (const auto &[source, targets] : fa->tables) {
            std::vector<ArchAndAddr> branches;
            std::vector<uint64_t> seen;
            for (uint64_t t : targets) {
                if (t == wasm::kUnresolved || t == wasm::kFunctionEnd ||
                    std::find(seen.begin(), seen.end(), t) != seen.end())
                    continue;
                seen.push_back(t);
                branches.push_back({arch_, t + kCodeBase});
            }
            if (!branches.empty())
                func->SetAutoIndirectBranches(arch_, source + kCodeBase, branches);
        }
    }

private:
    Ref<Architecture> arch_;
};

namespace {

// Access type for a load/store opcode, or null.
Ref<Type> accessType(uint32_t op) {
    switch (op) {
    case 0x28: case 0x36: return Type::IntegerType(4, true);
    case 0x29: case 0x37: return Type::IntegerType(8, true);
    case 0x2A: case 0x38: return Type::FloatType(4);
    case 0x2B: case 0x39: return Type::FloatType(8);
    case 0x2C: case 0x2D: case 0x30: case 0x31: case 0x3A: case 0x3C: return Type::IntegerType(1, false);
    case 0x2E: case 0x2F: case 0x32: case 0x33: case 0x3B: case 0x3D: return Type::IntegerType(2, false);
    case 0x34: case 0x35: case 0x3E: return Type::IntegerType(4, false);
    default: return nullptr;
    }
}

} // namespace

// Constant-address accesses (`i32.const A; <load/store off>`) become named data variables so HLIL
// prints `data_153b53` instead of `*0x153b53`.
void WasmView::defineDataVariables(const wasm::Module &m) {
    std::map<uint64_t, Ref<Type>> vars;
    for (const auto &f : m.functions) {
        if (!f.address)
            continue;
        uint64_t pos = *f.address;
        const uint64_t end = pos + f.size;
        std::optional<uint32_t> last;
        while (pos < end) {
            wasm::Instr ins;
            try {
                ins = wasm::decode(m.data, pos);
            } catch (const wasm::DecodeError &) {
                break;
            }
            pos += ins.size ? ins.size : 1;
            if (ins.op == wasm::op::I32Const) {
                last = static_cast<uint32_t>(ins.imm0);
                continue;
            }
            if (last) {
                if (const auto type = accessType(ins.op)) {
                    const uint64_t a = static_cast<uint64_t>(*last) + static_cast<uint64_t>(ins.imm0);
                    const bool mapped = std::any_of(m.dataSegments.begin(), m.dataSegments.end(), [&](const auto &s) {
                        return a >= s.memAddr && a + type->GetWidth() <= s.memAddr + s.size;
                    });
                    if (mapped)
                        vars.try_emplace(a, type);
                    else if (a + type->GetWidth() <= kCodeBase)
                        vars.try_emplace(a, type);
                }
            }
            last.reset();
        }
    }

    // Zero-initialised memory (.bss) has no data segment; back each touched gap with an empty segment.
    auto segs = m.dataSegments;
    std::sort(segs.begin(), segs.end(), [](const auto &a, const auto &b) { return a.memAddr < b.memAddr; });
    // Touched addresses merge only within kClusterSlack, so a stray constant can't zero-fill a huge range.
    constexpr uint64_t kClusterSlack = 0x1000;
    struct Gap {
        uint64_t lo = ~0ull, hi = 0;
    };
    std::vector<Gap> gaps;
    uint64_t curGapStart = ~0ull;
    for (const auto &[addr, type] : vars) {
        const auto after = std::upper_bound(segs.begin(), segs.end(), addr,
                                            [](uint64_t a, const auto &s) { return a < s.memAddr; });
        const uint64_t gapStart = after == segs.begin() ? 0 : std::prev(after)->memAddr + std::prev(after)->size;
        const uint64_t gapEnd = after == segs.end() ? kCodeBase : after->memAddr;
        if (addr < gapStart || addr + type->GetWidth() > gapEnd)
            continue;  // inside a data segment or straddling one
        if (gaps.empty() || gapStart != curGapStart || addr > gaps.back().hi + kClusterSlack) {
            gaps.emplace_back();
            curGapStart = gapStart;
        }
        Gap &g = gaps.back();
        g.lo = std::min(g.lo, addr);
        g.hi = std::max<uint64_t>(g.hi, addr + type->GetWidth());
    }
    for (const auto &g : gaps)
        AddAutoSegment(g.lo, g.hi - g.lo, 0, 0, SegmentReadable | SegmentWritable | SegmentContainsData);

    uint64_t next = 0;
    for (const auto &[addr, type] : vars) {
        if (addr < next)
            continue;
        DefineDataVariable(addr, type);
        DefineAutoSymbol(new Symbol(DataSymbol, std::format("data_{:x}", addr), addr));
        next = addr + type->GetWidth();
    }
}

WasmView::WasmView(BinaryView *data) : BinaryView("WASM-native", data->GetFile(), data), m_raw(data) {}

bool WasmView::Init() {
    const uint64_t size = m_raw->GetLength();
    std::vector<uint8_t> bytes(size);
    if (m_raw->Read(bytes.data(), 0, size) != size)
        return false;
    auto module = wasm::parseModule(std::move(bytes));
    if (!module)
        return false;
    auto st = std::make_shared<State>(std::move(*module));
    const wasm::Module &m = st->module;

    // Linear memory: active data segments, file-backed, below the code window.
    auto segs = m.dataSegments;
    std::sort(segs.begin(), segs.end(), [](const auto &a, const auto &b) { return a.memAddr < b.memAddr; });
    uint64_t memEnd = 0;
    for (const auto &s : segs) {
        if (!s.size || s.memAddr < memEnd || s.memAddr + s.size > kCodeBase)
            continue;
        AddAutoSegment(s.memAddr, s.size, s.fileOffset, s.size, SegmentReadable | SegmentWritable | SegmentContainsData);
        memEnd = s.memAddr + s.size;
    }

    AddAutoSegment(kCodeBase, size, 0, size,
                   SegmentReadable | SegmentExecutable | SegmentContainsData | SegmentContainsCode);

    size_t imports = 0;
    for (size_t i = 0; i < m.functions.size(); ++i)
        if (!m.functions[i].address)
            imports = i + 1;
    if (imports) {
        const uint64_t len = 8ull * imports;
        AddAutoSegment(kExternBase, len, 0, std::min<uint64_t>(len, size),
                       SegmentReadable | SegmentExecutable | SegmentContainsCode);
    }

    Ref<Architecture> arch = Architecture::GetByName("wasm-native");
    if (!arch)
        return false;
    SetDefaultArchitecture(arch);
    Ref<Platform> platform = arch->GetStandalonePlatform();
    SetDefaultPlatform(platform);

    for (uint32_t i = 0; i < m.functions.size(); ++i)
        DefineAutoSymbol(new Symbol(FunctionSymbol, m.functions[i].name, funcAddress(m, i)));

    defineDataVariables(m);
    st->ptrs = wasm::inferPointers(m);

    setState(st);
    RegisterNotification(new TableResolver(arch));

    for (uint32_t index : m.entries)
        if (index < m.functions.size() && m.functions[index].address)
            AddFunctionForAnalysis(platform, funcAddress(m, index));
    return true;
}

namespace {

Ref<Type> valueType(uint8_t t, bool ptr = false) {
    switch (t) {
    case wasm::kI32: return ptr ? Type::PointerType(4, Type::VoidType()) : Type::IntegerType(4, true);
    case wasm::kI64: return Type::IntegerType(8, true);
    case wasm::kF32: return Type::FloatType(4);
    case wasm::kF64: return Type::FloatType(8);
    default: return Type::IntegerType(8, true);
    }
}

class WasmDebugInfoParser : public CustomDebugInfoParser {
public:
    WasmDebugInfoParser() : CustomDebugInfoParser("WASM-native") {}

    bool IsValid(Ref<BinaryView> view) override { return view->GetTypeName() == "WASM-native"; }

    bool ParseInfo(Ref<DebugInfo> info, Ref<BinaryView> view, Ref<BinaryView>, ProgressFunction) override {
        const auto st = state();
        if (!st)
            return false;
        const wasm::Module &m = st->module;
        Ref<Platform> platform = view->GetDefaultPlatform();
        for (uint32_t i = 0; i < m.functions.size(); ++i) {
            const wasm::FuncType *sig = m.funcSig(i);
            if (!sig)
                continue;
            std::vector<FunctionParameter> params;
            const auto &pi = st->ptrs;
            for (size_t k = 0; k < sig->params.size(); ++k)
                params.emplace_back("a" + std::to_string(k),
                                    valueType(sig->params[k], i < pi.param.size() && k < pi.param[i].size() && pi.param[i][k]));
            Ref<Type> ret = sig->results.empty() ? Type::VoidType() : valueType(sig->results[0], i < pi.ret.size() && pi.ret[i]);
            const auto &f = m.functions[i];
            DebugFunctionInfo fn(f.name, f.name, f.name, funcAddress(m, i), nullptr, platform, {}, {});
            fn.type = Type::FunctionType(ret, nullptr, params);
            info->AddFunction(fn);
        }
        return true;
    }
};

} // namespace

WasmViewType::WasmViewType() : BinaryViewType("WASM-native", "WASM File (native)") {}

Ref<BinaryView> WasmViewType::Create(BinaryView *data) {
    try {
        return new WasmView(data);
    } catch (...) {
        return nullptr;
    }
}

bool WasmViewType::IsTypeValidForData(BinaryView *data) {
    static constexpr std::array<uint8_t, 8> kHeader = {0x00, 'a', 's', 'm', 0x01, 0x00, 0x00, 0x00};
    std::array<uint8_t, 8> header{};
    return data->Read(header.data(), 0, header.size()) == header.size() && header == kHeader;
}

void registerView() {
    BinaryViewType::Register(new WasmViewType());
    new WasmDebugInfoParser();
}

} // namespace bnwasm
