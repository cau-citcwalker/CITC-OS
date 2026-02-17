/*
 * vk_backend.h — Vulkan GPU 백엔드 (자체 정의)
 * ===============================================
 *
 * Vulkan SDK 헤더 없이 필요한 타입/상수/함수포인터를 직접 정의.
 * win32.h 패턴과 동일 — 실제 Vulkan 드라이버의 ABI만 맞추면 된다.
 *
 * dlopen("libvulkan.so.1") → dlsym으로 함수 포인터 로드.
 * Vulkan 없으면 SW 래스터라이저 fallback.
 *
 * 핸들 타입:
 *   Dispatchable (포인터):  VkInstance, VkPhysicalDevice, VkDevice,
 *                           VkQueue, VkCommandBuffer
 *   Non-dispatchable (u64): 나머지 전부 (VkImage, VkBuffer, ...)
 */

#ifndef CITC_VK_BACKEND_H
#define CITC_VK_BACKEND_H

#ifdef CITC_VULKAN_ENABLED

#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * Vulkan 기본 타입
 * ============================================================ */

typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;

/* Dispatchable handles (pointer) */
typedef struct VkInstance_T       *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T        *VkDevice;
typedef struct VkQueue_T         *VkQueue;
typedef struct VkCommandBuffer_T *VkCommandBuffer;

/* Non-dispatchable handles (uint64_t) */
typedef uint64_t VkImage;
typedef uint64_t VkImageView;
typedef uint64_t VkDeviceMemory;
typedef uint64_t VkBuffer;
typedef uint64_t VkRenderPass;
typedef uint64_t VkFramebuffer;
typedef uint64_t VkShaderModule;
typedef uint64_t VkPipeline;
typedef uint64_t VkPipelineLayout;
typedef uint64_t VkPipelineCache;
typedef uint64_t VkCommandPool;
typedef uint64_t VkFence;
typedef uint64_t VkSemaphore;
typedef uint64_t VkDescriptorSetLayout;
typedef uint64_t VkDescriptorPool;
typedef uint64_t VkDescriptorSet;
typedef uint64_t VkSampler;

#define VK_NULL_HANDLE 0

/* ============================================================
 * VkResult
 * ============================================================ */

typedef int32_t VkResult;
#define VK_SUCCESS                   0
#define VK_NOT_READY                 1
#define VK_TIMEOUT                   2
#define VK_INCOMPLETE                5
#define VK_ERROR_OUT_OF_HOST_MEMORY  (-1)
#define VK_ERROR_OUT_OF_DEVICE_MEMORY (-2)
#define VK_ERROR_INITIALIZATION_FAILED (-3)
#define VK_ERROR_DEVICE_LOST         (-4)
#define VK_ERROR_LAYER_NOT_PRESENT   (-6)
#define VK_ERROR_EXTENSION_NOT_PRESENT (-7)

/* ============================================================
 * VkStructureType (필요한 것만)
 * ============================================================ */

typedef int32_t VkStructureType;
#define VK_STRUCTURE_TYPE_APPLICATION_INFO              0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO          1
#define VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO      2
#define VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO            3
#define VK_STRUCTURE_TYPE_SUBMIT_INFO                   4
#define VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO          5
#define VK_STRUCTURE_TYPE_FENCE_CREATE_INFO             8
#define VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO         9
#define VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO            12
#define VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO             14
#define VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO        15
#define VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO     16
#define VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO    18
#define VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO 19
#define VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO 20
#define VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO  22
#define VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO 23
#define VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO  24
#define VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO 25
#define VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO   26
#define VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO       27
#define VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO   30
#define VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO       38
#define VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO 28
#define VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO      39
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO  40
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO     42
#define VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO        43
#define VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO       37
#define VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE           6
#define VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO  32
#define VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO   33
#define VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO  34
#define VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET          35
#define VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER         44
#define VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER          45

/* ============================================================
 * VkFormat (필요한 것만)
 * ============================================================ */

typedef int32_t VkFormat;
#define VK_FORMAT_UNDEFINED          0
#define VK_FORMAT_R8G8B8A8_UNORM     37
#define VK_FORMAT_B8G8R8A8_UNORM     44
#define VK_FORMAT_R32_SFLOAT         100
#define VK_FORMAT_R32G32_SFLOAT      103
#define VK_FORMAT_R32G32B32_SFLOAT   106
#define VK_FORMAT_R32G32B32A32_SFLOAT 109
#define VK_FORMAT_D32_SFLOAT         126

/* ============================================================
 * 기타 상수/열거
 * ============================================================ */

typedef int32_t VkImageType;
#define VK_IMAGE_TYPE_2D  1

typedef int32_t VkImageViewType;
#define VK_IMAGE_VIEW_TYPE_2D  1

