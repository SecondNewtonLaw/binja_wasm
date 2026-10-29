//
// Created by Dottik on 28/10/2026.
//

#pragma once

#include <memory>

#include "wasm/analysis.hpp"
#include "wasm/module.hpp"
#include "wasm/pointers.hpp"

namespace bnwasm {

// The view publishes the parsed module here for the architecture; one module at a time.
struct State {
    wasm::Module module;
    wasm::AnalysisCache cache;
    wasm::PtrInfo ptrs;
    explicit State(wasm::Module m) : module(std::move(m)) {}
};

void setState(std::shared_ptr<State> state);
std::shared_ptr<State> state();

} // namespace bnwasm
