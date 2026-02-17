/*
 * vk_backend.c — Vulkan GPU 백엔드 구현
 * =======================================
 *
 * dlopen("libvulkan.so.1")으로 Vulkan 라이브러리를 동적 로드하고,
 * VkInstance → VkPhysicalDevice → VkDevice → VkQueue → VkCommandPool
 * 순서로 초기화.
 *
 * Vulkan SDK 헤더 없이 vk_backend.h의 자체 정의만 사용.
 * Vulkan이 없는 환경(QEMU 등)에서는 graceful fail → SW fallback.
 *
 * 빌드: VULKAN=1 일 때만 컴파일 (-DCITC_VULKAN_ENABLED -ldl)
 */

#ifdef CITC_VULKAN_ENABLED

#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

#include "vk_backend.h"

/* ============================================================
 * dlopen + dlsym으로 Vulkan 함수 포인터 로드
 * ============================================================ */

int vk_load_vulkan(struct vk_backend *vk)
{
	memset(vk, 0, sizeof(*vk));

	/* libvulkan.so.1 로드 */
	vk->lib_handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!vk->lib_handle) {
		fprintf(stderr, "[VK] dlopen(libvulkan.so.1) failed: %s\n",
			dlerror());
		return -1;
	}

	/* vkGetInstanceProcAddr — 모든 다른 함수를 가져오는 진입점 */
	vk->GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)
		dlsym(vk->lib_handle, "vkGetInstanceProcAddr");
	if (!vk->GetInstanceProcAddr) {
		fprintf(stderr, "[VK] dlsym(vkGetInstanceProcAddr) failed\n");
		dlclose(vk->lib_handle);
		vk->lib_handle = NULL;
		return -1;
	}

	/* vkCreateInstance — NULL instance에서 가져옴 */
	vk->CreateInstance = (PFN_vkCreateInstance)
		vk->GetInstanceProcAddr(NULL, "vkCreateInstance");
	if (!vk->CreateInstance) {
		fprintf(stderr, "[VK] vkCreateInstance not found\n");
		dlclose(vk->lib_handle);
		vk->lib_handle = NULL;
		return -1;
	}

	return 0;
}