typedef int32_t VkImageLayout;
#define VK_IMAGE_LAYOUT_UNDEFINED                       0
#define VK_IMAGE_LAYOUT_GENERAL                         1
#define VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL        2
#define VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL 3
#define VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL            6
#define VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL            7
#define VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL        5

typedef int32_t VkImageTiling;
#define VK_IMAGE_TILING_OPTIMAL  0
#define VK_IMAGE_TILING_LINEAR   1

typedef VkFlags VkImageUsageFlags;
#define VK_IMAGE_USAGE_TRANSFER_SRC_BIT          0x00000001
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT          0x00000002
#define VK_IMAGE_USAGE_SAMPLED_BIT               0x00000004
#define VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT      0x00000010
#define VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT 0x00000020

typedef VkFlags VkMemoryPropertyFlags;
#define VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT      0x00000001
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT      0x00000002
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT     0x00000004

typedef VkFlags VkBufferUsageFlags;
#define VK_BUFFER_USAGE_TRANSFER_SRC_BIT         0x00000001
#define VK_BUFFER_USAGE_TRANSFER_DST_BIT         0x00000002
#define VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT       0x00000010
#define VK_BUFFER_USAGE_INDEX_BUFFER_BIT         0x00000040
#define VK_BUFFER_USAGE_VERTEX_BUFFER_BIT        0x00000080

typedef int32_t VkIndexType;
#define VK_INDEX_TYPE_UINT16 0
#define VK_INDEX_TYPE_UINT32 1

typedef int32_t VkSharingMode;
#define VK_SHARING_MODE_EXCLUSIVE  0

typedef VkFlags VkImageAspectFlags;
#define VK_IMAGE_ASPECT_COLOR_BIT  0x00000001
#define VK_IMAGE_ASPECT_DEPTH_BIT  0x00000002

typedef int32_t VkComponentSwizzle;
#define VK_COMPONENT_SWIZZLE_IDENTITY 0

typedef int32_t VkSampleCountFlagBits;
#define VK_SAMPLE_COUNT_1_BIT  0x00000001

typedef int32_t VkAttachmentLoadOp;
#define VK_ATTACHMENT_LOAD_OP_LOAD      0
#define VK_ATTACHMENT_LOAD_OP_CLEAR     1
#define VK_ATTACHMENT_LOAD_OP_DONT_CARE 2

typedef int32_t VkAttachmentStoreOp;
#define VK_ATTACHMENT_STORE_OP_STORE     0
#define VK_ATTACHMENT_STORE_OP_DONT_CARE 1

typedef int32_t VkPipelineBindPoint;
#define VK_PIPELINE_BIND_POINT_GRAPHICS 0

typedef int32_t VkSubpassContents;
#define VK_SUBPASS_CONTENTS_INLINE 0

typedef int32_t VkCommandBufferLevel;
#define VK_COMMAND_BUFFER_LEVEL_PRIMARY   0

typedef VkFlags VkCommandPoolCreateFlags;
#define VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT 0x00000002

typedef VkFlags VkCommandBufferUsageFlags;
#define VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT 0x00000001

typedef int32_t VkShaderStageFlagBits;
#define VK_SHADER_STAGE_VERTEX_BIT   0x00000001
#define VK_SHADER_STAGE_FRAGMENT_BIT 0x00000010

typedef int32_t VkVertexInputRate;
#define VK_VERTEX_INPUT_RATE_VERTEX   0

typedef int32_t VkPrimitiveTopology;
#define VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST 3

typedef int32_t VkPolygonMode;
#define VK_POLYGON_MODE_FILL 0

typedef int32_t VkCullModeFlagBits;
#define VK_CULL_MODE_NONE  0
#define VK_CULL_MODE_BACK_BIT 2

typedef int32_t VkFrontFace;
#define VK_FRONT_FACE_COUNTER_CLOCKWISE 0
#define VK_FRONT_FACE_CLOCKWISE         1

typedef int32_t VkCompareOp;
#define VK_COMPARE_OP_LESS           4
#define VK_COMPARE_OP_LESS_OR_EQUAL  5

typedef int32_t VkLogicOp;
#define VK_LOGIC_OP_COPY 3

typedef int32_t VkBlendFactor;
#define VK_BLEND_FACTOR_ZERO 0
#define VK_BLEND_FACTOR_ONE  1

typedef int32_t VkBlendOp;
#define VK_BLEND_OP_ADD 0

typedef int32_t VkDynamicState;
#define VK_DYNAMIC_STATE_VIEWPORT 0
#define VK_DYNAMIC_STATE_SCISSOR  1

typedef int32_t VkDescriptorType;
#define VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER 6

typedef VkFlags VkPipelineStageFlags;
#define VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT         0x00000001
#define VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT 0x00000400
#define VK_PIPELINE_STAGE_TRANSFER_BIT            0x00001000

