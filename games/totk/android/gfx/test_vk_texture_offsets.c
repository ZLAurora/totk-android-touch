/* Standalone raw-translator SPIR-V probe; no game files or runtime patches.
 * Usage: probe shader.spv cbufBinding samplerBinding storageBinding
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

enum { IMAGE_SIZE = 8, COMPONENTS = 4, CBUF_SIZE = 65536 };

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    bool coherent;
} HostBuffer;

typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceMemoryProperties memory_properties;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    HostBuffer cbuf, output, upload;
    VkImage image;
    VkDeviceMemory image_memory;
    VkImageView view;
    VkSampler sampler;
    VkDescriptorSetLayout descriptor_layout;
    VkPipelineLayout pipeline_layout;
    VkShaderModule module;
    VkPipeline pipeline;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkCommandPool command_pool;
    VkCommandBuffer command;
} Fixture;

typedef struct {
    float coord[2];
    uint32_t packed_offsets[2];
} ShaderInput;

_Static_assert(sizeof(float) == 4, "fixture requires 32-bit floats");
_Static_assert(sizeof(ShaderInput) == 16, "constant-buffer input must occupy 16 bytes");

static bool check_vk(VkResult result, const char *operation) {
    if (result == VK_SUCCESS) return true;
    fprintf(stderr, "%s: VkResult %d\n", operation, (int)result);
    return false;
}

#define VK_TRY(call) do { if (!check_vk((call), #call)) return false; } while (0)

static bool parse_binding(const char *text, uint32_t *binding) {
    char *end;
    if (*text < '0' || *text > '9') return false;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || value > UINT32_MAX) return false;
    *binding = (uint32_t)value;
    return true;
}

static bool load_shader(const char *path, uint32_t **words, size_t *bytes,
                        bool *gather_extended) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return false;
    }
    if (fseek(file, 0, SEEK_END)) {
        perror("seek shader");
        fclose(file);
        return false;
    }
    long length = ftell(file);
    if (length < 20 || length % 4) {
        fprintf(stderr, "%s: expected a complete SPIR-V module\n", path);
        fclose(file);
        return false;
    }
    rewind(file);
    uint32_t *code = malloc((size_t)length);
    if (!code) {
        fprintf(stderr, "cannot allocate shader module\n");
        fclose(file);
        return false;
    }
    bool complete = fread(code, 1, (size_t)length, file) == (size_t)length;
    if (fclose(file)) complete = false;
    if (!complete || code[0] != UINT32_C(0x07230203)) {
        fprintf(stderr, "%s: cannot read little-endian SPIR-V module\n", path);
        free(code);
        return false;
    }
    *gather_extended = false;
    size_t count = (size_t)length / sizeof(*code);
    for (size_t i = 5; i < count;) {
        uint32_t instruction_words = code[i] >> 16;
        uint32_t opcode = code[i] & UINT32_C(0xffff);
        if (!instruction_words || instruction_words > count - i ||
            (opcode == 17 && instruction_words != 2)) {
            fprintf(stderr, "%s: malformed SPIR-V instruction at word %zu\n", path, i);
            free(code);
            return false;
        }
        /* OpCapability ImageGatherExtended; no SPIR-V header dependency. */
        if (opcode == 17 && code[i + 1] == 25) *gather_extended = true;
        i += instruction_words;
    }
    *words = code;
    *bytes = (size_t)length;
    return true;
}

