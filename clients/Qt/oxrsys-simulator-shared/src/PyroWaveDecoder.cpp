// SPDX-License-Identifier: MPL-2.0

#include "PyroWaveDecoder.h"

#include <volk.h>
#include <pyrowave.h>

#include <QCoreApplication>
#include <QPointer>
#include <QVulkanInstance>
#include <QWidget>
#include <QWindow>

#include "yuv420_to_rgbx_spv.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

uint32_t PlaneWidth(int plane, int width)
{
    return uint32_t(plane == 0 ? width : width / 2);
}

uint32_t PlaneHeight(int plane, int height)
{
    return uint32_t(plane == 0 ? height : height / 2);
}

class PyroWaveView final : public QWindow
{
public:
    PyroWaveView(QVulkanInstance* instance, QWidget* inputTarget)
        : inputTarget_(inputTarget)
    {
        setSurfaceType(QSurface::VulkanSurface);
        setVulkanInstance(instance);
        setFlag(Qt::WindowDoesNotAcceptFocus);
    }

protected:
    bool event(QEvent* event) override
    {
        switch (event->type())
        {
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonRelease:
            case QEvent::MouseButtonDblClick:
            case QEvent::MouseMove:
            case QEvent::Wheel:
            case QEvent::KeyPress:
            case QEvent::KeyRelease:
                if (inputTarget_ != nullptr)
                {
                    return QCoreApplication::sendEvent(inputTarget_, event);
                }
                break;
            default:
                break;
        }
        return QWindow::event(event);
    }

private:
    QPointer<QWidget> inputTarget_;
};

} // namespace

