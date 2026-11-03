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
#include <string>

#include <lib/core/CHIPError.h>

namespace chip {
namespace fault_injection {

/**
 * @brief Unix domain socket server for receiving fault injection commands
 *
 * Listens on a configurable socket path and accepts JSON commands
 * to configure, trigger, or clear fault injections.
 */
class FaultSocketServer
{
public:
    FaultSocketServer()  = default;
    ~FaultSocketServer() = default;

    /**
     * @brief Start listening for fault injection commands
     *
     * @param socketPath Path to the Unix domain socket (e.g., "/tmp/matter_fault.sock")
     * @return CHIP_ERROR Result of socket initialization
     */
    CHIP_ERROR Start(const char * socketPath);

    /**
     * @brief Stop listening and close the socket
     */
    void Stop();

    /**
     * @brief Check if the server is running
     */
    bool IsRunning() const { return mRunning; }

private:
    bool mRunning = false;
    int mSocketFd = -1;
    std::string mSocketPath;
};

} // namespace fault_injection
} // namespace chip