/* Instance-level 함수 포인터 로드 (Instance 생성 후) */
static void load_instance_functions(struct vk_backend *vk)
{
#define LOAD_INST(name) \
	vk->name = (PFN_vk##name)vk->GetInstanceProcAddr(vk->instance, "vk" #name)

	LOAD_INST(DestroyInstance);
	LOAD_INST(EnumeratePhysicalDevices);
	LOAD_INST(GetPhysicalDeviceProperties);
	LOAD_INST(GetPhysicalDeviceMemoryProperties);
	LOAD_INST(GetPhysicalDeviceQueueFamilyProperties);
	LOAD_INST(CreateDevice);
	LOAD_INST(DestroyDevice);
	LOAD_INST(GetDeviceQueue);

	/* Command pool/buffer */
	LOAD_INST(CreateCommandPool);
	LOAD_INST(DestroyCommandPool);
	LOAD_INST(AllocateCommandBuffers);
	LOAD_INST(FreeCommandBuffers);
	LOAD_INST(BeginCommandBuffer);
	LOAD_INST(EndCommandBuffer);
	LOAD_INST(ResetCommandBuffer);

	/* Queue */
	LOAD_INST(QueueSubmit);
	LOAD_INST(QueueWaitIdle);

	/* Memory */
	LOAD_INST(AllocateMemory);
	LOAD_INST(FreeMemory);
	LOAD_INST(MapMemory);
	LOAD_INST(UnmapMemory);

	/* Buffer */
	LOAD_INST(CreateBuffer);
	LOAD_INST(DestroyBuffer);
	LOAD_INST(BindBufferMemory);
	LOAD_INST(GetBufferMemoryRequirements);

	/* Image */
	LOAD_INST(CreateImage);
	LOAD_INST(DestroyImage);
	LOAD_INST(BindImageMemory);
	LOAD_INST(GetImageMemoryRequirements);
	LOAD_INST(CreateImageView);
	LOAD_INST(DestroyImageView);

	/* Render pass / Framebuffer */
	LOAD_INST(CreateRenderPass);
	LOAD_INST(DestroyRenderPass);
	LOAD_INST(CreateFramebuffer);
	LOAD_INST(DestroyFramebuffer);

	/* Pipeline */
	LOAD_INST(CreateShaderModule);
	LOAD_INST(DestroyShaderModule);
	LOAD_INST(CreatePipelineLayout);
	LOAD_INST(DestroyPipelineLayout);
	LOAD_INST(CreateGraphicsPipelines);
	LOAD_INST(DestroyPipeline);

	/* Fence */
	LOAD_INST(CreateFence);
	LOAD_INST(DestroyFence);
	LOAD_INST(WaitForFences);
	LOAD_INST(ResetFences);

	/* Descriptor sets */
	LOAD_INST(CreateDescriptorSetLayout);
	LOAD_INST(DestroyDescriptorSetLayout);
	LOAD_INST(CreateDescriptorPool);
	LOAD_INST(DestroyDescriptorPool);
	LOAD_INST(AllocateDescriptorSets);
	LOAD_INST(UpdateDescriptorSets);

	/* Commands */
	LOAD_INST(CmdBeginRenderPass);
	LOAD_INST(CmdEndRenderPass);
	LOAD_INST(CmdBindPipeline);
	LOAD_INST(CmdBindVertexBuffers);
	LOAD_INST(CmdBindIndexBuffer);
	LOAD_INST(CmdDraw);
	LOAD_INST(CmdDrawIndexed);
	LOAD_INST(CmdSetViewport);
	LOAD_INST(CmdSetScissor);
	LOAD_INST(CmdCopyImageToBuffer);
	LOAD_INST(CmdPipelineBarrier);
	LOAD_INST(CmdClearColorImage);
	LOAD_INST(CmdBindDescriptorSets);

#undef LOAD_INST
}

/* ============================================================
 * Physical Device 선택
 * ============================================================
 *
 * Discrete GPU 우선, 없으면 Integrated, 그래도 없으면 첫 번째.
 */
static int select_physical_device(struct vk_backend *vk)
{
	uint32_t count = 0;
	VkResult r = vk->EnumeratePhysicalDevices(vk->instance, &count, NULL);
	if (r != VK_SUCCESS || count == 0) {
		fprintf(stderr, "[VK] No physical devices found\n");
		return -1;
	}

	/* 최대 8개까지만 */
	VkPhysicalDevice devs[8];
	if (count > 8) count = 8;
	vk->EnumeratePhysicalDevices(vk->instance, &count, devs);

	/* 1차: Discrete GPU 찾기 */
	int selected = -1;
	for (uint32_t i = 0; i < count; i++) {
		VkPhysicalDeviceProperties props;
		vk->GetPhysicalDeviceProperties(devs[i], &props);
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			selected = (int)i;
			break;
		}
	}

	/* 2차: Integrated GPU */
	if (selected < 0) {
		for (uint32_t i = 0; i < count; i++) {
			VkPhysicalDeviceProperties props;
			vk->GetPhysicalDeviceProperties(devs[i], &props);
			if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
				selected = (int)i;
				break;
			}
		}
	}

	/* 3차: 아무거나 */
	if (selected < 0)
		selected = 0;

	vk->physical_device = devs[selected];

	/* GPU 정보 저장 */
	VkPhysicalDeviceProperties props;
	vk->GetPhysicalDeviceProperties(vk->physical_device, &props);
	strncpy(vk->device_name, props.deviceName, sizeof(vk->device_name) - 1);
	vk->device_name[sizeof(vk->device_name) - 1] = '\0';
	vk->device_type = props.deviceType;

	/* 메모리 프로퍼티 저장 */
	vk->GetPhysicalDeviceMemoryProperties(vk->physical_device, &vk->mem_props);

	fprintf(stderr, "[VK] Selected GPU: %s (type=%d)\n",
		vk->device_name, vk->device_type);

	return 0;
}

