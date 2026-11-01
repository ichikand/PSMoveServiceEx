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
#include <mutex>
#include <future>
#include <deque>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <assert.h>
#include <gio/gio.h>

// -- constants ----
static const char *k_invalid_bluetooth_host = "00:00:00:00:00:00";
static const char *k_agent_object_path = "/org/psmoveservice/agent";
static const char *k_hid_service_uuid = "00001124-0000-1000-8000-00805f9b34fb";

#define CONN_RETRIES 80
#define CONN_DELAY 300
#define CONN_CHECK_NUM_TRIES 5
#define CONN_CHECK_DELAY 300
#define DBUS_CALL_TIMEOUT_MS 5000

static const char *k_bluez_agent_introspection_xml =
    "<node>"
    "  <interface name='org.bluez.Agent1'>"
    "    <method name='Release'/>"
    "    <method name='RequestPinCode'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='pincode' direction='out'/>"
    "    </method>"
    "    <method name='DisplayPinCode'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='pincode' direction='in'/>"
    "    </method>"
    "    <method name='RequestPasskey'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='out'/>"
    "    </method>"
    "    <method name='DisplayPasskey'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='in'/>"
    "      <arg type='q' name='entered' direction='in'/>"
    "    </method>"
    "    <method name='RequestConfirmation'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='in'/>"
    "    </method>"
    "    <method name='RequestAuthorization'>"
    "      <arg type='o' name='device' direction='in'/>"
    "    </method>"
    "    <method name='AuthorizeService'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "    </method>"
    "    <method name='Cancel'/>"
    "  </interface>"
    "</node>";

// -- BlueZ GDBus helper functions -----
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

static std::string bluez_build_device_path(const std::string &adapter_path, const std::string &dev_mac)
{
    if (adapter_path.empty() || !is_valid_mac(dev_mac)) return "";
    std::string formatted = to_upper_mac(dev_mac);
    std::replace(formatted.begin(), formatted.end(), ':', '_');
    return adapter_path + "/dev_" + formatted;
}

static GDBusConnection *bluez_get_system_bus()
{
    GError *error = nullptr;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (!conn)
    {
        SERVER_MT_LOG_ERROR("BlueZ") << "D-Bus connection failed: " << (error ? error->message : "unknown");
        if (error) g_error_free(error);
    }
    return conn;
}

static bool bluez_set_property_bool(
    GDBusConnection *conn,
    const std::string &object_path,
    const char *interface_name,
    const char *property_name,
    bool value)
{
    if (!conn || object_path.empty()) return false;

    GError *error = nullptr;
    GVariant *res = g_dbus_connection_call_sync(
        conn,
        "org.bluez",
        object_path.c_str(),
        "org.freedesktop.DBus.Properties",
        "Set",
        g_variant_new("(ssv)", interface_name, property_name, g_variant_new_boolean(value ? TRUE : FALSE)),
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        DBUS_CALL_TIMEOUT_MS,
        nullptr,
        &error);

    if (!res)
    {
        SERVER_MT_LOG_ERROR("BlueZ") << "Set property '" << property_name << "' on '" << interface_name << "' failed: " << (error ? error->message : "unknown");
        if (error) g_error_free(error);
        return false;
    }

    g_variant_unref(res);
    return true;
}

static bool bluez_get_property_bool(
    GDBusConnection *conn,
    const std::string &object_path,
    const char *interface_name,
    const char *property_name,
    bool &out_value)
{
    if (!conn || object_path.empty()) return false;

    GError *error = nullptr;
    GVariant *res = g_dbus_connection_call_sync(
        conn,
        "org.bluez",
        object_path.c_str(),
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", interface_name, property_name),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        DBUS_CALL_TIMEOUT_MS,
        nullptr,
        &error);

    if (!res)
    {
        SERVER_MT_LOG_ERROR("BlueZ") << "Get property '" << property_name << "' on '" << interface_name << "' failed: " << (error ? error->message : "unknown");
        if (error) g_error_free(error);
        return false;
    }

    GVariant *val = nullptr;
    g_variant_get(res, "(v)", &val);
    if (val)
    {
        out_value = (g_variant_get_boolean(val) == TRUE);
        g_variant_unref(val);
    }
    g_variant_unref(res);
    return (val != nullptr);
}

