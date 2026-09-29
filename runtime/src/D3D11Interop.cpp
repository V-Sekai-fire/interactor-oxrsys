// SPDX-License-Identifier: MPL-2.0
//
// Windows Vulkan interop, after mbucchia/VirtualDesktop-OpenXR (MIT,
// Copyright (c) 2022-2024 Matthieu Bucchianeri). See D3D11Interop.h.

#include "D3D11Interop.h"

#if defined(_WIN32) && defined(XR_USE_GRAPHICS_API_VULKAN)

#include "VulkanDispatch.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <vulkan/vulkan_win32.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

using Microsoft::WRL::ComPtr;

const char* const kWin32VulkanInstanceExtensions =
    "VK_KHR_external_memory_capabilities VK_KHR_external_semaphore_capabilities "
    "VK_KHR_external_fence_capabilities VK_KHR_get_physical_device_properties2";
const char* const kWin32VulkanDeviceExtensions =
    "VK_KHR_dedicated_allocation VK_KHR_get_memory_requirements2 VK_KHR_external_memory "
    "VK_KHR_external_memory_win32 VK_KHR_timeline_semaphore VK_KHR_external_semaphore "
    "VK_KHR_external_semaphore_win32";

namespace
{

struct AdapterInfo
{
    ComPtr<IDXGIAdapter1> adapter;
    LUID luid = {};
    UINT vendorId = 0;
};

bool AdapterEncodesAv1(IDXGIAdapter1* adapter)
{
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1};
    ComPtr<ID3D11Device> device;
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 1,
                                 D3D11_SDK_VERSION, &device, nullptr, nullptr)))
    {
        return false;
    }
    return NvencSupportsAv1(device.Get());
}

bool FindRuntimeAdapterUncached(AdapterInfo& out)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        spdlog::error("OXRSys: CreateDXGIFactory1 failed");
        return false;
    }

    // The stream is AV1 from NVENC, so the adapter that renders is the one that can encode
    // it; on this desk that is the RTX 4090, not the 3090 beside it.
    SIZE_T bestMemory = 0;
    bool found = false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }
        if (AdapterEncodesAv1(adapter.Get()))
        {
            out.adapter = adapter;
            out.luid = desc.AdapterLuid;
            out.vendorId = desc.VendorId;
            spdlog::info("OXRSys: runtime adapter {:04x}:{:04x} (NVENC AV1)", desc.VendorId, desc.DeviceId);
            return true;
        }
        if (!found || desc.DedicatedVideoMemory > bestMemory)
        {
            bestMemory = desc.DedicatedVideoMemory;
            out.adapter = adapter;
            out.luid = desc.AdapterLuid;
            out.vendorId = desc.VendorId;
            found = true;
        }
    }
    if (found)
    {
        spdlog::warn("OXRSys: no adapter encodes AV1 with NVENC; frames will render but not stream");
    }
    return found;
}

bool FindRuntimeAdapter(AdapterInfo& out)
{
    // Probing NVENC per adapter costs a device and an encode session each; do it once.
    static std::mutex mutex;
    static bool probed = false;
    static bool found = false;
    static AdapterInfo cached;
    std::scoped_lock lock(mutex);
    if (!probed)
    {
        found = FindRuntimeAdapterUncached(cached);
        probed = true;
    }
    out = cached;
    return found;
}

bool FindAdapterByLuid(const uint8_t luid[VK_LUID_SIZE], AdapterInfo& out)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if (std::memcmp(&desc.AdapterLuid, luid, sizeof(LUID)) == 0)
        {
            out.adapter = adapter;
            out.luid = desc.AdapterLuid;
            out.vendorId = desc.VendorId;
            return true;
        }
    }
    return false;
}

PFN_vkGetPhysicalDeviceProperties2 ResolveGetPhysicalDeviceProperties2(VkInstance instance)
{
    if (gVulkanDispatch.getInstanceProcAddr == nullptr)
    {
        return nullptr;
    }
    auto fn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        gVulkanDispatch.getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
    if (fn == nullptr)
    {
        fn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            gVulkanDispatch.getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2KHR"));
    }
    return fn;
}

