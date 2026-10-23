#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "capturer/gpu/vulkan_runtime.hpp"
#include "capturer/frame/frame_info.hpp"
#include "capturer/image_processing/image_processor.hpp"

#include <functional>
#include <memory>

namespace capturer::capture {

using FramePtr = capturer::frame::FramePtr;

struct CaptureResult {
    std::string processor_name;
    capturer::image_processing::ProcessorBackend backend = capturer::image_processing::ProcessorBackend::None;
    FramePtr frame;
    std::shared_ptr<capturer::image_processing::IProcessResult> result;
};

class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;
    virtual FramePtr capture_frame(std::size_t width, std::size_t height) = 0;
};

struct PipeWireStreamConfig {
    std::string node_name;
    std::string media_type;
    std::string media_subtype;
    std::size_t width = 1920;
    std::size_t height = 1080;
};

struct PipeWireFrameEvent {
    std::size_t width;
    std::size_t height;
    std::vector<std::uint8_t> bytes;
};

class PipeWireCaptureBackend : public CaptureBackend {
public:
    explicit PipeWireCaptureBackend(
        std::string source_name,
        std::shared_ptr<capturer::gpu::VulkanRuntime> runtime = std::make_shared<capturer::gpu::VulkanRuntime>())
        : source_name_(std::move(source_name)), runtime_(std::move(runtime)) {
        pipewire_initialized_ = false;
        stream_config_.node_name = source_name_;
        stream_config_.media_type = "Video";
        stream_config_.media_subtype = "Raw";
    }

    ~PipeWireCaptureBackend();

    FramePtr capture_frame(std::size_t width, std::size_t height) override;

    const std::string& source_name() const { return source_name_; }
    void set_frame_callback(std::function<void(const PipeWireFrameEvent&)> callback);
    void set_stream_config(const PipeWireStreamConfig& config);

private:
    std::string source_name_;
    std::shared_ptr<capturer::gpu::VulkanRuntime> runtime_;
    PipeWireStreamConfig stream_config_;
    bool pipewire_initialized_ = false;
    std::size_t frame_counter_ = 0;
    std::function<void(const PipeWireFrameEvent&)> frame_callback_;
};

class CapturePipeline {
public:
    CapturePipeline(std::unique_ptr<CaptureBackend> backend, capturer::image_processing::ImageProcessorDispatcher processor)
        : backend_(std::move(backend)), processor_(std::move(processor)) {}

    CaptureResult process_frame();

private:
    std::unique_ptr<CaptureBackend> backend_;
    capturer::image_processing::ImageProcessorDispatcher processor_;
    FramePtr previous_frame_;
};

}  // namespace capturer::capture
