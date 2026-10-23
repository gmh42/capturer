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

#include <dirent.h>
#include <drm_fourcc.h>

#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/pod/parser.h>
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

std::string make_error_message(const char* what, int code) {
    return std::string(what) + ": " + std::strerror(code < 0 ? -code : code);
}

capturer::frame::PixelFormat to_pixel_format(spa_video_format format) {
    switch (format) {
    case SPA_VIDEO_FORMAT_RGBA: return capturer::frame::PixelFormat::RGBA8;
    case SPA_VIDEO_FORMAT_BGRA: return capturer::frame::PixelFormat::BGRA8;
    case SPA_VIDEO_FORMAT_RGBx: return capturer::frame::PixelFormat::RGBX8;
    case SPA_VIDEO_FORMAT_BGRx: return capturer::frame::PixelFormat::BGRX8;
    case SPA_VIDEO_FORMAT_ARGB: return capturer::frame::PixelFormat::ARGB8;
    case SPA_VIDEO_FORMAT_NV12: return capturer::frame::PixelFormat::NV12;
    case SPA_VIDEO_FORMAT_I420: return capturer::frame::PixelFormat::I420;
    default: return capturer::frame::PixelFormat::Unknown;
    }
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
        const spa_pod* params[2];
        std::uint32_t param_count = 0;
        if (dma_buf_supported()) {
            params[param_count++] = build_format(pod_builder, true);
        }
        params[param_count++] = build_format(pod_builder, false);
        if (spa_pod_builder_corrupted(&pod_builder)) {
            throw std::runtime_error("failed to build PipeWire video format parameters");
        }

        const int connect_result = pw_stream_connect(
            stream,
            PW_DIRECTION_INPUT,
            config.target_node_id,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_DONT_RECONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
            params,
            param_count);
        if (connect_result < 0) {
            throw std::runtime_error(make_error_message("failed to connect PipeWire stream", connect_result));
        }

        if (pw_stream_set_active(stream, true) < 0) {
            throw std::runtime_error("failed to activate PipeWire stream");
        }
    }

    // A VM or container without a DRM render node cannot import dma-bufs, so only offer CPU formats there.
    bool dma_buf_supported() const {
        return config.allow_dma_buf && access("/dev/dri", F_OK) == 0 && has_render_node();
    }

    static bool has_render_node() {
        DIR* dir = opendir("/dev/dri");
        if (!dir) {
            return false;
        }
        bool found = false;
        while (const dirent* entry = readdir(dir)) {
            if (std::strncmp(entry->d_name, "renderD", 7) == 0) {
                found = true;
                break;
            }
        }
        closedir(dir);
        return found;
    }

    const spa_pod* build_format(spa_pod_builder& builder, bool with_modifier) const {
        const spa_rectangle preferred_size{
            static_cast<std::uint32_t>(config.width),
            static_cast<std::uint32_t>(config.height)};
        const spa_rectangle minimum_size{1, 1};
        const spa_rectangle maximum_size{kMaximumCaptureDimension, kMaximumCaptureDimension};
        const spa_fraction minimum_max_framerate{1, 1};
        // Prefer the highest rate so the producer's ceiling (normally the display refresh rate) decides.
        const spa_fraction maximum_max_framerate{kMaximumCaptureFrameRate, 1};

        spa_pod_frame object_frame;
        spa_pod_builder_push_object(&builder, &object_frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(
            &builder,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(
                5,
                SPA_VIDEO_FORMAT_BGRA,
                SPA_VIDEO_FORMAT_BGRA,
                SPA_VIDEO_FORMAT_BGRx,
                SPA_VIDEO_FORMAT_RGBA,
                SPA_VIDEO_FORMAT_RGBx),
            SPA_FORMAT_VIDEO_size,
            SPA_POD_CHOICE_RANGE_Rectangle(&preferred_size, &minimum_size, &maximum_size),
            SPA_FORMAT_VIDEO_maxFramerate,
            SPA_POD_CHOICE_RANGE_Fraction(&maximum_max_framerate, &minimum_max_framerate, &maximum_max_framerate),
            0);
        if (with_modifier) {
            // The first choice entry is the default; the rest are the alternatives.
            const std::uint64_t modifiers[] = {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID};
            spa_pod_frame choice_frame;
            spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
            spa_pod_builder_push_choice(&builder, &choice_frame, SPA_CHOICE_Enum, 0);
            for (const std::uint64_t modifier : modifiers) {
                spa_pod_builder_long(&builder, static_cast<std::int64_t>(modifier));
            }
            spa_pod_builder_pop(&builder, &choice_frame);
        }
        return static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &object_frame));
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
        frame.format = negotiated_format;

        if (!buffer || !buffer->buffer || buffer->buffer->n_datas == 0) {
            return frame;
        }

        struct spa_buffer* spa_buffer = buffer->buffer;
        if (const auto* header = static_cast<const struct spa_meta_header*>(
                spa_buffer_find_meta_data(spa_buffer, SPA_META_Header, sizeof(struct spa_meta_header)))) {
            frame.has_header = true;
            frame.sequence = header->seq;
            frame.pts_ns = header->pts;
        }
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
            frame.dma_buf_offset = chunk ? chunk->offset : 0;
            frame.has_drm_modifier = negotiated_has_modifier;
            frame.drm_modifier = negotiated_modifier;
            return frame;
        }

        // assign() copies straight into the vector without zero-filling it first.
        if (data.data != nullptr) {
            const auto* source = static_cast<const std::uint8_t*>(data.data);
            frame.pixels.assign(source, source + std::min(valid_size, static_cast<std::size_t>(data.maxsize)));
        } else if (data.fd >= 0 && (data.type == SPA_DATA_MemFd || data.type == SPA_DATA_Invalid)) {
            const std::size_t map_size = data.maxsize > 0 ? static_cast<std::size_t>(data.maxsize) : valid_size;
            void* mapped = mmap(nullptr, map_size, PROT_READ, MAP_SHARED, static_cast<int>(data.fd), static_cast<off_t>(data.mapoffset));
            if (mapped != MAP_FAILED) {
                const auto* source = static_cast<const std::uint8_t*>(mapped);
                frame.pixels.assign(source, source + std::min(valid_size, map_size));
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
        if (spa_format_video_parse(param, &info) < 0 || info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
            return;
        }
        self->config.width = std::max<std::size_t>(1, info.info.raw.size.width);
        self->config.height = std::max<std::size_t>(1, info.info.raw.size.height);
        // framerate is 0/1 for variable-rate streams, where max_framerate holds the ceiling instead.
        const spa_fraction fixed_rate = info.info.raw.framerate;
        self->negotiated_format = to_pixel_format(info.info.raw.format);
        self->negotiated_has_modifier = (info.info.raw.flags & SPA_VIDEO_FLAG_MODIFIER) != 0;
        self->negotiated_modifier = info.info.raw.modifier;
        self->negotiated_max_framerate = fixed_rate.num != 0 ? fixed_rate : info.info.raw.max_framerate;

        // A modifier in the negotiated format means the producer chose dma-buf.
        const bool use_dma_buf = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier) != nullptr;
        const std::int32_t data_types = use_dma_buf
            ? (1 << SPA_DATA_DmaBuf)
            : ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr));
        std::uint8_t pod_buffer[512];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
        const spa_pod* buffers = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder,
            SPA_TYPE_OBJECT_ParamBuffers,
            SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_dataType,
            SPA_POD_Int(data_types)));
        // The header meta (sequence, pts) is only attached to buffers when requested.
        const spa_pod* header_meta = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder,
            SPA_TYPE_OBJECT_ParamMeta,
            SPA_PARAM_Meta,
            SPA_PARAM_META_type,
            SPA_POD_Id(SPA_META_Header),
            SPA_PARAM_META_size,
            SPA_POD_Int(sizeof(struct spa_meta_header))));
        if (buffers && header_meta && !spa_pod_builder_corrupted(&builder)) {
            const spa_pod* update[] = {buffers, header_meta};
            pw_stream_update_params(self->stream, update, 2);
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
    spa_fraction negotiated_max_framerate{0, 1};
    capturer::frame::PixelFormat negotiated_format = capturer::frame::PixelFormat::Unknown;
    bool negotiated_has_modifier = false;
    std::uint64_t negotiated_modifier = 0;
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

double PipeWireDesktopCaptureSource::negotiated_max_framerate() const {
    const spa_fraction rate = impl_->negotiated_max_framerate;
    return rate.denom == 0 ? 0.0 : static_cast<double>(rate.num) / static_cast<double>(rate.denom);
}

std::optional<DesktopFrame> PipeWireDesktopCaptureSource::try_capture(std::chrono::milliseconds timeout) {
    return impl_->wait_for_frame(timeout);
}

}  // namespace capturer::sample

#endif