static bool select_device(Fixture *fixture, bool gather_extended) {
    uint32_t count = 0;
    VK_TRY(vkEnumeratePhysicalDevices(fixture->instance, &count, NULL));
    if (!count) {
        fprintf(stderr, "no Vulkan physical devices\n");
        return false;
    }
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) return false;
    VkResult result = vkEnumeratePhysicalDevices(fixture->instance, &count, devices);
    if (!check_vk(result, "vkEnumeratePhysicalDevices")) {
        free(devices);
        return false;
    }
    for (uint32_t i = 0; i < count && !fixture->physical; i++) {
        VkPhysicalDeviceFeatures features;
        VkPhysicalDeviceProperties properties;
        VkFormatProperties format;
        vkGetPhysicalDeviceFeatures(devices[i], &features);
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        vkGetPhysicalDeviceFormatProperties(devices[i], VK_FORMAT_R32_SFLOAT, &format);
        VkFormatFeatureFlags required_format = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                               VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((gather_extended && !features.shaderImageGatherExtended) ||
            properties.limits.maxUniformBufferRange < CBUF_SIZE ||
            (format.optimalTilingFeatures & required_format) != required_format) continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, NULL);
        if (!families) continue;
        VkQueueFamilyProperties *queues = calloc(families, sizeof(*queues));
        if (!queues) {
            free(devices);
            return false;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, queues);
        for (uint32_t family = 0; family < families; family++) {
            if (queues[family].queueCount && (queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                fixture->physical = devices[i];
                fixture->family = family;
                fprintf(stderr, "texture-offset fixture device: %s\n", properties.deviceName);
                break;
            }
        }
        free(queues);
    }
    free(devices);
    if (!fixture->physical) {
        fprintf(stderr, "no compute device supports the fixture's buffers, image and capabilities\n");
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(fixture->physical, &fixture->memory_properties);
    return true;
}

static bool memory_type(const Fixture *fixture, uint32_t bits,
                        VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
                        uint32_t *type) {
    for (unsigned pass = 0; pass < 2; pass++) {
        VkMemoryPropertyFlags flags = required | (pass == 0 ? preferred : 0);
        for (uint32_t i = 0; i < fixture->memory_properties.memoryTypeCount; i++) {
            if ((bits & (UINT32_C(1) << i)) &&
                (fixture->memory_properties.memoryTypes[i].propertyFlags & flags) == flags) {
                *type = i;
                return true;
            }
        }
    }
    fprintf(stderr, "no compatible Vulkan memory type\n");
    return false;
}

static bool create_host_buffer(Fixture *fixture, VkDeviceSize size,
                               VkBufferUsageFlags usage, HostBuffer *buffer) {
    VkBufferCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_TRY(vkCreateBuffer(fixture->device, &create, NULL, &buffer->buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(fixture->device, buffer->buffer, &requirements);
    uint32_t type;
    if (!memory_type(fixture, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) return false;
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    VK_TRY(vkAllocateMemory(fixture->device, &allocate, NULL, &buffer->memory));
    VK_TRY(vkBindBufferMemory(fixture->device, buffer->buffer, buffer->memory, 0));
    VK_TRY(vkMapMemory(fixture->device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped));
    buffer->coherent = (fixture->memory_properties.memoryTypes[type].propertyFlags &
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    return true;
}

static bool flush_buffer(Fixture *fixture, const HostBuffer *buffer) {
    if (!buffer->coherent) {
        VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = buffer->memory,
            .size = VK_WHOLE_SIZE,
        };
        VK_TRY(vkFlushMappedMemoryRanges(fixture->device, 1, &range));
    }
    return true;
}

static bool create_image(Fixture *fixture) {
    VkImageCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R32_SFLOAT,
        .extent = {IMAGE_SIZE, IMAGE_SIZE, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_TRY(vkCreateImage(fixture->device, &create, NULL, &fixture->image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(fixture->device, fixture->image, &requirements);
    uint32_t type;
    if (!memory_type(fixture, requirements.memoryTypeBits, 0,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) return false;
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    VK_TRY(vkAllocateMemory(fixture->device, &allocate, NULL, &fixture->image_memory));
    VK_TRY(vkBindImageMemory(fixture->device, fixture->image, fixture->image_memory, 0));
    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = fixture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R32_SFLOAT,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VK_TRY(vkCreateImageView(fixture->device, &view, NULL, &fixture->view));
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxAnisotropy = 1.0f,
        .minLod = 0.0f,
        .maxLod = 0.0f,
    };
    VK_TRY(vkCreateSampler(fixture->device, &sampler, NULL, &fixture->sampler));
    return true;
}

static bool create_pipeline(Fixture *fixture, const uint32_t *words, size_t bytes,
                            const uint32_t bindings[3]) {
    VkDescriptorSetLayoutBinding descriptors[] = {
        {.binding = bindings[0], .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = bindings[1], .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = bindings[2], .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
    };
    VkDescriptorSetLayoutCreateInfo descriptor_layout = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3,
        .pBindings = descriptors,
    };
    VK_TRY(vkCreateDescriptorSetLayout(fixture->device, &descriptor_layout, NULL,
                                      &fixture->descriptor_layout));
    VkPipelineLayoutCreateInfo pipeline_layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &fixture->descriptor_layout,
    };
    VK_TRY(vkCreatePipelineLayout(fixture->device, &pipeline_layout, NULL, &fixture->pipeline_layout));
    VkShaderModuleCreateInfo module = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes,
        .pCode = words,
    };
    VK_TRY(vkCreateShaderModule(fixture->device, &module, NULL, &fixture->module));
    VkComputePipelineCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = fixture->module,
            .pName = "main",
        },
        .layout = fixture->pipeline_layout,
    };
    VK_TRY(vkCreateComputePipelines(fixture->device, VK_NULL_HANDLE, 1, &pipeline, NULL,
                                    &fixture->pipeline));
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
    };
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 3,
        .pPoolSizes = sizes,
    };
    VK_TRY(vkCreateDescriptorPool(fixture->device, &pool, NULL, &fixture->descriptor_pool));
    VkDescriptorSetAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = fixture->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &fixture->descriptor_layout,
    };
    VK_TRY(vkAllocateDescriptorSets(fixture->device, &allocate, &fixture->descriptor_set));
    VkDescriptorBufferInfo cbuf = {.buffer = fixture->cbuf.buffer, .range = CBUF_SIZE};
    VkDescriptorBufferInfo output = {.buffer = fixture->output.buffer, .range = COMPONENTS * sizeof(float)};
    VkDescriptorImageInfo image = {
        .sampler = fixture->sampler,
        .imageView = fixture->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet writes[] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = fixture->descriptor_set,
         .dstBinding = bindings[0], .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &cbuf},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = fixture->descriptor_set,
         .dstBinding = bindings[1], .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = fixture->descriptor_set,
         .dstBinding = bindings[2], .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &output},
    };
    vkUpdateDescriptorSets(fixture->device, 3, writes, 0, NULL);
    return true;
}

