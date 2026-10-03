// SPDX-License-Identifier: MPL-2.0

#include "PyroWaveDecoder.h"

#include <volk.h>
#include <pyrowave.h>

#include "yuv420_to_rgbx_spv.h"

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

} // namespace

struct PyroWaveDecoder::Gpu
{
    pyrowave_device pyro = nullptr;
    pyrowave_decoder decoder = nullptr;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
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

    ~Gpu()
    {
        releaseFrame();
        if (device != VK_NULL_HANDLE)
        {
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

    bool makeImage(Image& out, uint32_t w, uint32_t h)
    {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8_UNORM;
        info.extent = {w, h, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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
            vkDestroyImage(device, image.image, nullptr);
            vkFreeMemory(device, image.memory, nullptr);
            image = {};
        }
        for (Buffer& b : packed)
        {
            releaseBuffer(b);
        }
        releaseBuffer(params);
        releaseBuffer(rgbx);
        width = 0;
        height = 0;
    }

    bool initialize()
    {
        if (pyrowave_create_default_device(&pyro) != PYROWAVE_SUCCESS || volkInitialize() != VK_SUCCESS)
        {
            return false;
        }
        VkInstance instance = VK_NULL_HANDLE;
        pyrowave_device_get_vk_device_handles(pyro, &instance, &physical, &device);
        volkLoadInstanceOnly(instance);
        volkLoadDevice(device);
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);

        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        uint32_t family = 0;
        while (family < count && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
        {
            ++family;
        }
        if (family == count)
        {
            return false;
        }
        vkGetDeviceQueue(device, family, 0, &queue);
        pyrowave_device_set_queue_type(pyro, VK_QUEUE_GRAPHICS_BIT);

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
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

    bool submitAndWait()
    {
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
        {
            return false;
        }
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
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
            if (!makeImage(planes[i], PlaneWidth(i, w), PlaneHeight(i, h)) ||
                !makeBuffer(packed[i], bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            {
                return false;
            }
        }
        const VkDeviceSize rgbxBytes = VkDeviceSize(w) * h * 4;
        if (!makeBuffer(params, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host))
        {
            return false;
        }
        if (!makeBuffer(rgbx, rgbxBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT) &&
            !makeBuffer(rgbx, rgbxBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host))
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
            VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.image = image.image;
            toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &toGeneral);
        }
        return submitAndWait();
    }

    bool decode(const QByteArray& data, QImage& out)
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
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
        if (!submitAndWait())
        {
            return false;
        }

        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = rgbx.memory;
        range.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(device, 1, &range);
        out = QImage(static_cast<const uchar*>(rgbx.mapped), width, height, width * 4, QImage::Format_RGBX8888).copy();
        return true;
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
    auto gpu = std::make_unique<Gpu>();
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

void PyroWaveDecoder::reset()
{
    if (gpu_)
    {
        gpu_->releaseFrame();
    }
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

bool PyroWaveDecoder::decode(const QByteArray& data, int64_t /*presentationTimeNs*/, QList<QImage>& frames)
{
    int width = 0;
    int height = 0;
    if (!gpu_ || !frameSize(data, width, height) || (width % 2) != 0 || (height % 2) != 0 ||
        !gpu_->ensureFrame(width, height))
    {
        return false;
    }
    QImage image;
    if (!gpu_->decode(data, image))
    {
        return false;
    }
    frames.append(image);
    return true;
}