bool GetPhysicalDeviceLuid(VkInstance instance, VkPhysicalDevice physicalDevice,
                           uint8_t luid[VK_LUID_SIZE])
{
    auto getProperties2 = ResolveGetPhysicalDeviceProperties2(instance);
    if (getProperties2 == nullptr)
    {
        return false;
    }
    VkPhysicalDeviceIDProperties idProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idProperties};
    getProperties2(physicalDevice, &properties);
    if (!idProperties.deviceLUIDValid)
    {
        return false;
    }
    std::memcpy(luid, idProperties.deviceLUID, VK_LUID_SIZE);
    return true;
}

// VkFormat -> the DXGI TYPELESS storage format; the Vulkan view picks sRGB or UNORM
// (VDXR utils.h:471, :588-611).
DXGI_FORMAT TypelessFormat(VkFormat format)
{
    switch (format)
    {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        case VK_FORMAT_D32_SFLOAT:
            return DXGI_FORMAT_R32_TYPELESS;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return DXGI_FORMAT_R32G8X24_TYPELESS;
        case VK_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24G8_TYPELESS;
        case VK_FORMAT_D16_UNORM:
            return DXGI_FORMAT_R16_TYPELESS;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

bool IsDepthVkFormat(VkFormat format)
{
    return format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D16_UNORM;
}

bool HasStencil(VkFormat format)
{
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
}

struct DeviceFuncs
{
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements2 getImageMemoryRequirements2 = nullptr;
    PFN_vkGetMemoryWin32HandlePropertiesKHR getMemoryWin32HandleProperties = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer resetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    PFN_vkCreateSemaphore createSemaphore = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;
    PFN_vkImportSemaphoreWin32HandleKHR importSemaphoreWin32Handle = nullptr;

    bool Load(VkDevice device)
    {
        PFN_vkGetDeviceProcAddr gdpa = gVulkanDispatch.getDeviceProcAddr;
        if (gdpa == nullptr)
        {
            return false;
        }
        auto get = [&](const char* name, const char* fallback = nullptr) {
            PFN_vkVoidFunction fn = gdpa(device, name);
            if (fn == nullptr && fallback != nullptr)
            {
                fn = gdpa(device, fallback);
            }
            return fn;
        };
#define OXR_LOAD(member, name, ...) member = reinterpret_cast<decltype(member)>(get(name, ##__VA_ARGS__))
        OXR_LOAD(createImage, "vkCreateImage");
        OXR_LOAD(destroyImage, "vkDestroyImage");
        OXR_LOAD(getImageMemoryRequirements2, "vkGetImageMemoryRequirements2",
                 "vkGetImageMemoryRequirements2KHR");
        OXR_LOAD(getMemoryWin32HandleProperties, "vkGetMemoryWin32HandlePropertiesKHR");
        OXR_LOAD(allocateMemory, "vkAllocateMemory");
        OXR_LOAD(freeMemory, "vkFreeMemory");
        OXR_LOAD(bindImageMemory, "vkBindImageMemory");
        OXR_LOAD(getDeviceQueue, "vkGetDeviceQueue");
        OXR_LOAD(createCommandPool, "vkCreateCommandPool");
        OXR_LOAD(destroyCommandPool, "vkDestroyCommandPool");
        OXR_LOAD(allocateCommandBuffers, "vkAllocateCommandBuffers");
        OXR_LOAD(resetCommandBuffer, "vkResetCommandBuffer");
        OXR_LOAD(beginCommandBuffer, "vkBeginCommandBuffer");
        OXR_LOAD(endCommandBuffer, "vkEndCommandBuffer");
        OXR_LOAD(cmdPipelineBarrier, "vkCmdPipelineBarrier");
        OXR_LOAD(queueSubmit, "vkQueueSubmit");
        OXR_LOAD(createFence, "vkCreateFence");
        OXR_LOAD(destroyFence, "vkDestroyFence");
        OXR_LOAD(waitForFences, "vkWaitForFences");
        OXR_LOAD(resetFences, "vkResetFences");
        OXR_LOAD(createSemaphore, "vkCreateSemaphore");
        OXR_LOAD(destroySemaphore, "vkDestroySemaphore");
        OXR_LOAD(importSemaphoreWin32Handle, "vkImportSemaphoreWin32HandleKHR");
#undef OXR_LOAD
        return createImage && destroyImage && getImageMemoryRequirements2 &&
               getMemoryWin32HandleProperties && allocateMemory && freeMemory && bindImageMemory &&
               getDeviceQueue && createCommandPool && destroyCommandPool && allocateCommandBuffers &&
               resetCommandBuffer && beginCommandBuffer && endCommandBuffer && cmdPipelineBarrier &&
               queueSubmit && createFence && destroyFence && waitForFences && resetFences &&
               createSemaphore && destroySemaphore && importSemaphoreWin32Handle;
    }
};

// One per VkDevice: the D3D11 submission device on the app's adapter, the shared fence
// imported as a timeline semaphore, and a command buffer on the app's queue for the
// one-time layout transitions (VDXR vulkan_interop.cpp:424-505).
struct Interop
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    DeviceFuncs vk;

    ComPtr<ID3D11Device5> d3d;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D11Fence> fence;
    HANDLE fenceHandle = nullptr;
    bool ntHandles = false;

    VkSemaphore timeline = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence flushFence = VK_NULL_HANDLE;

    std::mutex mutex; // guards fenceValue and the command buffer
    uint64_t fenceValue = 0;

    ~Interop()
    {
        if (device != VK_NULL_HANDLE)
        {
            if (flushFence != VK_NULL_HANDLE)
            {
                vk.destroyFence(device, flushFence, nullptr);
            }
            if (commandPool != VK_NULL_HANDLE)
            {
                vk.destroyCommandPool(device, commandPool, nullptr);
            }
            if (timeline != VK_NULL_HANDLE)
            {
                vk.destroySemaphore(device, timeline, nullptr);
            }
        }
        if (fenceHandle != nullptr)
        {
            CloseHandle(fenceHandle);
        }
    }

    bool Initialize(const VulkanGraphicsContext& ctx)
    {
        instance = static_cast<VkInstance>(ctx.instance);
        physicalDevice = static_cast<VkPhysicalDevice>(ctx.physicalDevice);
        device = static_cast<VkDevice>(ctx.device);

        if (!vk.Load(device))
        {
            spdlog::error("OXRSys: Vulkan interop functions missing; the app must enable: {}",
                          kWin32VulkanDeviceExtensions);
            return false;
        }

        uint8_t luid[VK_LUID_SIZE] = {};
        AdapterInfo adapter;
        if (!GetPhysicalDeviceLuid(instance, physicalDevice, luid) || !FindAdapterByLuid(luid, adapter))
        {
            spdlog::error("OXRSys: no DXGI adapter matches the Vulkan device LUID");
            return false;
        }
        ntHandles = adapter.vendorId == 0x8086;

        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1};
        ComPtr<ID3D11Device> baseDevice;
        ComPtr<ID3D11DeviceContext> baseContext;
        HRESULT hr = D3D11CreateDevice(adapter.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                       levels, 1, D3D11_SDK_VERSION,
                                       &baseDevice, nullptr, &baseContext);
        if (FAILED(hr) || FAILED(baseDevice.As(&d3d)) || FAILED(baseContext.As(&context)))
        {
            spdlog::error("OXRSys: D3D11 submission device creation failed (0x{:08x})",
                          static_cast<uint32_t>(hr));
            return false;
        }
        // The encoder thread converts and encodes while xrEndFrame queues copies.
        ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(context.As(&multithread)))
        {
            multithread->SetMultithreadProtected(TRUE);
        }

        if (FAILED(d3d->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) ||
            FAILED(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle)))
        {
            spdlog::error("OXRSys: shared D3D11 fence creation failed");
            return false;
        }

        VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &typeInfo};
        if (vk.createSemaphore(device, &semaphoreInfo, nullptr, &timeline) != VK_SUCCESS)
        {
            spdlog::error("OXRSys: timeline semaphore creation failed");
            return false;
        }
        VkImportSemaphoreWin32HandleInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        importInfo.semaphore = timeline;
        importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
        importInfo.handle = fenceHandle;
        if (vk.importSemaphoreWin32Handle(device, &importInfo) != VK_SUCCESS)
        {
            spdlog::error("OXRSys: importing the D3D11 fence as a timeline semaphore failed");
            return false;
        }

        vk.getDeviceQueue(device, ctx.queueFamilyIndex, ctx.queueIndex, &queue);

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = ctx.queueFamilyIndex;
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vk.createCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS ||
            (allocInfo.commandPool = commandPool,
             vk.allocateCommandBuffers(device, &allocInfo, &commandBuffer) != VK_SUCCESS) ||
            vk.createFence(device, &fenceInfo, nullptr, &flushFence) != VK_SUCCESS)
        {
            spdlog::error("OXRSys: interop command buffer creation failed");
            return false;
        }

        DXGI_ADAPTER_DESC1 desc = {};
        adapter.adapter->GetDesc1(&desc);
        spdlog::info("OXRSys: D3D11 interop on adapter {:04x}:{:04x} ({} handles)", desc.VendorId,
                     desc.DeviceId, ntHandles ? "NT" : "KMT");
        return true;
    }

    // One-shot submit on the app's queue, waited on the CPU (swapchain creation only).
    bool SubmitAndWait(VkCommandBuffer cb)
    {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        if (vk.queueSubmit(queue, 1, &submit, flushFence) != VK_SUCCESS)
        {
            return false;
        }
        VkResult result = vk.waitForFences(device, 1, &flushFence, VK_TRUE, 1'000'000'000ull);
        vk.resetFences(device, 1, &flushFence);
        return result == VK_SUCCESS;
    }
};

