/*
 *
 *    Copyright (c) 2025 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <cstdint>
#include <optional>

#include <lib/core/CHIPError.h>
#include <lib/core/Optional.h>
#include <app/ClusterProvider.h>

namespace chip {
namespace fault_injection {

/**
 * @brief Types of faults that can be injected
 */
enum class FaultType : uint8_t
{
    kNone            = 0,
    kDataCorruption  = 1,
    kTimingDelay     = 2,
    kCommandDrop     = 3,
    kInvalidStatus   = 4,
};

/**
 * @brief Configuration for a single fault injection
 */
struct FaultConfig
{
    FaultType faultType     = FaultType::kNone;
    chip::ClusterId cluster = 0;
    chip::AttributeId attribute;
    chip::CommandId command        = 0;
    uint32_t delayMs               = 0;
    chip::Protocols::InteractionModel::Status status =
        chip::Protocols::InteractionModel::Status::Success;

    union
    {
        int32_t intValue;
        float floatValue;
        bool boolValue;
    } corruptValue;
};

/**
 * @brief Delegate interface for fault injection notifications
 */
class FaultInjectorDelegate
{
public:
    virtual ~FaultInjectorDelegate() = default;

    virtual void OnFaultInjected(const FaultConfig & config, const char * description) = 0;
};

/**
 * @brief Singleton class for managing fault injection configuration
 */
class FaultInjector
{
public:
    static FaultInjector & Instance();

    void SetDelegate(FaultInjectorDelegate * delegate);

    CHIP_ERROR ConfigureFault(const FaultConfig & config);
    void ClearFault(chip::ClusterId cluster, chip::AttributeId attribute);
    void ClearAllFaults();

    bool HasFaultForAttribute(chip::ClusterId cluster, chip::AttributeId attribute) const;
    bool HasFaultForCommand(chip::ClusterId cluster, chip::CommandId command) const;

    const FaultConfig * GetFaultForAttribute(chip::ClusterId cluster, chip::AttributeId attribute) const;
    const FaultConfig * GetFaultForCommand(chip::ClusterId cluster, chip::CommandId command) const;

    CHIP_ERROR ApplyDataCorruption(const FaultConfig & config, chip::TLV::TLVWriter & writer);
    uint32_t GetDelayForFault(const FaultConfig & config) const;
    bool ShouldDropCommand(const FaultConfig & config) const;
    chip::Protocols::InteractionModel::Status GetInjectedStatus(const FaultConfig & config) const;

private:
    FaultInjector()  = default;
    ~FaultInjector() = default;

    static constexpr size_t kMaxFaults = 16;

    FaultInjectorDelegate * mDelegate = nullptr;
    FaultConfig mFaults[kMaxFaults];
    size_t mFaultCount = 0;
};

} // namespace fault_injection
} // namespace chip
