//
// Created by Dottik on 28/10/2026.
//

#include <binaryninjaapi.h>

#include "bn/architecture.hpp"
#include "bn/view.hpp"

BN_DECLARE_CORE_ABI_VERSION

extern "C" {
BINARYNINJAPLUGIN bool CorePluginInit() {
    bnwasm::registerArchitecture();
    bnwasm::registerView();
    return true;
}
}
