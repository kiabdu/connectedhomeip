/*
 *
 *    Copyright (c) 2026 Project CHIP Authors
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

#include <lib/core/CHIPError.h>
#include <lib/core/DataModelTypes.h>
#include <lib/core/TLV.h>
#include <protocols/interaction_model/StatusCode.h>

namespace chip {
namespace fault_injection {

/**
 * @brief Types of faults that can be injected
 */
enum class FaultType : uint8_t
{
    kNone           = 0,
    kDataCorruption = 1,
    kTimingDelay    = 2,
    kCommandDrop    = 3,
    kInvalidStatus  = 4,
};

/**
 * @brief Selects whether a fault matches attribute accesses or command invocations
 */
enum class FaultTarget : uint8_t
{
    kAttribute = 1,
    kCommand   = 2,
};

/**
 * @brief Selects which member of FaultConfig::corruptValue is valid
 */
enum class FaultValueType : uint8_t
{
    kInt32 = 1,
    kFloat = 2,
    kBool  = 3,
};

/**
 * @brief Configuration for a single fault injection
 */
struct FaultConfig
{
    FaultType faultType = FaultType::kNone;
    FaultTarget target  = FaultTarget::kAttribute;
    ClusterId cluster  = 0;
    AttributeId attribute = 0;
    CommandId command    = 0;
    uint32_t delayMs     = 0;
    Protocols::InteractionModel::Status status = Protocols::InteractionModel::Status::Success;

    FaultValueType corruptValueType = FaultValueType::kInt32;
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
    void ClearFault(ClusterId cluster, AttributeId attribute);
    void ClearAllFaults();

    bool HasFaultForAttribute(ClusterId cluster, AttributeId attribute) const;
    bool HasFaultForCommand(ClusterId cluster, CommandId command) const;

    const FaultConfig * GetFaultForAttribute(ClusterId cluster, AttributeId attribute) const;
    const FaultConfig * GetFaultForCommand(ClusterId cluster, CommandId command) const;

    CHIP_ERROR ApplyDataCorruption(const FaultConfig & config, TLV::TLVWriter & writer);
    uint32_t GetDelayForFault(const FaultConfig & config) const;
    bool ShouldDropCommand(const FaultConfig & config) const;
    Protocols::InteractionModel::Status GetInjectedStatus(const FaultConfig & config) const;

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
