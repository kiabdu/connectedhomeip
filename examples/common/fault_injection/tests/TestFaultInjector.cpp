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

#include <FaultInjector.h>

#include <lib/core/TLVReader.h>
#include <lib/core/TLVWriter.h>
#include <lib/support/tests/ExtraPwTestMacros.h>
#include <pw_unit_test/framework.h>

using namespace chip;
using namespace chip::fault_injection;
using chip::Protocols::InteractionModel::Status;

namespace {

constexpr EndpointId kEndpoint      = 1;
constexpr EndpointId kOtherEndpoint = 2;
constexpr ClusterId kOnOffCluster   = 0x0006;
constexpr ClusterId kTempCluster    = 0x0402;
constexpr AttributeId kMeasuredValueAttr = 0x0000;
constexpr ClusterId kLevelCluster   = 0x0008;
constexpr AttributeId kOnOffAttr    = 0x0000;
constexpr CommandId kOffCommand     = 0x00;
constexpr CommandId kToggleCommand  = 0x02;
constexpr AttributeId kFaultCapacity = static_cast<AttributeId>(FaultInjector::kMaxFaults);

FaultConfig AttributeFault(ClusterId cluster, AttributeId attribute, FaultType type = FaultType::kDataCorruption)
{
    FaultConfig config;
    config.faultType = type;
    config.target    = FaultTarget::kAttribute;
    config.cluster   = cluster;
    config.attribute = attribute;
    return config;
}

FaultConfig CommandFault(ClusterId cluster, CommandId command, FaultType type = FaultType::kCommandDrop)
{
    FaultConfig config;
    config.faultType = type;
    config.target    = FaultTarget::kCommand;
    config.cluster   = cluster;
    config.command   = command;
    return config;
}

class CountingDelegate : public FaultInjectorDelegate
{
public:
    void OnFaultInjected(const FaultConfig &, const char *) override { ++mCount; }
    int mCount = 0;
};

class TestFaultInjector : public ::testing::Test
{
protected:
    void SetUp() override
    {
        FaultInjector::Instance().ClearAllFaults();
        FaultInjector::Instance().SetDelegate(nullptr);
    }
    void TearDown() override { SetUp(); }

