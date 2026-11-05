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

#include "FaultSocket.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <json/json.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/PlatformManager.h>
#include <system/SystemError.h>

#include "FaultInjector.h"

namespace chip {
namespace fault_injection {

namespace {

constexpr size_t kMaxRequestBytes      = 4096;
constexpr int kAcceptPollIntervalMs    = 200;
constexpr time_t kConnectionTimeoutSec = 1;

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

constexpr char kResponseOk[]            = "{\"result\":\"ok\"}\n";
constexpr char kResponseInvalidJson[]   = "{\"result\":\"error\",\"detail\":\"invalid json\"}\n";
constexpr char kResponseUnknownAction[] = "{\"result\":\"error\",\"detail\":\"unknown action\"}\n";
constexpr char kResponseInvalidFault[]  = "{\"result\":\"error\",\"detail\":\"invalid fault\"}\n";
constexpr char kResponseInvalidClear[]  = "{\"result\":\"error\",\"detail\":\"invalid clear\"}\n";
constexpr char kResponseTableFull[]     = "{\"result\":\"error\",\"detail\":\"fault table full\"}\n";

// jsoncpp is built with JSON_USE_EXCEPTION=0: converting a value of the wrong
// type aborts the process. Every accessor below checks the type first.

/// Reads `object[key]` into `out` if present. Accepts non-negative JSON integers and
/// decimal or "0x"-prefixed strings. Leaves `out` unchanged if `key` is absent.
CHIP_ERROR ReadUnsigned(const Json::Value & object, const char * key, uint32_t maxValue, uint32_t & out)
{
    if (!object.isMember(key))
    {
        return CHIP_NO_ERROR;
    }

    const Json::Value & value = object[key];
    uint64_t parsed           = 0;
    if (value.isUInt())
    {
        parsed = value.asUInt();
    }
    else if (value.isString())
    {
        const char * text = value.asCString();
        VerifyOrReturnError(text[0] >= '0' && text[0] <= '9', CHIP_ERROR_INVALID_ARGUMENT);
        char * end = nullptr;
        errno      = 0;
        parsed     = strtoull(text, &end, 0);
        VerifyOrReturnError(errno == 0 && *end == '\0', CHIP_ERROR_INVALID_ARGUMENT);
    }
    else
    {
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    VerifyOrReturnError(parsed <= maxValue, CHIP_ERROR_INVALID_ARGUMENT);
    out = static_cast<uint32_t>(parsed);
    return CHIP_NO_ERROR;
}

/// Reads `object[key]` into `out` if present. Leaves `out` unchanged if `key` is absent.
CHIP_ERROR ReadString(const Json::Value & object, const char * key, std::string & out)
{
    if (!object.isMember(key))
    {
        return CHIP_NO_ERROR;
    }
    const Json::Value & value = object[key];
    VerifyOrReturnError(value.isString(), CHIP_ERROR_INVALID_ARGUMENT);
    out = value.asString();
    return CHIP_NO_ERROR;
}

CHIP_ERROR ReadScope(const Json::Value & object, FaultTarget & target)
{
    std::string scope = "attribute";
    ReturnErrorOnFailure(ReadString(object, "scope", scope));
    if (scope == "attribute")
    {
        target = FaultTarget::kAttribute;
        return CHIP_NO_ERROR;
    }
    if (scope == "command")
    {
        target = FaultTarget::kCommand;
        return CHIP_NO_ERROR;
    }
    return CHIP_ERROR_INVALID_ARGUMENT;
}

CHIP_ERROR ReadCluster(const Json::Value & object, ClusterId & cluster)
{
    VerifyOrReturnError(object.isMember("cluster"), CHIP_ERROR_INVALID_ARGUMENT);
    ReturnErrorOnFailure(ReadUnsigned(object, "cluster", std::numeric_limits<ClusterId>::max(), cluster));
    VerifyOrReturnError(cluster != 0, CHIP_ERROR_INVALID_ARGUMENT);
    return CHIP_NO_ERROR;
}

/// Reads the optional "endpoint" field. Absent means all endpoints.
CHIP_ERROR ReadEndpoint(const Json::Value & object, std::optional<EndpointId> & endpoint)
{
    endpoint.reset();
    if (!object.isMember("endpoint"))
    {
        return CHIP_NO_ERROR;
    }
    uint32_t value = 0;
    ReturnErrorOnFailure(ReadUnsigned(object, "endpoint", std::numeric_limits<EndpointId>::max(), value));
    endpoint = static_cast<EndpointId>(value);
    return CHIP_NO_ERROR;
}

CHIP_ERROR ReadCorruptValue(const Json::Value & value, FaultConfig & config)
{
    switch (value.type())
    {
    case Json::nullValue:
        config.corruptValueType = FaultValueType::kNull;
        return CHIP_NO_ERROR;
    case Json::booleanValue:
        config.corruptValueType       = FaultValueType::kBool;
        config.corruptValue.boolValue = value.asBool();
        return CHIP_NO_ERROR;
    case Json::intValue:
    case Json::uintValue:
        VerifyOrReturnError(value.isInt(), CHIP_ERROR_INVALID_ARGUMENT);
        config.corruptValueType      = FaultValueType::kInt32;
        config.corruptValue.intValue = value.asInt();
        return CHIP_NO_ERROR;
    case Json::realValue: {
        const double number = value.asDouble();
        constexpr double kMaxFloat = std::numeric_limits<float>::max();
        VerifyOrReturnError(number >= -kMaxFloat && number <= kMaxFloat, CHIP_ERROR_INVALID_ARGUMENT);
        config.corruptValueType        = FaultValueType::kFloat;
        config.corruptValue.floatValue = static_cast<float>(number);
        return CHIP_NO_ERROR;
    }
    default:
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
}

CHIP_ERROR ParseFaultType(const std::string & type, FaultType & faultType)
{
    if (type == "data_corruption")
    {
        faultType = FaultType::kDataCorruption;
    }
    else if (type == "timing_delay")
    {
        faultType = FaultType::kTimingDelay;
    }
    else if (type == "command_drop")
    {
        faultType = FaultType::kCommandDrop;
    }
    else if (type == "invalid_status")
    {
        faultType = FaultType::kInvalidStatus;
    }
    else
    {
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ParseFaultConfig(const Json::Value & fault, FaultConfig & config)
{
    VerifyOrReturnError(fault.isObject(), CHIP_ERROR_INVALID_ARGUMENT);

    std::string type;
    ReturnErrorOnFailure(ReadString(fault, "type", type));
    ReturnErrorOnFailure(ParseFaultType(type, config.faultType));
    ReturnErrorOnFailure(ReadScope(fault, config.target));
    ReturnErrorOnFailure(ReadEndpoint(fault, config.endpoint));
    ReturnErrorOnFailure(ReadCluster(fault, config.cluster));
    ReturnErrorOnFailure(ReadUnsigned(fault, "attribute", std::numeric_limits<AttributeId>::max(), config.attribute));
    ReturnErrorOnFailure(ReadUnsigned(fault, "command", std::numeric_limits<CommandId>::max(), config.command));
    ReturnErrorOnFailure(ReadUnsigned(fault, "delay_ms", std::numeric_limits<uint32_t>::max(), config.delayMs));

    uint32_t status = 0;
    ReturnErrorOnFailure(ReadUnsigned(fault, "status", std::numeric_limits<uint8_t>::max(), status));
    config.status = static_cast<Protocols::InteractionModel::Status>(status);

    if (fault.isMember("corrupt_value"))
    {
        ReturnErrorOnFailure(ReadCorruptValue(fault["corrupt_value"], config));
    }

    switch (config.faultType)
    {
    case FaultType::kDataCorruption:
        VerifyOrReturnError(fault.isMember("corrupt_value"), CHIP_ERROR_INVALID_ARGUMENT);
        break;
    case FaultType::kTimingDelay:
        VerifyOrReturnError(config.delayMs > 0, CHIP_ERROR_INVALID_ARGUMENT);
        break;
    case FaultType::kInvalidStatus:
        VerifyOrReturnError(fault.isMember("status"), CHIP_ERROR_INVALID_ARGUMENT);
        break;
    case FaultType::kCommandDrop:
    case FaultType::kNone:
        break;
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR ApplyClear(const Json::Value & root)
{
    FaultTarget target = FaultTarget::kAttribute;
    std::optional<EndpointId> endpoint;
    ClusterId cluster = 0;
    ReturnErrorOnFailure(ReadScope(root, target));
    ReturnErrorOnFailure(ReadEndpoint(root, endpoint));
    ReturnErrorOnFailure(ReadCluster(root, cluster));

    if (target == FaultTarget::kCommand)
    {
        CommandId command = 0;
        ReturnErrorOnFailure(ReadUnsigned(root, "command", std::numeric_limits<CommandId>::max(), command));
        FaultInjector::Instance().ClearCommandFault(endpoint, cluster, command);
    }
    else
    {
        AttributeId attribute = 0;
        ReturnErrorOnFailure(ReadUnsigned(root, "attribute", std::numeric_limits<AttributeId>::max(), attribute));
        FaultInjector::Instance().ClearAttributeFault(endpoint, cluster, attribute);
    }
    return CHIP_NO_ERROR;
}

void SetConnectionOptions(int connectionFd)
{
    timeval timeout = {};
    timeout.tv_sec  = kConnectionTimeoutSec;
    setsockopt(connectionFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(connectionFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int enable = 1;
    setsockopt(connectionFd, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable));
#endif
}

} // anonymous namespace

const char * ProcessFaultRequest(CharSpan request)
{
    VerifyOrReturnValue(!request.empty(), kResponseInvalidJson);

    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errors;
    if (!reader->parse(request.data(), request.data() + request.size(), &root, &errors) || !root.isObject())
    {
        ChipLogError(AppServer, "FaultSocket: request is not a JSON object");
        return kResponseInvalidJson;
    }

    std::string action;
    VerifyOrReturnValue(ReadString(root, "action", action) == CHIP_NO_ERROR, kResponseUnknownAction);

    if (action == "clear_all")
    {
        FaultInjector::Instance().ClearAllFaults();
        return kResponseOk;
    }

    if (action == "clear")
    {
        return ApplyClear(root) == CHIP_NO_ERROR ? kResponseOk : kResponseInvalidClear;
    }

    if (action == "configure")
    {
        FaultConfig config;
        VerifyOrReturnValue(root.isMember("fault"), kResponseInvalidFault);
        CHIP_ERROR err = ParseFaultConfig(root["fault"], config);
        if (err == CHIP_NO_ERROR)
        {
            err = FaultInjector::Instance().ConfigureFault(config);
        }
        if (err == CHIP_NO_ERROR)
        {
            return kResponseOk;
        }
        ChipLogError(AppServer, "FaultSocket: configure failed: %" CHIP_ERROR_FORMAT, err.Format());
        return err == CHIP_ERROR_NO_MEMORY ? kResponseTableFull : kResponseInvalidFault;
    }

    return kResponseUnknownAction;
}

CHIP_ERROR FaultSocketServer::Start(const char * socketPath)
{
    VerifyOrReturnError(socketPath != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(!mRunning, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(strlen(socketPath) < sizeof(sockaddr_un::sun_path), CHIP_ERROR_INVALID_ARGUMENT);

    int listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd < 0)
    {
        int error = errno;
        ChipLogError(AppServer, "FaultSocket: socket() failed: %s", strerror(error));
        return CHIP_ERROR_POSIX(error);
    }

    sockaddr_un addr = {};
    addr.sun_family  = AF_UNIX;
    strncpy(addr.sun_path, socketPath, sizeof(addr.sun_path) - 1);

    unlink(socketPath);

    if (bind(listenFd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        int error = errno;
        ChipLogError(AppServer, "FaultSocket: bind(%s) failed: %s", socketPath, strerror(error));
        close(listenFd);
        return CHIP_ERROR_POSIX(error);
    }

    if (listen(listenFd, 4) < 0)
    {
        int error = errno;
        ChipLogError(AppServer, "FaultSocket: listen() failed: %s", strerror(error));
        close(listenFd);
        unlink(socketPath);
        return CHIP_ERROR_POSIX(error);
    }

    mSocketFd   = listenFd;
    mSocketPath = socketPath;
    mRunning    = true;
    mThread     = std::thread(&FaultSocketServer::AcceptLoop, this);

    ChipLogProgress(AppServer, "FaultSocket: listening on %s", socketPath);
    return CHIP_NO_ERROR;
}

void FaultSocketServer::Stop()
{
    if (!mRunning.exchange(false))
    {
        return;
    }

    // AcceptLoop() checks mRunning at least every kAcceptPollIntervalMs.
    if (mThread.joinable())
    {
        mThread.join();
    }

    close(mSocketFd);
    mSocketFd = -1;
    unlink(mSocketPath.c_str());
    mSocketPath.clear();
}

void FaultSocketServer::AcceptLoop()
{
    while (mRunning)
    {
        pollfd listenPoll = {};
        listenPoll.fd     = mSocketFd;
        listenPoll.events = POLLIN;

        int ready = poll(&listenPoll, 1, kAcceptPollIntervalMs);
        if (ready < 0 && errno != EINTR)
        {
            ChipLogError(AppServer, "FaultSocket: poll() failed: %s", strerror(errno));
            return;
        }
        if (ready <= 0)
        {
            continue;
        }

        int connectionFd = accept(mSocketFd, nullptr, nullptr);
        if (connectionFd < 0)
        {
            if (errno == EINTR || errno == ECONNABORTED)
            {
                continue;
            }
            ChipLogError(AppServer, "FaultSocket: accept() failed: %s", strerror(errno));
            return;
        }

        SetConnectionOptions(connectionFd);
        HandleConnection(connectionFd);
        close(connectionFd);
    }
}

void FaultSocketServer::HandleConnection(int connectionFd)
{
    char buffer[kMaxRequestBytes];
    size_t used = 0;
    while (used < kMaxRequestBytes)
    {
        ssize_t readBytes = read(connectionFd, buffer + used, kMaxRequestBytes - used);
        if (readBytes <= 0)
        {
            break;
        }
        used += static_cast<size_t>(readBytes);
        if (memchr(buffer, '\n', used) != nullptr)
        {
            break;
        }
    }

    const char * response;
    {
        DeviceLayer::StackLock lock;
        response = ProcessFaultRequest(CharSpan(buffer, used));
    }

    ssize_t ignored = send(connectionFd, response, strlen(response), kSendFlags);
    (void) ignored;
}

} // namespace fault_injection
} // namespace chip