static std::string bluez_find_adapter_path(GDBusConnection *conn, const std::string &host_mac)
{
    if (!conn) return "";

    GVariant *result = g_dbus_connection_call_sync(
        conn,
        "org.bluez",
        "/",
        "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects",
        nullptr,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE,
        DBUS_CALL_TIMEOUT_MS,
        nullptr,
        nullptr);

    if (!result) return "";

    const std::string target_mac = to_upper_mac(host_mac);
    std::string matched_path;
    std::string fallback_path;

    GVariantIter *objects_iter = nullptr;
    g_variant_get(result, "(a{oa{sa{sv}}})", &objects_iter);

    const gchar *object_path = nullptr;
    GVariantIter *ifaces_iter = nullptr;

    while (g_variant_iter_next(objects_iter, "{&oa{sa{sv}}}", &object_path, &ifaces_iter))
    {
        const gchar *iface_name = nullptr;
        GVariant *props_dict = nullptr;

        while (g_variant_iter_next(ifaces_iter, "{&s@a{sv}}", &iface_name, &props_dict))
        {
            if (g_strcmp0(iface_name, "org.bluez.Adapter1") == 0)
            {
                if (fallback_path.empty())
                {
                    fallback_path = object_path;
                }

                const gchar *addr = nullptr;
                if (g_variant_lookup(props_dict, "Address", "&s", &addr) && addr != nullptr)
                {
                    if (!target_mac.empty() && to_upper_mac(addr) == target_mac)
                    {
                        matched_path = object_path;
                    }
                }
            }
            g_variant_unref(props_dict);
        }
        g_variant_iter_free(ifaces_iter);
    }

    g_variant_iter_free(objects_iter);
    g_variant_unref(result);

    return !matched_path.empty() ? matched_path : fallback_path;
}

static bool bluez_find_device_paths(
    GDBusConnection *conn,
    const std::string &dev_mac,
    std::string &out_device_path,
    std::string &out_adapter_path)
{
    out_device_path.clear();
    out_adapter_path.clear();
    if (!conn) return false;

    GVariant *result = g_dbus_connection_call_sync(
        conn,
        "org.bluez",
        "/",
        "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects",
        nullptr,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE,
        DBUS_CALL_TIMEOUT_MS,
        nullptr,
        nullptr);

    if (!result) return false;

    const std::string target_mac = to_upper_mac(dev_mac);
    GVariantIter *objects_iter = nullptr;
    g_variant_get(result, "(a{oa{sa{sv}}})", &objects_iter);

    const gchar *object_path = nullptr;
    GVariantIter *ifaces_iter = nullptr;

    while (g_variant_iter_next(objects_iter, "{&oa{sa{sv}}}", &object_path, &ifaces_iter))
    {
        const gchar *iface_name = nullptr;
        GVariant *props_dict = nullptr;

        while (g_variant_iter_next(ifaces_iter, "{&s@a{sv}}", &iface_name, &props_dict))
        {
            if (g_strcmp0(iface_name, "org.bluez.Device1") == 0)
            {
                const gchar *addr = nullptr;
                const gchar *adapter = nullptr;
                if (g_variant_lookup(props_dict, "Address", "&s", &addr) && addr != nullptr)
                {
                    if (to_upper_mac(addr) == target_mac)
                    {
                        out_device_path = object_path;
                        if (g_variant_lookup(props_dict, "Adapter", "&o", &adapter) && adapter != nullptr)
                        {
                            out_adapter_path = adapter;
                        }
                    }
                }
            }
            g_variant_unref(props_dict);
        }
        g_variant_iter_free(ifaces_iter);
    }

    g_variant_iter_free(objects_iter);
    g_variant_unref(result);

    return !out_device_path.empty();
}

