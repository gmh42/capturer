# capturer

A CMake-based C++ prototype for a desktop capture pipeline that is designed to support:

- pipewire-style capture backends
- GPU-ready frame staging with a Vulkan-facing abstraction
- downstream processing through an interchangeable frame processor interface
- a tile-difference processor for change detection on 16x16 blocks
- an X11 sample application that captures the desktop and feeds the frames to the diff pipeline
- an optional PipeWire stream source that preserves dma-buf frames for Wayland zero-copy integration

## Structure

- include/capturer/capture/pipewire_capture.hpp: capture backend and pipeline API
- include/capturer/image_processing/frame_processor.hpp: processor abstraction
- include/capturer/image_processing/tile_difference.hpp: reference tile-difference helper
- include/capturer/gpu/vulkan_runtime.hpp: Vulkan runtime abstraction for future GPU integration
- src/: implementation files for the pipeline, processors, and runtime stub
- tests/: simple tests for the tile-difference logic and pipeline flow

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
