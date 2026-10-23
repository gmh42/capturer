# capturer

A CMake-based C++ prototype for a desktop capture pipeline that is designed to support:

- pipewire-style capture backends
- GPU-ready frame staging with a Vulkan-facing abstraction
- a platform-neutral frame description (`IFrameInfo`) covering CPU and GPU-resident frames
- downstream processing through an interchangeable image processor interface (`IImageProcessor`)
- a tile-difference processor for change detection on 16x16 blocks
- an X11 sample application that captures the desktop and feeds the frames to the diff pipeline
- an optional PipeWire stream source that preserves dma-buf frames for Wayland zero-copy integration

## Structure

- include/capturer/capture/pipewire_capture.hpp: capture backend and pipeline API
- include/capturer/frame/frame_info.hpp: `IFrameInfo`, pixel formats, plane layouts, `CpuFrame`/`GpuFrame`
- include/capturer/image_processing/image_processor.hpp: `IImageProcessor`, null and CPU processors, dispatcher
- include/capturer/image_processing/vulkan_tile_processor.hpp: Vulkan compute tile processor over imported dma-bufs
- include/capturer/image_processing/tile_difference.hpp: reference tile-difference helper
- include/capturer/gpu/vulkan_runtime.hpp: Vulkan runtime abstraction for future GPU integration
- src/: implementation files for the pipeline, processors, and runtime stub
- tests/: simple tests for the tile-difference logic and pipeline flow

## Frames and processors

`IFrameInfo` describes a frame without exposing Vulkan or D3D types: width, height, `PixelFormat`
(RGBA8, BGRA8, RGBX8, BGRX8, ARGB8, NV12, I420, P010), per-plane offset/stride/size, colour space and range,
producer sequence and timestamp, and where the pixels live (`MemoryKind`: Cpu, DmaBuf, VulkanImage, D3D11Texture,
D3D12Resource). `describe(format)` reports plane count, components, bits per component and per pixel, alpha and
chroma subsampling. CPU frames expose `cpu_data()`; GPU frames expose a `GpuHandle` (dma-buf fds, DRM fourcc and
modifier, native handle, acquire fence) and close their fds on destruction.

An `IImageProcessor` declares the memory kinds and formats it accepts and processes a (previous, current) frame pair
into an `IProcessResult`, which may stay on the GPU until `tile_diff()` reads it back. `ImageProcessorDispatcher`
picks the first registered processor that accepts each frame and otherwise falls back to `NullImageProcessor`
(skip). It never copies between CPU and GPU memory; `fell_back()` reports a mismatch. Register the preferred backend
first:

```cpp
ImageProcessorDispatcher dispatcher;
dispatcher.add(std::make_unique<VulkanTileDifferenceProcessor>(runtime, 16));  // dma-buf frames
dispatcher.add(std::make_unique<CpuTileDifferenceProcessor>(16));              // CPU frames
auto result = dispatcher.process(previous.get(), *current);
```

The Vulkan processor imports each dma-buf as a Vulkan image (`VK_EXT_external_memory_dma_buf` +
`VK_EXT_image_drm_format_modifier`, no copy) and runs a compute shader that compares it with a device-local copy of
the previously processed frame (the producer recycles its buffers, so the old dma-buf cannot be re-read). It is
stateful: `previous` must be the frame passed as `current` on the preceding call, otherwise all tiles are reported
changed. It accepts single-plane RGB frames with an explicit DRM modifier that the device can import; anything else
falls through to the next processor. It ignores the padding byte of BGRX/RGBX frames, which the CPU processor
compares. The shader is compiled with `glslangValidator` at build time; without it the processor refuses all frames.
`tests/test_vulkan_dma_buf.cpp` checks it against the CPU processor on GBM-allocated buffers (skipped without a GPU).

## Build and test

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/capturer_sample

# Capture PipeWire frames for three seconds and report capture FPS
./build/capturer_pipewire_benchmark

# Wayland path: the sample opens the desktop portal and asks you to select a monitor
./build/capturer_sample
```