static bool bluez_is_connected_by_path(GDBusConnection *conn, const std::string &dev_path)
{
    bool connected = false;
    if (bluez_get_property_bool(conn, dev_path, "org.bluez.Device1", "Connected", connected))
    {
        return connected;
    }
    return false;
}

static void bluez_trust_device_by_path(GDBusConnection *conn, const std::string &dev_path)
{
    bluez_set_property_bool(conn, dev_path, "org.bluez.Device1", "Trusted", true);
}

static void bluez_remove_device(const std::string &mac)
{
    GDBusConnection *conn = bluez_get_system_bus();
    if (!conn) return;

    std::string dev_path, adapter_path;
    if (bluez_find_device_paths(conn, mac, dev_path, adapter_path) && !adapter_path.empty())
    {
        GVariant *res = g_dbus_connection_call_sync(
            conn,
            "org.bluez",
            adapter_path.c_str(),
            "org.bluez.Adapter1",
            "RemoveDevice",
            g_variant_new("(o)", dev_path.c_str()),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            DBUS_CALL_TIMEOUT_MS,
            nullptr,
            nullptr);
        if (res) g_variant_unref(res);
    }

    g_object_unref(conn);
}

static bool bluez_setup_adapter(
    GDBusConnection *conn,
    const std::string &host_mac,
    std::string &out_adapter_path,
    bool &out_was_discoverable,
    bool &out_was_pairable,
    bool &out_was_powered)
{
    out_adapter_path = bluez_find_adapter_path(conn, host_mac);
    if (out_adapter_path.empty())
    {
        return false;
    }

    out_was_discoverable = false;
    out_was_pairable = false;
    out_was_powered = false;

    bluez_get_property_bool(conn, out_adapter_path, "org.bluez.Adapter1", "Discoverable", out_was_discoverable);
    bluez_get_property_bool(conn, out_adapter_path, "org.bluez.Adapter1", "Pairable", out_was_pairable);
    bluez_get_property_bool(conn, out_adapter_path, "org.bluez.Adapter1", "Powered", out_was_powered);

    bluez_set_property_bool(conn, out_adapter_path, "org.bluez.Adapter1", "Powered", true);
    bluez_set_property_bool(conn, out_adapter_path, "org.bluez.Adapter1", "Pairable", true);
    bluez_set_property_bool(conn, out_adapter_path, "org.bluez.Adapter1", "Discoverable", true);

    return true;
}

static void bluez_restore_adapter(
    GDBusConnection *conn,
    const std::string &adapter_path,
    bool was_discoverable,
    bool was_pairable,
    bool was_powered)
{
    if (!conn || adapter_path.empty()) return;
    bluez_set_property_bool(conn, adapter_path, "org.bluez.Adapter1", "Discoverable", was_discoverable);
    bluez_set_property_bool(conn, adapter_path, "org.bluez.Adapter1", "Pairable", was_pairable);
    bluez_set_property_bool(conn, adapter_path, "org.bluez.Adapter1", "Powered", was_powered);
}

// -- BluezPairingAgent -----
class BluezPairingAgent
{
public:
    BluezPairingAgent()
        : m_context(nullptr)
        , m_loop(nullptr)
        , m_stop_requested(false)
    {
    }

    ~BluezPairingAgent()
    {
        stop();
    }

    bool start(const std::string &target_mac, const std::string &host_mac)
    {
        stop();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop_requested = false;
            m_context = nullptr;
            m_loop = nullptr;
            m_host_mac = host_mac;
        }

        std::string formatted_mac = to_upper_mac(target_mac);
        std::replace(formatted_mac.begin(), formatted_mac.end(), ':', '_');
        m_target_dev_suffix = "/DEV_" + formatted_mac;