static bool submit(Fixture *fixture) {
    VkSubmitInfo info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &fixture->command,
    };
    VK_TRY(vkQueueSubmit(fixture->queue, 1, &info, VK_NULL_HANDLE));
    VK_TRY(vkQueueWaitIdle(fixture->queue));
    return true;
}

static bool upload_image(Fixture *fixture) {
    float *texels = fixture->upload.mapped;
    for (int y = 0; y < IMAGE_SIZE; y++) {
        for (int x = 0; x < IMAGE_SIZE; x++) texels[y * IMAGE_SIZE + x] = (float)(100 * y + x);
    }
    if (!flush_buffer(fixture, &fixture->upload)) return false;
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_TRY(vkBeginCommandBuffer(fixture->command, &begin));
    VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = fixture->upload.buffer,
        .size = VK_WHOLE_SIZE,
    };
    VkImageMemoryBarrier image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = fixture->image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(fixture->command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 1, &host, 1, &image);
    VkBufferImageCopy copy = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {IMAGE_SIZE, IMAGE_SIZE, 1},
    };
    vkCmdCopyBufferToImage(fixture->command, fixture->upload.buffer, fixture->image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    image.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(fixture->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &image);
    VK_TRY(vkEndCommandBuffer(fixture->command));
    if (!submit(fixture)) return false;
    VK_TRY(vkResetCommandBuffer(fixture->command, 0));
    return true;
}

static bool record_compute(Fixture *fixture) {
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_TRY(vkBeginCommandBuffer(fixture->command, &begin));
    VkBufferMemoryBarrier host[] = {
        {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .buffer = fixture->cbuf.buffer, .size = VK_WHOLE_SIZE},
        {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .buffer = fixture->output.buffer, .size = VK_WHOLE_SIZE},
    };
    vkCmdPipelineBarrier(fixture->command, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 2, host, 0, NULL);
    vkCmdBindPipeline(fixture->command, VK_PIPELINE_BIND_POINT_COMPUTE, fixture->pipeline);
    vkCmdBindDescriptorSets(fixture->command, VK_PIPELINE_BIND_POINT_COMPUTE, fixture->pipeline_layout,
                            0, 1, &fixture->descriptor_set, 0, NULL);
    vkCmdDispatch(fixture->command, 1, 1, 1);
    VkBufferMemoryBarrier output = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = fixture->output.buffer,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(fixture->command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &output, 0, NULL);
    VK_TRY(vkEndCommandBuffer(fixture->command));
    return true;
}

