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

#include "FaultInjector.h"

#include <cinttypes>

#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>

namespace chip {
namespace fault_injection {

namespace {

const char * FaultTypeName(FaultType type)
{
    switch (type)
    {
    case FaultType::kDataCorruption:
        return "data_corruption";
    case FaultType::kTimingDelay:
        return "timing_delay";
    case FaultType::kCommandDrop:
        return "command_drop";
    case FaultType::kInvalidStatus:
        return "invalid_status";
    case FaultType::kNone:
        break;
    }
    return "none";
}

uint32_t PathId(const FaultConfig & config)
{
    return config.target == FaultTarget::kCommand ? config.command : config.attribute;
}

/// Compares fault identity. An empty endpoint only equals another empty endpoint.
bool Matches(const FaultConfig & config, FaultTarget target, std::optional<EndpointId> endpoint, ClusterId cluster,
             uint32_t pathId)
{
    return config.target == target && config.endpoint == endpoint && config.cluster == cluster && PathId(config) == pathId;
}

} // anonymous namespace

FaultInjector & FaultInjector::Instance()
{
    static FaultInjector sInstance;
    return sInstance;
}

void FaultInjector::SetDelegate(FaultInjectorDelegate * delegate)
{
    mDelegate = delegate;
}

CHIP_ERROR FaultInjector::ConfigureFault(const FaultConfig & config)
{
    VerifyOrReturnError(config.faultType != FaultType::kNone, CHIP_ERROR_INVALID_ARGUMENT);

    for (size_t i = 0; i < mFaultCount; ++i)
    {
        if (Matches(mFaults[i], config.target, config.endpoint, config.cluster, PathId(config)))
        {
            mFaults[i] = config;
            NotifyConfigured(config, "updated");
            return CHIP_NO_ERROR;
        }
    }

    VerifyOrReturnError(mFaultCount < kMaxFaults, CHIP_ERROR_NO_MEMORY);

    mFaults[mFaultCount] = config;
    ++mFaultCount;
    NotifyConfigured(config, "configured");
    return CHIP_NO_ERROR;
}

void FaultInjector::NotifyConfigured(const FaultConfig & config, const char * verb)
{
    const char * kind = config.target == FaultTarget::kCommand ? "command" : "attribute";
    if (config.endpoint.has_value())
    {
        ChipLogProgress(AppServer, "FaultInjector: %s %s fault on endpoint %u cluster 0x%08" PRIx32 " %s 0x%08" PRIx32, verb,
                        FaultTypeName(config.faultType), *config.endpoint, config.cluster, kind, PathId(config));
    }
    else
    {
        ChipLogProgress(AppServer, "FaultInjector: %s %s fault on all endpoints cluster 0x%08" PRIx32 " %s 0x%08" PRIx32, verb,
                        FaultTypeName(config.faultType), config.cluster, kind, PathId(config));
    }
    if (mDelegate != nullptr)
    {
        mDelegate->OnFaultInjected(config, FaultTypeName(config.faultType));
    }
}

void FaultInjector::ClearAttributeFault(std::optional<EndpointId> endpoint, ClusterId cluster, AttributeId attribute)
{
    Remove(FaultTarget::kAttribute, endpoint, cluster, attribute);
}

void FaultInjector::ClearCommandFault(std::optional<EndpointId> endpoint, ClusterId cluster, CommandId command)
{
    Remove(FaultTarget::kCommand, endpoint, cluster, command);
}

void FaultInjector::Remove(FaultTarget target, std::optional<EndpointId> endpoint, ClusterId cluster, uint32_t pathId)
{
    size_t writeIndex = 0;
    for (size_t readIndex = 0; readIndex < mFaultCount; ++readIndex)
    {
        if (!Matches(mFaults[readIndex], target, endpoint, cluster, pathId))
        {
            mFaults[writeIndex] = mFaults[readIndex];
            ++writeIndex;
        }
    }
    mFaultCount = writeIndex;
}

void FaultInjector::ClearAllFaults()
{
    mFaultCount = 0;
}

bool FaultInjector::HasFaultForAttribute(EndpointId endpoint, ClusterId cluster, AttributeId attribute) const
{
    return GetFaultForAttribute(endpoint, cluster, attribute) != nullptr;
}

bool FaultInjector::HasFaultForCommand(EndpointId endpoint, ClusterId cluster, CommandId command) const
{
    return GetFaultForCommand(endpoint, cluster, command) != nullptr;
}

const FaultConfig * FaultInjector::GetFaultForAttribute(EndpointId endpoint, ClusterId cluster, AttributeId attribute) const
{
    return FindForConcretePath(FaultTarget::kAttribute, endpoint, cluster, attribute);
}

const FaultConfig * FaultInjector::GetFaultForCommand(EndpointId endpoint, ClusterId cluster, CommandId command) const
{
    return FindForConcretePath(FaultTarget::kCommand, endpoint, cluster, command);
}

const FaultConfig * FaultInjector::FindForConcretePath(FaultTarget target, EndpointId endpoint, ClusterId cluster,
                                                       uint32_t pathId) const
{
    const FaultConfig * fault = Find(target, std::make_optional(endpoint), cluster, pathId);
    return fault != nullptr ? fault : Find(target, std::nullopt, cluster, pathId);
}

const FaultConfig * FaultInjector::Find(FaultTarget target, std::optional<EndpointId> endpoint, ClusterId cluster,
                                        uint32_t pathId) const
{
    for (size_t i = 0; i < mFaultCount; ++i)
    {
        if (Matches(mFaults[i], target, endpoint, cluster, pathId))
        {
            return &mFaults[i];
        }
    }
    return nullptr;
}

CHIP_ERROR FaultInjector::ApplyDataCorruption(const FaultConfig & config, TLV::TLVWriter & writer, TLV::Tag tag)
{
    switch (config.corruptValueType)
    {
    case FaultValueType::kInt32:
        return writer.Put(tag, config.corruptValue.intValue);
    case FaultValueType::kFloat:
        return writer.Put(tag, config.corruptValue.floatValue);
    case FaultValueType::kBool:
        return writer.PutBoolean(tag, config.corruptValue.boolValue);
    case FaultValueType::kNull:
        return writer.PutNull(tag);
    }
    return CHIP_ERROR_INVALID_ARGUMENT;
}

uint32_t FaultInjector::GetDelayForFault(const FaultConfig & config) const
{
    return config.delayMs;
}

bool FaultInjector::ShouldDropCommand(const FaultConfig & config) const
{
    return config.faultType == FaultType::kCommandDrop;
}

Protocols::InteractionModel::Status FaultInjector::GetInjectedStatus(const FaultConfig & config) const
{
    return config.status;
}

} // namespace fault_injection
} // namespace chip
