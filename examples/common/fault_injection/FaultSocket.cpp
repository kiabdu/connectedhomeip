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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <json/json.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <system/SystemError.h>

#include "FaultInjector.h"

namespace chip {
namespace fault_injection {

namespace {

constexpr size_t kMaxRequestBytes = 4096;

uint32_t ParseNumber(const Json::Value & value)
{
    if (value.isUInt())
    {
        return value.asUInt();
    }
    if (value.isInt())
    {
        return static_cast<uint32_t>(value.asInt());
    }
    if (value.isString())
    {
        // Accept "0x0006" style strings in addition to plain JSON integers.
        return static_cast<uint32_t>(strtoul(value.asCString(), nullptr, 0));
    }
    return 0;
}

CHIP_ERROR ParseFaultConfig(const Json::Value & fault, FaultConfig & config)
{
    VerifyOrReturnError(fault.isObject(), CHIP_ERROR_INVALID_ARGUMENT);

    const std::string type = fault.get("type", "").asString();
    if (type == "data_corruption")
    {
        config.faultType = FaultType::kDataCorruption;
    }
    else if (type == "timing_delay")
    {
        config.faultType = FaultType::kTimingDelay;
    }
    else if (type == "command_drop")
    {
        config.faultType = FaultType::kCommandDrop;
    }
    else if (type == "invalid_status")
    {
        config.faultType = FaultType::kInvalidStatus;
    }
    else
    {
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    const std::string target = fault.get("scope", "attribute").asString();
    if (target == "command")
    {
        config.target = FaultTarget::kCommand;
    }
    else if (target == "attribute")
    {
        config.target = FaultTarget::kAttribute;
    }
    else
    {
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    config.cluster   = ParseNumber(fault["cluster"]);
    config.attribute = ParseNumber(fault["attribute"]);
    config.command   = ParseNumber(fault["command"]);
    config.delayMs   = fault.get("delay_ms", 0).asUInt();
    config.status    = static_cast<Protocols::InteractionModel::Status>(fault.get("status", 0).asUInt());

    VerifyOrReturnError(config.cluster != 0, CHIP_ERROR_INVALID_ARGUMENT);

    if (fault.isMember("corrupt_value"))
    {
        const Json::Value & value = fault["corrupt_value"];
        if (value.isBool())
        {
            config.corruptValueType       = FaultValueType::kBool;
            config.corruptValue.boolValue = value.asBool();
        }
        else if (value.isInt() || value.isUInt())
        {
            config.corruptValueType      = FaultValueType::kInt32;
            config.corruptValue.intValue = value.asInt();
        }
        else if (value.isDouble())
        {
            config.corruptValueType       = FaultValueType::kFloat;
            config.corruptValue.floatValue = static_cast<float>(value.asDouble());
        }
        else
        {
            return CHIP_ERROR_INVALID_ARGUMENT;
        }
    }

    return CHIP_NO_ERROR;
}

} // anonymous namespace

CHIP_ERROR FaultSocketServer::Start(const char * socketPath)
{
    VerifyOrReturnError(socketPath != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(!mRunning, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(strlen(socketPath) < sizeof(sockaddr_un::sun_path), CHIP_ERROR_INVALID_ARGUMENT);

    int listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd < 0)
    {
        ChipLogError(AppServer, "FaultSocket: socket() failed: %s", strerror(errno));
        return CHIP_ERROR_POSIX(errno);
    }

    sockaddr_un addr = {};
    addr.sun_family   = AF_UNIX;
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
    if (!mRunning)
    {
        return;
    }

    mRunning = false;
    shutdown(mSocketFd, SHUT_RDWR);
    close(mSocketFd);
    mSocketFd = -1;

    if (mThread.joinable())
    {
        mThread.join();
    }

    unlink(mSocketPath.c_str());
    mSocketPath.clear();
}

void FaultSocketServer::AcceptLoop()
{
    while (mRunning)
    {
        int connectionFd = accept(mSocketFd, nullptr, nullptr);
        if (connectionFd < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            // Stop() shuts the socket down to wake accept(); any other error
            // leaves the listening socket unusable.
            break;
        }

        HandleConnection(connectionFd);
        close(connectionFd);
    }
}

void FaultSocketServer::HandleConnection(int connectionFd)
{
    char buffer[kMaxRequestBytes + 1];
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
    buffer[used] = '\0';

    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    bool parsed = reader->parse(buffer, buffer + used, &root, &errors);

    char response[128];
    if (!parsed || !root.isObject())
    {
        ChipLogError(AppServer, "FaultSocket: invalid JSON: %s", errors.c_str());
        snprintf(response, sizeof(response), "{\"result\":\"error\",\"detail\":\"%.64s\"}\n", errors.c_str());
    }
    else
    {
        const std::string action = root.get("action", "").asString();
        if (action == "clear_all")
        {
            FaultInjector::Instance().ClearAllFaults();
            snprintf(response, sizeof(response), "{\"result\":\"ok\"}\n");
        }
        else if (action == "clear")
        {
            ClusterId cluster     = ParseNumber(root["cluster"]);
            AttributeId attribute = ParseNumber(root["attribute"]);
            FaultInjector::Instance().ClearFault(cluster, attribute);
            snprintf(response, sizeof(response), "{\"result\":\"ok\"}\n");
        }
        else if (action == "configure")
        {
            FaultConfig config;
            CHIP_ERROR err = ParseFaultConfig(root["fault"], config);
            if (err == CHIP_NO_ERROR)
            {
                err = FaultInjector::Instance().ConfigureFault(config);
            }
            if (err == CHIP_NO_ERROR)
            {
                snprintf(response, sizeof(response), "{\"result\":\"ok\"}\n");
            }
            else
            {
                ChipLogError(AppServer, "FaultSocket: configure failed: %" CHIP_ERROR_FORMAT, err.Format());
                snprintf(response, sizeof(response), "{\"result\":\"error\",\"detail\":\"%s\"}\n", err.AsString());
            }
        }
        else
        {
            snprintf(response, sizeof(response), "{\"result\":\"error\",\"detail\":\"unknown action\"}\n");
        }
    }

    ssize_t ignored = write(connectionFd, response, strlen(response));
    (void) ignored;
}

} // namespace fault_injection
} // namespace chip