static bool create_fixture(Fixture *fixture, const uint32_t *words, size_t bytes,
                           bool gather_extended, const uint32_t bindings[3]) {
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "texture-offset fixture",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo instance = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VK_TRY(vkCreateInstance(&instance, NULL, &fixture->instance));
    if (!select_device(fixture, gather_extended)) return false;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = fixture->family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    /* Enable no optional features except an explicitly declared gather capability. */
    VkPhysicalDeviceFeatures features = {.shaderImageGatherExtended = gather_extended ? VK_TRUE : VK_FALSE};
    VkDeviceCreateInfo device = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
        .pEnabledFeatures = &features,
    };
    VK_TRY(vkCreateDevice(fixture->physical, &device, NULL, &fixture->device));
    vkGetDeviceQueue(fixture->device, fixture->family, 0, &fixture->queue);
    if (!create_host_buffer(fixture, CBUF_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &fixture->cbuf) ||
        !create_host_buffer(fixture, COMPONENTS * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            &fixture->output) ||
        !create_host_buffer(fixture, IMAGE_SIZE * IMAGE_SIZE * sizeof(float),
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &fixture->upload) ||
        !create_image(fixture) || !create_pipeline(fixture, words, bytes, bindings)) return false;
    memset(fixture->cbuf.mapped, 0, CBUF_SIZE);
    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = fixture->family,
    };
    VK_TRY(vkCreateCommandPool(fixture->device, &pool, NULL, &fixture->command_pool));
    VkCommandBufferAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = fixture->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VK_TRY(vkAllocateCommandBuffers(fixture->device, &allocate, &fixture->command));
    return upload_image(fixture) && record_compute(fixture);
}

static bool run_cases(Fixture *fixture) {
    static const struct {
        int base[2];
        int offset[COMPONENTS][2];
    } cases[] = {
        {{3, 3}, {{ 0,  0}, { 0,  0}, { 0,  0}, { 0,  0}}},
        {{3, 3}, {{ 0,  0}, { 1,  0}, { 0,  1}, { 2,  2}}},
        {{3, 3}, {{ 0,  0}, {-1,  0}, { 0, -1}, {-2, -2}}},
        {{3, 3}, {{-3,  2}, { 2, -3}, {-1, -2}, { 3,  3}}},
        {{3, 3}, {{ 3,  3}, {-1, -2}, { 2, -3}, {-3,  2}}},
        {{4, 4}, {{-2,  2}, {-1,  1}, { 1, -1}, { 2, -2}}},
        {{2, 4}, {{-2, -3}, { 3,  2}, { 1, -4}, { 4,  1}}},
        {{5, 2}, {{ 1, -1}, {-3,  3}, { 0,  2}, {-4,  0}}},
    };
    size_t matched = 0;
    size_t case_count = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < case_count; i++) {
        ShaderInput input = {
            .coord = {((float)cases[i].base[0] + 0.75f) / IMAGE_SIZE,
                      ((float)cases[i].base[1] + 0.75f) / IMAGE_SIZE},
            .packed_offsets = {0, 0},
        };
        /* PTP words contain (x0,y0,x1,y1), then (x2,y2,x3,y3).
         * Each byte carries a signed six-bit value; leave its spare bits clear. */
        for (unsigned component = 0; component < COMPONENTS; component++) {
            for (unsigned axis = 0; axis < 2; axis++) {
                unsigned shift = (component % 2 * 2 + axis) * 8;
                input.packed_offsets[component / 2] |=
                    ((uint32_t)cases[i].offset[component][axis] & UINT32_C(0x3f)) << shift;
            }
        }
        memcpy(fixture->cbuf.mapped, &input, sizeof(input));
        memset(fixture->output.mapped, 0xff, COMPONENTS * sizeof(float));
        if (!flush_buffer(fixture, &fixture->cbuf) ||
            !flush_buffer(fixture, &fixture->output) || !submit(fixture)) return false;
        if (!fixture->output.coherent) {
            VkMappedMemoryRange range = {
                .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = fixture->output.memory,
                .size = VK_WHOLE_SIZE,
            };
            VK_TRY(vkInvalidateMappedMemoryRanges(fixture->device, 1, &range));
        }
        float actual[COMPONENTS];
        memcpy(actual, fixture->output.mapped, sizeof(actual));
        /* Coordinates are positive and away from gather boundaries, so truncation
         * is floor(coord * size - .5), even with the translator's tiny bias.
         * Guest PTP component i selects gather W at base + its own offset_i. */
        int base_x = (int)(input.coord[0] * IMAGE_SIZE - 0.5f);
        int base_y = (int)(input.coord[1] * IMAGE_SIZE - 0.5f);
        for (unsigned component = 0; component < COMPONENTS; component++) {
            int x = base_x + cases[i].offset[component][0];
            int y = base_y + cases[i].offset[component][1];
            if (x < 0 || x >= IMAGE_SIZE || y < 0 || y >= IMAGE_SIZE) {
                fprintf(stderr, "fixture input %zu component %u leaves the image\n", i, component);
                return false;
            }
            float expected = (float)(100 * y + x);
            if (actual[component] == expected) {
                matched++;
            } else {
                fprintf(stderr, "input %zu component %u: offset (%d,%d), texel (%d,%d), "
                                "expected %.0f, got %.9g\n", i, component,
                        cases[i].offset[component][0], cases[i].offset[component][1],
                        x, y, (double)expected, (double)actual[component]);
            }
        }
    }
    size_t total = case_count * COMPONENTS;
    printf("texture-offset fixture: %zu/%zu exact results (%zu inputs x %u components)\n",
           matched, total, case_count, (unsigned)COMPONENTS);
    return matched == total;
}