std::mutex gInteropMutex;
std::shared_ptr<Interop> gInterop;

std::shared_ptr<Interop> GetInterop(const VulkanGraphicsContext& ctx)
{
    std::scoped_lock lock(gInteropMutex);
    if (gInterop && gInterop->device == static_cast<VkDevice>(ctx.device))
    {
        return gInterop;
    }
    auto interop = std::make_shared<Interop>();
    if (!interop->Initialize(ctx))
    {
        return nullptr;
    }
    gInterop = interop;
    return gInterop;
}

constexpr uint32_t kEyeSlotsPerSlice = 3;

struct EyeSlot
{
    ComPtr<ID3D11Texture2D> texture;
    std::shared_ptr<std::atomic_bool> inUse = std::make_shared<std::atomic_bool>(false);
};

// The typed format an eye is copied into; the copy reinterprets the typeless storage, so
// sRGB-encoded bytes arrive as they are, which is what the video path wants.
DXGI_FORMAT EyeFormat(VkFormat format)
{
    switch (format)
    {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN; // FP16 needs a tone-mapping pass; depth is never shown
    }
}

struct SwapchainState
{
    std::shared_ptr<Interop> interop;
    VkFormat format = VK_FORMAT_UNDEFINED;
    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 1;
    uint32_t arraySize = 1;
    std::vector<ComPtr<ID3D11Texture2D>> textures;
    std::vector<HANDLE> ntHandles;
    std::vector<VkImage> images;
    std::vector<VkDeviceMemory> memories;
    std::vector<std::array<EyeSlot, kEyeSlotsPerSlice>> eyes; // per array slice
    std::mutex eyesMutex;

