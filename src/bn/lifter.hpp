//
// Created by Dottik on 28/10/2026.
//

#pragma once

#include <binaryninjaapi.h>

#include "wasm/analysis.hpp"
#include "wasm/decode.hpp"
#include "wasm/module.hpp"

namespace bnwasm {

// Lifts the instruction at file offset `addr`. Branch edges mirror WasmArchitecture::GetInstructionInfo.
void lift(BinaryNinja::LowLevelILFunction &il, BinaryNinja::Architecture *arch, const wasm::Instr &ins,
          uint64_t addr, const wasm::Module &module, const wasm::FunctionAnalysis *fa);

// BN address of a function (code or extern stub).
uint64_t funcAddress(const wasm::Module &module, uint32_t index);

} // namespace bnwasm
