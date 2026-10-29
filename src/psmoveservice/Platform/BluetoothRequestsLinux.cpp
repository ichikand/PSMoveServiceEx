// A large chunk of this file was adapted from PSMoveAPI.
// Reproducing the license here:
/*
The PS Move API library is licensed under the terms of the license below.
However, some optional third party libraries might have a different license.
Be sure to read the README file for details on third party licenses.

====

Copyright (c) 2011, 2012 Thomas Perl <m@thp.io>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

/*
Modern Linux BlueZ pairing implementation:
Copyright (c) 2026 ichikand. All rights reserved.
SPDX-License-Identifier: Apache-2.0
*/

// -- includes -----
#include "BluetoothRequests.h"
#include "BluetoothQueries.h"
#include "DeviceManager.h"
#include "ServerControllerView.h"
#include "ServerNetworkManager.h"
#include "ServerLog.h"
#include "ServerUtility.h"

#include "PSMoveProtocol.pb.h"
#include "PSMoveProtocolInterface.h"

#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <assert.h>
#include <fstream>
#include <filesystem>

// -- constants ----
static const char *k_invalid_bluetooth_host = "00:00:00:00:00:00";

#define CONN_RETRIES 80
#define CONN_DELAY 300
#define CONN_CHECK_NUM_TRIES 5
#define CONN_CHECK_DELAY 300

// -- BlueZ helper functions -----
static std::string to_upper_mac(const std::string &mac)
{
    std::string res = mac;
    std::transform(res.begin(), res.end(), res.begin(),
        [](unsigned char c) { return std::toupper(c); });
    return res;
}

static bool is_valid_mac(const std::string &mac)
{
    if (mac.length() != 17) return false;
    for (size_t i = 0; i < mac.length(); ++i)
    {
        if ((i + 1) % 3 == 0)
        {
            if (mac[i] != ':') return false;
        }
        else
        {
            if (!std::isxdigit(static_cast<unsigned char>(mac[i]))) return false;
        }
    }
    return true;
}

static bool run_cmd_output(const std::string &cmd, std::string &output)
{
    output.clear();
    FILE *fp = popen(cmd.c_str(), "r");
    if (!fp) return false;

    char buf[256];
    while (fgets(buf, sizeof(buf), fp) != nullptr)
    {
        output += buf;
    }
    return (pclose(fp) == 0);
}

static bool bluez_is_connected(const std::string &mac)
{
    std::string out;
    std::string cmd = "bluetoothctl info " + to_upper_mac(mac) + " 2>&1";
    run_cmd_output(cmd, out);
    return (out.find("Connected: yes") != std::string::npos);
}

static bool bluez_device_exists(const std::string &mac)
{
    std::string out;
    std::string cmd = "bluetoothctl info " + to_upper_mac(mac) + " 2>&1";
    run_cmd_output(cmd, out);
    return (out.find("Device " + to_upper_mac(mac)) != std::string::npos);
}

static void bluez_trust_device(const std::string &mac)
{
    std::string out;
    run_cmd_output("bluetoothctl trust " + to_upper_mac(mac) + " 2>&1", out);
}

static void bluez_remove_device(const std::string &mac)
{
    std::string out;
    run_cmd_output("bluetoothctl remove " + to_upper_mac(mac) + " 2>&1", out);
}

static bool bluez_setup_adapter()
{
    std::string out;
    run_cmd_output("bluetoothctl show 2>&1", out);
    bool was_discoverable = (out.find("Discoverable: yes") != std::string::npos);

    run_cmd_output("bluetoothctl power on 2>&1", out);
    run_cmd_output("bluetoothctl pairable on 2>&1", out);
    run_cmd_output("bluetoothctl discoverable on 2>&1", out);

    return was_discoverable;
}

static void bluez_restore_adapter(bool was_discoverable)
{
    std::string out;
    if (!was_discoverable)
    {
        run_cmd_output("bluetoothctl discoverable off 2>&1", out);
    }
}