        std::promise<bool> ready_promise;
        std::future<bool> ready_future = ready_promise.get_future();

        try
        {
            m_thread = std::thread(&BluezPairingAgent::threadFunc, this, std::move(ready_promise));
        }
        catch (...)
        {
            SERVER_MT_LOG_ERROR("BluezPairingAgent") << "Failed to spawn GDBus agent thread";
            return false;
        }

        if (!ready_future.get())
        {
            if (m_thread.joinable())
            {
                m_thread.join();
            }
            return false;
        }

        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop_requested = true;
            if (m_context != nullptr && m_loop != nullptr)
            {
                g_main_context_invoke_full(
                    m_context,
                    G_PRIORITY_DEFAULT,
                    &BluezPairingAgent::onQuitInvoke,
                    g_main_loop_ref(m_loop),
                    reinterpret_cast<GDestroyNotify>(g_main_loop_unref));
            }
        }

        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

private:
    static gboolean onQuitInvoke(gpointer user_data)
    {
        GMainLoop *loop = static_cast<GMainLoop *>(user_data);
        g_main_loop_quit(loop);
        return G_SOURCE_REMOVE;
    }

    bool matchesTargetDevice(const char *device_path) const
    {
        if (!device_path || m_target_dev_suffix.empty())
        {
            return false;
        }

        std::string path_upper = to_upper_mac(device_path);
        if (path_upper.length() < m_target_dev_suffix.length())
        {
            return false;
        }

        return path_upper.compare(
            path_upper.length() - m_target_dev_suffix.length(),
            m_target_dev_suffix.length(),
            m_target_dev_suffix) == 0;
    }

    static void trustDeviceAsync(GDBusConnection *conn, const char *device_path)
    {
        if (!conn || !device_path) return;

        g_dbus_connection_call(
            conn,
            "org.bluez",
            device_path,
            "org.freedesktop.DBus.Properties",
            "Set",
            g_variant_new("(ssv)", "org.bluez.Device1", "Trusted", g_variant_new_boolean(TRUE)),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            DBUS_CALL_TIMEOUT_MS,
            nullptr,
            nullptr,
            nullptr);
    }

    std::string generateZcm1Pin() const
    {
        std::string pin = "0000";
        if (is_valid_mac(m_host_mac))
        {
            unsigned int mac[6];
            if (sscanf(m_host_mac.c_str(), "%x:%x:%x:%x:%x:%x",
                &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6)
            {
                char buffer[7];
                buffer[0] = static_cast<char>(mac[5]);
                buffer[1] = static_cast<char>(mac[4]);
                buffer[2] = static_cast<char>(mac[3]);
                buffer[3] = static_cast<char>(mac[2]);
                buffer[4] = static_cast<char>(mac[1]);
                buffer[5] = static_cast<char>(mac[0]);
                buffer[6] = '\0';
                pin = std::string(buffer, 6);
            }
        }
        return pin;
    }

    static void onMethodCall(
        GDBusConnection *connection,
        const gchar * /*sender*/,
        const gchar * /*object_path*/,
        const gchar * /*interface_name*/,
        const gchar *method_name,
        GVariant *parameters,
        GDBusMethodInvocation *invocation,
        gpointer user_data)
    {
        BluezPairingAgent *self = static_cast<BluezPairingAgent *>(user_data);

        if (g_strcmp0(method_name, "AuthorizeService") == 0)
        {
            const gchar *device_path = nullptr;
            const gchar *uuid = nullptr;
            g_variant_get(parameters, "(&o&s)", &device_path, &uuid);

            if (self->matchesTargetDevice(device_path) &&
                (g_ascii_strcasecmp(uuid, k_hid_service_uuid) == 0 ||
                 g_ascii_strcasecmp(uuid, "1124") == 0))
            {
                SERVER_MT_LOG_INFO("BluezPairingAgent") << "Authorized HID service for " << device_path;
                trustDeviceAsync(connection, device_path);
                g_dbus_method_invocation_return_value(invocation, nullptr);
            }
            else
            {
                g_dbus_method_invocation_return_dbus_error(
                    invocation, "org.bluez.Error.Rejected", "Rejected");
            }
        }
        else if (g_strcmp0(method_name, "RequestPinCode") == 0)
        {
            const gchar *device_path = nullptr;
            g_variant_get(parameters, "(&o)", &device_path);

            if (self->matchesTargetDevice(device_path))
            {
                SERVER_MT_LOG_INFO("BluezPairingAgent") << "Providing PIN code for " << device_path;
                trustDeviceAsync(connection, device_path);
                
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "0000"));
            }
            else
            {
                g_dbus_method_invocation_return_dbus_error(
                    invocation, "org.bluez.Error.Rejected", "Rejected");
            }
        }
        else if (g_strcmp0(method_name, "RequestPasskey") == 0)
        {
            const gchar *device_path = nullptr;
            g_variant_get(parameters, "(&o)", &device_path);

            if (self->matchesTargetDevice(device_path))
            {
                trustDeviceAsync(connection, device_path);
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 0));
            }
            else
            {
                g_dbus_method_invocation_return_dbus_error(
                    invocation, "org.bluez.Error.Rejected", "Rejected");
            }
        }
        else if (g_strcmp0(method_name, "RequestConfirmation") == 0)
        {
            const gchar *device_path = nullptr;
            guint32 passkey = 0;
            g_variant_get(parameters, "(&ou)", &device_path, &passkey);

            if (self->matchesTargetDevice(device_path))
            {
                trustDeviceAsync(connection, device_path);
                g_dbus_method_invocation_return_value(invocation, nullptr);
            }
            else
            {
                g_dbus_method_invocation_return_dbus_error(
                    invocation, "org.bluez.Error.Rejected", "Rejected");
            }
        }
        else if (g_strcmp0(method_name, "RequestAuthorization") == 0)
        {
            const gchar *device_path = nullptr;
            g_variant_get(parameters, "(&o)", &device_path);

            if (self->matchesTargetDevice(device_path))
            {
                trustDeviceAsync(connection, device_path);
                g_dbus_method_invocation_return_value(invocation, nullptr);
            }
            else
            {
                g_dbus_method_invocation_return_dbus_error(
                    invocation, "org.bluez.Error.Rejected", "Rejected");
            }
        }
        else if (g_strcmp0(method_name, "DisplayPinCode") == 0)
        {
            const gchar *device_path = nullptr;
            const gchar *pincode = nullptr;
            g_variant_get(parameters, "(&o&s)", &device_path, &pincode);

            if (self->matchesTargetDevice(device_path))
            {
                g_dbus_method_invocation_return_value(invocation, nullptr);
            }
            else
            {
                g_dbus_method_invocation_return_dbus_error(
                    invocation, "org.bluez.Error.Rejected", "Rejected");
            }
        }
        else if (g_strcmp0(method_name, "DisplayPasskey") == 0)
        {
            const gchar *device_path = nullptr;
            guint32 passkey = 0;
            guint16 entered = 0;
            g_variant_get(parameters, "(&ouq)", &device_path, &passkey, &entered);

            if (self->matchesTargetDevice(device_path))
            {
                g_dbus_method_invocation_return_value(invocation, nullptr);
            }
            else
            {
                g_dbus_method_invocation_return_dbus_error(
                    invocation, "org.bluez.Error.Rejected", "Rejected");
            }
        }
        else if (g_strcmp0(method_name, "Release") == 0 || g_strcmp0(method_name, "Cancel") == 0)
        {
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }
        else
        {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.bluez.Error.Rejected", "Unsupported method");
        }
    }

    void threadFunc(std::promise<bool> ready_promise)
    {
        GMainContext *context = g_main_context_new();
        g_main_context_push_thread_default(context);

        GError *error = nullptr;
        GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
        if (!conn)
        {
            SERVER_MT_LOG_ERROR("BluezPairingAgent") << "Failed to connect to system D-Bus: "
                << (error ? error->message : "unknown");
            if (error) g_error_free(error);
            g_main_context_pop_thread_default(context);
            g_main_context_unref(context);
            ready_promise.set_value(false);
            return;
        }

        GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(k_bluez_agent_introspection_xml, &error);
        if (!node_info)
        {
            SERVER_MT_LOG_ERROR("BluezPairingAgent") << "Failed to parse introspection XML: "
                << (error ? error->message : "unknown");
            if (error) g_error_free(error);
            g_object_unref(conn);
            g_main_context_pop_thread_default(context);
            g_main_context_unref(context);
            ready_promise.set_value(false);
            return;
        }

        static const GDBusInterfaceVTable vtable = {
            &BluezPairingAgent::onMethodCall,
            nullptr,
            nullptr,
            { nullptr }
        };

        guint reg_id = g_dbus_connection_register_object(
            conn,
            k_agent_object_path,
            node_info->interfaces[0],
            &vtable,
            this,
            nullptr,
            &error);

        if (reg_id == 0)
        {
            SERVER_MT_LOG_ERROR("BluezPairingAgent") << "Failed to register agent object: "
                << (error ? error->message : "unknown");
            if (error) g_error_free(error);
            g_dbus_node_info_unref(node_info);
            g_object_unref(conn);
            g_main_context_pop_thread_default(context);
            g_main_context_unref(context);
            ready_promise.set_value(false);
            return;
        }

        GVariant *res = g_dbus_connection_call_sync(
            conn,
            "org.bluez",
            "/org/bluez",
            "org.bluez.AgentManager1",
            "RegisterAgent",
            g_variant_new("(os)", k_agent_object_path, "NoInputNoOutput"),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            DBUS_CALL_TIMEOUT_MS,
            nullptr,
            &error);

        if (!res)
        {
            SERVER_MT_LOG_ERROR("BluezPairingAgent") << "RegisterAgent failed: "
                << (error ? error->message : "unknown");
            if (error) g_error_free(error);
            g_dbus_connection_unregister_object(conn, reg_id);
            g_dbus_node_info_unref(node_info);
            g_object_unref(conn);
            g_main_context_pop_thread_default(context);
            g_main_context_unref(context);
            ready_promise.set_value(false);
            return;
        }
        g_variant_unref(res);

        res = g_dbus_connection_call_sync(
            conn,
            "org.bluez",
            "/org/bluez",
            "org.bluez.AgentManager1",
            "RequestDefaultAgent",
            g_variant_new("(o)", k_agent_object_path),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            DBUS_CALL_TIMEOUT_MS,
            nullptr,
            &error);

        if (!res)
        {
            SERVER_MT_LOG_WARNING("BluezPairingAgent") << "RequestDefaultAgent warning: "
                << (error ? error->message : "unknown");
            if (error)
            {
                g_error_free(error);
                error = nullptr;
            }
        }
        else
        {
            g_variant_unref(res);
        }

        GMainLoop *loop = g_main_loop_new(context, FALSE);
        bool should_run = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_context = g_main_context_ref(context);
            m_loop = g_main_loop_ref(loop);
            should_run = !m_stop_requested;
        }

        ready_promise.set_value(true);

        if (should_run)
        {
            g_main_loop_run(loop);
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_loop)
            {
                g_main_loop_unref(m_loop);
                m_loop = nullptr;
            }
            if (m_context)
            {
                g_main_context_unref(m_context);
                m_context = nullptr;
            }
        }
        g_main_loop_unref(loop);

        res = g_dbus_connection_call_sync(
            conn,
            "org.bluez",
            "/org/bluez",
            "org.bluez.AgentManager1",
            "UnregisterAgent",
            g_variant_new("(o)", k_agent_object_path),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            DBUS_CALL_TIMEOUT_MS,
            nullptr,
            nullptr);
        if (res) g_variant_unref(res);

        g_dbus_connection_unregister_object(conn, reg_id);
        g_dbus_node_info_unref(node_info);
        g_object_unref(conn);

        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
    }

    std::string m_target_dev_suffix;
    std::string m_host_mac;
    std::mutex m_mutex;
    GMainContext *m_context;
    GMainLoop *m_loop;
    bool m_stop_requested;
    std::thread m_thread;
};

