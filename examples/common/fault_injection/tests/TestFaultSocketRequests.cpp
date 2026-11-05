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
#include <FaultSocket.h>

#include <cstring>
#include <memory>
#include <string>

#include <json/json.h>
#include <lib/support/Span.h>
#include <pw_unit_test/framework.h>

using namespace chip;
using namespace chip::fault_injection;
using chip::Protocols::InteractionModel::Status;

namespace {

constexpr char kOk[] = "{\"result\":\"ok\"}\n";

constexpr EndpointId kEndpoint = 1;

std::string Process(const char * request)
{
    return ProcessFaultRequest(CharSpan::fromCharString(request));
}

/// Parses a response line and returns its "detail", or "" for {"result":"ok"}.
/// Fails the test if the response is not exactly one valid JSON object line.
std::string ErrorDetail(const std::string & response)
{
    EXPECT_FALSE(response.empty());
    EXPECT_EQ(response.back(), '\n');
    EXPECT_EQ(response.find('\n'), response.size() - 1);

    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errors;
    EXPECT_TRUE(reader->parse(response.data(), response.data() + response.size(), &root, &errors));
    if (!root.isObject() || !root.isMember("result") || !root["result"].isString())
    {
        ADD_FAILURE() << "malformed response: " << response;
        return "malformed";
    }
    if (root["result"].asString() == "ok")
    {
        return "";
    }
    EXPECT_EQ(root["result"].asString(), "error");
    return root.isMember("detail") && root["detail"].isString() ? root["detail"].asString() : "malformed";
}

class TestFaultSocketRequests : public ::testing::Test
{
protected:
    void SetUp() override { FaultInjector::Instance().ClearAllFaults(); }
    void TearDown() override { FaultInjector::Instance().ClearAllFaults(); }

    FaultInjector & mInjector = FaultInjector::Instance();
};

TEST_F(TestFaultSocketRequests, ConfigureAttributeFault)
{
    EXPECT_EQ(Process(R"({"action": "configure", "fault": {"type": "data_corruption", "cluster": "0x0006",
                           "attribute": 0, "corrupt_value": true}})"
                      "\n"),
              kOk);

    const FaultConfig * fault = mInjector.GetFaultForAttribute(kEndpoint, 0x0006, 0x0000);
    ASSERT_NE(fault, nullptr);
    EXPECT_EQ(fault->faultType, FaultType::kDataCorruption);
    EXPECT_EQ(fault->corruptValueType, FaultValueType::kBool);
    EXPECT_TRUE(fault->corruptValue.boolValue);
}

TEST_F(TestFaultSocketRequests, ConfigureCommandFault)
{
    EXPECT_EQ(Process(R"({"action": "configure", "fault": {"type": "invalid_status", "scope": "command",
                           "cluster": 6, "command": 2, "status": 156}})"),
              kOk);

    const FaultConfig * fault = mInjector.GetFaultForCommand(kEndpoint, 0x0006, 0x02);
    ASSERT_NE(fault, nullptr);
    EXPECT_EQ(fault->status, Status::Busy);
}

TEST_F(TestFaultSocketRequests, CorruptValueTypes)
{
    EXPECT_EQ(Process(R"({"action": "configure", "fault": {"type": "data_corruption", "cluster": 1026, "attribute": 0,
                           "corrupt_value": -40000}})"),
              kOk);
    const FaultConfig * fault = mInjector.GetFaultForAttribute(kEndpoint, 0x0402, 0);
    ASSERT_NE(fault, nullptr);
    EXPECT_EQ(fault->corruptValueType, FaultValueType::kInt32);
    EXPECT_EQ(fault->corruptValue.intValue, -40000);

    EXPECT_EQ(Process(R"({"action": "configure", "fault": {"type": "data_corruption", "cluster": 1026, "attribute": 0,
                           "corrupt_value": 25.0}})"),
              kOk);
    fault = mInjector.GetFaultForAttribute(kEndpoint, 0x0402, 0);
    ASSERT_NE(fault, nullptr);
    EXPECT_EQ(fault->corruptValueType, FaultValueType::kFloat);
    EXPECT_EQ(fault->corruptValue.floatValue, 25.0f);
}

