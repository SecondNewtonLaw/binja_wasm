//
// Created by Dottik on 28/10/2026.
//

#include "bn/regs.hpp"

#include <array>
#include <format>

namespace bnwasm {

namespace {

constexpr std::array<uint32_t, static_cast<size_t>(Bank::Count)> kCounts = {
    1, kNLocals, kNLocals, kNGlobals, kNArgs, kNResults, kNStack, kNStack, kNLocals, kNStack, kNArgs, kNResults,
};
constexpr std::array<const char *, static_cast<size_t>(Bank::Count)> kPrefix = {
    "sp", "l", "v", "g", "a", "r", "s", "x", "ld", "sd", "ah", "rh",
};

// Ids start at 1; bank bases are cumulative.
constexpr std::array<uint32_t, static_cast<size_t>(Bank::Count)> bases() {
    std::array<uint32_t, static_cast<size_t>(Bank::Count)> b{};
    uint32_t next = 1;
    for (size_t i = 0; i < b.size(); ++i) {
        b[i] = next;
        next += kCounts[i];
    }
    return b;
}
constexpr auto kBases = bases();

} // namespace

uint32_t regId(Bank bank, uint32_t index) { return kBases[static_cast<size_t>(bank)] + index; }

const std::vector<RegDef> &allRegs() {
    static const std::vector<RegDef> regs = [] {
        std::vector<RegDef> r;
        for (size_t b = 0; b < kCounts.size(); ++b) {
            const auto bank = static_cast<Bank>(b);
            for (uint32_t i = 0; i < kCounts[b]; ++i) {
                const uint32_t id = regId(bank, i);
                if (bank == Bank::Sp) {
                    r.push_back({"sp", 4, id, false});
                } else if (bank == Bank::G && i == 0) {
                    r.push_back({"g0", 4, id, false});
                } else if (bank == Bank::Ld) {
                    r.push_back({std::format("ld{}", i), 4, regId(Bank::L, i), true});
                } else if (bank == Bank::Sd) {
                    r.push_back({std::format("sd{}", i), 4, regId(Bank::S, i), true});
                } else {
                    const uint32_t size = bank == Bank::Lv || bank == Bank::X ? 16 : 8;
                    r.push_back({std::format("{}{}", kPrefix[b], i), size, id, false});
                }
            }
        }
        return r;
    }();
    return regs;
}

} // namespace bnwasm