/* ============================================================
 * Graphics 큐 패밀리 인덱스 탐색
 * ============================================================ */
static int find_graphics_queue_family(struct vk_backend *vk)
{
	uint32_t count = 0;
	vk->GetPhysicalDeviceQueueFamilyProperties(vk->physical_device,
						   &count, NULL);
	if (count == 0)
		return -1;

	VkQueueFamilyProperties families[16];
	if (count > 16) count = 16;
	vk->GetPhysicalDeviceQueueFamilyProperties(vk->physical_device,
						   &count, families);

	for (uint32_t i = 0; i < count; i++) {
		if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			vk->graphics_queue_family = i;
			return 0;
		}
	}

	fprintf(stderr, "[VK] No graphics queue family found\n");
	return -1;
}

/* ============================================================
 * vk_backend_init — 전체 초기화
 * ============================================================
 *
 * vk_load_vulkan() 호출 후 사용.
 * Instance → Physical Device → Queue Family → Logical Device
 *   → Queue → Command Pool 순서.
 */
int vk_backend_init(struct vk_backend *vk)
{
	VkResult r;

	/* 1. VkInstance 생성 */
	VkApplicationInfo app_info = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "CITC-WCL",
		.applicationVersion = 1,
		.pEngineName = "CITC-D3D11",
		.engineVersion = 1,
		.apiVersion = (1 << 22) | (0 << 12),  /* Vulkan 1.0 */
	};

	VkInstanceCreateInfo inst_ci = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app_info,
	};

	r = vk->CreateInstance(&inst_ci, NULL, &vk->instance);
	if (r != VK_SUCCESS) {
		fprintf(stderr, "[VK] vkCreateInstance failed: %d\n", r);
		return -1;
	}

	/* Instance 함수 포인터 로드 */
	load_instance_functions(vk);

	/* 2. Physical Device 선택 */
	if (select_physical_device(vk) < 0) {
		vk->DestroyInstance(vk->instance, NULL);
		vk->instance = NULL;
		return -1;
	}

	/* 3. Graphics 큐 패밀리 탐색 */
	if (find_graphics_queue_family(vk) < 0) {
		vk->DestroyInstance(vk->instance, NULL);
		vk->instance = NULL;
		return -1;
	}

	/* 4. Logical Device 생성 */
	float queue_priority = 1.0f;
	VkDeviceQueueCreateInfo queue_ci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = vk->graphics_queue_family,
		.queueCount = 1,
		.pQueuePriorities = &queue_priority,
	};

	VkDeviceCreateInfo dev_ci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue_ci,
	};

	r = vk->CreateDevice(vk->physical_device, &dev_ci, NULL, &vk->device);
	if (r != VK_SUCCESS) {
		fprintf(stderr, "[VK] vkCreateDevice failed: %d\n", r);
		vk->DestroyInstance(vk->instance, NULL);
		vk->instance = NULL;
		return -1;
	}

	/* 5. Graphics Queue 가져오기 */
	vk->GetDeviceQueue(vk->device, vk->graphics_queue_family, 0,
			   &vk->graphics_queue);

	/* 6. Command Pool 생성 */
	VkCommandPoolCreateInfo pool_ci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = vk->graphics_queue_family,
	};

	r = vk->CreateCommandPool(vk->device, &pool_ci, NULL, &vk->cmd_pool);
	if (r != VK_SUCCESS) {
		fprintf(stderr, "[VK] vkCreateCommandPool failed: %d\n", r);
		vk->DestroyDevice(vk->device, NULL);
		vk->DestroyInstance(vk->instance, NULL);
		vk->device = NULL;
		vk->instance = NULL;
		return -1;
	}

	vk->initialized = 1;
	fprintf(stderr, "[VK] Backend initialized: %s (queue family %u)\n",
		vk->device_name, vk->graphics_queue_family);

	return 0;
}

