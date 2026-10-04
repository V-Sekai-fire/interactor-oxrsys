// SPDX-License-Identifier: MPL-2.0
//
// Windows Vulkan interop, after mbucchia/VirtualDesktop-OpenXR (MIT,
// Copyright (c) 2022-2024 Matthieu Bucchianeri; vulkan_interop.cpp and
// d3d11_native.cpp). The runtime owns a D3D11 device on the app's adapter,
// swapchain images are shared D3D11 textures imported into the app's VkDevice,
// and one shared D3D11 fence is imported as a Vulkan timeline semaphore that
// the app's queue signals at xrEndFrame. Unlike VDXR there is no LibOVR
// compositor: the D3D11 side copies released eyes into textures that the PyroWave
// encoder (PyroWaveVideoEncoder.cpp) imports and encodes without leaving the GPU.
// D3D11 apps get the same shared textures opened on their own device, and the shared fence
// opened there too, so their frames reach the same staging copies with no Vulkan in between.

#pragma once

#if defined(_WIN32) && defined(XR_USE_GRAPHICS_API_VULKAN)

#include "GraphicsTypes.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan.h>
#include <openxr/openxr.h>

struct ID3D11Texture2D;

// Instance and device extensions the app must enable (VDXR vulkan_interop.cpp:44-47, :90-93).
extern const char* const kWin32VulkanInstanceExtensions;
extern const char* const kWin32VulkanDeviceExtensions;

// LUID of the adapter the runtime renders and encodes on: the hardware adapter with the
// most dedicated video memory (VDXR takes the HMD's adapter; there is no HMD here).
bool Win32GetRuntimeAdapterLuid(uint8_t luid[8]);

// LUID of the adapter an ID3D11Device was created on.
bool Win32DeviceAdapterLuid(void* d3d11Device, uint8_t luid[8]);

// Pick the VkPhysicalDevice whose deviceLUID matches the runtime adapter.
VkPhysicalDevice Win32SelectPhysicalDevice(VkInstance instance);

// True when the adapter needs NT handles (Intel: its driver cannot import KMT handles).
// NT handles cannot share the stencil formats.
bool Win32RequiresNtHandles();

// The Vulkan swapchain formats VDXR offers, in preference order (VDXR swapchain.cpp:247-259).
std::vector<int64_t> Win32SupportedVulkanFormats();

// Signal the shared timeline semaphore on the app's queue after its rendering for this
// frame and return the value; the D3D11 side waits for it before copying. 0 on failure.
uint64_t Win32SerializeVulkanFrame(const VulkanGraphicsContext& context);

// Create imageCount shared D3D11 textures for createInfo and import each into the app's
// VkDevice, transitioned to the attachment layout. vkImages receives the VkImage handles.
// The returned state owns the D3D11 textures, the VkImages, their memory and the eye
// textures; empty on failure (logged).
std::shared_ptr<void> Win32CreateSwapchainImages(const VulkanGraphicsContext& context,
                                                 const XrSwapchainCreateInfo& createInfo,
                                                 uint32_t imageCount,
                                                 std::vector<uint64_t>& vkImages);

// Queue a GPU copy of array slice arrayIndex of image imageIndex into a free eye texture,
// ordered after the last Win32SerializeVulkanFrame. Empty when every eye texture is still
// leased to the encoder, and for formats the encoder does not take (depth, FP16).
FrameImageSource Win32StageSwapchainSlice(const std::shared_ptr<void>& state,
                                          uint32_t imageIndex, uint32_t arrayIndex);

// One eye, copied out of the swapchain on the runtime's D3D11 device; handed to the
// encoder as FrameImageSource.image.
struct Win32EyeImage
{
    ID3D11Texture2D* texture = nullptr; // D3D11_USAGE_DEFAULT, typed 8-bit UNORM
    uint32_t width = 0;
    uint32_t height = 0;
};

// The D3D11 swapchain formats (DXGI_FORMAT values), in preference order.
std::vector<int64_t> Win32SupportedD3D11Formats();

// Check the app's ID3D11Device (hardware adapter, fence support), create the runtime device on
// its adapter and open the shared fence on the app's device. The session holds the result for
// its lifetime; empty on failure (logged).
std::shared_ptr<void> Win32CreateD3D11Interop(const D3D11GraphicsContext& context);

// Signal the shared fence on the app's immediate context, from the thread calling xrEndFrame,
// and queue the matching wait on the runtime's context. 0 on failure.
uint64_t Win32SerializeD3D11Frame(const D3D11GraphicsContext& context);

// Create imageCount shared textures on the runtime device and open each on the app's device.
// textures receives the app-side ID3D11Texture2D pointers, owned by the returned state.
std::shared_ptr<void> Win32CreateD3D11SwapchainImages(const D3D11GraphicsContext& context,
                                                      const XrSwapchainCreateInfo& createInfo,
                                                      uint32_t imageCount, std::vector<void*>& textures);

// The Vulkan app's interop, for its session to hold so the encoder's D3D11 device outlives any one swapchain.
std::shared_ptr<void> Win32CreateVulkanInterop(const VulkanGraphicsContext& context);

// The runtime's D3D11 device (ID3D11Device*) for this session's graphics binding, or null.
void* Win32InteropD3D11Device(const GraphicsContext& context);

#endif