typedef VkFlags VkAccessFlags;
#define VK_ACCESS_TRANSFER_READ_BIT               0x00000800
#define VK_ACCESS_TRANSFER_WRITE_BIT              0x00001000
#define VK_ACCESS_MEMORY_READ_BIT                 0x00008000
#define VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT       0x00000100

typedef VkFlags VkQueueFlags;
#define VK_QUEUE_GRAPHICS_BIT  0x00000001

typedef int32_t VkPhysicalDeviceType;
#define VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   2
#define VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU 1

typedef VkFlags VkColorComponentFlags;
#define VK_COLOR_COMPONENT_R_BIT 0x00000001
#define VK_COLOR_COMPONENT_G_BIT 0x00000002
#define VK_COLOR_COMPONENT_B_BIT 0x00000004
#define VK_COLOR_COMPONENT_A_BIT 0x00000008

typedef VkFlags VkFenceCreateFlags;
#define VK_FENCE_CREATE_SIGNALED_BIT 0x00000001

/* ============================================================
 * Vulkan 구조체
 * ============================================================ */

typedef struct {
	int32_t x, y;
} VkOffset2D;

typedef struct {
	uint32_t width, height;
} VkExtent2D;

typedef struct {
	uint32_t width, height, depth;
} VkExtent3D;

typedef struct {
	VkOffset2D offset;
	VkExtent2D extent;
} VkRect2D;

typedef struct {
	float x, y, width, height;
	float minDepth, maxDepth;
} VkViewport;

typedef struct {
	float float32[4];
} VkClearColorValue;

typedef union {
	VkClearColorValue color;
	struct { float depth; uint32_t stencil; } depthStencil;
} VkClearValue;

typedef struct {
	VkComponentSwizzle r, g, b, a;
} VkComponentMapping;

typedef struct {
	VkImageAspectFlags aspectMask;
	uint32_t baseMipLevel;
	uint32_t levelCount;
	uint32_t baseArrayLayer;
	uint32_t layerCount;
} VkImageSubresourceRange;

typedef struct {
	VkImageAspectFlags aspectMask;
	uint32_t mipLevel;
	uint32_t baseArrayLayer;
	uint32_t layerCount;
} VkImageSubresourceLayers;

typedef struct {
	int32_t x, y, z;
} VkOffset3D;

typedef struct {
	VkImageSubresourceLayers imageSubresource;
	VkOffset3D imageOffset;
	VkExtent3D imageExtent;
	VkDeviceSize bufferOffset;
	uint32_t bufferRowLength;
	uint32_t bufferImageHeight;
} VkBufferImageCopy;

/* VkPhysicalDeviceProperties */
typedef struct {
	uint32_t apiVersion;
	uint32_t driverVersion;
	uint32_t vendorID;
	uint32_t deviceID;
	VkPhysicalDeviceType deviceType;
	char deviceName[256];
	uint8_t pipelineCacheUUID[16];
	/* limits + sparseProperties — 큰 구조체, 패딩 */
	uint8_t _pad[1024];
} VkPhysicalDeviceProperties;

/* VkQueueFamilyProperties */
typedef struct {
	VkQueueFlags queueFlags;
	uint32_t queueCount;
	uint32_t timestampValidBits;
	VkExtent3D minImageTransferGranularity;
} VkQueueFamilyProperties;

/* VkMemoryType / VkMemoryHeap / VkPhysicalDeviceMemoryProperties */
typedef struct {
	VkMemoryPropertyFlags propertyFlags;
	uint32_t heapIndex;
} VkMemoryType;

typedef struct {
	VkDeviceSize size;
	VkFlags flags;
} VkMemoryHeap;

#define VK_MAX_MEMORY_TYPES 32
#define VK_MAX_MEMORY_HEAPS 16

typedef struct {
	uint32_t memoryTypeCount;
	VkMemoryType memoryTypes[VK_MAX_MEMORY_TYPES];
	uint32_t memoryHeapCount;
	VkMemoryHeap memoryHeaps[VK_MAX_MEMORY_HEAPS];
} VkPhysicalDeviceMemoryProperties;

/* Create info 구조체들 */

