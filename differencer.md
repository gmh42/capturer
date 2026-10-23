# Frame differencer notes

## Interface

```cpp
using FramePtr = std::shared_ptr<const IFrameInfo>;
std::unique_ptr<IProcessResult> IImageProcessor::process(const FramePtr& previous, const FramePtr& current);
```

`process()` shares ownership of both frames. A GPU result may still be running when `process()` returns, so the
result keeps both frames, and anything they own (`GpuHandle::lifetime`), alive until it is destroyed.
`previous` may be null (all tiles reported changed) or any frame that is still alive; it need not be the last
frame processed. A previous frame that differs in size or format, or cannot be imported, also reports all tiles
changed. Processors are single-threaded.

## Vulkan dma-buf import

`VulkanTileDifferenceProcessor` accepts single-plane RGB dma-bufs (BGRA8, BGRX8, RGBA8, RGBX8) with an explicit DRM
modifier that the device reports as importable (`VkDrmFormatModifierPropertiesListEXT` plus the external-memory
importable flag).

- Extensions: `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`,
  `VK_KHR_external_semaphore_fd`, `VK_EXT_queue_family_foreign`.
- Each frame's fd is duplicated and imported with the modifier, plane offset and stride from the frame (dedicated
  allocation). Ownership is acquired from `VK_QUEUE_FAMILY_FOREIGN_EXT` with `oldLayout = UNDEFINED`.
- Synchronisation: the frame's `acquire_fence_fd` if set, else a sync_file exported from the dma-buf
  (`DMA_BUF_IOCTL_EXPORT_SYNC_FILE`), else implicit sync.
- The compute shader (`src/gpu/shaders/tile_difference.comp`, built with `glslangValidator`) reads the current and
  previous images as sampled textures and writes one word per tile to a host-visible buffer. The fence is waited on
  in `tile_diff()` or when the result is destroyed.
- The processor caches the last imported image, so the common `process(last, next)` imports one frame per call.
- The GPU path ignores the padding byte of BGRX/RGBX frames and compares alpha only for alpha formats; the CPU
  processor compares every byte.

## Buffer lifetime (ping-pong)

PipeWire producers recycle buffers: once a `pw_buffer` is queued back, the compositor may overwrite it. Earlier
code requeued it as soon as the frame was decoded, which let the compositor write while the GPU was still reading.

Now each dma-buf `DesktopFrame` carries a `buffer_lease`, moved into `GpuHandle::lifetime` by `to_frame_info()`. The
buffer is requeued only when the last reference to the frame (or a pending result) goes away, so previous and current
frames occupy two distinct producer buffers. CPU frames are copied out and requeued immediately.

- Releases may happen on any thread, so they go on a mutex-protected queue that is drained on the PipeWire loop
  thread (start of `on_stream_process` and each `wait_for_frame` iteration).
- `remove_buffer` bumps a generation counter so leases for buffers that no longer exist are not requeued.
- Destroying the capture source closes the queue; frames outliving it release nothing. Their dup'd fds keep the
  memory valid.
- dma-buf streams ask for 4-16 buffers (default 6) since two or more are held by the consumer. If all buffers are
  held the compositor has none to render into and frames are dropped, which is natural backpressure: consumers must
  drop frames they no longer need.
- A new frame replacing an unread one releases the older one.

## Known limitations

- Offered modifiers are fixed to LINEAR + INVALID rather than taken from the Vulkan modifier query.
- Multi-GPU: the Vulkan device may not be the one that allocated the compositor's buffer; this is not handled.
- Only single-plane RGB is imported; ARGB8 has no Vulkan mapping and YUV formats are not imported.
- Live PipeWire behaviour of the lease/buffer-count change is untested (unit tests cover the processor only).

## Other GPU APIs that can consume dma-bufs

- EGL/OpenGL: `EGL_EXT_image_dma_buf_import` with `_modifiers`, then compute shaders or texture sampling.
- OpenCL: `cl_khr_external_memory_dma_buf` (driver support varies).
- VA-API: surface import for video-processing pipelines (encode/scale, not custom compute).
- CUDA / Level Zero: external memory import; CUDA lacks DRM-modifier support.
- Windows equivalents: D3D11/D3D12 shared handles for Desktop Duplication.