/* ============================================================
 * vk_backend_shutdown — 역순 파괴
 * ============================================================ */
void vk_backend_shutdown(struct vk_backend *vk)
{
	if (!vk->initialized)
		return;

	if (vk->device) {
		vk->QueueWaitIdle(vk->graphics_queue);

		if (vk->cmd_pool)
			vk->DestroyCommandPool(vk->device, vk->cmd_pool, NULL);
		vk->DestroyDevice(vk->device, NULL);
	}

	if (vk->instance)
		vk->DestroyInstance(vk->instance, NULL);

	if (vk->lib_handle)
		dlclose(vk->lib_handle);

	memset(vk, 0, sizeof(*vk));
}

/* ============================================================
 * vk_find_memory_type — 메모리 타입 검색
 * ============================================================
 *
 * GPU 메모리 할당 시 필요한 메모리 타입 인덱스를 찾는다.
 * type_filter: VkMemoryRequirements.memoryTypeBits
 * properties:  원하는 VK_MEMORY_PROPERTY_* 플래그
 */
int vk_find_memory_type(struct vk_backend *vk, uint32_t type_filter,
                        VkMemoryPropertyFlags properties)
{
	for (uint32_t i = 0; i < vk->mem_props.memoryTypeCount; i++) {
		if ((type_filter & (1u << i)) &&
		    (vk->mem_props.memoryTypes[i].propertyFlags & properties) == properties)
			return (int)i;
	}
	return -1;
}

/* ============================================================
 * 헬퍼: 단일 커맨드 버퍼 실행
 * ============================================================
 *
 * begin → (호출자가 커맨드 녹화) → end → submit → wait.
 */
static VkCommandBuffer begin_one_shot(struct vk_backend *vk)
{
	VkCommandBufferAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = vk->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cmd;
	if (vk->AllocateCommandBuffers(vk->device, &ai, &cmd) != VK_SUCCESS)
		return NULL;

	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->BeginCommandBuffer(cmd, &bi);
	return cmd;
}

static void end_and_submit(struct vk_backend *vk, VkCommandBuffer cmd)
{
	vk->EndCommandBuffer(cmd);

	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmd,
	};
	vk->QueueSubmit(vk->graphics_queue, 1, &si, VK_NULL_HANDLE);
	vk->QueueWaitIdle(vk->graphics_queue);

	vk->FreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
}

/* ============================================================
 * vk_create_render_target — 오프스크린 렌더 타깃 생성
 * ============================================================
 *
 * VkImage(R8G8B8A8_UNORM) + VkImageView + VkRenderPass + VkFramebuffer
 * + Staging Buffer(readback용) + Command Buffer.
 */
