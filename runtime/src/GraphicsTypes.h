// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <memory>

enum class GraphicsApi
{
    Metal,
    Vulkan,
    D3D11,
};

struct VulkanGraphicsContext
{
    void* instance = nullptr;
    void* physicalDevice = nullptr;
    void* device = nullptr;
    uint32_t queueFamilyIndex = 0;
    uint32_t queueIndex = 0;
};

struct D3D11GraphicsContext
{
    void* device = nullptr; // the app's ID3D11Device
    bool encoderDevice = false; // device is the runtime's own, used by the encoder as is
};

struct GraphicsContext
{
    GraphicsApi api = GraphicsApi::Metal;
    void* metalDevice = nullptr;
    void* metalCommandQueue = nullptr;
    VulkanGraphicsContext vulkan = {};
    D3D11GraphicsContext d3d11 = {};

    static GraphicsContext Metal(void* device, void* commandQueue = nullptr)
    {
        GraphicsContext context = {};
        context.api = GraphicsApi::Metal;
        context.metalDevice = device;
        context.metalCommandQueue = commandQueue;
        return context;
    }

    static GraphicsContext Vulkan(const VulkanGraphicsContext& vulkanContext,
                                  void* debugMetalDevice = nullptr)
    {
        GraphicsContext context = {};
        context.api = GraphicsApi::Vulkan;
        context.metalDevice = debugMetalDevice;
        context.vulkan = vulkanContext;
        return context;
    }

    static GraphicsContext D3D11(const D3D11GraphicsContext& d3d11Context)
    {
        GraphicsContext context = {};
        context.api = GraphicsApi::D3D11;
        context.d3d11 = d3d11Context;
        return context;
    }
};

struct FrameSyncToken
{
    GraphicsApi api = GraphicsApi::Metal;
    std::shared_ptr<void> waitObject = {};
    uint64_t waitValue = 0;

    bool IsValid() const
    {
        return waitObject != nullptr && waitValue != 0;
    }
};

struct FrameImageSource
{
    GraphicsApi api = GraphicsApi::Metal;
    std::shared_ptr<void> image = {};
    FrameSyncToken sync = {};
    std::shared_ptr<void> lifetime = {};

    void* GetImage() const
    {
        return image.get();
    }

    bool IsValid() const
    {
        return image != nullptr;
    }

    void Reset()
    {
        image.reset();
        sync = {};
        lifetime.reset();
    }
};

struct FrameSource
{
    FrameImageSource left = {};
    FrameImageSource right = {};
    bool alphaBlend = false;

    bool IsStereoValid() const
    {
        return left.IsValid() && right.IsValid();
    }

    void Reset()
    {
        left.Reset();
        right.Reset();
        alphaBlend = false;
    }
};