static bool bluez_provision_device_info(const std::string &host_mac, const std::string &dev_mac)
{
    if (!is_valid_mac(host_mac) || !is_valid_mac(dev_mac))
    {
        return false;
    }

    const std::string host_upper = to_upper_mac(host_mac);
    const std::string dev_upper = to_upper_mac(dev_mac);
    const std::string dir_path = "/var/lib/bluetooth/" + host_upper + "/" + dev_upper;
    const std::string file_path = dir_path + "/info";

    std::error_code ec;
    std::filesystem::create_directories(dir_path, ec);
    if (ec)
    {
        SERVER_LOG_ERROR("BlueZ") << "Failed to create directory: " << dir_path << " (" << ec.message() << ")";
        return false;
    }

    std::ofstream info_file(file_path, std::ios::trunc);
    if (!info_file.is_open())
    {
        SERVER_LOG_ERROR("BlueZ") << "Failed to write device info: " << file_path;
        return false;
    }

    info_file << "# Generated automatically by PSMoveService for PS Move HID pairing\n";
    info_file << "# Remove or unpair via PSMoveConfigTool\n";
    info_file << "[General]\n";
    info_file << "Name=Motion Controller\n";
    info_file << "Class=0x002508\n";
    info_file << "SupportedTechnologies=BR/EDR;\n";
    info_file << "Trusted=true\n";
    info_file << "Blocked=false\n";
    info_file << "Services=00001124-0000-1000-8000-00805f9b34fb;\n";
    info_file.close();

    return true;
}

static void bluez_cleanup_device_dir(const std::string &host_mac, const std::string &dev_mac)
{
    if (is_valid_mac(host_mac) && is_valid_mac(dev_mac))
    {
        const std::string dir_path = "/var/lib/bluetooth/" + to_upper_mac(host_mac) + "/" + to_upper_mac(dev_mac);
        std::error_code ec;
        std::filesystem::remove_all(dir_path, ec);
    }
}

// -- definitions -----
class BluetoothAsyncOperationState
{
public:
    std::thread worker_thread;

    virtual ~BluetoothAsyncOperationState()
    {
        if (worker_thread.joinable())
        {
            worker_thread.join();
        }
    }

    virtual void initialize(const int id)
    {
        controller_id = id;
        main_thread_id = std::this_thread::get_id();
        worker_thread_id = main_thread_id;

        subStatus_WorkerThread.store(0);
        isCanceled_WorkerThread.store(false);
        subStatus_MainThread = 0;
    }

    inline bool isMainThread() const
    {
        return std::this_thread::get_id() == main_thread_id;
    }

    inline bool isWorkerThread() const
    {
        return std::this_thread::get_id() == worker_thread_id;
    }

    int getControllerID() const
    {
        return controller_id;
    }

    template <typename t_enum_status>
    void setSubStatus_WorkerThread(t_enum_status newSubStatus)
    {
        subStatus_WorkerThread.store(static_cast<int32_t>(newSubStatus));
    }

    template <typename t_enum_status>
    t_enum_status getSubStatus_WorkerThread()
    {
        return static_cast<t_enum_status>(subStatus_WorkerThread.load());
    }

    bool getIsCanceled_WorkerThread()
    {
        return isCanceled_WorkerThread.load();
    }

    template <typename t_enum_status>
    bool pollSubStatus_MainThread(t_enum_status &outNewSubStatus)
    {
        assert(isMainThread());
        int32_t newSubStatus = subStatus_WorkerThread.load();
        if (newSubStatus != subStatus_MainThread)
        {
            subStatus_MainThread = newSubStatus;
            outNewSubStatus = static_cast<t_enum_status>(newSubStatus);
            return true;
        }
        return false;
    }

    bool getIsCanceled_MainThread()
    {
        assert(isMainThread());
        return isCanceled_WorkerThread.load();
    }

    void setIsCanceled_MainThread()
    {
        assert(isMainThread());
        isCanceled_WorkerThread.store(true);
    }

    std::thread::id worker_thread_id;

protected:
    int controller_id;
    std::thread::id main_thread_id;
    std::atomic<int32_t> subStatus_WorkerThread;
    std::atomic<bool> isCanceled_WorkerThread;
    int32_t subStatus_MainThread;
};

class BluetoothUnpairDeviceState : public BluetoothAsyncOperationState
{
public:
    enum eStatus
    {
        start,
        removingBluetoothDevice,

        k_total_steps,
        success = k_total_steps,
        failed,
    };

