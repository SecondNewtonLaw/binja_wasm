//
// Created by Dottik on 28/10/2026.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wasm {

// Locals beyond this limit are not modelled (matches the lifter's register set).
constexpr size_t kLocalLimit = 128;

constexpr uint32_t kNoType = ~0u;

// Imports come first in the function index space and have no body.
// Addresses are file offsets of the body's first instruction (after the locals header).
struct Function {
    std::string name;
    std::optional<uint64_t> address;
    uint64_t size = 0;
    uint32_t type = kNoType;
};

struct FuncType {
    std::vector<uint8_t> params;
    std::vector<uint8_t> results;
};

// Active data segment: bytes at `fileOffset` initialise linear memory at `memAddr`.
struct DataSegment {
    uint64_t memAddr;
    uint64_t fileOffset;
    uint64_t size;
};

struct Module {
    std::vector<uint8_t> data;
    std::vector<DataSegment> dataSegments;
    std::vector<FuncType> types;
    std::vector<Function> functions;
    std::vector<uint32_t> entries;
    std::vector<uint64_t> starts;                              // sorted body addresses
    std::unordered_map<uint64_t, uint32_t> addrIndex;          // body address -> function index
    std::unordered_set<uint64_t> simdFuncs;                    // bodies with v128 params/locals
    std::unordered_map<uint64_t, std::vector<uint8_t>> localTypes;  // body -> valtypes of first kLocalLimit locals

    const FuncType *sig(uint32_t type) const { return type < types.size() ? &types[type] : nullptr; }
    const FuncType *funcSig(uint32_t index) const {
        return index < functions.size() ? sig(functions[index].type) : nullptr;
    }
    // Index into `starts` of the function containing `addr`, or -1.
    ptrdiff_t startIndex(uint64_t addr) const;
};

// Reads an unsigned LEB128 at `pos`, advancing it. Returns nullopt on truncation or overflow.
std::optional<uint64_t> readUleb(std::span<const uint8_t> data, size_t &pos);

// Validates the 8-byte header and parses every section the plugin needs. Takes ownership of the bytes.
std::optional<Module> parseModule(std::vector<uint8_t> data);

} // namespace wasm