struct PyroWaveDecoder::Gpu
{
    QVulkanInstance instance;
    std::vector<QByteArray> extensionNames;
    std::vector<const char*> extensionPointers;
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    const char* deviceExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};

    pyrowave_device pyro = nullptr;
    pyrowave_decoder decoder = nullptr;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties = {};

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;

    int width = 0;
    int height = 0;
    Image planes[3];
    Buffer packed[3]; // plane bytes, four to a word, as the kernel reads them
    Buffer params;
    Buffer rgbx;
    Image frame; // RGBA8, the blit source; left in TRANSFER_SRC_OPTIMAL
    bool hasFrame = false;

    QPointer<QWindow> view;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D swapExtent = {};
    std::vector<VkImage> swapImages;
    std::vector<VkSemaphore> rendered;
    VkSemaphore acquired = VK_NULL_HANDLE;

    ~Gpu()
    {
        releaseFrame();
        if (device != VK_NULL_HANDLE)
        {
            releaseSwapchain();
            vkDestroySemaphore(device, acquired, nullptr);
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            vkDestroyPipeline(device, pipeline, nullptr);
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
            vkDestroyFence(device, fence, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
        }
        if (pyro != nullptr)
        {
            pyrowave_device_destroy(pyro);
        }
        if (device != VK_NULL_HANDLE)
        {
            vkDestroyDevice(device, nullptr);
        }
        // The surface belongs to the platform window, which must go before the instance.
        if (view != nullptr)
        {
            view->destroy();
        }
    }

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const
    {
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        {
            if ((bits & (1u << i)) != 0 && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags)
            {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags flags, VkDeviceMemory& memory)
    {
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize = requirements.size;
        info.memoryTypeIndex = memoryType(requirements.memoryTypeBits, flags);
        return info.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(device, &info, nullptr, &memory) == VK_SUCCESS;
    }

    bool makeBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags)
    {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        if (vkCreateBuffer(device, &info, nullptr, &out.buffer) != VK_SUCCESS)
        {
            return false;
        }
        VkMemoryRequirements requirements = {};
        vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
        if (!allocate(requirements, flags, out.memory) ||
            vkBindBufferMemory(device, out.buffer, out.memory, 0) != VK_SUCCESS)
        {
            releaseBuffer(out);
            return false;
        }
        return (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 ||
               vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) == VK_SUCCESS;
    }

    bool makeImage(Image& out, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage)
    {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {w, h, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        if (vkCreateImage(device, &info, nullptr, &out.image) != VK_SUCCESS)
        {
            return false;
        }
        VkMemoryRequirements requirements = {};
        vkGetImageMemoryRequirements(device, out.image, &requirements);
        return allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out.memory) &&
               vkBindImageMemory(device, out.image, out.memory, 0) == VK_SUCCESS;
    }

    void releaseBuffer(Buffer& b)
    {
        vkDestroyBuffer(device, b.buffer, nullptr);
        vkFreeMemory(device, b.memory, nullptr);
        b = {};
    }

    void releaseImage(Image& image)
    {
        vkDestroyImage(device, image.image, nullptr);
        vkFreeMemory(device, image.memory, nullptr);
        image = {};
    }

    void releaseFrame()
    {
        if (decoder != nullptr)
        {
            pyrowave_decoder_destroy(decoder);
            decoder = nullptr;
        }
        if (device == VK_NULL_HANDLE)
        {
            return;
        }
        vkDeviceWaitIdle(device);
        for (Image& image : planes)
        {
            releaseImage(image);
        }
        for (Buffer& b : packed)
        {
            releaseBuffer(b);
        }
        releaseBuffer(params);
        releaseBuffer(rgbx);
        releaseImage(frame);
        hasFrame = false;
        width = 0;
        height = 0;
    }

    void releaseSwapchain()
    {
        vkDeviceWaitIdle(device);
        for (VkSemaphore semaphore : rendered)
        {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
        rendered.clear();
        swapImages.clear();
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
        swapExtent = {};
    }

    bool createDevice()
    {
        instance.setApiVersion(QVersionNumber(1, 3));
        if (volkInitialize() != VK_SUCCESS || !instance.create())
        {
            return false;
        }
        volkLoadInstanceOnly(instance.vkInstance());

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance.vkInstance(), &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance.vkInstance(), &count, devices.data());
        for (VkPhysicalDevice candidate : devices)
        {
            VkPhysicalDeviceProperties properties = {};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion >= VK_API_VERSION_1_3 &&
                (physical == VK_NULL_HANDLE || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))
            {
                physical = candidate;
                if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                {
                    break;
                }
            }
        }
        if (physical == VK_NULL_HANDLE)
        {
            return false;
        }
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);

        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        while (family < count && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
        {
            ++family;
        }
        if (family == count)
        {
            return false;
        }

        // Every supported core feature is enabled, which covers PyroWave's subgroup and 8-bit storage needs.
        features.pNext = &features11;
        features11.pNext = &features12;
        features12.pNext = &features13;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        features.features.robustBufferAccess = VK_FALSE;

        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        deviceInfo.pNext = &features;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1;
        deviceInfo.ppEnabledExtensionNames = &deviceExtension;
        if (vkCreateDevice(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS)
        {
            return false;
        }
        volkLoadDevice(device);
        vkGetDeviceQueue(device, family, 0, &queue);

        for (const QByteArray& name : instance.extensions())
        {
            extensionNames.push_back(name);
        }
        for (const QByteArray& name : extensionNames)
        {
            extensionPointers.push_back(name.constData());
        }
        appInfo.apiVersion = VK_API_VERSION_1_3;
        instanceInfo.pApplicationInfo = &appInfo;
        instanceInfo.enabledExtensionCount = uint32_t(extensionPointers.size());
        instanceInfo.ppEnabledExtensionNames = extensionPointers.data();

        pyrowave_device_create_queue_info queues = {queue, family, 0};
        pyrowave_device_create_info info = {};
        info.GetInstanceProcAddr = vkGetInstanceProcAddr;
        info.instance = instance.vkInstance();
        info.physical_device = physical;
        info.device = device;
        info.instance_create_info = &instanceInfo;
        info.device_create_info = &deviceInfo;
        info.queue_info = &queues;
        info.queue_info_count = 1;
        if (pyrowave_create_device(&info, &pyro) != PYROWAVE_SUCCESS)
        {
            return false;
        }
        pyrowave_device_set_queue_type(pyro, VK_QUEUE_GRAPHICS_BIT);
        return true;
    }

    bool initialize()
    {
        if (!createDevice())
        {
            return false;
        }

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS ||
            vkCreateSemaphore(device, &semaphoreInfo, nullptr, &acquired) != VK_SUCCESS)
        {
            return false;
        }
        VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmdInfo.commandPool = pool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &cmdInfo, &cmd) != VK_SUCCESS)
        {
            return false;
        }

        VkDescriptorSetLayoutBinding bindings[5] = {};
        for (uint32_t i = 0; i < 5; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = 5;
        setInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout) != VK_SUCCESS)
        {
            return false;
        }
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout;
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = sizeof(k_yuv420_to_rgbx_spv);
        moduleInfo.pCode = k_yuv420_to_rgbx_spv;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS ||
            vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS)
        {
            return false;
        }
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = pipelineLayout;
        const VkResult made = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        if (made != VK_SUCCESS)
        {
            return false;
        }

        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
        VkDescriptorPoolCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        descriptorInfo.maxSets = 1;
        descriptorInfo.poolSizeCount = 2;
        descriptorInfo.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &descriptorInfo, nullptr, &descriptorPool) != VK_SUCCESS)
        {
            return false;
        }
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &setLayout;
        return vkAllocateDescriptorSets(device, &allocInfo, &set) == VK_SUCCESS;
    }

    bool beginCommands()
    {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return vkResetCommandBuffer(cmd, 0) == VK_SUCCESS && vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS;
    }

    bool submitAndWait(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE)
    {
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
        {
            return false;
        }
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1 : 0;
        submit.pWaitSemaphores = &wait;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = signal != VK_NULL_HANDLE ? 1 : 0;
        submit.pSignalSemaphores = &signal;
        vkResetFences(device, 1, &fence);
        return vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS &&
               vkWaitForFences(device, 1, &fence, VK_TRUE, 1'000'000'000ull) == VK_SUCCESS;
    }

    void barrier(VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst, VkAccessFlags dstAccess)
    {
        VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memory.srcAccessMask = srcAccess;
        memory.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &memory, 0, nullptr, 0, nullptr);
    }

    void transition(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                    VkAccessFlags dstAccess, VkPipelineStageFlags src, VkPipelineStageFlags dst)
    {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    bool ensureFrame(int w, int h)
    {
        if (decoder != nullptr && w == width && h == height)
        {
            return true;
        }
        releaseFrame();
        pyrowave_decoder_create_info info = {pyro, w, h, PYROWAVE_CHROMA_SUBSAMPLING_420, false};
        if (pyrowave_decoder_create(&info, &decoder) != PYROWAVE_SUCCESS)
        {
            return false;
        }
        const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (int i = 0; i < 3; ++i)
        {
            const VkDeviceSize bytes = (VkDeviceSize(PlaneWidth(i, w)) * PlaneHeight(i, h) + 3) & ~VkDeviceSize(3);
            if (!makeImage(planes[i], VK_FORMAT_R8_UNORM, PlaneWidth(i, w), PlaneHeight(i, h),
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
                !makeBuffer(packed[i], bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            {
                return false;
            }
        }
        if (!makeBuffer(params, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host) ||
            !makeBuffer(rgbx, VkDeviceSize(w) * h * 4,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
            !makeImage(frame, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(w), uint32_t(h),
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        {
            return false;
        }
        const uint32_t dims[2] = {uint32_t(w), uint32_t(h)};
        std::memcpy(params.mapped, dims, sizeof(dims));

        VkDescriptorBufferInfo infos[5] = {{params.buffer, 0, VK_WHOLE_SIZE},    {packed[0].buffer, 0, VK_WHOLE_SIZE},
                                           {packed[1].buffer, 0, VK_WHOLE_SIZE}, {packed[2].buffer, 0, VK_WHOLE_SIZE},
                                           {rgbx.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[5] = {};
        for (uint32_t i = 0; i < 5; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
        width = w;
        height = h;

        // The planes stay in GENERAL from here on.
        if (!beginCommands())
        {
            return false;
        }
        for (Image& image : planes)
        {
            transition(image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        return submitAndWait();
    }

    bool ensureSwapchain()
    {
        if (view == nullptr || !view->isExposed())
        {
            return false;
        }
        if (surface == VK_NULL_HANDLE)
        {
            surface = QVulkanInstance::surfaceForWindow(view);
            VkBool32 supported = VK_FALSE;
            if (surface == VK_NULL_HANDLE ||
                vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, &supported) != VK_SUCCESS || !supported)
            {
                surface = VK_NULL_HANDLE;
                return false;
            }
        }
        VkSurfaceCapabilitiesKHR caps = {};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps) != VK_SUCCESS)
        {
            return false;
        }
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX)
        {
            const QSize pixels = view->size() * view->devicePixelRatio();
            extent = {uint32_t(pixels.width()), uint32_t(pixels.height())};
        }
        if (extent.width == 0 || extent.height == 0)
        {
            return false;
        }
        if (swapchain != VK_NULL_HANDLE && extent.width == swapExtent.width && extent.height == swapExtent.height)
        {
            return true;
        }
        if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
        {
            return false;
        }

        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data());
        if (formats.empty())
        {
            return false;
        }
        VkSurfaceFormatKHR format = formats[0];
        for (const VkSurfaceFormatKHR& candidate : formats)
        {
            if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM || candidate.format == VK_FORMAT_R8G8B8A8_UNORM)
            {
                format = candidate;
                break;
            }
        }
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, modes.data());
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        for (VkPresentModeKHR preferred : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
        {
            if (std::find(modes.begin(), modes.end(), preferred) != modes.end())
            {
                mode = preferred;
                break;
            }
        }

        VkSwapchainKHR old = swapchain;
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface;
        info.minImageCount = std::max(caps.minImageCount + 1, 2u);
        if (caps.maxImageCount != 0)
        {
            info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
        }
        info.imageFormat = format.format;
        info.imageColorSpace = format.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0
                                  ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                  : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
        info.presentMode = mode;
        info.clipped = VK_TRUE;
        info.oldSwapchain = old;
        VkSwapchainKHR created = VK_NULL_HANDLE;
        const VkResult result = vkCreateSwapchainKHR(device, &info, nullptr, &created);
        releaseSwapchain();
        if (result != VK_SUCCESS)
        {
            return false;
        }
        swapchain = created;
        swapExtent = extent;
        vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
        swapImages.resize(count);
        vkGetSwapchainImagesKHR(device, swapchain, &count, swapImages.data());
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        rendered.resize(count);
        for (VkSemaphore& semaphore : rendered)
        {
            vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore);
        }
        return true;
    }

    // Clears the swapchain image and blits the left eye into it, letterboxed.
    void recordPresent(VkImage target)
    {
        transition(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const VkClearColorValue background = {{4.0f / 255.0f, 6.0f / 255.0f, 9.0f / 255.0f, 1.0f}};
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &background, 1, &range);
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);

        const double eyeWidth = width / 2;
        const double scale = std::min(swapExtent.width / eyeWidth, swapExtent.height / double(height));
        const int32_t w = std::max(1, int32_t(eyeWidth * scale));
        const int32_t h = std::max(1, int32_t(height * scale));
        const int32_t x = (int32_t(swapExtent.width) - w) / 2;
        const int32_t y = (int32_t(swapExtent.height) - h) / 2;
        VkImageBlit blit = {};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {width / 2, height, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x, y, 0};
        blit.dstOffsets[1] = {x + w, y + h, 1};
        vkCmdBlitImage(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        transition(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }

    bool decode(const QByteArray& data)
    {
        pyrowave_decoder_clear(decoder);
        if (pyrowave_decoder_push_packet(decoder, data.constData(), size_t(data.size())) != PYROWAVE_SUCCESS ||
            !pyrowave_decoder_decode_is_ready(decoder, false) || !beginCommands())
        {
            return false;
        }

        pyrowave_gpu_buffers views = {};
        for (int i = 0; i < 3; ++i)
        {
            views.planes[i].image = planes[i].image;
            views.planes[i].width = PlaneWidth(i, width);
            views.planes[i].height = PlaneHeight(i, height);
            views.planes[i].image_format = VK_FORMAT_R8_UNORM;
            views.planes[i].view_format = VK_FORMAT_R8_UNORM;
            views.planes[i].aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            views.planes[i].swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
            views.planes[i].layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        pyrowave_device_set_command_buffer(pyro, cmd);
        const pyrowave_result decoded = pyrowave_decoder_decode_gpu_buffer(decoder, nullptr, nullptr, &views);
        pyrowave_device_set_command_buffer(pyro, VK_NULL_HANDLE);
        if (decoded != PYROWAVE_SUCCESS)
        {
            vkEndCommandBuffer(cmd);
            return false;
        }

        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        for (int i = 0; i < 3; ++i)
        {
            VkBufferImageCopy region = {};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {PlaneWidth(i, width), PlaneHeight(i, height), 1};
            vkCmdCopyImageToBuffer(cmd, planes[i].image, VK_IMAGE_LAYOUT_GENERAL, packed[i].buffer, 1, &region);
        }
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, (uint32_t(width) * uint32_t(height) + 63) / 64, 1, 1);
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);

        transition(frame.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy toFrame = {};
        toFrame.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        toFrame.imageExtent = {uint32_t(width), uint32_t(height), 1};
        vkCmdCopyBufferToImage(cmd, rgbx.buffer, frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &toFrame);
        transition(frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);

        uint32_t index = UINT32_MAX;
        if (ensureSwapchain())
        {
            const VkResult got = vkAcquireNextImageKHR(device, swapchain, 100'000'000ull, acquired, VK_NULL_HANDLE, &index);
            if (got == VK_ERROR_OUT_OF_DATE_KHR)
            {
                swapExtent = {};
            }
            if (got != VK_SUCCESS && got != VK_SUBOPTIMAL_KHR)
            {
                index = UINT32_MAX;
            }
        }
        if (index != UINT32_MAX)
        {
            recordPresent(swapImages[index]);
        }
        const bool presenting = index != UINT32_MAX;
        if (!submitAndWait(presenting ? acquired : VK_NULL_HANDLE, presenting ? rendered[index] : VK_NULL_HANDLE))
        {
            return false;
        }
        hasFrame = true;
        if (presenting)
        {
            VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &rendered[index];
            present.swapchainCount = 1;
            present.pSwapchains = &swapchain;
            present.pImageIndices = &index;
            const VkResult shown = vkQueuePresentKHR(queue, &present);
            if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR)
            {
                swapExtent = {};
            }
        }
        return true;
    }

    QImage snapshot()
    {
        if (!hasFrame)
        {
            return {};
        }
        Buffer staging;
        const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
        if (!makeBuffer(staging, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
            !beginCommands())
        {
            releaseBuffer(staging);
            return {};
        }
        const VkBufferCopy region = {0, 0, bytes};
        vkCmdCopyBuffer(cmd, rgbx.buffer, staging.buffer, 1, &region);
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
        QImage image;
        if (submitAndWait())
        {
            image = QImage(static_cast<const uchar*>(staging.mapped), width, height, width * 4,
                           QImage::Format_RGBX8888)
                        .copy();
        }
        releaseBuffer(staging);
        return image;
    }
};

PyroWaveDecoder::PyroWaveDecoder() = default;

PyroWaveDecoder::~PyroWaveDecoder() = default;

bool PyroWaveDecoder::isInitialized() const
{
    return gpu_ != nullptr;
}

bool PyroWaveDecoder::initialize(QString* error)
{
    if (gpu_)
    {
        return true;
    }
    std::unique_ptr<Gpu> gpu = std::make_unique<Gpu>();
    if (!gpu->initialize())
    {
        if (error != nullptr)
        {
            *error = "PyroWave: no Vulkan device for GPU decoding";
        }
        return false;
    }
    gpu_ = std::move(gpu);
    return true;
}

QWindow* PyroWaveDecoder::createView(QWidget* inputTarget)
{
    if (!gpu_)
    {
        return nullptr;
    }
    QWindow* view = new PyroWaveView(&gpu_->instance, inputTarget);
    gpu_->view = view;
    return view;
}

void PyroWaveDecoder::reset()
{
    if (gpu_)
    {
        gpu_->releaseFrame();
    }
}

QSize PyroWaveDecoder::decodedSize() const
{
    return gpu_ ? QSize(gpu_->width, gpu_->height) : QSize();
}

QImage PyroWaveDecoder::snapshot()
{
    return gpu_ ? gpu_->snapshot() : QImage();
}

bool PyroWaveDecoder::frameSize(const QByteArray& data, int& width, int& height)
{
    if (data.size() < 8)
    {
        return false;
    }
    uint32_t word = 0;
    std::memcpy(&word, data.constData(), sizeof(word));
    if ((word >> 31) != 1)
    {
        return false;
    }
    width = static_cast<int>(word & 0x3FFF) + 1;
    height = static_cast<int>((word >> 14) & 0x3FFF) + 1;
    return true;
}

bool PyroWaveDecoder::decode(const QByteArray& data, int64_t /*presentationTimeNs*/)
{
    int width = 0;
    int height = 0;
    return gpu_ && frameSize(data, width, height) && (width % 2) == 0 && (height % 2) == 0 &&
           gpu_->ensureFrame(width, height) && gpu_->decode(data);
}
