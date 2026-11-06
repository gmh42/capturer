# capturer

A CMake-based C++ prototype for a desktop capture pipeline that is designed to support:

- pipewire-style capture backends
- GPU-ready frame staging with a Vulkan-facing abstraction
- a platform-neutral frame description (`IFrameInfo`) covering CPU and GPU-resident frames
- downstream processing through an interchangeable image processor interface (`IImageProcessor`)
- a tile-difference processor for change detection on 16x16 blocks
- an X11 sample application that captures the desktop and feeds the frames to the diff pipeline
- an optional SDL2 tile-difference viewer for live CPU or Vulkan processing
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
auto result = dispatcher.process(previous, current);  // FramePtr = shared_ptr<const IFrameInfo>
```

The Vulkan processor imports dma-bufs as Vulkan images without copying and compares any two live frames. It is
described, with its limits and the buffer-lifetime rules, in [differencer.md](differencer.md).

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

### Live tile viewer

Install the SDL2 development package (e.g. `libsdl2-dev` on Debian/Ubuntu) before configuring.
When SDL2 is available, CMake builds `capturer_diff_viewer`:

```bash
./build/capturer_diff_viewer                          # select source and processor automatically
./build/capturer_diff_viewer --backend=cpu            # request CPU frames
./build/capturer_diff_viewer --backend=vulkan         # require Vulkan dma-buf frames
./build/capturer_diff_viewer --source=x11 --backend=cpu
```

The window shows one 4x4 red square for each changed 16x16 source tile, and black for unchanged
tiles. The first frame is entirely red. Close the window or press Escape to exit. If the window
itself is captured, its updates may also cause changes in the display.

Automatic source selection uses the Wayland screen-cast portal when `WAYLAND_DISPLAY` is set,
otherwise X11; `--source=wayland` explicitly requests the portal. The portal asks you to select a
screen. CPU mode requests CPU buffers; Vulkan requires compatible single-plane dma-bufs, a Vulkan
device and the shader compiler at build time. X11 capture supplies CPU frames only. Automatic mode
selects a processor for the received memory type without copying; unsupported frames produce an
error rather than silently switching modes. PipeWire support is required for Wayland capture.
X11 root-window capture may not work through XWayland; use the portal source in a Wayland session.

SDL is used only to display the backend-neutral tile result, leaving capture, processing, and the
display separate. This is a regular opaque window, not a desktop overlay. Transparent, click-through
overlays would need additional platform/compositor-specific work (especially on Wayland).