typedef struct {
	VkStructureType sType;
	const void *pNext;
	const char *pApplicationName;
	uint32_t applicationVersion;
	const char *pEngineName;
	uint32_t engineVersion;
	uint32_t apiVersion;
} VkApplicationInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	const VkApplicationInfo *pApplicationInfo;
	uint32_t enabledLayerCount;
	const char *const *ppEnabledLayerNames;
	uint32_t enabledExtensionCount;
	const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	uint32_t queueFamilyIndex;
	uint32_t queueCount;
	const float *pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	uint32_t queueCreateInfoCount;
	const VkDeviceQueueCreateInfo *pQueueCreateInfos;
	uint32_t enabledLayerCount;
	const char *const *ppEnabledLayerNames;
	uint32_t enabledExtensionCount;
	const char *const *ppEnabledExtensionNames;
	const void *pEnabledFeatures;
} VkDeviceCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	uint32_t queueFamilyIndex;
} VkCommandPoolCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkCommandPool commandPool;
	VkCommandBufferLevel level;
	uint32_t commandBufferCount;
} VkCommandBufferAllocateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkCommandBufferUsageFlags flags;
	const void *pInheritanceInfo;
} VkCommandBufferBeginInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkDeviceSize allocationSize;
	uint32_t memoryTypeIndex;
} VkMemoryAllocateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	VkDeviceSize size;
	VkBufferUsageFlags usage;
	VkSharingMode sharingMode;
	uint32_t queueFamilyIndexCount;
	const uint32_t *pQueueFamilyIndices;
} VkBufferCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	VkImageType imageType;
	VkFormat format;
	VkExtent3D extent;
	uint32_t mipLevels;
	uint32_t arrayLayers;
	VkSampleCountFlagBits samples;
	VkImageTiling tiling;
	VkImageUsageFlags usage;
	VkSharingMode sharingMode;
	uint32_t queueFamilyIndexCount;
	const uint32_t *pQueueFamilyIndices;
	VkImageLayout initialLayout;
} VkImageCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	VkImage image;
	VkImageViewType viewType;
	VkFormat format;
	VkComponentMapping components;
	VkImageSubresourceRange subresourceRange;
} VkImageViewCreateInfo;

typedef struct {
	VkFlags flags;
	VkFormat format;
	VkSampleCountFlagBits samples;
	VkAttachmentLoadOp loadOp;
	VkAttachmentStoreOp storeOp;
	VkAttachmentLoadOp stencilLoadOp;
	VkAttachmentStoreOp stencilStoreOp;
	VkImageLayout initialLayout;
	VkImageLayout finalLayout;
} VkAttachmentDescription;

typedef struct {
	uint32_t attachment;
	VkImageLayout layout;
} VkAttachmentReference;

typedef struct {
	VkFlags flags;
	VkPipelineBindPoint pipelineBindPoint;
	uint32_t inputAttachmentCount;
	const VkAttachmentReference *pInputAttachments;
	uint32_t colorAttachmentCount;
	const VkAttachmentReference *pColorAttachments;
	const VkAttachmentReference *pResolveAttachments;
	const VkAttachmentReference *pDepthStencilAttachment;
	uint32_t preserveAttachmentCount;
	const uint32_t *pPreserveAttachments;
} VkSubpassDescription;

typedef struct {
	uint32_t srcSubpass;
	uint32_t dstSubpass;
	VkPipelineStageFlags srcStageMask;
	VkPipelineStageFlags dstStageMask;
	VkAccessFlags srcAccessMask;
	VkAccessFlags dstAccessMask;
	VkFlags dependencyFlags;
} VkSubpassDependency;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	uint32_t attachmentCount;
	const VkAttachmentDescription *pAttachments;
	uint32_t subpassCount;
	const VkSubpassDescription *pSubpasses;
	uint32_t dependencyCount;
	const VkSubpassDependency *pDependencies;
} VkRenderPassCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	VkRenderPass renderPass;
	uint32_t attachmentCount;
	const VkImageView *pAttachments;
	uint32_t width;
	uint32_t height;
	uint32_t layers;
} VkFramebufferCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkRenderPass renderPass;
	VkFramebuffer framebuffer;
	VkRect2D renderArea;
	uint32_t clearValueCount;
	const VkClearValue *pClearValues;
} VkRenderPassBeginInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	size_t codeSize;
	const uint32_t *pCode;
} VkShaderModuleCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	VkShaderStageFlagBits stage;
	VkShaderModule module;
	const char *pName;
	const void *pSpecializationInfo;
} VkPipelineShaderStageCreateInfo;

typedef struct {
	uint32_t binding;
	uint32_t stride;
	VkVertexInputRate inputRate;
} VkVertexInputBindingDescription;