// -- definitions -----
class BluetoothAsyncOperationState
{
public:
    std::thread worker_thread;

    virtual ~BluetoothAsyncOperationState()
    {
        joinWorkerThread();
    }

    void joinWorkerThread()
    {
        if (worker_thread.joinable())
        {
            worker_thread.join();
        }
    }

    virtual void initialize(const int id)
    {
        joinWorkerThread();

        controller_id = id;
        main_thread_id = std::this_thread::get_id();
        worker_thread_id.store(main_thread_id);

        subStatus_WorkerThread.store(0);
        isCanceled_WorkerThread.store(false);

        std::lock_guard<std::mutex> lock(status_mutex);
        status_queue.clear();
    }

    inline bool isMainThread() const
    {
        return std::this_thread::get_id() == main_thread_id;
    }

    inline bool isWorkerThread() const
    {
        return std::this_thread::get_id() == worker_thread_id.load();
    }

    int getControllerID() const
    {
        return controller_id;
    }

    template <typename t_enum_status>
    void setSubStatus_WorkerThread(t_enum_status newSubStatus)
    {
        int32_t val = static_cast<int32_t>(newSubStatus);
        subStatus_WorkerThread.store(val);

        std::lock_guard<std::mutex> lock(status_mutex);
        if (status_queue.empty() || status_queue.back() != val)
        {
            status_queue.push_back(val);
        }
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
        std::lock_guard<std::mutex> lock(status_mutex);
        if (!status_queue.empty())
        {
            outNewSubStatus = static_cast<t_enum_status>(status_queue.front());
            status_queue.pop_front();
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

    std::atomic<std::thread::id> worker_thread_id;

protected:
    int controller_id;
    std::thread::id main_thread_id;
    std::atomic<int32_t> subStatus_WorkerThread;
    std::atomic<bool> isCanceled_WorkerThread;
    std::mutex status_mutex;
    std::deque<int32_t> status_queue;
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
    state->worker_thread_id.store(std::this_thread::get_id());
    state->setSubStatus_WorkerThread(BluetoothUnpairDeviceState::removingBluetoothDevice);

    const std::string mac = state->getControllerAddress();
    SERVER_MT_LOG_INFO("AsyncBluetoothUnpairDeviceRequest") << "Removing device from BlueZ: " << mac;

    bluez_remove_device(mac);

    state->setSubStatus_WorkerThread(BluetoothUnpairDeviceState::success);
}

void AsyncBluetoothUnpairDeviceRequest::update()
{
    BluetoothUnpairDeviceState *state = reinterpret_cast<BluetoothUnpairDeviceState *>(m_internal_state);

    BluetoothUnpairDeviceState::eStatus subStatus;
    while (state->pollSubStatus_MainThread(subStatus))
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
            break;
        }
    }
}

