//
// Created by Dottik on 28/10/2026.
//

#include "bn/state.hpp"

#include <atomic>

namespace bnwasm {

static std::atomic<std::shared_ptr<State>> g_state;

void setState(std::shared_ptr<State> s) { g_state.store(std::move(s)); }
std::shared_ptr<State> state() { return g_state.load(); }

} // namespace bnwasm
