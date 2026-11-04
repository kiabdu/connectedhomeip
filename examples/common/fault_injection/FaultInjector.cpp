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

bool Matches(const FaultConfig & config, FaultTarget target, ClusterId cluster, AttributeId attribute, CommandId command)
{
    if (config.target != target || config.cluster != cluster)
    {
        return false;
    }
    return target == FaultTarget::kCommand ? config.command == command : config.attribute == attribute;
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
        if (Matches(mFaults[i], config.target, config.cluster, config.attribute, config.command))
        {
            mFaults[i] = config;
            ChipLogProgress(AppServer, "FaultInjector: updated %s fault on cluster 0x%08" PRIx32,
                            FaultTypeName(config.faultType), config.cluster);
            if (mDelegate != nullptr)
            {
                mDelegate->OnFaultInjected(config, FaultTypeName(config.faultType));
            }
            return CHIP_NO_ERROR;
        }
    }

    VerifyOrReturnError(mFaultCount < kMaxFaults, CHIP_ERROR_NO_MEMORY);

    mFaults[mFaultCount] = config;
    ++mFaultCount;

    ChipLogProgress(AppServer, "FaultInjector: configured %s fault on cluster 0x%08" PRIx32, FaultTypeName(config.faultType),
                    config.cluster);
    if (mDelegate != nullptr)
    {
        mDelegate->OnFaultInjected(config, FaultTypeName(config.faultType));
    }

    return CHIP_NO_ERROR;
}

void FaultInjector::ClearFault(ClusterId cluster, AttributeId attribute)
{
    size_t writeIndex = 0;
    for (size_t readIndex = 0; readIndex < mFaultCount; ++readIndex)
    {
        if (!Matches(mFaults[readIndex], FaultTarget::kAttribute, cluster, attribute, 0))
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

bool FaultInjector::HasFaultForAttribute(ClusterId cluster, AttributeId attribute) const
{
    return GetFaultForAttribute(cluster, attribute) != nullptr;
}

bool FaultInjector::HasFaultForCommand(ClusterId cluster, CommandId command) const
{
    return GetFaultForCommand(cluster, command) != nullptr;
}

const FaultConfig * FaultInjector::GetFaultForAttribute(ClusterId cluster, AttributeId attribute) const
{
    for (size_t i = 0; i < mFaultCount; ++i)
    {
        if (Matches(mFaults[i], FaultTarget::kAttribute, cluster, attribute, 0))
        {
            return &mFaults[i];
        }
    }
    return nullptr;
}

const FaultConfig * FaultInjector::GetFaultForCommand(ClusterId cluster, CommandId command) const
{
    for (size_t i = 0; i < mFaultCount; ++i)
    {
        if (Matches(mFaults[i], FaultTarget::kCommand, cluster, command, 0))
        {
            return &mFaults[i];
        }
    }
    return nullptr;
}

CHIP_ERROR FaultInjector::ApplyDataCorruption(const FaultConfig & config, TLV::TLVWriter & writer)
{
    TLV::Tag tag = TLV::AnonymousTag();
    switch (config.corruptValueType)
    {
    case FaultValueType::kInt32:
        return writer.Put(tag, config.corruptValue.intValue);
    case FaultValueType::kFloat:
        return writer.Put(tag, config.corruptValue.floatValue);
    case FaultValueType::kBool:
        return writer.PutBoolean(tag, config.corruptValue.boolValue);
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
