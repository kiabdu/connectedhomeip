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
#include <string>
#include <thread>

#include <lib/core/CHIPError.h>

namespace chip {
namespace fault_injection {

/**
 * @brief Unix domain socket server for receiving fault injection commands
 *
 * Listens on a configurable socket path and accepts newline-terminated JSON
 * commands to configure or clear fault injections. Each connection is
 * handled synchronously on a dedicated thread; the response is a single
 * JSON line of the form {"result":"ok"} or {"result":"error","detail":"..."}.
 */
class FaultSocketServer
{
public:
    FaultSocketServer()  = default;
    ~FaultSocketServer() = default;

    /**
     * @brief Start listening for fault injection commands
     *
     * Starts a thread that accepts connections and dispatches incoming
     * JSON commands to FaultInjector::Instance().
     *
     * @param socketPath Path to the Unix domain socket (e.g., "/tmp/matter_fault.sock")
     * @return CHIP_ERROR Result of socket initialization
     */
    CHIP_ERROR Start(const char * socketPath);

    /**
     * @brief Stop listening, close the socket, and wait for the accept
     * thread to exit.
     */
    void Stop();

    /**
     * @brief Check if the server is running
     */
    bool IsRunning() const { return mRunning; }

private:
    void AcceptLoop();
    void HandleConnection(int connectionFd);

    bool mRunning  = false;
    int mSocketFd  = -1;
    std::string mSocketPath;
    std::thread mThread;
};

} // namespace fault_injection
} // namespace chip