TEST_F(TestFaultSocketRequests, NullCorruptValue)
{
    EXPECT_EQ(Process(R"({"action": "configure", "fault": {"type": "data_corruption", "cluster": "0x0402", "attribute": 0,
                           "corrupt_value": null}})"),
              kOk);
    const FaultConfig * fault = mInjector.GetFaultForAttribute(kEndpoint, 0x0402, 0);
    ASSERT_NE(fault, nullptr);
    EXPECT_EQ(fault->corruptValueType, FaultValueType::kNull);
}

TEST_F(TestFaultSocketRequests, EndpointField)
{
    EXPECT_EQ(Process(R"({"action": "configure", "fault": {"type": "data_corruption", "endpoint": 2, "cluster": "0x0402",
                           "attribute": 0, "corrupt_value": -30000}})"),
              kOk);
    EXPECT_EQ(Process(R"({"action": "configure", "fault": {"type": "data_corruption", "cluster": "0x0402",
                           "attribute": 0, "corrupt_value": 9999}})"),
              kOk);

    const FaultConfig * fault = mInjector.GetFaultForAttribute(2, 0x0402, 0);
    ASSERT_NE(fault, nullptr);
    ASSERT_TRUE(fault->endpoint.has_value());
    EXPECT_EQ(*fault->endpoint, 2);
    EXPECT_EQ(fault->corruptValue.intValue, -30000);

    fault = mInjector.GetFaultForAttribute(kEndpoint, 0x0402, 0);
    ASSERT_NE(fault, nullptr);
    EXPECT_FALSE(fault->endpoint.has_value());
    EXPECT_EQ(fault->corruptValue.intValue, 9999);

    EXPECT_EQ(Process(R"({"action": "clear", "endpoint": "0x2", "cluster": "0x0402", "attribute": 0})"), kOk);
    fault = mInjector.GetFaultForAttribute(2, 0x0402, 0);
    ASSERT_NE(fault, nullptr);
    EXPECT_FALSE(fault->endpoint.has_value());
}

TEST_F(TestFaultSocketRequests, InvalidJsonGivesValidErrorResponse)
{
    EXPECT_EQ(ErrorDetail(Process("{\"action\": \"configure\", ")), "invalid json");
    EXPECT_EQ(ErrorDetail(Process("[1, 2]")), "invalid json");
    EXPECT_EQ(ErrorDetail(ProcessFaultRequest(CharSpan())), "invalid json");
}

// jsoncpp aborts on wrongly typed conversions in this build. These requests
// must be rejected with an error response instead of terminating the test.
TEST_F(TestFaultSocketRequests, WrongFieldTypesAreRejected)
{
    const char * requests[] = {
        R"({"action": {}})",
        R"({"action": "configure", "fault": 5})",
        R"({"action": "configure", "fault": {"type": 1, "cluster": 6}})",
        R"({"action": "configure", "fault": {"type": "timing_delay", "cluster": 6, "delay_ms": "100ms"}})",
        R"({"action": "configure", "fault": {"type": "timing_delay", "cluster": 6, "delay_ms": -1}})",
        R"({"action": "configure", "fault": {"type": "timing_delay", "cluster": 6, "delay_ms": 1.5}})",
        R"({"action": "configure", "fault": {"type": "invalid_status", "cluster": 6, "status": "x"}})",
        R"({"action": "configure", "fault": {"type": "invalid_status", "cluster": 6, "status": 300}})",
        R"({"action": "configure", "fault": {"type": "data_corruption", "cluster": 6, "corrupt_value": 4000000000}})",
        R"({"action": "configure", "fault": {"type": "data_corruption", "cluster": 6, "corrupt_value": "on"}})",
        R"({"action": "configure", "fault": {"type": "data_corruption", "cluster": 6, "corrupt_value": 1e300}})",
        R"({"action": "configure", "fault": {"type": "command_drop", "cluster": "six"}})",
        R"({"action": "configure", "fault": {"type": "command_drop", "cluster": "0x1g"}})",
        R"({"action": "configure", "fault": {"type": "command_drop", "cluster": "-1"}})",
        R"({"action": "configure", "fault": {"type": "command_drop", "cluster": "0x100000000"}})",
        R"({"action": "configure", "fault": {"type": "command_drop", "cluster": 6, "scope": 1}})",
        R"({"action": "clear", "cluster": [6]})",
        R"({"action": "clear", "endpoint": 70000, "cluster": 6})",
        R"({"action": "configure", "fault": {"type": "command_drop", "cluster": 6, "endpoint": 65536}})",
        R"({"action": "configure", "fault": {"type": "command_drop", "cluster": 6, "endpoint": "one"}})",
        R"({"action": "configure", "fault": {"type": "command_drop", "cluster": 6, "endpoint": null}})",
    };

    for (const char * request : requests)
    {
        EXPECT_NE(ErrorDetail(Process(request)), "") << request;
    }
}