typedef struct {
	uint32_t location;
	uint32_t binding;
	VkFormat format;
	uint32_t offset;
} VkVertexInputAttributeDescription;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	uint32_t vertexBindingDescriptionCount;
	const VkVertexInputBindingDescription *pVertexBindingDescriptions;
	uint32_t vertexAttributeDescriptionCount;
	const VkVertexInputAttributeDescription *pVertexAttributeDescriptions;
} VkPipelineVertexInputStateCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	VkPrimitiveTopology topology;
	VkBool32 primitiveRestartEnable;
} VkPipelineInputAssemblyStateCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	uint32_t viewportCount;
	const VkViewport *pViewports;
	uint32_t scissorCount;
	const VkRect2D *pScissors;
} VkPipelineViewportStateCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	VkBool32 depthClampEnable;
	VkBool32 rasterizerDiscardEnable;
	VkPolygonMode polygonMode;
	VkCullModeFlagBits cullMode;
	VkFrontFace frontFace;
	VkBool32 depthBiasEnable;
	float depthBiasConstantFactor;
	float depthBiasClamp;
	float depthBiasSlopeFactor;
	float lineWidth;
} VkPipelineRasterizationStateCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	VkSampleCountFlagBits rasterizationSamples;
	VkBool32 sampleShadingEnable;
	float minSampleShading;
	const uint32_t *pSampleMask;
	VkBool32 alphaToCoverageEnable;
	VkBool32 alphaToOneEnable;
} VkPipelineMultisampleStateCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	VkBool32 depthTestEnable;
	VkBool32 depthWriteEnable;
	VkCompareOp depthCompareOp;
	VkBool32 depthBoundsTestEnable;
	VkBool32 stencilTestEnable;
	uint8_t _front[28]; /* VkStencilOpState */
	uint8_t _back[28];
	float minDepthBounds;
	float maxDepthBounds;
} VkPipelineDepthStencilStateCreateInfo;

typedef struct {
	VkBool32 blendEnable;
	VkBlendFactor srcColorBlendFactor;
	VkBlendFactor dstColorBlendFactor;
	VkBlendOp colorBlendOp;
	VkBlendFactor srcAlphaBlendFactor;
	VkBlendFactor dstAlphaBlendFactor;
	VkBlendOp alphaBlendOp;
	VkColorComponentFlags colorWriteMask;
} VkPipelineColorBlendAttachmentState;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	VkBool32 logicOpEnable;
	VkLogicOp logicOp;
	uint32_t attachmentCount;
	const VkPipelineColorBlendAttachmentState *pAttachments;
	float blendConstants[4];
} VkPipelineColorBlendStateCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	uint32_t dynamicStateCount;
	const VkDynamicState *pDynamicStates;
} VkPipelineDynamicStateCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	uint32_t setLayoutCount;
	const VkDescriptorSetLayout *pSetLayouts;
	uint32_t pushConstantRangeCount;
	const void *pPushConstantRanges;
} VkPipelineLayoutCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	uint32_t stageCount;
	const VkPipelineShaderStageCreateInfo *pStages;
	const VkPipelineVertexInputStateCreateInfo *pVertexInputState;
	const VkPipelineInputAssemblyStateCreateInfo *pInputAssemblyState;
	const void *pTessellationState;
	const VkPipelineViewportStateCreateInfo *pViewportState;
	const VkPipelineRasterizationStateCreateInfo *pRasterizationState;
	const VkPipelineMultisampleStateCreateInfo *pMultisampleState;
	const VkPipelineDepthStencilStateCreateInfo *pDepthStencilState;
	const VkPipelineColorBlendStateCreateInfo *pColorBlendState;
	const VkPipelineDynamicStateCreateInfo *pDynamicState;
	VkPipelineLayout layout;
	VkRenderPass renderPass;
	uint32_t subpass;
	VkPipeline basePipelineHandle;
	int32_t basePipelineIndex;
} VkGraphicsPipelineCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
} VkFenceCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	uint32_t waitSemaphoreCount;
	const VkSemaphore *pWaitSemaphores;
	const VkPipelineStageFlags *pWaitDstStageMask;
	uint32_t commandBufferCount;
	const VkCommandBuffer *pCommandBuffers;
	uint32_t signalSemaphoreCount;
	const VkSemaphore *pSignalSemaphores;
} VkSubmitInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkPipelineStageFlags srcStageMask;
	VkPipelineStageFlags dstStageMask;
	VkAccessFlags srcAccessMask;
	VkAccessFlags dstAccessMask;
	uint32_t srcQueueFamilyIndex;
	uint32_t dstQueueFamilyIndex;
	VkImage image;
	VkImageSubresourceRange subresourceRange;
	VkImageLayout oldLayout;
	VkImageLayout newLayout;
} VkImageMemoryBarrier;

/* Descriptor set 관련 */
typedef struct {
	uint32_t binding;
	VkDescriptorType descriptorType;
	uint32_t descriptorCount;
	VkShaderStageFlagBits stageFlags;
	const VkSampler *pImmutableSamplers;
} VkDescriptorSetLayoutBinding;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	uint32_t bindingCount;
	const VkDescriptorSetLayoutBinding *pBindings;
} VkDescriptorSetLayoutCreateInfo;

typedef struct {
	VkDescriptorType type;
	uint32_t descriptorCount;
} VkDescriptorPoolSize;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	uint32_t maxSets;
	uint32_t poolSizeCount;
	const VkDescriptorPoolSize *pPoolSizes;
} VkDescriptorPoolCreateInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkDescriptorPool descriptorPool;
	uint32_t descriptorSetCount;
	const VkDescriptorSetLayout *pSetLayouts;
} VkDescriptorSetAllocateInfo;

typedef struct {
	VkBuffer buffer;
	VkDeviceSize offset;
	VkDeviceSize range;
} VkDescriptorBufferInfo;

