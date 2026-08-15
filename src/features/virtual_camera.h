#pragma once

#include <cstdint>
#include <atomic>
#include <cstddef>

// This works independently of OBS Studio - the driver just needs to be installed

bool StartVirtualCamera(uint32_t width, uint32_t height);

void StopVirtualCamera();

// Keeps the producer enabled/disabled and resized to the preferred monitor
// resolution. Both render backends call this once per real game frame.
void SyncVirtualCameraRuntimeState(bool enabled);

bool WriteVirtualCameraFrame(const uint8_t* rgba_data, uint32_t width, uint32_t height, uint64_t timestamp);

// nv12_data must be width*height*3/2 bytes (NV12 format)
bool WriteVirtualCameraFrameNV12(const uint8_t* nv12_data, uint32_t width, uint32_t height, uint64_t timestamp);

bool WriteVirtualCameraFrameNV12Planes(const uint8_t* y_plane, const uint8_t* uv_plane, uint32_t width, uint32_t height,
									   uint64_t timestamp);

// Vulkan can import these page-aligned shared-memory slots and write NV12
// directly from the GPU. Publication is deferred until the command buffer's
// existing completion query retires, so the virtual-camera consumer never sees
// a partially written frame and no full-frame CPU readback is required.
struct VirtualCameraGpuFrame {
    uint8_t* data = nullptr;
    size_t capacityBytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t slot = 0;
    uint32_t writeIndex = 0;
    uint64_t timestamp = 0;
    uint64_t generation = 0;
};

bool AcquireVirtualCameraGpuFrame(uint64_t timestamp,
                                  VirtualCameraGpuFrame& outFrame);
bool PublishVirtualCameraGpuFrame(const VirtualCameraGpuFrame& frame);
void AbandonVirtualCameraGpuFrame(const VirtualCameraGpuFrame& frame);

bool IsVirtualCameraActive();

// Check if OBS Virtual Camera driver is installed
bool IsVirtualCameraDriverInstalled();

bool IsVirtualCameraInUseByOBS();

bool ShouldCaptureVirtualCameraFrame();

// Single public resize entry point: updates dimensions in-place if capacity allows,
// otherwise recreates the shared memory producer. Thread-safe.
bool EnsureVirtualCameraSize(uint32_t width, uint32_t height);

bool GetVirtualCameraResolution(uint32_t& outWidth, uint32_t& outHeight);

void GetVirtualCameraMonitorSize(uint32_t& outWidth, uint32_t& outHeight);

bool GetPreferredVirtualCameraResolution(uint32_t& outWidth, uint32_t& outHeight);

// Call this when the game window is resized. Records pending resize state;
// the actual resize is deferred and debounced via FlushPendingVirtualCameraResize().
void OnGameWindowResized(uint32_t newWidth, uint32_t newHeight);

// Called once per frame (from SyncVirtualCameraRuntimeState) to apply any pending
// debounced resize. Returns true if a resize was actually performed.
bool FlushPendingVirtualCameraResize();

void RequestVirtualCameraRecoveryFrames();

const char* GetVirtualCameraError();

extern std::atomic<bool> g_virtualCameraActive;


