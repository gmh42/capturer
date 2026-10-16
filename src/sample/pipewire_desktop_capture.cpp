#include "capturer/sample/desktop_capture.hpp"

#if HAVE_PIPEWIRE

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <sys/mman.h>
#include <unistd.h>

#include <pipewire/context.h>
#include <pipewire/main-loop.h>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>

#include <spa/buffer/buffer.h>
#include <spa/param/buffers.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

namespace capturer::sample {

namespace {

// Bound the accepted PipeWire format range while covering current high-resolution, high-refresh displays.
constexpr std::uint32_t kMaximumCaptureDimension = 16384;
constexpr std::uint32_t kMaximumCaptureFrameRate = 240;
constexpr std::uint32_t kSupportedCaptureDataTypes =
    (1U << SPA_DATA_DmaBuf) | (1U << SPA_DATA_MemFd) | (1U << SPA_DATA_MemPtr);

std::string make_error_message(const char* what, int code) {
    return std::string(what) + ": " + std::strerror(code < 0 ? -code : code);
}

}  // namespace

struct PipeWireDesktopCaptureSource::Impl {
    explicit Impl(PipeWireDesktopCaptureConfig config_in)
        : config(std::move(config_in)) {
        pw_init(nullptr, nullptr);
        pipewire_initialized = true;

        loop = pw_main_loop_new(nullptr);
        if (!loop) {
            throw std::runtime_error("failed to create PipeWire main loop");
        }

        context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
        if (!context) {
            throw std::runtime_error("failed to create PipeWire context");
        }

        if (config.pipewire_fd >= 0) {
            core = pw_context_connect_fd(context, dup(config.pipewire_fd), nullptr, 0);
        } else {
            core = pw_context_connect(context, nullptr, 0);
        }
        if (!core) {
            throw std::runtime_error("failed to connect to PipeWire core");
        }

        struct pw_properties* props = pw_properties_new(
            "media.type", "Video",
            "media.subtype", "Raw",
            "media.class", "Stream/Input/Video",
            "media.role", "screen",
            "remote.intention", "screencast",
            "node.name", config.stream_name.c_str(),
            "node.description", config.stream_name.c_str(),
            nullptr);
        if (!props) {
            throw std::runtime_error("failed to allocate PipeWire stream properties");
        }
        stream = pw_stream_new(core, config.stream_name.c_str(), props);
        if (!stream) {
            throw std::runtime_error("failed to create PipeWire stream");
        }

        pw_stream_add_listener(stream, &listener, &stream_events, this);

        std::uint8_t pod_buffer[4096];
        spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
        const spa_rectangle preferred_size{
            static_cast<std::uint32_t>(config.width),
            static_cast<std::uint32_t>(config.height)};
        const spa_rectangle minimum_size{1, 1};
        const spa_rectangle maximum_size{kMaximumCaptureDimension, kMaximumCaptureDimension};
        const spa_fraction preferred_max_framerate{
            config.framerate_numerator,
            config.framerate_denominator};
        const spa_fraction minimum_max_framerate{1, 1};
        const spa_fraction maximum_max_framerate{kMaximumCaptureFrameRate, 1};
        const spa_pod* format = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &pod_builder,
            SPA_TYPE_OBJECT_Format,
            SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType,
            SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype,
            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(
                4,
                SPA_VIDEO_FORMAT_BGRA,
                SPA_VIDEO_FORMAT_BGRx,
                SPA_VIDEO_FORMAT_RGBA,
                SPA_VIDEO_FORMAT_RGBx),
            SPA_FORMAT_VIDEO_size,
            SPA_POD_CHOICE_RANGE_Rectangle(&preferred_size, &minimum_size, &maximum_size),
            SPA_FORMAT_VIDEO_maxFramerate,
            SPA_POD_CHOICE_RANGE_Fraction(
                &preferred_max_framerate,
                &minimum_max_framerate,
                &maximum_max_framerate)));
        if (!format || spa_pod_builder_corrupted(&pod_builder)) {
            throw std::runtime_error("failed to build PipeWire video format parameters");
        }
        const spa_pod* buffers = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &pod_builder,
            SPA_TYPE_OBJECT_ParamBuffers,
            SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int(kSupportedCaptureDataTypes)));
        if (!buffers || spa_pod_builder_corrupted(&pod_builder)) {
            throw std::runtime_error("failed to build PipeWire buffer parameters");
        }
        const spa_pod* params[] = {format, buffers};

        const int connect_result = pw_stream_connect(
            stream,
            PW_DIRECTION_INPUT,
            config.target_node_id,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_DONT_RECONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
            params,
            2);
        if (connect_result < 0) {
            throw std::runtime_error(make_error_message("failed to connect PipeWire stream", connect_result));
        }

        if (pw_stream_set_active(stream, true) < 0) {
            throw std::runtime_error("failed to activate PipeWire stream");
        }
    }

    ~Impl() {
        if (stream) {
            pw_stream_destroy(stream);
            stream = nullptr;
        }
        if (core) {
            pw_core_disconnect(core);
            core = nullptr;
        }
        if (context) {
            pw_context_destroy(context);
            context = nullptr;
        }
        if (loop) {
            pw_main_loop_destroy(loop);
            loop = nullptr;
        }
        if (pipewire_initialized) {
            pw_deinit();
            pipewire_initialized = false;
        }
    }

    std::optional<DesktopFrame> wait_for_frame(std::chrono::milliseconds timeout) {
        frame_ready = false;
        stream_error.clear();
        captured_frame = {};

        struct pw_loop* event_loop = pw_main_loop_get_loop(loop);
        pw_loop_enter(event_loop);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!frame_ready) {
            if (!stream_error.empty()) {
                pw_loop_leave(event_loop);
                throw std::runtime_error(stream_error);
            }
            const int result = pw_loop_iterate(event_loop, 25);
            if (result < 0) {
                pw_loop_leave(event_loop);
                throw std::runtime_error("PipeWire loop iteration failed");
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                pw_loop_leave(event_loop);
                return std::nullopt;
            }
        }
        pw_loop_leave(event_loop);
        return std::move(captured_frame);
    }

    DesktopFrame decode_buffer(struct pw_buffer* buffer) {
        DesktopFrame frame{};
        frame.width = config.width;
        frame.height = config.height;
        frame.stride = config.width * 4;
        frame.storage = FrameStorage::CpuRgba8;

        if (!buffer || !buffer->buffer || buffer->buffer->n_datas == 0) {
            return frame;
        }

        struct spa_buffer* spa_buffer = buffer->buffer;
        const struct spa_data& data = spa_buffer->datas[0];
        const struct spa_chunk* chunk = data.chunk;
        if (chunk && chunk->stride > 0) {
            frame.stride = static_cast<std::size_t>(chunk->stride);
        }

        const std::size_t fallback_size = frame.stride * frame.height;
        const std::size_t valid_size = chunk && chunk->size > 0 ? chunk->size : fallback_size;

        if (data.type == SPA_DATA_DmaBuf && data.fd >= 0) {
            frame.storage = FrameStorage::DmaBuf;
            frame.dma_buf_fd = dup(static_cast<int>(data.fd));
            return frame;
        }

        frame.pixels.resize(valid_size);

        if (data.data != nullptr) {
            std::memcpy(frame.pixels.data(), data.data, std::min(valid_size, static_cast<std::size_t>(data.maxsize)));
        } else if (data.fd >= 0 && (data.type == SPA_DATA_MemFd || data.type == SPA_DATA_Invalid)) {
            const std::size_t map_size = data.maxsize > 0 ? static_cast<std::size_t>(data.maxsize) : valid_size;
            void* mapped = mmap(nullptr, map_size, PROT_READ, MAP_SHARED, static_cast<int>(data.fd), static_cast<off_t>(data.mapoffset));
            if (mapped != MAP_FAILED) {
                std::memcpy(frame.pixels.data(), mapped, std::min(valid_size, map_size));
                munmap(mapped, map_size);
            }
        }

        return frame;
    }

    static void on_stream_state_changed(void* data, enum pw_stream_state, enum pw_stream_state state, const char* error) {
        auto* self = static_cast<Impl*>(data);
        if (state == PW_STREAM_STATE_ERROR) {
            self->stream_error = error ? error : "PipeWire stream error";
        }
    }

    static void on_stream_process(void* data) {
        auto* self = static_cast<Impl*>(data);
        if (!self->stream) {
            return;
        }

        struct pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream);
        if (!buffer) {
            return;
        }

        self->captured_frame = self->decode_buffer(buffer);
        self->frame_ready = true;
        pw_stream_queue_buffer(self->stream, buffer);
    }

    static void on_stream_param_changed(void* data, uint32_t id, const struct spa_pod* param) {
        auto* self = static_cast<Impl*>(data);
        if (id != SPA_PARAM_Format || !param) {
            return;
        }

        struct spa_video_info info{};
        if (spa_format_video_parse(param, &info) == 0 && info.media_subtype == SPA_MEDIA_SUBTYPE_raw) {
            self->config.width = std::max<std::size_t>(1, info.info.raw.size.width);
            self->config.height = std::max<std::size_t>(1, info.info.raw.size.height);
        }
    }

    PipeWireDesktopCaptureConfig config;
    struct pw_main_loop* loop = nullptr;
    struct pw_context* context = nullptr;
    struct pw_core* core = nullptr;
    struct pw_stream* stream = nullptr;
    struct spa_hook listener{};
    DesktopFrame captured_frame{};
    std::string stream_error;
    bool frame_ready = false;
    bool pipewire_initialized = false;

    const struct pw_stream_events stream_events = {
        PW_VERSION_STREAM_EVENTS,
        nullptr,
        &Impl::on_stream_state_changed,
        nullptr,
        nullptr,
        &Impl::on_stream_param_changed,
        nullptr,
        nullptr,
        &Impl::on_stream_process,
        nullptr,
        nullptr,
        nullptr,
    };
};

PipeWireDesktopCaptureSource::PipeWireDesktopCaptureSource(PipeWireDesktopCaptureConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

PipeWireDesktopCaptureSource::~PipeWireDesktopCaptureSource() = default;

std::string PipeWireDesktopCaptureSource::name() const {
    return impl_->config.stream_name;
}

DesktopFrame PipeWireDesktopCaptureSource::capture() {
    auto frame = impl_->wait_for_frame(std::chrono::seconds(10));
    if (!frame) {
        throw std::runtime_error("timed out waiting for PipeWire frame");
    }
    return std::move(*frame);
}

std::optional<DesktopFrame> PipeWireDesktopCaptureSource::try_capture(std::chrono::milliseconds timeout) {
    return impl_->wait_for_frame(timeout);
}

}  // namespace capturer::sample

#endif