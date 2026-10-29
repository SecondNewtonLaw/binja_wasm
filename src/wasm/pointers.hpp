//
// Created by Dottik on 28/10/2026.
//

#pragma once

#include <vector>

#include "wasm/module.hpp"

namespace wasm {

// Which i32 parameters and results behave as linear-memory addresses (wasm has no pointer type).
struct PtrInfo {
    std::vector<std::vector<char>> param;  // [function index][param index]
    std::vector<char> ret;                 // [function index], first result
};

PtrInfo inferPointers(const Module &m);

} // namespace wasm