void AsyncBluetoothUnpairDeviceRequest::cancel(AsyncBluetoothRequest::eCancelReason /*reason*/)
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
    state->worker_thread_id.store(std::this_thread::get_id());
    const std::string mac = state->controller_serial_string;
    const std::string host_mac = state->host_address_string;

    GDBusConnection *conn = bluez_get_system_bus();
    if (!conn)
    {
        state->setSubStatus_WorkerThread(BluetoothPairDeviceState::failed);
        return;
    }

    bool isCompleted = false;
    bool adapter_configured = false;
    bool was_discoverable = false;
    bool was_pairable = false;
    bool was_powered = false;
    std::string adapter_path;
    std::string device_path;
    BluezPairingAgent pairing_agent;

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
                    SERVER_MT_LOG_INFO("AsyncBluetoothPairDeviceRequest") << "Configuring BlueZ adapter and starting agent for " << mac;

                    if (!bluez_setup_adapter(conn, host_mac, adapter_path, was_discoverable, was_pairable, was_powered))
                    {
                        SERVER_MT_LOG_ERROR("AsyncBluetoothPairDeviceRequest") << "Failed to configure BlueZ adapter";
                        nextSubStatus = BluetoothPairDeviceState::failed;
                        break;
                    }

                    adapter_configured = true;
                    device_path = bluez_build_device_path(adapter_path, mac);

                    if (!pairing_agent.start(mac, host_mac))
                    {
                        SERVER_MT_LOG_ERROR("AsyncBluetoothPairDeviceRequest") << "Failed to start BlueZ pairing agent";
                        nextSubStatus = BluetoothPairDeviceState::failed;
                        break;
                    }

                    nextSubStatus = BluetoothPairDeviceState::deviceScan;
                }
                break;

            case BluetoothPairDeviceState::deviceScan:
                {
                    state->connectionAttemptCount = 0;
                    nextSubStatus = BluetoothPairDeviceState::attemptConnection;
                }
                break;

            case BluetoothPairDeviceState::attemptConnection:
                {
                    if (bluez_is_connected_by_path(conn, device_path))
                    {
                        SERVER_MT_LOG_INFO("AsyncBluetoothPairDeviceRequest") << "Controller connected via BlueZ: " << mac;
                        bluez_trust_device_by_path(conn, device_path);
                        nextSubStatus = BluetoothPairDeviceState::patchRegistry;
                    }
                    else
                    {
                        ++state->connectionAttemptCount;
                        if (state->connectionAttemptCount >= CONN_RETRIES)
                        {
                            SERVER_MT_LOG_ERROR("AsyncBluetoothPairDeviceRequest")
                                << "Timed out waiting for Bluetooth connection from " << mac;
                            nextSubStatus = BluetoothPairDeviceState::failed;
                        }
                        else
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(CONN_DELAY));
                        }
                    }
                }
                break;

            case BluetoothPairDeviceState::patchRegistry:
                {
                    bluez_trust_device_by_path(conn, device_path);
                    state->verifyConnectionCount = 0;
                    nextSubStatus = BluetoothPairDeviceState::verifyConnection;
                }
                break;

            case BluetoothPairDeviceState::verifyConnection:
                {
                    if (bluez_is_connected_by_path(conn, device_path))
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

        if (nextSubStatus == BluetoothPairDeviceState::success ||
            nextSubStatus == BluetoothPairDeviceState::failed)
        {
            pairing_agent.stop();
            if (adapter_configured)
            {
                bluez_restore_adapter(conn, adapter_path, was_discoverable, was_pairable, was_powered);
            }
            isCompleted = true;
        }

        if (nextSubStatus != subStatus)
        {
            state->setSubStatus_WorkerThread(nextSubStatus);
        }
    }

    g_object_unref(conn);
}

void AsyncBluetoothPairDeviceRequest::update()
{
    BluetoothPairDeviceState *state = reinterpret_cast<BluetoothPairDeviceState *>(m_internal_state);

    BluetoothPairDeviceState::eStatus subStatus;
    while (state->pollSubStatus_MainThread(subStatus))
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
            break;
        }
    }
}

void AsyncBluetoothPairDeviceRequest::cancel(AsyncBluetoothRequest::eCancelReason /*reason*/)
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