int vk_create_render_target(struct vk_backend *vk, struct vk_render_target *rt,
                            uint32_t width, uint32_t height)
{
	VkResult r;
	memset(rt, 0, sizeof(*rt));
	rt->width = width;
	rt->height = height;

	/* 1. VkImage 생성 */
	VkImageCreateInfo img_ci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { width, height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	r = vk->CreateImage(vk->device, &img_ci, NULL, &rt->image);
	if (r != VK_SUCCESS) {
		fprintf(stderr, "[VK] CreateImage failed: %d\n", r);
		return -1;
	}

	/* 이미지 메모리 할당 + 바인드 */
	VkMemoryRequirements mem_req;
	vk->GetImageMemoryRequirements(vk->device, rt->image, &mem_req);

	int mem_type = vk_find_memory_type(vk, (uint32_t)mem_req.memoryTypeBits,
					   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mem_type < 0) {
		fprintf(stderr, "[VK] No suitable memory type for image\n");
		vk->DestroyImage(vk->device, rt->image, NULL);
		return -1;
	}

	VkMemoryAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_req.size,
		.memoryTypeIndex = (uint32_t)mem_type,
	};

	r = vk->AllocateMemory(vk->device, &alloc_info, NULL, &rt->image_memory);
	if (r != VK_SUCCESS) {
		vk->DestroyImage(vk->device, rt->image, NULL);
		return -1;
	}
	vk->BindImageMemory(vk->device, rt->image, rt->image_memory, 0);

	/* 2. VkImageView */
	VkImageViewCreateInfo iv_ci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = rt->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.components = { VK_COMPONENT_SWIZZLE_IDENTITY,
				VK_COMPONENT_SWIZZLE_IDENTITY,
				VK_COMPONENT_SWIZZLE_IDENTITY,
				VK_COMPONENT_SWIZZLE_IDENTITY },
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0, .levelCount = 1,
			.baseArrayLayer = 0, .layerCount = 1,
		},
	};

	r = vk->CreateImageView(vk->device, &iv_ci, NULL, &rt->image_view);
	if (r != VK_SUCCESS) {
		vk->FreeMemory(vk->device, rt->image_memory, NULL);
		vk->DestroyImage(vk->device, rt->image, NULL);
		return -1;
	}

	/* 2b. Depth Image (D32_SFLOAT) */
	VkImageCreateInfo depth_ci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_D32_SFLOAT,
		.extent = { width, height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	r = vk->CreateImage(vk->device, &depth_ci, NULL, &rt->depth_image);
	if (r != VK_SUCCESS) {
		fprintf(stderr, "[VK] Depth CreateImage failed: %d\n", r);
		vk->DestroyImageView(vk->device, rt->image_view, NULL);
		vk->FreeMemory(vk->device, rt->image_memory, NULL);
		vk->DestroyImage(vk->device, rt->image, NULL);
		return -1;
	}

	VkMemoryRequirements depth_req;
	vk->GetImageMemoryRequirements(vk->device, rt->depth_image, &depth_req);
	int depth_mem_type = vk_find_memory_type(vk,
		(uint32_t)depth_req.memoryTypeBits,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (depth_mem_type < 0) {
		vk->DestroyImage(vk->device, rt->depth_image, NULL);
		vk->DestroyImageView(vk->device, rt->image_view, NULL);
		vk->FreeMemory(vk->device, rt->image_memory, NULL);
		vk->DestroyImage(vk->device, rt->image, NULL);
		return -1;
	}

	VkMemoryAllocateInfo depth_alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = depth_req.size,
		.memoryTypeIndex = (uint32_t)depth_mem_type,
	};
	r = vk->AllocateMemory(vk->device, &depth_alloc, NULL, &rt->depth_memory);
	if (r != VK_SUCCESS) {
		vk->DestroyImage(vk->device, rt->depth_image, NULL);
		vk->DestroyImageView(vk->device, rt->image_view, NULL);
		vk->FreeMemory(vk->device, rt->image_memory, NULL);
		vk->DestroyImage(vk->device, rt->image, NULL);
		return -1;
	}
	vk->BindImageMemory(vk->device, rt->depth_image, rt->depth_memory, 0);

	VkImageViewCreateInfo div_ci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = rt->depth_image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_D32_SFLOAT,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
			.baseMipLevel = 0, .levelCount = 1,
			.baseArrayLayer = 0, .layerCount = 1,
		},
	};
	r = vk->CreateImageView(vk->device, &div_ci, NULL, &rt->depth_view);
	if (r != VK_SUCCESS) {
		vk->FreeMemory(vk->device, rt->depth_memory, NULL);
		vk->DestroyImage(vk->device, rt->depth_image, NULL);
		vk->DestroyImageView(vk->device, rt->image_view, NULL);
		vk->FreeMemory(vk->device, rt->image_memory, NULL);
		vk->DestroyImage(vk->device, rt->image, NULL);
		return -1;
	}
	rt->has_depth = 1;

	/* 3. VkRenderPass (color + depth) */
	VkAttachmentDescription attachments[2] = {
		{	/* [0] Color */
			.format = VK_FORMAT_R8G8B8A8_UNORM,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		},
		{	/* [1] Depth */
			.format = VK_FORMAT_D32_SFLOAT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		},
	};

	VkAttachmentReference color_ref = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	VkAttachmentReference depth_ref = {
		.attachment = 1,
		.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	};

	VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color_ref,
		.pDepthStencilAttachment = &depth_ref,
	};

	VkRenderPassCreateInfo rp_ci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 2,
		.pAttachments = attachments,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};

	r = vk->CreateRenderPass(vk->device, &rp_ci, NULL, &rt->render_pass);
	if (r != VK_SUCCESS) {
		vk->DestroyImageView(vk->device, rt->depth_view, NULL);
		vk->FreeMemory(vk->device, rt->depth_memory, NULL);
		vk->DestroyImage(vk->device, rt->depth_image, NULL);
		vk->DestroyImageView(vk->device, rt->image_view, NULL);
		vk->FreeMemory(vk->device, rt->image_memory, NULL);
		vk->DestroyImage(vk->device, rt->image, NULL);
		return -1;
	}

	/* 4. VkFramebuffer (color + depth) */
	VkImageView fb_attachments[2] = { rt->image_view, rt->depth_view };

	VkFramebufferCreateInfo fb_ci = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = rt->render_pass,
		.attachmentCount = 2,
		.pAttachments = fb_attachments,
		.width = width,
		.height = height,
		.layers = 1,
	};

	r = vk->CreateFramebuffer(vk->device, &fb_ci, NULL, &rt->framebuffer);
	if (r != VK_SUCCESS) {
		vk->DestroyRenderPass(vk->device, rt->render_pass, NULL);
		vk->DestroyImageView(vk->device, rt->depth_view, NULL);
		vk->FreeMemory(vk->device, rt->depth_memory, NULL);
		vk->DestroyImage(vk->device, rt->depth_image, NULL);
		vk->DestroyImageView(vk->device, rt->image_view, NULL);
		vk->FreeMemory(vk->device, rt->image_memory, NULL);
		vk->DestroyImage(vk->device, rt->image, NULL);
		return -1;
	}

	/* 5. Staging Buffer (readback용, host visible) */
	rt->staging_size = (VkDeviceSize)width * height * 4;

	VkBufferCreateInfo buf_ci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = rt->staging_size,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	r = vk->CreateBuffer(vk->device, &buf_ci, NULL, &rt->staging_buf);
	if (r != VK_SUCCESS) {
		vk->DestroyFramebuffer(vk->device, rt->framebuffer, NULL);
		vk->DestroyRenderPass(vk->device, rt->render_pass, NULL);
		vk->DestroyImageView(vk->device, rt->image_view, NULL);
		vk->FreeMemory(vk->device, rt->image_memory, NULL);
		vk->DestroyImage(vk->device, rt->image, NULL);
		return -1;
	}

	VkMemoryRequirements buf_req;
	vk->GetBufferMemoryRequirements(vk->device, rt->staging_buf, &buf_req);

	int buf_mem_type = vk_find_memory_type(vk, (uint32_t)buf_req.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (buf_mem_type < 0) {
		fprintf(stderr, "[VK] No host visible memory for staging\n");
		vk->DestroyBuffer(vk->device, rt->staging_buf, NULL);
		vk->DestroyFramebuffer(vk->device, rt->framebuffer, NULL);
		vk->DestroyRenderPass(vk->device, rt->render_pass, NULL);
		vk->DestroyImageView(vk->device, rt->image_view, NULL);
		vk->FreeMemory(vk->device, rt->image_memory, NULL);
		vk->DestroyImage(vk->device, rt->image, NULL);
		return -1;
	}

	VkMemoryAllocateInfo buf_alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = buf_req.size,
		.memoryTypeIndex = (uint32_t)buf_mem_type,
	};

	r = vk->AllocateMemory(vk->device, &buf_alloc, NULL, &rt->staging_mem);
	if (r != VK_SUCCESS) {
		vk->DestroyBuffer(vk->device, rt->staging_buf, NULL);
		vk->DestroyFramebuffer(vk->device, rt->framebuffer, NULL);
		vk->DestroyRenderPass(vk->device, rt->render_pass, NULL);
		vk->DestroyImageView(vk->device, rt->image_view, NULL);
		vk->FreeMemory(vk->device, rt->image_memory, NULL);
		vk->DestroyImage(vk->device, rt->image, NULL);
		return -1;
	}
	vk->BindBufferMemory(vk->device, rt->staging_buf, rt->staging_mem, 0);

	/* 6. Command Buffer 할당 */
	VkCommandBufferAllocateInfo cb_ai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = vk->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	r = vk->AllocateCommandBuffers(vk->device, &cb_ai, &rt->cmd);
	if (r != VK_SUCCESS) {
		vk->FreeMemory(vk->device, rt->staging_mem, NULL);
		vk->DestroyBuffer(vk->device, rt->staging_buf, NULL);
		vk->DestroyFramebuffer(vk->device, rt->framebuffer, NULL);
		vk->DestroyRenderPass(vk->device, rt->render_pass, NULL);
		vk->DestroyImageView(vk->device, rt->image_view, NULL);
		vk->FreeMemory(vk->device, rt->image_memory, NULL);
		vk->DestroyImage(vk->device, rt->image, NULL);
		return -1;
	}

	rt->active = 1;
	return 0;
}

