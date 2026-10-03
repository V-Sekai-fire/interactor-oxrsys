// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{

constexpr const char* kVulkanEnable2 = "XR_KHR_vulkan_enable2";

void CheckXr(XrResult result, const char* expression)
{
    INFO(expression << " returned " << static_cast<int>(result));
    REQUIRE(result == XR_SUCCESS);
}

#define XR_CHECK(expr) CheckXr((expr), #expr)

struct TestInstance
{
    explicit TestInstance(std::vector<const char*> extensions)
    {
        XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
        std::strncpy(createInfo.applicationInfo.applicationName, "oxrsys_runtime_d3d11_tests",
                     XR_MAX_APPLICATION_NAME_SIZE);
        createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.enabledExtensionNames = extensions.data();
        XR_CHECK(xrCreateInstance(&createInfo, &instance));

        XrSystemGetInfo systemGetInfo = {XR_TYPE_SYSTEM_GET_INFO};
        systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XR_CHECK(xrGetSystem(instance, &systemGetInfo, &systemId));
    }

    ~TestInstance()
    {
        if (session != XR_NULL_HANDLE)
        {
            xrDestroySession(session);
        }
        xrDestroyInstance(instance);
    }

    XrGraphicsRequirementsD3D11KHR Requirements() const
    {
        PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
        XR_CHECK(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
                                       reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)));
        XrGraphicsRequirementsD3D11KHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        XR_CHECK(getRequirements(instance, systemId, &requirements));
        return requirements;
    }

    XrResult CreateSession(ID3D11Device* device)
    {
        XrGraphicsBindingD3D11KHR binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = device;
        XrSessionCreateInfo createInfo = {XR_TYPE_SESSION_CREATE_INFO};
        createInfo.next = &binding;
        createInfo.systemId = systemId;
        return xrCreateSession(instance, &createInfo, &session);
    }

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
};

bool SameLuid(const LUID& a, const LUID& b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

ComPtr<ID3D11Device> CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType)
{
    ComPtr<ID3D11Device> device;
    HRESULT hr = D3D11CreateDevice(adapter, driverType, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, nullptr, nullptr);
    REQUIRE(SUCCEEDED(hr));
    return device;
}

ComPtr<ID3D11Device> CreateDeviceOnLuid(const LUID& luid)
{
    ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter1> adapter;
    REQUIRE(SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))));
    return CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN);
}

ComPtr<ID3D11Device> CreateWarpDevice()
{
    return CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP);
}

std::vector<int64_t> EnumerateFormats(XrSession session)
{
    uint32_t count = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &count, nullptr));
    std::vector<int64_t> formats(count);
    XR_CHECK(xrEnumerateSwapchainFormats(session, count, &count, formats.data()));
    return formats;
}

XrSwapchainCreateInfo SwapchainInfo(int64_t format, XrSwapchainUsageFlags usage)
{
    XrSwapchainCreateInfo createInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = usage;
    createInfo.format = format;
    createInfo.sampleCount = 1;
    createInfo.width = 640;
    createInfo.height = 480;
    createInfo.faceCount = 1;
    createInfo.arraySize = 2;
    createInfo.mipCount = 1;
    return createInfo;
}

std::vector<XrSwapchainImageD3D11KHR> EnumerateImages(XrSwapchain swapchain)
{
    uint32_t count = 0;
    XR_CHECK(xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr));
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(swapchain, count, &count,
                                        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
    return images;
}

} // namespace