TEST_F(TestFaultSocketRequests, MissingRequiredFieldsAreRejected)
{
    EXPECT_EQ(ErrorDetail(Process(R"({"action": "configure"})")), "invalid fault");
    EXPECT_EQ(ErrorDetail(Process(R"({"action": "configure", "fault": {"type": "command_drop"}})")), "invalid fault");
    EXPECT_EQ(ErrorDetail(Process(R"({"action": "configure", "fault": {"type": "command_drop", "cluster": 0}})")),
              "invalid fault");
    EXPECT_EQ(ErrorDetail(Process(R"({"action": "configure", "fault": {"type": "data_corruption", "cluster": 6}})")),
              "invalid fault");
    EXPECT_EQ(ErrorDetail(Process(R"({"action": "configure", "fault": {"type": "timing_delay", "cluster": 6}})")),
              "invalid fault");
    EXPECT_EQ(ErrorDetail(Process(R"({"action": "configure", "fault": {"type": "invalid_status", "cluster": 6}})")),
              "invalid fault");
    EXPECT_EQ(ErrorDetail(Process(R"({"action": "configure", "fault": {"type": "unknown", "cluster": 6}})")),
              "invalid fault");
    EXPECT_EQ(ErrorDetail(Process(R"({"action": "clear"})")), "invalid clear");
    EXPECT_EQ(ErrorDetail(Process(R"({"cluster": 6})")), "unknown action");
    EXPECT_EQ(ErrorDetail(Process(R"({"action": "reboot"})")), "unknown action");
}

TEST_F(TestFaultSocketRequests, ClearByScope)
{
    EXPECT_EQ(Process(R"({"action": "configure", "fault": {"type": "command_drop", "scope": "command", "cluster": 6, "command": 2}})"),
              kOk);
    EXPECT_EQ(Process(R"({"action": "configure", "fault": {"type": "command_drop", "cluster": 6, "attribute": 2}})"), kOk);

    EXPECT_EQ(Process(R"({"action": "clear", "cluster": 6, "attribute": 2})"), kOk);
    EXPECT_FALSE(mInjector.HasFaultForAttribute(kEndpoint, 6, 2));
    EXPECT_TRUE(mInjector.HasFaultForCommand(kEndpoint, 6, 2));

    EXPECT_EQ(Process(R"({"action": "clear", "scope": "command", "cluster": "0x0006", "command": 2})"), kOk);
    EXPECT_FALSE(mInjector.HasFaultForCommand(kEndpoint, 6, 2));
}

TEST_F(TestFaultSocketRequests, ClearAll)
{
    EXPECT_EQ(Process(R"({"action": "configure", "fault": {"type": "command_drop", "scope": "command", "cluster": 6, "command": 1}})"),
              kOk);
    EXPECT_EQ(Process(R"({"action": "clear_all"})"), kOk);
    EXPECT_FALSE(mInjector.HasFaultForCommand(kEndpoint, 6, 1));
}

TEST_F(TestFaultSocketRequests, TableFull)
{
    for (size_t i = 0; i < FaultInjector::kMaxFaults; ++i)
    {
        std::string request = R"({"action": "configure", "fault": {"type": "command_drop", "cluster": 6, "attribute": )" +
            std::to_string(i) + "}}";
        EXPECT_EQ(Process(request.c_str()), kOk);
    }
    EXPECT_EQ(ErrorDetail(Process(R"({"action": "configure", "fault": {"type": "command_drop", "cluster": 6, "attribute": 99}})")),
              "fault table full");
}

} // namespace