    FaultInjector & mInjector = FaultInjector::Instance();
};

TEST_F(TestFaultInjector, RejectsFaultTypeNone)
{
    FaultConfig config = AttributeFault(kOnOffCluster, kOnOffAttr, FaultType::kNone);
    EXPECT_EQ(mInjector.ConfigureFault(config), CHIP_ERROR_INVALID_ARGUMENT);
    EXPECT_FALSE(mInjector.HasFaultForAttribute(kEndpoint, kOnOffCluster, kOnOffAttr));
}

TEST_F(TestFaultInjector, AttributeFaultLookup)
{
    EXPECT_SUCCESS(mInjector.ConfigureFault(AttributeFault(kOnOffCluster, kOnOffAttr)));

    EXPECT_TRUE(mInjector.HasFaultForAttribute(kEndpoint, kOnOffCluster, kOnOffAttr));
    EXPECT_FALSE(mInjector.HasFaultForAttribute(kEndpoint, kOnOffCluster, 0x4000));
    EXPECT_FALSE(mInjector.HasFaultForAttribute(kEndpoint, kLevelCluster, kOnOffAttr));
    // Attribute 0 and command 0 are distinct paths.
    EXPECT_FALSE(mInjector.HasFaultForCommand(kEndpoint, kOnOffCluster, kOffCommand));
}

TEST_F(TestFaultInjector, CommandFaultLookupUsesCommandId)
{
    EXPECT_SUCCESS(mInjector.ConfigureFault(CommandFault(kOnOffCluster, kToggleCommand)));

    const FaultConfig * fault = mInjector.GetFaultForCommand(kEndpoint, kOnOffCluster, kToggleCommand);
    ASSERT_NE(fault, nullptr);
    EXPECT_EQ(fault->command, kToggleCommand);
    EXPECT_TRUE(mInjector.ShouldDropCommand(*fault));

    EXPECT_FALSE(mInjector.HasFaultForCommand(kEndpoint, kOnOffCluster, kOffCommand));
    EXPECT_FALSE(mInjector.HasFaultForAttribute(kEndpoint, kOnOffCluster, kToggleCommand));
}

TEST_F(TestFaultInjector, SamePathReplacesFault)
{
    CountingDelegate delegate;
    mInjector.SetDelegate(&delegate);

    FaultConfig config = AttributeFault(kLevelCluster, 0x0000, FaultType::kTimingDelay);
    config.delayMs     = 100;
    EXPECT_SUCCESS(mInjector.ConfigureFault(config));
    config.delayMs = 12000;
    EXPECT_SUCCESS(mInjector.ConfigureFault(config));

    const FaultConfig * fault = mInjector.GetFaultForAttribute(kEndpoint, kLevelCluster, 0x0000);
    ASSERT_NE(fault, nullptr);
    EXPECT_EQ(mInjector.GetDelayForFault(*fault), 12000u);
    EXPECT_EQ(delegate.mCount, 2);

    mInjector.ClearAttributeFault(std::nullopt, kLevelCluster, 0x0000);
    EXPECT_FALSE(mInjector.HasFaultForAttribute(kEndpoint, kLevelCluster, 0x0000));
}

TEST_F(TestFaultInjector, CapacityLimit)
{
    for (AttributeId attribute = 0; attribute < kFaultCapacity; ++attribute)
    {
        EXPECT_SUCCESS(mInjector.ConfigureFault(AttributeFault(kOnOffCluster, attribute)));
    }

    EXPECT_EQ(mInjector.ConfigureFault(AttributeFault(kOnOffCluster, kFaultCapacity)), CHIP_ERROR_NO_MEMORY);
    // Replacing an existing path still works when the table is full.
    EXPECT_SUCCESS(mInjector.ConfigureFault(AttributeFault(kOnOffCluster, 0, FaultType::kInvalidStatus)));

    mInjector.ClearAttributeFault(std::nullopt, kOnOffCluster, 3);
    EXPECT_SUCCESS(mInjector.ConfigureFault(AttributeFault(kOnOffCluster, kFaultCapacity)));
}

TEST_F(TestFaultInjector, ClearByTarget)
{
    EXPECT_SUCCESS(mInjector.ConfigureFault(AttributeFault(kOnOffCluster, 0x0000)));
    EXPECT_SUCCESS(mInjector.ConfigureFault(CommandFault(kOnOffCluster, 0x0000)));
    EXPECT_SUCCESS(mInjector.ConfigureFault(CommandFault(kOnOffCluster, kToggleCommand)));

    mInjector.ClearAttributeFault(std::nullopt, kOnOffCluster, 0x0000);
    EXPECT_FALSE(mInjector.HasFaultForAttribute(kEndpoint, kOnOffCluster, 0x0000));
    EXPECT_TRUE(mInjector.HasFaultForCommand(kEndpoint, kOnOffCluster, 0x0000));

    mInjector.ClearCommandFault(std::nullopt, kOnOffCluster, 0x0000);
    EXPECT_FALSE(mInjector.HasFaultForCommand(kEndpoint, kOnOffCluster, 0x0000));
    EXPECT_TRUE(mInjector.HasFaultForCommand(kEndpoint, kOnOffCluster, kToggleCommand));

    mInjector.ClearAllFaults();
    EXPECT_FALSE(mInjector.HasFaultForCommand(kEndpoint, kOnOffCluster, kToggleCommand));
}

TEST_F(TestFaultInjector, InjectedStatus)
{
    FaultConfig config = CommandFault(kOnOffCluster, kToggleCommand, FaultType::kInvalidStatus);
    config.status      = Status::Busy;
    EXPECT_SUCCESS(mInjector.ConfigureFault(config));

    const FaultConfig * fault = mInjector.GetFaultForCommand(kEndpoint, kOnOffCluster, kToggleCommand);
    ASSERT_NE(fault, nullptr);
    EXPECT_EQ(mInjector.GetInjectedStatus(*fault), Status::Busy);
    EXPECT_FALSE(mInjector.ShouldDropCommand(*fault));
}

TEST_F(TestFaultInjector, ApplyDataCorruptionEncodesEachType)
{
    uint8_t buffer[32];
    const TLV::Tag tag = TLV::ContextTag(2);

    FaultConfig config;
    config.corruptValueType       = FaultValueType::kBool;
    config.corruptValue.boolValue = true;
    {
        TLV::TLVWriter writer;
        writer.Init(buffer);
        EXPECT_SUCCESS(mInjector.ApplyDataCorruption(config, writer, tag));
        EXPECT_SUCCESS(writer.Finalize());

        TLV::TLVReader reader;
        reader.Init(buffer, writer.GetLengthWritten());
        EXPECT_SUCCESS(reader.Next(tag));
        bool value = false;
        EXPECT_SUCCESS(reader.Get(value));
        EXPECT_TRUE(value);
    }

    config.corruptValueType      = FaultValueType::kInt32;
    config.corruptValue.intValue = -5;
    {
        TLV::TLVWriter writer;
        writer.Init(buffer);
        EXPECT_SUCCESS(mInjector.ApplyDataCorruption(config, writer, tag));
        EXPECT_SUCCESS(writer.Finalize());

        TLV::TLVReader reader;
        reader.Init(buffer, writer.GetLengthWritten());
        EXPECT_SUCCESS(reader.Next(tag));
        int32_t value = 0;
        EXPECT_SUCCESS(reader.Get(value));
        EXPECT_EQ(value, -5);
    }

    config.corruptValueType        = FaultValueType::kFloat;
    config.corruptValue.floatValue = 1.5f;
    {
        TLV::TLVWriter writer;
        writer.Init(buffer);
        EXPECT_SUCCESS(mInjector.ApplyDataCorruption(config, writer, tag));
        EXPECT_SUCCESS(writer.Finalize());

        TLV::TLVReader reader;
        reader.Init(buffer, writer.GetLengthWritten());
        EXPECT_SUCCESS(reader.Next(tag));
        float value = 0;
        EXPECT_SUCCESS(reader.Get(value));
        EXPECT_EQ(value, 1.5f);
    }
}

TEST_F(TestFaultInjector, EndpointSpecificFault)
{
    FaultConfig config = AttributeFault(kTempCluster, kMeasuredValueAttr);
    config.endpoint    = kOtherEndpoint;
    EXPECT_SUCCESS(mInjector.ConfigureFault(config));

    EXPECT_TRUE(mInjector.HasFaultForAttribute(kOtherEndpoint, kTempCluster, kMeasuredValueAttr));
    EXPECT_FALSE(mInjector.HasFaultForAttribute(kEndpoint, kTempCluster, kMeasuredValueAttr));
}

TEST_F(TestFaultInjector, EndpointFaultTakesPrecedenceOverAllEndpoints)
{
    FaultConfig allEndpoints = AttributeFault(kTempCluster, kMeasuredValueAttr, FaultType::kTimingDelay);
    allEndpoints.delayMs     = 100;
    FaultConfig oneEndpoint  = AttributeFault(kTempCluster, kMeasuredValueAttr, FaultType::kTimingDelay);
    oneEndpoint.endpoint     = kOtherEndpoint;
    oneEndpoint.delayMs      = 200;

    // Configure in both orders to check that lookup order does not depend on insertion order.
    EXPECT_SUCCESS(mInjector.ConfigureFault(oneEndpoint));
    EXPECT_SUCCESS(mInjector.ConfigureFault(allEndpoints));

    const FaultConfig * fault = mInjector.GetFaultForAttribute(kOtherEndpoint, kTempCluster, kMeasuredValueAttr);
    ASSERT_NE(fault, nullptr);
    EXPECT_EQ(fault->delayMs, 200u);

    fault = mInjector.GetFaultForAttribute(kEndpoint, kTempCluster, kMeasuredValueAttr);
    ASSERT_NE(fault, nullptr);
    EXPECT_EQ(fault->delayMs, 100u);

    // Clearing the all-endpoints fault keeps the endpoint-specific one.
    mInjector.ClearAttributeFault(std::nullopt, kTempCluster, kMeasuredValueAttr);
    EXPECT_FALSE(mInjector.HasFaultForAttribute(kEndpoint, kTempCluster, kMeasuredValueAttr));
    EXPECT_TRUE(mInjector.HasFaultForAttribute(kOtherEndpoint, kTempCluster, kMeasuredValueAttr));

    mInjector.ClearAttributeFault(kOtherEndpoint, kTempCluster, kMeasuredValueAttr);
    EXPECT_FALSE(mInjector.HasFaultForAttribute(kOtherEndpoint, kTempCluster, kMeasuredValueAttr));

    mInjector.ClearAllFaults();
    EXPECT_SUCCESS(mInjector.ConfigureFault(allEndpoints));
    EXPECT_SUCCESS(mInjector.ConfigureFault(oneEndpoint));
    fault = mInjector.GetFaultForAttribute(kOtherEndpoint, kTempCluster, kMeasuredValueAttr);
    ASSERT_NE(fault, nullptr);
    EXPECT_EQ(fault->delayMs, 200u);
}

TEST_F(TestFaultInjector, SamePathOnDifferentEndpointsAreSeparateFaults)
{
    FaultConfig config = CommandFault(kOnOffCluster, kToggleCommand);
    config.endpoint    = kEndpoint;
    EXPECT_SUCCESS(mInjector.ConfigureFault(config));
    config.endpoint = kOtherEndpoint;
    config.faultType = FaultType::kInvalidStatus;
    EXPECT_SUCCESS(mInjector.ConfigureFault(config));

    const FaultConfig * fault = mInjector.GetFaultForCommand(kEndpoint, kOnOffCluster, kToggleCommand);
    ASSERT_NE(fault, nullptr);
    EXPECT_EQ(fault->faultType, FaultType::kCommandDrop);

    fault = mInjector.GetFaultForCommand(kOtherEndpoint, kOnOffCluster, kToggleCommand);
    ASSERT_NE(fault, nullptr);
    EXPECT_EQ(fault->faultType, FaultType::kInvalidStatus);
}

TEST_F(TestFaultInjector, ApplyDataCorruptionEncodesNull)
{
    uint8_t buffer[8];
    const TLV::Tag tag = TLV::ContextTag(2);

    FaultConfig config;
    config.corruptValueType = FaultValueType::kNull;

    TLV::TLVWriter writer;
    writer.Init(buffer);
    EXPECT_SUCCESS(mInjector.ApplyDataCorruption(config, writer, tag));
    EXPECT_SUCCESS(writer.Finalize());

    TLV::TLVReader reader;
    reader.Init(buffer, writer.GetLengthWritten());
    EXPECT_SUCCESS(reader.Next(tag));
    EXPECT_EQ(reader.GetType(), TLV::kTLVType_Null);
}

TEST_F(TestFaultInjector, DefaultCorruptValueIsZero)
{
    FaultConfig config;
    EXPECT_EQ(config.corruptValueType, FaultValueType::kInt32);
    EXPECT_EQ(config.corruptValue.intValue, 0);
}

} // namespace