    ~SwapchainState()
    {
        if (interop)
        {
            for (VkImage image : images)
            {
                if (image != VK_NULL_HANDLE)
                {
                    interop->vk.destroyImage(interop->device, image, nullptr);
                }
            }
            for (VkDeviceMemory memory : memories)
            {
                if (memory != VK_NULL_HANDLE)
                {
                    interop->vk.freeMemory(interop->device, memory, nullptr);
                }
            }
        }
        for (HANDLE handle : ntHandles)
        {
            if (handle != nullptr)
            {
                CloseHandle(handle);
            }
        }
    }
};

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags flags,
                        bool& found)
{
    found = false;
    if (gVulkanDispatch.getPhysicalDeviceMemoryProperties == nullptr)
    {
        return 0;
    }
    VkPhysicalDeviceMemoryProperties properties = {};
    gVulkanDispatch.getPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) != 0 && (properties.memoryTypes[i].propertyFlags & flags) == flags)
        {
            found = true;
            return i;
        }
    }
    return 0;
}

bool ImportImage(SwapchainState& state, uint32_t index, HANDLE handle,
                 VkExternalMemoryHandleTypeFlagBits handleType, const XrSwapchainCreateInfo& createInfo)
{
    Interop& interop = *state.interop;

    VkExternalMemoryImageCreateInfo externalInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    externalInfo.handleTypes = handleType;

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &externalInfo};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = state.format;
    imageInfo.extent = {state.width, state.height, 1};
    imageInfo.mipLevels = state.mipCount;
    imageInfo.arrayLayers = state.arraySize;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // VDXR swapchain usage mapping (vulkan_interop.cpp:696-716), with MUTABLE_FORMAT in
    // .flags where it belongs.
    const XrSwapchainUsageFlags usage = createInfo.usageFlags;
    if (usage & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)
        imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
        imageInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_SAMPLED_BIT)
        imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)
        imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT)
        imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)
        imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT)
        imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (imageInfo.usage == 0)
    {
        imageInfo.usage = IsDepthVkFormat(state.format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                        : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }

    VkImage image = VK_NULL_HANDLE;
    if (interop.vk.createImage(interop.device, &imageInfo, nullptr, &image) != VK_SUCCESS)
    {
        spdlog::error("OXRSys: vkCreateImage for a shared D3D11 texture failed");
        return false;
    }
    state.images[index] = image;

    VkImageMemoryRequirementsInfo2 requirementInfo{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    requirementInfo.image = image;
    VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    interop.vk.getImageMemoryRequirements2(interop.device, &requirementInfo, &requirements);

    VkMemoryWin32HandlePropertiesKHR handleProperties{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    if (interop.vk.getMemoryWin32HandleProperties(interop.device, handleType, handle, &handleProperties) !=
        VK_SUCCESS)
    {
        spdlog::error("OXRSys: vkGetMemoryWin32HandlePropertiesKHR failed");
        return false;
    }

    bool found = false;
    uint32_t memoryType = FindMemoryType(interop.physicalDevice,
                                         handleProperties.memoryTypeBits & requirements.memoryRequirements.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, found);
    if (!found)
    {
        memoryType = FindMemoryType(interop.physicalDevice, handleProperties.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, found);
    }
    if (!found)
    {
        spdlog::error("OXRSys: no device-local memory type accepts the shared texture");
        return false;
    }

    VkImportMemoryWin32HandleInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    importInfo.handleType = handleType;
    importInfo.handle = handle;
    VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &importInfo};
    dedicatedInfo.image = image;
    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicatedInfo};
    allocateInfo.allocationSize = requirements.memoryRequirements.size;
    allocateInfo.memoryTypeIndex = memoryType;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (interop.vk.allocateMemory(interop.device, &allocateInfo, nullptr, &memory) != VK_SUCCESS)
    {
        spdlog::error("OXRSys: importing the shared texture memory failed");
        return false;
    }
    state.memories[index] = memory;
    if (interop.vk.bindImageMemory(interop.device, image, memory, 0) != VK_SUCCESS)
    {
        spdlog::error("OXRSys: vkBindImageMemory for the shared texture failed");
        return false;
    }
    return true;
}