TEST_CASE("D3D11 enable is advertised and gated by the instance", "[runtime][d3d11]")
{
    uint32_t count = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
    std::vector<XrExtensionProperties> properties(count, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, properties.data()));
    const bool advertised = std::any_of(properties.begin(), properties.end(), [](const XrExtensionProperties& p) {
        return std::strcmp(p.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
    });
    CHECK(advertised);

    // Negative control: without the extension the entry point is not handed out.
    TestInstance vulkanOnly({kVulkanEnable2});
    PFN_xrVoidFunction function = nullptr;
    CHECK(xrGetInstanceProcAddr(vulkanOnly.instance, "xrGetD3D11GraphicsRequirementsKHR", &function) ==
          XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(function == nullptr);
}

TEST_CASE("D3D11 graphics requirements name the runtime's hardware adapter", "[runtime][d3d11]")
{
    TestInstance instance({XR_KHR_D3D11_ENABLE_EXTENSION_NAME});
    const XrGraphicsRequirementsD3D11KHR requirements = instance.Requirements();
    CHECK(requirements.minFeatureLevel <= D3D_FEATURE_LEVEL_11_1);

    ComPtr<IDXGIFactory6> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    LUID expected = {};
    bool found = false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; !found && SUCCEEDED(factory->EnumAdapterByGpuPreference(
                                   i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
         ++i)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
        {
            expected = desc.AdapterLuid;
            found = true;
        }
    }
    REQUIRE(found);
    CHECK(SameLuid(requirements.adapterLuid, expected));

    // Negative control: the comparison tells adapters apart.
    ComPtr<IDXGIAdapter1> warp;
    REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    DXGI_ADAPTER_DESC1 warpDesc = {};
    warp->GetDesc1(&warpDesc);
    CHECK_FALSE(SameLuid(requirements.adapterLuid, warpDesc.AdapterLuid));
}

TEST_CASE("D3D11 session hands out shared textures on the app's device", "[runtime][d3d11][swapchain]")
{
    TestInstance instance({XR_KHR_D3D11_ENABLE_EXTENSION_NAME});
    ComPtr<ID3D11Device> device = CreateDeviceOnLuid(instance.Requirements().adapterLuid);
    XR_CHECK(instance.CreateSession(device.Get()));

    const std::vector<int64_t> formats = EnumerateFormats(instance.session);
    REQUIRE(!formats.empty());
    CHECK(formats.front() == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    for (int64_t format : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                           DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D16_UNORM})
    {
        INFO("format " << format);
        CHECK(std::find(formats.begin(), formats.end(), format) != formats.end());
    }

    XrSwapchainCreateInfo colorInfo = SwapchainInfo(
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
    XrSwapchain color = XR_NULL_HANDLE;
    XR_CHECK(xrCreateSwapchain(instance.session, &colorInfo, &color));
    const std::vector<XrSwapchainImageD3D11KHR> colorImages = EnumerateImages(color);
    REQUIRE(colorImages.size() == 3);
    for (const XrSwapchainImageD3D11KHR& image : colorImages)
    {
        REQUIRE(image.texture != nullptr);
        D3D11_TEXTURE2D_DESC desc = {};
        image.texture->GetDesc(&desc);
        CHECK(desc.Width == 640);
        CHECK(desc.Height == 480);
        CHECK(desc.ArraySize == 2);
        CHECK(desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS);
        CHECK((desc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0);
        ComPtr<ID3D11Device> owner;
        image.texture->GetDevice(&owner);
        CHECK(owner.Get() == device.Get());

        // An app renders into a slice through an sRGB view of the typeless texture.
        D3D11_RENDER_TARGET_VIEW_DESC viewDesc = {};
        viewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        viewDesc.Texture2DArray.FirstArraySlice = 1;
        viewDesc.Texture2DArray.ArraySize = 1;
        ComPtr<ID3D11RenderTargetView> view;
        CHECK(SUCCEEDED(device->CreateRenderTargetView(image.texture, &viewDesc, &view)));
    }

    XrSwapchainCreateInfo depthInfo =
        SwapchainInfo(DXGI_FORMAT_D32_FLOAT, XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    XrSwapchain depth = XR_NULL_HANDLE;
    XR_CHECK(xrCreateSwapchain(instance.session, &depthInfo, &depth));
    const std::vector<XrSwapchainImageD3D11KHR> depthImages = EnumerateImages(depth);
    REQUIRE(depthImages.size() == 3);
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthImages[0].texture->GetDesc(&depthDesc);
    CHECK(depthDesc.ArraySize == 2);
    CHECK((depthDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0);

    // Negative control: a format outside the enumerated list is refused.
    XrSwapchainCreateInfo badInfo =
        SwapchainInfo(DXGI_FORMAT_R32G32B32_FLOAT, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
    XrSwapchain bad = XR_NULL_HANDLE;
    CHECK(xrCreateSwapchain(instance.session, &badInfo, &bad) == XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);

    XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XR_CHECK(xrBeginSession(instance.session, &beginInfo));
    XrReferenceSpaceCreateInfo spaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space = XR_NULL_HANDLE;
    XR_CHECK(xrCreateReferenceSpace(instance.session, &spaceInfo, &space));

    XrFrameState frameState = {XR_TYPE_FRAME_STATE};
    XR_CHECK(xrWaitFrame(instance.session, nullptr, &frameState));
    const XrResult beginResult = xrBeginFrame(instance.session, nullptr);
    REQUIRE((beginResult == XR_SUCCESS || beginResult == XR_FRAME_DISCARDED));

    uint32_t index = 0;
    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    XR_CHECK(xrAcquireSwapchainImage(color, nullptr, &index));
    XR_CHECK(xrWaitSwapchainImage(color, &waitInfo));
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    D3D11_RENDER_TARGET_VIEW_DESC viewDesc = {};
    viewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    viewDesc.Texture2DArray.ArraySize = 2;
    ComPtr<ID3D11RenderTargetView> view;
    REQUIRE(SUCCEEDED(device->CreateRenderTargetView(colorImages[index].texture, &viewDesc, &view)));
    const float clear[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    context->ClearRenderTargetView(view.Get(), clear);
    XR_CHECK(xrReleaseSwapchainImage(color, nullptr));

    XrCompositionLayerProjectionView views[2] = {};
    for (uint32_t eye = 0; eye < 2; ++eye)
    {
        views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        views[eye].pose.orientation.w = 1.0f;
        views[eye].fov = {-0.5f, 0.5f, 0.5f, -0.5f};
        views[eye].subImage.swapchain = color;
        views[eye].subImage.imageRect.extent = {640, 480};
        views[eye].subImage.imageArrayIndex = eye;
    }
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = space;
    layer.viewCount = 2;
    layer.views = views;
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = 1;
    endInfo.layers = layers;
    CHECK(xrEndFrame(instance.session, &endInfo) == XR_SUCCESS);

    XR_CHECK(xrDestroySpace(space));
    XR_CHECK(xrDestroySwapchain(depth));
    XR_CHECK(xrDestroySwapchain(color));
}

TEST_CASE("D3D11 session creation rejects invalid bindings", "[runtime][d3d11]")
{
    SECTION("a null device")
    {
        TestInstance instance({XR_KHR_D3D11_ENABLE_EXTENSION_NAME});
        instance.Requirements();
        CHECK(instance.CreateSession(nullptr) == XR_ERROR_GRAPHICS_DEVICE_INVALID);
        CHECK(instance.session == XR_NULL_HANDLE);
    }
    SECTION("a software adapter's device")
    {
        TestInstance instance({XR_KHR_D3D11_ENABLE_EXTENSION_NAME});
        instance.Requirements();
        ComPtr<ID3D11Device> warp = CreateWarpDevice();
        CHECK(instance.CreateSession(warp.Get()) == XR_ERROR_GRAPHICS_DEVICE_INVALID);
        CHECK(instance.session == XR_NULL_HANDLE);
    }
    SECTION("no graphics requirements call")
    {
        TestInstance instance({XR_KHR_D3D11_ENABLE_EXTENSION_NAME});
        ComPtr<ID3D11Device> device = CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE);
        CHECK(instance.CreateSession(device.Get()) == XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING);
        CHECK(instance.session == XR_NULL_HANDLE);
    }
    SECTION("an instance that only enabled Vulkan")
    {
        TestInstance instance({kVulkanEnable2});
        ComPtr<ID3D11Device> device = CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE);
        CHECK(instance.CreateSession(device.Get()) == XR_ERROR_VALIDATION_FAILURE);
        CHECK(instance.session == XR_NULL_HANDLE);
    }
}