/* ============================================================
 * vk_destroy_render_target
 * ============================================================ */
void vk_destroy_render_target(struct vk_backend *vk, struct vk_render_target *rt)
{
	if (!rt->active)
		return;

	vk->QueueWaitIdle(vk->graphics_queue);

	if (rt->cmd)
		vk->FreeCommandBuffers(vk->device, vk->cmd_pool, 1, &rt->cmd);
	if (rt->staging_mem)
		vk->FreeMemory(vk->device, rt->staging_mem, NULL);
	if (rt->staging_buf)
		vk->DestroyBuffer(vk->device, rt->staging_buf, NULL);
	if (rt->framebuffer)
		vk->DestroyFramebuffer(vk->device, rt->framebuffer, NULL);
	if (rt->render_pass)
		vk->DestroyRenderPass(vk->device, rt->render_pass, NULL);
	if (rt->depth_view)
		vk->DestroyImageView(vk->device, rt->depth_view, NULL);
	if (rt->depth_memory)
		vk->FreeMemory(vk->device, rt->depth_memory, NULL);
	if (rt->depth_image)
		vk->DestroyImage(vk->device, rt->depth_image, NULL);
	if (rt->image_view)
		vk->DestroyImageView(vk->device, rt->image_view, NULL);
	if (rt->image_memory)
		vk->FreeMemory(vk->device, rt->image_memory, NULL);
	if (rt->image)
		vk->DestroyImage(vk->device, rt->image, NULL);

	memset(rt, 0, sizeof(*rt));
}