// UNDEFINED -> the attachment layout the app receives images in (VDXR vulkan_interop.cpp:760-807).
bool TransitionImages(SwapchainState& state)
{
    Interop& interop = *state.interop;
    const bool depth = IsDepthVkFormat(state.format);

    std::scoped_lock lock(interop.mutex);
    VkCommandBuffer cb = interop.commandBuffer;
    interop.vk.resetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    interop.vk.beginCommandBuffer(cb, &beginInfo);

    std::vector<VkImageMemoryBarrier> barriers;
    for (VkImage image : state.images)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                      : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                  : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask =
            depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(state.format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                  : VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = state.mipCount;
        barrier.subresourceRange.layerCount = state.arraySize;
        barriers.push_back(barrier);
    }
    interop.vk.cmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0,
                                  0, nullptr, 0, nullptr, static_cast<uint32_t>(barriers.size()),
                                  barriers.data());
    interop.vk.endCommandBuffer(cb);
    return interop.SubmitAndWait(cb);
}

} // namespace

bool Win32GetRuntimeAdapterLuid(uint8_t luid[8])
{
    AdapterInfo adapter;
    if (!FindRuntimeAdapter(adapter))
    {
        return false;
    }
    std::memcpy(luid, &adapter.luid, sizeof(LUID));
    return true;
}

bool Win32RequiresNtHandles()
{
    AdapterInfo adapter;
    return FindRuntimeAdapter(adapter) && adapter.vendorId == 0x8086;
}

