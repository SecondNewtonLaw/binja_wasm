//
// Created by Dottik on 28/10/2026.
//

#pragma once

#include <binaryninjaapi.h>

namespace wasm {
struct Module;
}

namespace bnwasm {

class WasmView : public BinaryNinja::BinaryView {
public:
    explicit WasmView(BinaryNinja::BinaryView *data);

    bool Init() override;
    void defineDataVariables(const wasm::Module &m);
    bool PerformIsExecutable() const override { return true; }
    size_t PerformGetAddressSize() const override { return 4; }

private:
    BinaryNinja::Ref<BinaryNinja::BinaryView> m_raw;
};

class WasmViewType : public BinaryNinja::BinaryViewType {
public:
    WasmViewType();

    BinaryNinja::Ref<BinaryNinja::BinaryView> Create(BinaryNinja::BinaryView *data) override;
    bool IsTypeValidForData(BinaryNinja::BinaryView *data) override;
};

void registerView();

} // namespace bnwasm
