//
// Created by Dottik on 28/10/2026.
//

#pragma once

#include <binaryninjaapi.h>

namespace bnwasm {

class WasmArchitecture : public BinaryNinja::Architecture {
public:
    WasmArchitecture();

    BNEndianness GetEndianness() const override;
    size_t GetAddressSize() const override;
    size_t GetDefaultIntegerSize() const override;
    size_t GetInstructionAlignment() const override;
    size_t GetMaxInstructionLength() const override;

    bool GetInstructionInfo(const uint8_t *data, uint64_t addr, size_t maxLen,
                            BinaryNinja::InstructionInfo &result) override;
    bool GetInstructionText(const uint8_t *data, uint64_t addr, size_t &len,
                            std::vector<BinaryNinja::InstructionTextToken> &result) override;
    bool GetInstructionLowLevelIL(const uint8_t *data, uint64_t addr, size_t &len,
                                  BinaryNinja::LowLevelILFunction &il) override;

    std::string GetRegisterName(uint32_t reg) override;
    BNRegisterInfo GetRegisterInfo(uint32_t reg) override;
    uint32_t GetStackPointerRegister() override;
    std::vector<uint32_t> GetAllRegisters() override;
    std::vector<uint32_t> GetFullWidthRegisters() override;
    std::vector<uint32_t> GetGlobalRegisters() override;

    std::string GetIntrinsicName(uint32_t intrinsic) override;
    std::vector<uint32_t> GetAllIntrinsics() override;
    std::vector<BinaryNinja::NameAndType> GetIntrinsicInputs(uint32_t intrinsic) override;
    std::vector<BinaryNinja::Confidence<BinaryNinja::Ref<BinaryNinja::Type>>>
    GetIntrinsicOutputs(uint32_t intrinsic) override;
};

void registerArchitecture();

} // namespace bnwasm