    virtual void initialize(const int id, const std::string &bt_address_string)
    {
        BluetoothAsyncOperationState::initialize(id);
        controllerAddress = bt_address_string;
    }

    const std::string &getControllerAddress() const
    {
        return controllerAddress;
    }

protected:
    std::string controllerAddress;
};

class BluetoothPairDeviceState : public BluetoothAsyncOperationState
{
public:
    enum eStatus
    {
        start,
        setupBluetoothRadio,
        deviceScan,
        attemptConnection,
        patchRegistry,
        verifyConnection,

        k_total_steps,
        success = k_total_steps,
        failed,
    };

    std::string controller_serial_string;
    std::string host_address_string;
    CommonDeviceState::eDeviceType controller_device_type;

    int scanCount;
    int connectionAttemptCount;
    int verifyConnectionCount;

    virtual void initialize(const int id) override
    {
        BluetoothAsyncOperationState::initialize(id);

        controller_serial_string.clear();
        host_address_string.clear();
        controller_device_type = CommonDeviceState::SUPPORTED_CONTROLLER_TYPE_COUNT;
        scanCount = 0;
        connectionAttemptCount = 0;
        verifyConnectionCount = 0;
    }
};

// -- prototypes -----
static void AsyncBluetoothUnpairDeviceThreadFunction(BluetoothUnpairDeviceState *state);
static void AsyncBluetoothPairDeviceThreadFunction(BluetoothPairDeviceState *state);

static void send_unpair_completed_notification_to_client(int connectionID, PSMoveProtocol::Response_ResultCode resultCode);
static void send_pair_completed_notification_to_client(int connectionID, PSMoveProtocol::Response_ResultCode resultCode);
static void send_progress_notification_to_client(int connectionID, int controllerID, int stepsCompleted, int totalSteps);

// -- AsyncBluetoothUnpairDeviceRequest -----
AsyncBluetoothUnpairDeviceRequest::AsyncBluetoothUnpairDeviceRequest(
    int connectionId,
    ServerControllerViewPtr controllerView)
    : AsyncBluetoothRequest(connectionId)
    , m_controllerView(controllerView)
    , m_internal_state(nullptr)
{
    m_internal_state = new BluetoothUnpairDeviceState();
}

AsyncBluetoothUnpairDeviceRequest::~AsyncBluetoothUnpairDeviceRequest()
{
    delete reinterpret_cast<BluetoothUnpairDeviceState *>(m_internal_state);
}

bool AsyncBluetoothUnpairDeviceRequest::start()
{
    bool success = true;
    const int controller_id = m_controllerView->getDeviceID();
    const std::string bt_address_string = m_controllerView->getSerial();

    BluetoothUnpairDeviceState *state = reinterpret_cast<BluetoothUnpairDeviceState *>(m_internal_state);
    state->initialize(controller_id, bt_address_string);

    if (m_controllerView->getIsOpen() && !m_controllerView->getIsBluetooth())
    {
        if (!m_controllerView->setHostBluetoothAddress(k_invalid_bluetooth_host))
        {
            SERVER_LOG_WARNING("AsyncBluetoothUnpairDeviceRequest") << "Failed to clear host address on USB controller";
        }
    }

    if (!is_valid_mac(bt_address_string))
    {
        SERVER_LOG_ERROR("AsyncBluetoothUnpairDeviceRequest") << "Invalid controller MAC address: " << bt_address_string;
        success = false;
    }

    if (success)
    {
        try
        {
            state->worker_thread = std::thread(AsyncBluetoothUnpairDeviceThreadFunction, state);
        }
        catch (...)
        {
            SERVER_LOG_ERROR("AsyncBluetoothUnpairDeviceRequest") << "Failed to start worker thread!";
            success = false;
        }
    }

    m_status = success ? AsyncBluetoothRequest::running : AsyncBluetoothRequest::failed;
    return success;
}

static void AsyncBluetoothUnpairDeviceThreadFunction(BluetoothUnpairDeviceState *state)
{
    state->worker_thread_id = std::this_thread::get_id();
    state->setSubStatus_WorkerThread(BluetoothUnpairDeviceState::removingBluetoothDevice);

    const std::string mac = state->getControllerAddress();
    SERVER_MT_LOG_INFO("AsyncBluetoothUnpairDeviceRequest") << "Removing device from BlueZ: " << mac;

    bluez_remove_device(mac);

    std::string host_mac;
    if (bluetooth_get_host_address(host_mac))
    {
        bluez_cleanup_device_dir(host_mac, mac);
    }

    state->setSubStatus_WorkerThread(BluetoothUnpairDeviceState::success);
}

