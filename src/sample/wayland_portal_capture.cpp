#include "capturer/sample/desktop_capture.hpp"

#if HAVE_PIPEWIRE

#include <gio/gio.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <memory>
#include <initializer_list>
#include <unistd.h>

namespace capturer::sample {

namespace {

constexpr const char* kPortalBus = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCastInterface = "org.freedesktop.portal.ScreenCast";

struct PortalResponse {
    bool complete = false;
    std::uint32_t response = 1;
    GVariant* results = nullptr;
};

void throw_gerror(const char* operation, GError* error) {
    const std::string message = error ? error->message : "unknown D-Bus error";
    if (error) {
        g_error_free(error);
    }
    throw std::runtime_error(std::string(operation) + ": " + message);
}

void on_portal_response(
    GDBusConnection*,
    const gchar*,
    const gchar*,
    const gchar*,
    const gchar*,
    GVariant* parameters,
    gpointer user_data) {
    auto* response = static_cast<PortalResponse*>(user_data);
    g_variant_get(parameters, "(u@a{sv})", &response->response, &response->results);
    response->complete = true;
}

GVariant* wait_for_request(GDBusConnection* connection, const gchar* request_path) {
    PortalResponse response;
    const guint subscription = g_dbus_connection_signal_subscribe(
        connection,
        kPortalBus,
        "org.freedesktop.portal.Request",
        "Response",
        request_path,
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
        &on_portal_response,
        &response,
        nullptr);

    while (!response.complete) {
        g_main_context_iteration(nullptr, TRUE);
    }
    g_dbus_connection_signal_unsubscribe(connection, subscription);

    if (response.response != 0) {
        if (response.results) {
            g_variant_unref(response.results);
        }
        throw std::runtime_error("Wayland screen-cast request was cancelled");
    }
    return response.results;
}

GVariant* call_request(
    GDBusConnection* connection,
    const char* method,
    GVariant* parameters,
    const GVariantType* reply_type) {
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        kPortalBus,
        kPortalPath,
        kScreenCastInterface,
        method,
        parameters,
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error);
    if (!reply) {
        throw_gerror(method, error);
    }

    const gchar* request_path = nullptr;
    g_variant_get(reply, "(&o)", &request_path);
    GVariant* results = wait_for_request(connection, request_path);
    g_variant_unref(reply);
    if (reply_type && results && !g_variant_is_of_type(results, reply_type)) {
        g_variant_unref(results);
        throw std::runtime_error(std::string(method) + ": unexpected portal response type");
    }
    return results;
}

GVariant* make_option(const char* name, GVariant* value) {
    return g_variant_new_dict_entry(g_variant_new_string(name), g_variant_new_variant(value));
}

GVariant* make_options(std::initializer_list<GVariant*> entries) {
    return g_variant_new_array(G_VARIANT_TYPE("{sv}"), entries.begin(), entries.size());
}

GVariant* take_result(GVariant* results, const char* name) {
    for (gsize index = 0; index < g_variant_n_children(results); ++index) {
        GVariant* entry = g_variant_get_child_value(results, index);
        GVariant* key = g_variant_get_child_value(entry, 0);
        const bool matches = std::string(g_variant_get_string(key, nullptr)) == name;
        g_variant_unref(key);
        if (matches) {
            GVariant* value = g_variant_get_child_value(entry, 1);
            g_variant_unref(entry);
            if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
                GVariant* unwrapped = g_variant_get_variant(value);
                g_variant_unref(value);
                return unwrapped;
            }
            return value;
        }
        g_variant_unref(entry);
    }
    return nullptr;
}

}  // namespace