/* ============================================================
 * vk_clear_color — GPU ClearRenderTargetView
 * ============================================================
 *
 * 1. Transition UNDEFINED → TRANSFER_DST
 * 2. vkCmdClearColorImage
 * 3. Transition → COLOR_ATTACHMENT_OPTIMAL
 * 4. Submit + Wait
 */
int vk_clear_color(struct vk_backend *vk, struct vk_render_target *rt,
                   float r, float g, float b, float a)
{
	VkCommandBuffer cmd = begin_one_shot(vk);
	if (!cmd) return -1;

	/* Transition to TRANSFER_DST */
	VkImageMemoryBarrier barrier1 = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = 0xFFFFFFFF, /* VK_QUEUE_FAMILY_IGNORED */
		.dstQueueFamilyIndex = 0xFFFFFFFF,
		.image = rt->image,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0, .levelCount = 1,
			.baseArrayLayer = 0, .layerCount = 1,
		},
	};
	vk->CmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 0, NULL, 1, &barrier1);

	/* Clear */
	VkClearColorValue clear_color = { .float32 = { r, g, b, a } };
	VkImageSubresourceRange range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0, .levelCount = 1,
		.baseArrayLayer = 0, .layerCount = 1,
	};
	vk->CmdClearColorImage(cmd, rt->image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		&clear_color, 1, &range);

	/* Transition to COLOR_ATTACHMENT_OPTIMAL */
	VkImageMemoryBarrier barrier2 = barrier1;
	barrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier2.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier2.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	vk->CmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, NULL, 0, NULL, 1, &barrier2);

	end_and_submit(vk, cmd);
	return 0;
}

