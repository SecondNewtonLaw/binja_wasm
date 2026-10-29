//
// Created by Dottik on 28/10/2026.
//

#include "bn/state.hpp"

#include <mutex>

namespace bnwasm {

// libc++ has no std::atomic<std::shared_ptr>.
static std::mutex g_mutex;
static std::shared_ptr<State> g_state;

void setState(std::shared_ptr<State> s) {
    std::lock_guard lock(g_mutex);
    g_state = std::move(s);
}
std::shared_ptr<State> state() {
    std::lock_guard lock(g_mutex);
    return g_state;
}

} // namespace bnwasm