PipeWireDesktopCaptureConfig open_wayland_portal_capture() {
    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!connection) {
        throw_gerror("connect to session D-Bus", error);
    }

    const std::string token = "capturer_" + std::to_string(static_cast<unsigned long>(getpid()));

    GVariant* create_reply = g_dbus_connection_call_sync(
        connection,
        kPortalBus,
        kPortalPath,
        kScreenCastInterface,
        "CreateSession",
        g_variant_new("(@a{sv})", make_options({make_option("session_handle_token", g_variant_new_string(token.c_str()))})),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &error);
    if (!create_reply) {
        g_object_unref(connection);
        throw_gerror("CreateSession", error);
    }

    const gchar* request_path = nullptr;
    g_variant_get(create_reply, "(&o)", &request_path);
    GVariant* create_results = wait_for_request(connection, request_path);
    g_variant_unref(create_reply);

    GVariant* session_value = take_result(create_results, "session_handle");
    if (session_value && g_variant_is_of_type(session_value, G_VARIANT_TYPE_VARIANT)) {
        GVariant* unwrapped = g_variant_get_variant(session_value);
        g_variant_unref(session_value);
        session_value = unwrapped;
    }
    if (!session_value || (!g_variant_is_of_type(session_value, G_VARIANT_TYPE_OBJECT_PATH) &&
                           !g_variant_is_of_type(session_value, G_VARIANT_TYPE_STRING))) {
        g_variant_unref(create_results);
        if (session_value) {
            g_variant_unref(session_value);
        }
        g_object_unref(connection);
        throw std::runtime_error("CreateSession did not return a valid session handle");
    }
    const std::string session(g_variant_get_string(session_value, nullptr));
    g_variant_unref(session_value);
    g_variant_unref(create_results);

    GVariant* source_results = call_request(
        connection,
        "SelectSources",
        g_variant_new(
            "(o@a{sv})",
            session.c_str(),
            make_options({
                make_option("types", g_variant_new_uint32(1)),
                make_option("multiple", g_variant_new_boolean(FALSE)),
                make_option("cursor_mode", g_variant_new_uint32(2)),
            })),
        G_VARIANT_TYPE("a{sv}"));
    g_variant_unref(source_results);

    GVariant* start_results = call_request(
        connection,
        "Start",
        g_variant_new("(os@a{sv})", session.c_str(), "", make_options({})),
        G_VARIANT_TYPE("a{sv}"));

    GVariant* streams = take_result(start_results, "streams");
    if (!streams || !g_variant_is_of_type(streams, G_VARIANT_TYPE("a(ua{sv})")) ||
        g_variant_n_children(streams) == 0) {
        if (streams) {
            g_variant_unref(streams);
        }
        g_variant_unref(start_results);
        g_object_unref(connection);
        throw std::runtime_error("Start did not return a PipeWire stream");
    }
    GVariant* stream = g_variant_get_child_value(streams, 0);
    guint32 node_id = 0;
    g_variant_get_child(stream, 0, "u", &node_id);
    // The portal reports the selected monitor's resolution as the "size" property, a (ii) tuple.
    gint stream_width = 0;
    gint stream_height = 0;
    GVariant* stream_properties = g_variant_get_child_value(stream, 1);
    GVariant* size_value = take_result(stream_properties, "size");
    if (size_value && g_variant_is_of_type(size_value, G_VARIANT_TYPE("(ii)"))) {
        g_variant_get(size_value, "(ii)", &stream_width, &stream_height);
    }
    if (size_value) {
        g_variant_unref(size_value);
    }
    g_variant_unref(stream_properties);
    g_variant_unref(stream);
    g_variant_unref(streams);
    g_variant_unref(start_results);

    GVariant* remote_reply = nullptr;
    GUnixFDList* fd_list = nullptr;
    remote_reply = g_dbus_connection_call_with_unix_fd_list_sync(
        connection,
        kPortalBus,
        kPortalPath,
        kScreenCastInterface,
        "OpenPipeWireRemote",
        g_variant_new("(o@a{sv})", session.c_str(), make_options({})),
        G_VARIANT_TYPE("(h)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &fd_list,
        nullptr,
        &error);
    if (!remote_reply) {
        g_object_unref(connection);
        throw_gerror("OpenPipeWireRemote", error);
    }

    gint fd_index = -1;
    g_variant_get(remote_reply, "(h)", &fd_index);
    const gint pipewire_fd = g_unix_fd_list_get(fd_list, fd_index, &error);
    g_variant_unref(remote_reply);
    g_object_unref(fd_list);
    if (pipewire_fd < 0) {
        g_object_unref(connection);
        throw_gerror("read PipeWire remote FD", error);
    }

    PipeWireDesktopCaptureConfig config;
    config.pipewire_fd = pipewire_fd;
    config.target_node_id = node_id;
    if (stream_width > 0 && stream_height > 0) {
        config.width = static_cast<std::size_t>(stream_width);
        config.height = static_cast<std::size_t>(stream_height);
    }
    config.portal_connection = std::shared_ptr<void>(
        connection,
        [](void* value) { g_object_unref(static_cast<GDBusConnection*>(value)); });
    return config;
}

}  // namespace capturer::sample

#endif