/* ============================================================
 * vk_readback_pixels — GPU → CPU 픽셀 복사
 * ============================================================
 *
 * 1. Transition COLOR_ATTACHMENT → TRANSFER_SRC
 * 2. vkCmdCopyImageToBuffer (→ staging)
 * 3. Submit + Wait
 * 4. Map staging → memcpy → Unmap
 *
 * 출력: R8G8B8A8 → XRGB8888 (alpha 무시, 기존 SW 래스터라이저와 호환)
 */
int vk_readback_pixels(struct vk_backend *vk, struct vk_render_target *rt,
                       uint32_t *out_pixels)
{
	VkCommandBuffer cmd = begin_one_shot(vk);
	if (!cmd) return -1;

	/* Transition to TRANSFER_SRC */
	VkImageMemoryBarrier barrier1 = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = 0xFFFFFFFF,
		.dstQueueFamilyIndex = 0xFFFFFFFF,
		.image = rt->image,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0, .levelCount = 1,
			.baseArrayLayer = 0, .layerCount = 1,
		},
	};
	vk->CmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 0, NULL, 1, &barrier1);

	/* Copy image → staging buffer */
	VkBufferImageCopy region = {
		.bufferOffset = 0,
		.bufferRowLength = 0,
		.bufferImageHeight = 0,
		.imageSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel = 0,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
		.imageOffset = { 0, 0, 0 },
		.imageExtent = { rt->width, rt->height, 1 },
	};
	vk->CmdCopyImageToBuffer(cmd, rt->image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		rt->staging_buf, 1, &region);

	/* Transition back to COLOR_ATTACHMENT_OPTIMAL */
	VkImageMemoryBarrier barrier2 = barrier1;
	barrier2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier2.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier2.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	vk->CmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, NULL, 0, NULL, 1, &barrier2);

	end_and_submit(vk, cmd);

	/* Map + copy to CPU buffer */
	void *mapped;
	VkResult res = vk->MapMemory(vk->device, rt->staging_mem, 0,
				     rt->staging_size, 0, &mapped);
	if (res != VK_SUCCESS)
		return -1;

	/* R8G8B8A8 → XRGB8888 변환
	 * Vulkan: R(byte0) G(byte1) B(byte2) A(byte3)
	 * SW 버퍼: 0x00RRGGBB (little-endian = BB GG RR 00)
	 */
	const uint8_t *src = (const uint8_t *)mapped;
	uint32_t total = rt->width * rt->height;
	for (uint32_t i = 0; i < total; i++) {
		uint8_t rv = src[i * 4 + 0];
		uint8_t gv = src[i * 4 + 1];
		uint8_t bv = src[i * 4 + 2];
		out_pixels[i] = ((uint32_t)rv << 16) | ((uint32_t)gv << 8) | bv;
	}

	vk->UnmapMemory(vk->device, rt->staging_mem);
	return 0;
}

#endif /* CITC_VULKAN_ENABLED */
