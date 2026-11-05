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
#include <optional>

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
    kNull  = 4, ///< Encodes a TLV null; corruptValue is unused
};

/**
 * @brief Configuration for a single fault injection
 *
 * A fault is identified by (target, endpoint, cluster, attribute or command).
 * An empty `endpoint` matches every endpoint.
 */
struct FaultConfig
{
    FaultType faultType = FaultType::kNone;
    FaultTarget target  = FaultTarget::kAttribute;
    std::optional<EndpointId> endpoint;
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
    } corruptValue = {};
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
 *
 * Not thread-safe. All calls must happen on the Matter event loop or with the
 * Matter stack lock held (DeviceLayer::PlatformMgr().LockChipStack()).
 *
 * Pointers returned by GetFaultFor*() are valid until the next Configure or
 * Clear call.
 */
class FaultInjector
{
public:
    static FaultInjector & Instance();

    void SetDelegate(FaultInjectorDelegate * delegate);

    /**
     * Adds a fault, or replaces the fault with the same identity (see
     * FaultConfig). A fault for one endpoint and a fault for all endpoints on
     * the same path are separate entries. Returns CHIP_ERROR_NO_MEMORY when
     * kMaxFaults faults are configured and none has the same identity.
     */
    CHIP_ERROR ConfigureFault(const FaultConfig & config);

    /**
     * Removes the fault with exactly this identity. An empty `endpoint`
     * removes only the all-endpoints fault, not endpoint-specific ones.
     */
    void ClearAttributeFault(std::optional<EndpointId> endpoint, ClusterId cluster, AttributeId attribute);
    void ClearCommandFault(std::optional<EndpointId> endpoint, ClusterId cluster, CommandId command);
    void ClearAllFaults();

    bool HasFaultForAttribute(EndpointId endpoint, ClusterId cluster, AttributeId attribute) const;
    bool HasFaultForCommand(EndpointId endpoint, ClusterId cluster, CommandId command) const;

    /**
     * Returns the fault for a concrete path. A fault configured for `endpoint`
     * takes precedence over an all-endpoints fault on the same path.
     */
    const FaultConfig * GetFaultForAttribute(EndpointId endpoint, ClusterId cluster, AttributeId attribute) const;
    const FaultConfig * GetFaultForCommand(EndpointId endpoint, ClusterId cluster, CommandId command) const;

    /**
     * Writes the configured corrupt value to `writer` using `tag`. The value is
     * encoded as a signed int, float, bool or null depending on
     * corruptValueType, regardless of the real attribute type.
     */
    CHIP_ERROR ApplyDataCorruption(const FaultConfig & config, TLV::TLVWriter & writer, TLV::Tag tag);
    uint32_t GetDelayForFault(const FaultConfig & config) const;
    bool ShouldDropCommand(const FaultConfig & config) const;
    Protocols::InteractionModel::Status GetInjectedStatus(const FaultConfig & config) const;

    static constexpr size_t kMaxFaults = 16;

private:
    FaultInjector()  = default;
    ~FaultInjector() = default;

    const FaultConfig * Find(FaultTarget target, std::optional<EndpointId> endpoint, ClusterId cluster, uint32_t pathId) const;
    const FaultConfig * FindForConcretePath(FaultTarget target, EndpointId endpoint, ClusterId cluster, uint32_t pathId) const;
    void Remove(FaultTarget target, std::optional<EndpointId> endpoint, ClusterId cluster, uint32_t pathId);
    void NotifyConfigured(const FaultConfig & config, const char * verb);

    FaultInjectorDelegate * mDelegate = nullptr;
    FaultConfig mFaults[kMaxFaults];
    size_t mFaultCount = 0;
};

} // namespace fault_injection
} // namespace chip