void AsyncBluetoothUnpairDeviceRequest::update()
{
    BluetoothUnpairDeviceState *state = reinterpret_cast<BluetoothUnpairDeviceState *>(m_internal_state);

    BluetoothUnpairDeviceState::eStatus subStatus;
    if (state->pollSubStatus_MainThread(subStatus))
    {
        send_progress_notification_to_client(
            m_connectionId,
            m_controllerView->getDeviceID(),
            static_cast<int>(subStatus),
            BluetoothUnpairDeviceState::k_total_steps);

        if (subStatus == BluetoothUnpairDeviceState::success ||
            subStatus == BluetoothUnpairDeviceState::failed)
        {
            if (subStatus == BluetoothUnpairDeviceState::success)
            {
                m_status = AsyncBluetoothRequest::succeeded;
                send_unpair_completed_notification_to_client(m_connectionId, PSMoveProtocol::Response_ResultCode_RESULT_OK);
            }
            else
            {
                m_status = AsyncBluetoothRequest::failed;
                send_unpair_completed_notification_to_client(
                    m_connectionId,
                    state->getIsCanceled_MainThread()
                    ? PSMoveProtocol::Response_ResultCode_RESULT_CANCELED
                    : PSMoveProtocol::Response_ResultCode_RESULT_ERROR);
            }

            if (state->worker_thread.joinable())
            {
                state->worker_thread.join();
            }
        }
    }
}

void AsyncBluetoothUnpairDeviceRequest::cancel(AsyncBluetoothRequest::eCancelReason reason)
{
    BluetoothUnpairDeviceState *state = reinterpret_cast<BluetoothUnpairDeviceState *>(m_internal_state);
    state->setIsCanceled_MainThread();
}

AsyncBluetoothRequest::eStatusCode AsyncBluetoothUnpairDeviceRequest::getStatusCode()
{
    return m_status;
}

std::string AsyncBluetoothUnpairDeviceRequest::getDescription()
{
    std::ostringstream description;
    description << "[Unpair] Controller ID: " << m_controllerView->getDeviceID() << " Conn: " << m_connectionId;
    return description.str();
}

// -- AsyncBluetoothPairDeviceRequest -----
AsyncBluetoothPairDeviceRequest::AsyncBluetoothPairDeviceRequest(
    int connectionId,
    ServerControllerViewPtr controllerView)
    : AsyncBluetoothRequest(connectionId)
    , m_controllerView(controllerView)
    , m_internal_state(nullptr)
{
    m_internal_state = new BluetoothPairDeviceState();
}

AsyncBluetoothPairDeviceRequest::~AsyncBluetoothPairDeviceRequest()
{
    delete reinterpret_cast<BluetoothPairDeviceState *>(m_internal_state);
}

bool AsyncBluetoothPairDeviceRequest::start()
{
    bool success = true;
    const int controller_id = m_controllerView->getDeviceID();

    BluetoothPairDeviceState *state = reinterpret_cast<BluetoothPairDeviceState *>(m_internal_state);
    state->initialize(controller_id);
    state->controller_serial_string = m_controllerView->getSerial();
    state->controller_device_type = m_controllerView->getControllerDeviceType();

    if (!m_controllerView->getIsOpen() || m_controllerView->getIsBluetooth())
    {
        SERVER_LOG_ERROR("AsyncBluetoothPairDeviceRequest")
            << "Controller " << controller_id << " isn't an open USB device";
        success = false;
    }

    if (success && !is_valid_mac(state->controller_serial_string))
    {
        SERVER_LOG_ERROR("AsyncBluetoothPairDeviceRequest")
            << "Malformed controller bluetooth address: " << state->controller_serial_string;
        success = false;
    }

    if (success)
    {
        if (!bluetooth_get_host_address(state->host_address_string) || state->host_address_string.empty())
        {
            SERVER_LOG_ERROR("AsyncBluetoothPairDeviceRequest") << "Failed to find host bluetooth adapter address";
            success = false;
        }
    }

    if (success)
    {
        if (m_controllerView->setHostBluetoothAddress(state->host_address_string))
        {
            SERVER_LOG_INFO("AsyncBluetoothPairDeviceRequest")
                << "Assigned host address " << state->host_address_string
                << " to controller id " << controller_id;
        }
        else
        {
            SERVER_LOG_ERROR("AsyncBluetoothPairDeviceRequest")
                << "Failed to set host address " << state->host_address_string
                << " on controller id " << controller_id;
            success = false;
        }
    }

    if (success)
    {
        try
        {
            state->worker_thread = std::thread(AsyncBluetoothPairDeviceThreadFunction, state);
        }
        catch (...)
        {
            SERVER_LOG_ERROR("AsyncBluetoothPairDeviceRequest") << "Failed to start worker thread!";
            success = false;
        }
    }

    m_status = success ? AsyncBluetoothRequest::running : AsyncBluetoothRequest::failed;
    return success;
}