typedef struct {
	VkStructureType sType;
	const void *pNext;
	VkDescriptorSet dstSet;
	uint32_t dstBinding;
	uint32_t dstArrayElement;
	uint32_t descriptorCount;
	VkDescriptorType descriptorType;
	const void *pImageInfo;
	const VkDescriptorBufferInfo *pBufferInfo;
	const void *pTexelBufferView;
} VkWriteDescriptorSet;

typedef struct {
	VkDeviceSize size;
	VkDeviceSize alignment;
	uint32_t memoryTypeBits;
} VkMemoryRequirements;

/* ============================================================
 * 함수 포인터 typedef
 * ============================================================ */

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char *);

/* Instance/Device lifecycle */
typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo *, const void *, VkInstance *);
typedef void     (*PFN_vkDestroyInstance)(VkInstance, const void *);
typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t *, VkPhysicalDevice *);
typedef void     (*PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties *);
typedef void     (*PFN_vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *);
typedef void     (*PFN_vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties *);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo *, const void *, VkDevice *);
typedef void     (*PFN_vkDestroyDevice)(VkDevice, const void *);
typedef void     (*PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue *);

/* Command pool/buffer */
typedef VkResult (*PFN_vkCreateCommandPool)(VkDevice, const VkCommandPoolCreateInfo *, const void *, VkCommandPool *);
typedef void     (*PFN_vkDestroyCommandPool)(VkDevice, VkCommandPool, const void *);
typedef VkResult (*PFN_vkAllocateCommandBuffers)(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *);
typedef void     (*PFN_vkFreeCommandBuffers)(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer *);
typedef VkResult (*PFN_vkBeginCommandBuffer)(VkCommandBuffer, const VkCommandBufferBeginInfo *);
typedef VkResult (*PFN_vkEndCommandBuffer)(VkCommandBuffer);
typedef VkResult (*PFN_vkResetCommandBuffer)(VkCommandBuffer, VkFlags);

/* Queue submit */
typedef VkResult (*PFN_vkQueueSubmit)(VkQueue, uint32_t, const VkSubmitInfo *, VkFence);
typedef VkResult (*PFN_vkQueueWaitIdle)(VkQueue);

/* Memory */
typedef VkResult (*PFN_vkAllocateMemory)(VkDevice, const VkMemoryAllocateInfo *, const void *, VkDeviceMemory *);
typedef void     (*PFN_vkFreeMemory)(VkDevice, VkDeviceMemory, const void *);
typedef VkResult (*PFN_vkMapMemory)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void **);
typedef void     (*PFN_vkUnmapMemory)(VkDevice, VkDeviceMemory);

/* Buffer */
typedef VkResult (*PFN_vkCreateBuffer)(VkDevice, const VkBufferCreateInfo *, const void *, VkBuffer *);
typedef void     (*PFN_vkDestroyBuffer)(VkDevice, VkBuffer, const void *);
typedef VkResult (*PFN_vkBindBufferMemory)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
typedef void     (*PFN_vkGetBufferMemoryRequirements)(VkDevice, VkBuffer, VkMemoryRequirements *);

/* Image */
typedef VkResult (*PFN_vkCreateImage)(VkDevice, const VkImageCreateInfo *, const void *, VkImage *);
typedef void     (*PFN_vkDestroyImage)(VkDevice, VkImage, const void *);
typedef VkResult (*PFN_vkBindImageMemory)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
typedef void     (*PFN_vkGetImageMemoryRequirements)(VkDevice, VkImage, VkMemoryRequirements *);
typedef VkResult (*PFN_vkCreateImageView)(VkDevice, const VkImageViewCreateInfo *, const void *, VkImageView *);
typedef void     (*PFN_vkDestroyImageView)(VkDevice, VkImageView, const void *);

/* Render pass / Framebuffer */
typedef VkResult (*PFN_vkCreateRenderPass)(VkDevice, const VkRenderPassCreateInfo *, const void *, VkRenderPass *);
typedef void     (*PFN_vkDestroyRenderPass)(VkDevice, VkRenderPass, const void *);
typedef VkResult (*PFN_vkCreateFramebuffer)(VkDevice, const VkFramebufferCreateInfo *, const void *, VkFramebuffer *);
typedef void     (*PFN_vkDestroyFramebuffer)(VkDevice, VkFramebuffer, const void *);

/* Pipeline */
typedef VkResult (*PFN_vkCreateShaderModule)(VkDevice, const VkShaderModuleCreateInfo *, const void *, VkShaderModule *);
typedef void     (*PFN_vkDestroyShaderModule)(VkDevice, VkShaderModule, const void *);
typedef VkResult (*PFN_vkCreatePipelineLayout)(VkDevice, const VkPipelineLayoutCreateInfo *, const void *, VkPipelineLayout *);
typedef void     (*PFN_vkDestroyPipelineLayout)(VkDevice, VkPipelineLayout, const void *);
typedef VkResult (*PFN_vkCreateGraphicsPipelines)(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *, const void *, VkPipeline *);
typedef void     (*PFN_vkDestroyPipeline)(VkDevice, VkPipeline, const void *);

