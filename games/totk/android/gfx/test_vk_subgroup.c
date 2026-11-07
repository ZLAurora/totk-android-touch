/* Standalone production-rewrite probe. GPU mode needs no game files. */
#include <assert.h>
#include <stdio.h>
#include <vulkan/vulkan.h>
#include "vk_subgroup.h"
#define CHECK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s: %d\n", #x, r_); abort(); } } while (0)

static void gpu(const uint32_t *words, size_t count) {
    VkInstance instance; VkDevice device; VkPhysicalDevice physical; uint32_t n = 1;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&ici, NULL, &instance)); CHECK(vkEnumeratePhysicalDevices(instance, &n, &physical));
    VkPhysicalDeviceSubgroupProperties subgroup = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &subgroup};
    vkGetPhysicalDeviceProperties2(physical, &props);
    fprintf(stderr, "subgroup fixture device %s, physical lanes %u, logical lanes 32\n", props.properties.deviceName, subgroup.subgroupSize);
    assert(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT);
    assert(subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT);
    uint32_t families = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, NULL);
    VkQueueFamilyProperties *qp = calloc(families, sizeof *qp); assert(qp);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, qp);
    uint32_t family = 0; while (family < families && !(qp[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) family++;
    assert(family < families); free(qp);
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    CHECK(vkCreateDevice(physical, &dci, NULL, &device)); VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    VkBuffer buffer; VkDeviceMemory memory;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 128 * 7 * 4, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    CHECK(vkCreateBuffer(device, &bci, NULL, &buffer)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, buffer, &req);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    uint32_t type = 0; while (type < mp.memoryTypeCount && (!(req.memoryTypeBits & (1u << type)) ||
        (mp.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) type++;
    assert(type < mp.memoryTypeCount);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    CHECK(vkAllocateMemory(device, &mai, NULL, &memory)); CHECK(vkBindBufferMemory(device, buffer, memory, 0));
    uint32_t *data; CHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&data)); memset(data, 0xcc, 128 * 7 * 4);
    VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(device, &lci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = count * 4, .pCode = words};
    VkShaderModule module; CHECK(vkCreateShaderModule(device, &smci, NULL, &module));
    VkComputePipelineCreateInfo pci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}};
    VkPipeline pipeline; CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device, &dsai, &set));
    VkDescriptorBufferInfo bi = {buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi};
    vkUpdateDescriptorSets(device, 1, &wr, 0, NULL);
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool cp; CHECK(vkCreateCommandPool(device, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(device, &cbai, &cb));
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cb, &begin));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL); vkCmdDispatch(cb, 1, 1, 1);
    VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cb)); VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    uint32_t width = subgroup.subgroupSize < 32 ? subgroup.subgroupSize : 32;
    uint32_t mask = width == 32 ? UINT32_MAX : (1u << width) - 1;
    for (uint32_t i = 0; i < 128; i++) {
        uint32_t pred = !((i / 32) & 1), expected[7] = {(i % subgroup.subgroupSize) & 31, pred ? mask : 0, 0, i ^ 1, pred, pred, 32};
        for (unsigned j = 0; j < 7; j++) {
            if (data[i*7+j] != expected[j]) fprintf(stderr, "lane %u field %u: %u != %u\n", i, j, data[i*7+j], expected[j]);
            assert(data[i*7+j] == expected[j]);
        }
    }
    fprintf(stderr, "subgroup fixture: 128 invocations x 7 outputs match 32-lane active-mask semantics%s\n", width < 32 ? " (partial hardware subgroups)" : "");
    vkUnmapMemory(device, memory); vkDestroyCommandPool(device, cp, NULL); vkDestroyDescriptorPool(device, pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL); vkDestroyShaderModule(device, module, NULL); vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, dsl, NULL); vkDestroyBuffer(device, buffer, NULL); vkFreeMemory(device, memory, NULL);
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
}

int main(int argc, char **argv) {
    assert(argc == 3 || argc == 4);
    assert(vk_subgroup_required(8, 1, ~0u, 32, 128) == 0); /* never geometry */
    assert(vk_subgroup_required(16, 0, ~0u, 32, 128) == 0);
    assert(vk_subgroup_required(16, 1, 32, 32, 128) == 0);
    assert(vk_subgroup_required(16, 1, ~0u, 32, 128) == 32);
    assert(vk_subgroup_required(32, 1, ~0u, 64, 128) == 64);
    assert(vk_subgroup_required(32, 1, ~0u, 128, 128) == 0);
    FILE *f = fopen(argv[1], "rb"); assert(f); assert(!fseek(f, 0, SEEK_END)); long bytes = ftell(f); assert(bytes >= 20 && !(bytes % 4)); rewind(f);
    uint32_t *words = malloc((size_t)bytes); assert(words); assert(fread(words, 1, (size_t)bytes, f) == (size_t)bytes); fclose(f);
    size_t count = (size_t)bytes / 4; int result = vk_spirv_subgroup32(&words, &count);
    if (result < 0) { free(words); return 2; }
    f = fopen(argv[2], "wb"); assert(f); assert(fwrite(words, 4, count, f) == count); assert(!fclose(f));
    if (argc == 4) gpu(words, count);
    free(words); return 0;
}