static void AsyncBluetoothPairDeviceThreadFunction(BluetoothPairDeviceState *state)
{
    state->worker_thread_id = std::this_thread::get_id();
    const std::string mac = state->controller_serial_string;
    const std::string host_mac = state->host_address_string;
    bool isCompleted = false;
    bool was_discoverable = false;

    state->setSubStatus_WorkerThread(BluetoothPairDeviceState::setupBluetoothRadio);

    while (!isCompleted)
    {
        const BluetoothPairDeviceState::eStatus subStatus =
            state->getSubStatus_WorkerThread<BluetoothPairDeviceState::eStatus>();
        BluetoothPairDeviceState::eStatus nextSubStatus = subStatus;

        if (!state->getIsCanceled_WorkerThread())
        {
            switch (subStatus)
            {
            case BluetoothPairDeviceState::setupBluetoothRadio:
                {
                    SERVER_MT_LOG_INFO("AsyncBluetoothPairDeviceRequest") << "Configuring BlueZ adapter and provisioning " << mac;
                    was_discoverable = bluez_setup_adapter();
                    bluez_provision_device_info(host_mac, mac);
                    bluez_trust_device(mac);
                    nextSubStatus = BluetoothPairDeviceState::deviceScan;
                }
                break;

            case BluetoothPairDeviceState::deviceScan:
                {
                    bluez_trust_device(mac);
                    state->connectionAttemptCount = 0;
                    nextSubStatus = BluetoothPairDeviceState::attemptConnection;
                }
                break;

            case BluetoothPairDeviceState::attemptConnection:
                {
                    if (bluez_device_exists(mac))
                    {
                        bluez_trust_device(mac);
                    }

                    if (bluez_is_connected(mac))
                    {
                        SERVER_MT_LOG_INFO("AsyncBluetoothPairDeviceRequest") << "Controller connected via BlueZ: " << mac;
                        nextSubStatus = BluetoothPairDeviceState::patchRegistry;
                    }
                    else
                    {
                        ++state->connectionAttemptCount;
                        if (state->connectionAttemptCount >= CONN_RETRIES)
                        {
                            state->connectionAttemptCount = 0;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(CONN_DELAY));
                    }
                }
                break;

            case BluetoothPairDeviceState::patchRegistry:
                {
                    bluez_trust_device(mac);
                    state->verifyConnectionCount = 0;
                    nextSubStatus = BluetoothPairDeviceState::verifyConnection;
                }
                break;

            case BluetoothPairDeviceState::verifyConnection:
                {
                    if (bluez_is_connected(mac))
                    {
                        ++state->verifyConnectionCount;
                        if (state->verifyConnectionCount >= CONN_CHECK_NUM_TRIES)
                        {
                            SERVER_MT_LOG_INFO("AsyncBluetoothPairDeviceRequest") << "Bluetooth connection verified!";
                            nextSubStatus = BluetoothPairDeviceState::success;
                        }
                        else
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(CONN_CHECK_DELAY));
                        }
                    }
                    else
                    {
                        state->connectionAttemptCount = 0;
                        nextSubStatus = BluetoothPairDeviceState::attemptConnection;
                    }
                }
                break;

            default:
                nextSubStatus = BluetoothPairDeviceState::failed;
                break;
            }
        }
        else
        {
            nextSubStatus = BluetoothPairDeviceState::failed;
        }

        if (nextSubStatus != subStatus)
        {
            state->setSubStatus_WorkerThread(nextSubStatus);
        }

        if (nextSubStatus == BluetoothPairDeviceState::success ||
            nextSubStatus == BluetoothPairDeviceState::failed)
        {
            bluez_restore_adapter(was_discoverable);
            isCompleted = true;
        }
    }
}