VkPhysicalDevice Win32SelectPhysicalDevice(VkInstance instance)
{
    if (gVulkanDispatch.enumeratePhysicalDevices == nullptr)
    {
        return VK_NULL_HANDLE;
    }
    uint8_t runtimeLuid[VK_LUID_SIZE] = {};
    if (!Win32GetRuntimeAdapterLuid(runtimeLuid))
    {
        spdlog::error("OXRSys: no hardware DXGI adapter found");
        return VK_NULL_HANDLE;
    }

    uint32_t count = 0;
    gVulkanDispatch.enumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    gVulkanDispatch.enumeratePhysicalDevices(instance, &count, devices.data());
    for (VkPhysicalDevice device : devices)
    {
        uint8_t luid[VK_LUID_SIZE] = {};
        if (GetPhysicalDeviceLuid(instance, device, luid) &&
            std::memcmp(luid, runtimeLuid, VK_LUID_SIZE) == 0)
        {
            return device;
        }
    }
    spdlog::error("OXRSys: no Vulkan physical device matches the runtime adapter LUID");
    return VK_NULL_HANDLE;
}

std::vector<int64_t> Win32SupportedVulkanFormats()
{
    std::vector<int64_t> formats = {
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_D32_SFLOAT,
    };
    // Stencil formats are not shareable through NT handles (VDXR swapchain.cpp:252-256).
    if (!Win32RequiresNtHandles())
    {
        formats.push_back(VK_FORMAT_D32_SFLOAT_S8_UINT);
        formats.push_back(VK_FORMAT_D24_UNORM_S8_UINT);
    }
    formats.push_back(VK_FORMAT_D16_UNORM);
    return formats;
}

uint64_t Win32SerializeVulkanFrame(const VulkanGraphicsContext& ctx)
{
    std::shared_ptr<Interop> interop = GetInterop(ctx);
    if (!interop)
    {
        return 0;
    }

    std::scoped_lock lock(interop->mutex);
    const uint64_t value = interop->fenceValue + 1;
    VkTimelineSemaphoreSubmitInfo timelineInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &value;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timelineInfo};
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &interop->timeline;
    if (interop->vk.queueSubmit(interop->queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
    {
        spdlog::error("OXRSys: signalling the frame timeline semaphore failed");
        return 0;
    }
    interop->fenceValue = value;
    // GPU-side wait: copies queued on the D3D11 context after this run after the app's frame.
    interop->context->Wait(interop->fence.Get(), value);
    return value;
}

std::shared_ptr<void> Win32CreateSwapchainImages(const VulkanGraphicsContext& ctx,
                                                 const XrSwapchainCreateInfo& createInfo,
                                                 uint32_t imageCount, std::vector<uint64_t>& vkImages)
{
    auto state = std::make_shared<SwapchainState>();
    state->interop = GetInterop(ctx);
    if (!state->interop)
    {
        return nullptr;
    }
    Interop& interop = *state->interop;

    state->format = static_cast<VkFormat>(createInfo.format);
    state->typeless = TypelessFormat(state->format);
    state->width = createInfo.width;
    state->height = createInfo.height;
    state->mipCount = std::max(createInfo.mipCount, 1u);
    state->arraySize = std::max(createInfo.arraySize, 1u);

    if (state->typeless == DXGI_FORMAT_UNKNOWN ||
        (interop.ntHandles && HasStencil(state->format)))
    {
        spdlog::error("OXRSys: swapchain format {} is not supported", createInfo.format);
        return nullptr;
    }
    if (createInfo.sampleCount > 1 || createInfo.faceCount > 1)
    {
        spdlog::error("OXRSys: swapchain sampleCount {} / faceCount {} is not supported",
                      createInfo.sampleCount, createInfo.faceCount);
        return nullptr;
    }

    const bool depth = IsDepthVkFormat(state->format);
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = state->width;
    desc.Height = state->height;
    desc.MipLevels = state->mipCount;
    desc.ArraySize = state->arraySize;
    desc.Format = state->typeless;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                     (depth ? D3D11_BIND_DEPTH_STENCIL : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS));
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | (interop.ntHandles ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE : 0);

    const VkExternalMemoryHandleTypeFlagBits handleType =
        interop.ntHandles ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT
                          : VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

    state->textures.resize(imageCount);
    state->ntHandles.resize(imageCount, nullptr);
    state->images.resize(imageCount, VK_NULL_HANDLE);
    state->memories.resize(imageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < imageCount; ++i)
    {
        HRESULT hr = interop.d3d->CreateTexture2D(&desc, nullptr, &state->textures[i]);
        if (FAILED(hr))
        {
            spdlog::error("OXRSys: CreateTexture2D for a shared swapchain image failed (0x{:08x})",
                          static_cast<uint32_t>(hr));
            return nullptr;
        }

        HANDLE handle = nullptr;
        if (interop.ntHandles)
        {
            ComPtr<IDXGIResource1> resource;
            state->textures[i].As(&resource);
            hr = resource ? resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle) : E_FAIL;
            state->ntHandles[i] = handle;
        }
        else
        {
            ComPtr<IDXGIResource> resource;
            state->textures[i].As(&resource);
            hr = resource ? resource->GetSharedHandle(&handle) : E_FAIL;
        }
        if (FAILED(hr) || handle == nullptr)
        {
            spdlog::error("OXRSys: sharing a swapchain image failed (0x{:08x})", static_cast<uint32_t>(hr));
            return nullptr;
        }

        if (!ImportImage(*state, i, handle, handleType, createInfo))
        {
            return nullptr;
        }
    }

    if (!TransitionImages(*state))
    {
        spdlog::error("OXRSys: swapchain layout transition failed");
        return nullptr;
    }

    if (EyeFormat(state->format) != DXGI_FORMAT_UNKNOWN)
    {
        state->eyes.resize(state->arraySize);
    }

    vkImages.assign(imageCount, 0);
    for (uint32_t i = 0; i < imageCount; ++i)
    {
        vkImages[i] = reinterpret_cast<uint64_t>(state->images[i]);
    }
    return state;
}