/* Fence */
typedef VkResult (*PFN_vkCreateFence)(VkDevice, const VkFenceCreateInfo *, const void *, VkFence *);
typedef void     (*PFN_vkDestroyFence)(VkDevice, VkFence, const void *);
typedef VkResult (*PFN_vkWaitForFences)(VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t);
typedef VkResult (*PFN_vkResetFences)(VkDevice, uint32_t, const VkFence *);

/* Descriptor sets */
typedef VkResult (*PFN_vkCreateDescriptorSetLayout)(VkDevice, const VkDescriptorSetLayoutCreateInfo *, const void *, VkDescriptorSetLayout *);
typedef void     (*PFN_vkDestroyDescriptorSetLayout)(VkDevice, VkDescriptorSetLayout, const void *);
typedef VkResult (*PFN_vkCreateDescriptorPool)(VkDevice, const VkDescriptorPoolCreateInfo *, const void *, VkDescriptorPool *);
typedef void     (*PFN_vkDestroyDescriptorPool)(VkDevice, VkDescriptorPool, const void *);
typedef VkResult (*PFN_vkAllocateDescriptorSets)(VkDevice, const VkDescriptorSetAllocateInfo *, VkDescriptorSet *);
typedef void     (*PFN_vkUpdateDescriptorSets)(VkDevice, uint32_t, const VkWriteDescriptorSet *, uint32_t, const void *);