void AsyncBluetoothPairDeviceRequest::update()
{
    BluetoothPairDeviceState *state = reinterpret_cast<BluetoothPairDeviceState *>(m_internal_state);

    BluetoothPairDeviceState::eStatus subStatus;
    if (state->pollSubStatus_MainThread(subStatus))
    {
        send_progress_notification_to_client(
            m_connectionId,
            m_controllerView->getDeviceID(),
            static_cast<int>(subStatus),
            BluetoothPairDeviceState::k_total_steps);

        if (subStatus == BluetoothPairDeviceState::success ||
            subStatus == BluetoothPairDeviceState::failed)
        {
            if (subStatus == BluetoothPairDeviceState::success)
            {
                m_status = AsyncBluetoothRequest::succeeded;
                send_pair_completed_notification_to_client(m_connectionId, PSMoveProtocol::Response_ResultCode_RESULT_OK);
            }
            else
            {
                m_status = AsyncBluetoothRequest::failed;
                send_pair_completed_notification_to_client(
                    m_connectionId,
                    state->getIsCanceled_MainThread()
                    ? PSMoveProtocol::Response_ResultCode_RESULT_CANCELED
                    : PSMoveProtocol::Response_ResultCode_RESULT_ERROR);
            }

            if (state->worker_thread.joinable())
            {
                state->worker_thread.join();
            }
        }
    }
}

void AsyncBluetoothPairDeviceRequest::cancel(AsyncBluetoothRequest::eCancelReason reason)
{
    BluetoothPairDeviceState *state = reinterpret_cast<BluetoothPairDeviceState *>(m_internal_state);
    state->setIsCanceled_MainThread();
}

AsyncBluetoothRequest::eStatusCode AsyncBluetoothPairDeviceRequest::getStatusCode()
{
    return m_status;
}

std::string AsyncBluetoothPairDeviceRequest::getDescription()
{
    std::ostringstream description;
    description << "[Pair] ID: " << m_controllerView->getDeviceID() << " Conn: " << m_connectionId;
    return description.str();
}

//-- network notification helper methods ----
static void send_unpair_completed_notification_to_client(int connectionID, PSMoveProtocol::Response_ResultCode resultCode)
{
    ResponsePtr notification(new PSMoveProtocol::Response);

    notification->set_type(PSMoveProtocol::Response_ResponseType_UNPAIR_REQUEST_COMPLETED);
    notification->set_request_id(-1);
    notification->set_result_code(resultCode);

    ServerNetworkManager::get_instance()->send_notification(connectionID, notification);
}

static void send_pair_completed_notification_to_client(int connectionID, PSMoveProtocol::Response_ResultCode resultCode)
{
    ResponsePtr notification(new PSMoveProtocol::Response);

    notification->set_type(PSMoveProtocol::Response_ResponseType_PAIR_REQUEST_COMPLETED);
    notification->set_request_id(-1);
    notification->set_result_code(resultCode);

    ServerNetworkManager::get_instance()->send_notification(connectionID, notification);
}

static void send_progress_notification_to_client(int connectionID, int controllerID, int stepsCompleted, int totalSteps)
{
    ResponsePtr notification(new PSMoveProtocol::Response);

    notification->set_type(PSMoveProtocol::Response_ResponseType_BLUETOOTH_REQUEST_PROGRESS);
    notification->set_request_id(-1);
    notification->set_result_code(PSMoveProtocol::Response_ResultCode_RESULT_OK);

    PSMoveProtocol::Response_ResultBluetoothRequestProgress *progress =
        notification->mutable_result_bluetooth_request_progress();

    progress->set_controller_id(controllerID);
    progress->set_steps_completed(stepsCompleted);
    progress->set_total_steps(totalSteps);

    ServerNetworkManager::get_instance()->send_notification(connectionID, notification);
}