FrameImageSource Win32StageSwapchainSlice(const std::shared_ptr<void>& opaqueState, uint32_t imageIndex,
                                          uint32_t arrayIndex)
{
    auto state = std::static_pointer_cast<SwapchainState>(opaqueState);
    if (!state || arrayIndex >= state->eyes.size() || imageIndex >= state->textures.size())
    {
        return {};
    }
    Interop& interop = *state->interop;

    EyeSlot* slot = nullptr;
    {
        std::scoped_lock lock(state->eyesMutex);
        for (EyeSlot& candidate : state->eyes[arrayIndex])
        {
            bool expected = false;
            if (candidate.inUse->compare_exchange_strong(expected, true))
            {
                slot = &candidate;
                break;
            }
        }
    }
    if (slot == nullptr)
    {
        return {}; // the encoder still holds every eye texture; skip this frame
    }

    if (!slot->texture)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = state->width;
        desc.Height = state->height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = EyeFormat(state->format);
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; // video processor input
        if (FAILED(interop.d3d->CreateTexture2D(&desc, nullptr, &slot->texture)))
        {
            slot->inUse->store(false);
            spdlog::error("OXRSys: eye texture creation failed");
            return {};
        }
    }

    // Runs on the GPU after the Wait queued by Win32SerializeVulkanFrame.
    interop.context->CopySubresourceRegion(slot->texture.Get(), 0, 0, 0, 0, state->textures[imageIndex].Get(),
                                           D3D11CalcSubresource(0, arrayIndex, state->mipCount), nullptr);

    auto* eye = new Win32EyeImage{};
    eye->texture = slot->texture.Get();
    eye->texture->AddRef();
    eye->width = state->width;
    eye->height = state->height;

    FrameImageSource source = {};
    source.api = GraphicsApi::Vulkan;
    std::shared_ptr<std::atomic_bool> inUse = slot->inUse;
    source.image = std::shared_ptr<void>(eye, [inUse](void* ptr) {
        auto* image = static_cast<Win32EyeImage*>(ptr);
        image->texture->Release();
        delete image;
        inUse->store(false);
    });
    return source;
}

void* Win32InteropD3D11Device(const VulkanGraphicsContext& context)
{
    std::shared_ptr<Interop> interop = GetInterop(context);
    return interop ? interop->d3d.Get() : nullptr;
}

#endif