static void destroy_host_buffer(Fixture *fixture, HostBuffer *buffer) {
    if (buffer->mapped) vkUnmapMemory(fixture->device, buffer->memory);
    vkDestroyBuffer(fixture->device, buffer->buffer, NULL);
    vkFreeMemory(fixture->device, buffer->memory, NULL);
}

static void destroy_fixture(Fixture *fixture) {
    if (fixture->device) {
        (void)vkDeviceWaitIdle(fixture->device);
        vkDestroyCommandPool(fixture->device, fixture->command_pool, NULL);
        vkDestroyDescriptorPool(fixture->device, fixture->descriptor_pool, NULL);
        vkDestroyPipeline(fixture->device, fixture->pipeline, NULL);
        vkDestroyShaderModule(fixture->device, fixture->module, NULL);
        vkDestroyPipelineLayout(fixture->device, fixture->pipeline_layout, NULL);
        vkDestroyDescriptorSetLayout(fixture->device, fixture->descriptor_layout, NULL);
        vkDestroySampler(fixture->device, fixture->sampler, NULL);
        vkDestroyImageView(fixture->device, fixture->view, NULL);
        vkDestroyImage(fixture->device, fixture->image, NULL);
        vkFreeMemory(fixture->device, fixture->image_memory, NULL);
        destroy_host_buffer(fixture, &fixture->upload);
        destroy_host_buffer(fixture, &fixture->output);
        destroy_host_buffer(fixture, &fixture->cbuf);
        vkDestroyDevice(fixture->device, NULL);
    }
    if (fixture->instance) vkDestroyInstance(fixture->instance, NULL);
}

int main(int argc, char **argv) {
    uint32_t bindings[3];
    if (argc != 5 || !parse_binding(argv[2], &bindings[0]) ||
        !parse_binding(argv[3], &bindings[1]) || !parse_binding(argv[4], &bindings[2]) ||
        bindings[0] == bindings[1] || bindings[0] == bindings[2] || bindings[1] == bindings[2]) {
        fprintf(stderr, "usage: %s shader.spv cbufBinding samplerBinding storageBinding\n", argv[0]);
        return EXIT_FAILURE;
    }
    uint32_t *words = NULL;
    size_t bytes = 0;
    bool gather_extended = false;
    if (!load_shader(argv[1], &words, &bytes, &gather_extended)) return EXIT_FAILURE;
    Fixture fixture = {0};
    bool success = create_fixture(&fixture, words, bytes, gather_extended, bindings) && run_cases(&fixture);
    destroy_fixture(&fixture);
    free(words);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