/* Commands */
typedef void (*PFN_vkCmdBeginRenderPass)(VkCommandBuffer, const VkRenderPassBeginInfo *, VkSubpassContents);
typedef void (*PFN_vkCmdEndRenderPass)(VkCommandBuffer);
typedef void (*PFN_vkCmdBindPipeline)(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
typedef void (*PFN_vkCmdBindVertexBuffers)(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *, const VkDeviceSize *);
typedef void (*PFN_vkCmdBindIndexBuffer)(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType);
typedef void (*PFN_vkCmdDraw)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (*PFN_vkCmdDrawIndexed)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
typedef void (*PFN_vkCmdSetViewport)(VkCommandBuffer, uint32_t, uint32_t, const VkViewport *);
typedef void (*PFN_vkCmdSetScissor)(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D *);
typedef void (*PFN_vkCmdCopyImageToBuffer)(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy *);
typedef void (*PFN_vkCmdPipelineBarrier)(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkFlags, uint32_t, const void *, uint32_t, const void *, uint32_t, const VkImageMemoryBarrier *);
typedef void (*PFN_vkCmdClearColorImage)(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue *, uint32_t, const VkImageSubresourceRange *);
typedef void (*PFN_vkCmdBindDescriptorSets)(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet *, uint32_t, const uint32_t *);

/* ============================================================
 * vk_backend 구조체
 * ============================================================
 *
 * Vulkan 디바이스 + 함수 포인터를 한데 모은 구조체.
 * d3d11.c에서 전역 인스턴스로 사용.
 */

struct vk_backend {
	int initialized;
	void *lib_handle;        /* dlopen handle */

	/* Core handles */
	VkInstance instance;
	VkPhysicalDevice physical_device;
	VkDevice device;
	VkQueue graphics_queue;
	uint32_t graphics_queue_family;
	VkCommandPool cmd_pool;

	/* Memory properties */
	VkPhysicalDeviceMemoryProperties mem_props;

	/* GPU info */
	char device_name[256];
	VkPhysicalDeviceType device_type;

	/* ---- 함수 포인터 ---- */

	/* Loader */
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr;

	/* Instance */
	PFN_vkCreateInstance CreateInstance;
	PFN_vkDestroyInstance DestroyInstance;
	PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
	PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
	PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;

	/* Device */
	PFN_vkCreateDevice CreateDevice;
	PFN_vkDestroyDevice DestroyDevice;
	PFN_vkGetDeviceQueue GetDeviceQueue;

	/* Command pool/buffer */
	PFN_vkCreateCommandPool CreateCommandPool;
	PFN_vkDestroyCommandPool DestroyCommandPool;
	PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
	PFN_vkFreeCommandBuffers FreeCommandBuffers;
	PFN_vkBeginCommandBuffer BeginCommandBuffer;
	PFN_vkEndCommandBuffer EndCommandBuffer;
	PFN_vkResetCommandBuffer ResetCommandBuffer;

	/* Queue */
	PFN_vkQueueSubmit QueueSubmit;
	PFN_vkQueueWaitIdle QueueWaitIdle;

	/* Memory */
	PFN_vkAllocateMemory AllocateMemory;
	PFN_vkFreeMemory FreeMemory;
	PFN_vkMapMemory MapMemory;
	PFN_vkUnmapMemory UnmapMemory;

	/* Buffer */
	PFN_vkCreateBuffer CreateBuffer;
	PFN_vkDestroyBuffer DestroyBuffer;
	PFN_vkBindBufferMemory BindBufferMemory;
	PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;

	/* Image */
	PFN_vkCreateImage CreateImage;
	PFN_vkDestroyImage DestroyImage;
	PFN_vkBindImageMemory BindImageMemory;
	PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
	PFN_vkCreateImageView CreateImageView;
	PFN_vkDestroyImageView DestroyImageView;

	/* Render pass / Framebuffer */
	PFN_vkCreateRenderPass CreateRenderPass;
	PFN_vkDestroyRenderPass DestroyRenderPass;
	PFN_vkCreateFramebuffer CreateFramebuffer;
	PFN_vkDestroyFramebuffer DestroyFramebuffer;

	/* Pipeline */
	PFN_vkCreateShaderModule CreateShaderModule;
	PFN_vkDestroyShaderModule DestroyShaderModule;
	PFN_vkCreatePipelineLayout CreatePipelineLayout;
	PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
	PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
	PFN_vkDestroyPipeline DestroyPipeline;

	/* Fence */
	PFN_vkCreateFence CreateFence;
	PFN_vkDestroyFence DestroyFence;
	PFN_vkWaitForFences WaitForFences;
	PFN_vkResetFences ResetFences;

	/* Descriptor sets */
	PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
	PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
	PFN_vkCreateDescriptorPool CreateDescriptorPool;
	PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
	PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
	PFN_vkUpdateDescriptorSets UpdateDescriptorSets;

	/* Commands */
	PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
	PFN_vkCmdEndRenderPass CmdEndRenderPass;
	PFN_vkCmdBindPipeline CmdBindPipeline;
	PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers;
	PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer;
	PFN_vkCmdDraw CmdDraw;
	PFN_vkCmdDrawIndexed CmdDrawIndexed;
	PFN_vkCmdSetViewport CmdSetViewport;
	PFN_vkCmdSetScissor CmdSetScissor;
	PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
	PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
	PFN_vkCmdClearColorImage CmdClearColorImage;
	PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
};

/* ============================================================
 * 공개 API
 * ============================================================ */

/* ============================================================
 * 렌더 타깃 (Class 41)
 * ============================================================
 *
 * 오프스크린 VkImage + VkImageView + VkRenderPass + VkFramebuffer.
 * Present 시 Staging Buffer로 readback하여 CPU 픽셀 버퍼에 복사.
 */

struct vk_render_target {
	int active;
	uint32_t width, height;

	/* Color attachment */
	VkImage image;
	VkImageView image_view;
	VkDeviceMemory image_memory;

	/* Depth attachment (D32_SFLOAT) */
	VkImage depth_image;
	VkImageView depth_view;
	VkDeviceMemory depth_memory;
	int has_depth;

	VkRenderPass render_pass;
	VkFramebuffer framebuffer;

	/* Staging buffer (readback용) */
	VkBuffer staging_buf;
	VkDeviceMemory staging_mem;
	VkDeviceSize staging_size;

	/* Command buffer (이 RT 전용) */
	VkCommandBuffer cmd;
};

/* ============================================================
 * 공개 API
 * ============================================================ */

/* Vulkan 라이브러리 로드 + 함수 포인터 해석. 성공 시 0 */
int vk_load_vulkan(struct vk_backend *vk);

/* Instance/Device/Queue/CommandPool 초기화. 성공 시 0 */
int vk_backend_init(struct vk_backend *vk);

/* 정리 (역순 파괴) */
void vk_backend_shutdown(struct vk_backend *vk);

/* 메모리 타입 검색. 못 찾으면 -1 */
int vk_find_memory_type(struct vk_backend *vk, uint32_t type_filter,
                        VkMemoryPropertyFlags properties);

/* 렌더 타깃 생성. 성공 시 0 */
int vk_create_render_target(struct vk_backend *vk, struct vk_render_target *rt,
                            uint32_t width, uint32_t height);

/* 렌더 타깃 파괴 */
void vk_destroy_render_target(struct vk_backend *vk, struct vk_render_target *rt);

/* GPU에서 Clear. 성공 시 0 */
int vk_clear_color(struct vk_backend *vk, struct vk_render_target *rt,
                   float r, float g, float b, float a);

/* GPU 이미지 → CPU 픽셀 버퍼로 readback. 성공 시 0 */
int vk_readback_pixels(struct vk_backend *vk, struct vk_render_target *rt,
                       uint32_t *out_pixels);

#endif /* CITC_VULKAN_ENABLED */
#endif /* CITC_VK_BACKEND_H */
