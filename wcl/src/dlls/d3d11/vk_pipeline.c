/*
 * vk_pipeline.c — Vulkan 그래픽스 파이프라인 구현
 * =================================================
 *
 * GPU에서 삼각형을 래스터라이즈.
 *
 * 기본 파이프라인:
 *   - 하드코딩된 SPIR-V VS/PS (pass-through position + interpolate color)
 *   - Vertex input: float3 pos (location 0) + float4 color (location 1)
 *   - Triangle list, solid fill, no cull, no depth, no blend
 *   - Dynamic viewport/scissor
 *
 * Class 42: 기본 Draw
 * Class 44: SPIR-V + CB + Depth 통합
 */

#ifdef CITC_VULKAN_ENABLED

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "vk_pipeline.h"

/* ============================================================
 * 하드코딩 SPIR-V 셰이더
 * ============================================================
 *
 * Vertex Shader:
 *   layout(location=0) in vec3 inPos;
 *   layout(location=1) in vec4 inColor;
 *   layout(location=0) out vec4 outColor;
 *   void main() {
 *       gl_Position = vec4(inPos, 1.0);
 *       outColor = inColor;
 *   }
 *
 * Fragment Shader:
 *   layout(location=0) in vec4 inColor;
 *   layout(location=0) out vec4 outFragColor;
 *   void main() {
 *       outFragColor = inColor;
 *   }
 */

/* SPIR-V Vertex Shader — pass-through position + color */
static const uint32_t spirv_vs[] = {
	/* Header */
	0x07230203, /* SPIR-V magic */
	0x00010000, /* Version 1.0 */
	0x00000000, /* Generator */
	0x00000020, /* Bound (max ID + 1 = 32) */
	0x00000000, /* Schema */

	/* OpCapability Shader */
	0x00020011, 0x00000001,

	/* OpMemoryModel Logical GLSL450 */
	0x00030006, 0x00000001, 0x00000001,

	/* OpEntryPoint Vertex %main "main" %inPos %inColor %outColor %gl_Position */
	0x000a000F, 0x00000000, 0x00000002, 0x6e69616d, 0x00000000,
	0x00000003, 0x00000004, 0x00000005, 0x00000006,

	/* OpDecorate %inPos Location 0 */
	0x00040047, 0x00000003, 0x0000001E, 0x00000000,
	/* OpDecorate %inColor Location 1 */
	0x00040047, 0x00000004, 0x0000001E, 0x00000001,
	/* OpDecorate %outColor Location 0 */
	0x00040047, 0x00000005, 0x0000001E, 0x00000000,
	/* OpDecorate %gl_Position BuiltIn Position */
	0x00040047, 0x00000006, 0x0000000B, 0x00000000,

	/* Types */
	/* %void = OpTypeVoid */
	0x00020013, 0x00000007,
	/* %func_void = OpTypeFunction %void */
	0x00030021, 0x00000008, 0x00000007,
	/* %float = OpTypeFloat 32 */
	0x00030016, 0x00000009, 0x00000020,
	/* %vec3 = OpTypeVector %float 3 */
	0x00040017, 0x0000000A, 0x00000009, 0x00000003,
	/* %vec4 = OpTypeVector %float 4 */
	0x00040017, 0x0000000B, 0x00000009, 0x00000004,

	/* Pointer types */
	/* %ptr_in_vec3 = OpTypePointer Input %vec3 */
	0x00040020, 0x0000000C, 0x00000001, 0x0000000A,
	/* %ptr_in_vec4 = OpTypePointer Input %vec4 */
	0x00040020, 0x0000000D, 0x00000001, 0x0000000B,
	/* %ptr_out_vec4 = OpTypePointer Output %vec4 */
	0x00040020, 0x0000000E, 0x00000003, 0x0000000B,

	/* Variables */
	/* %inPos = OpVariable %ptr_in_vec3 Input */
	0x0004003B, 0x0000000C, 0x00000003, 0x00000001,
	/* %inColor = OpVariable %ptr_in_vec4 Input */
	0x0004003B, 0x0000000D, 0x00000004, 0x00000001,
	/* %outColor = OpVariable %ptr_out_vec4 Output */
	0x0004003B, 0x0000000E, 0x00000005, 0x00000003,
	/* %gl_Position = OpVariable %ptr_out_vec4 Output */
	0x0004003B, 0x0000000E, 0x00000006, 0x00000003,

	/* Constants */
	/* %float_1 = OpConstant %float 1.0 */
	0x0004002B, 0x00000009, 0x0000000F, 0x3F800000,

	/* Function */
	/* %main = OpFunction %void None %func_void */
	0x00050036, 0x00000007, 0x00000002, 0x00000000, 0x00000008,
	/* %entry = OpLabel */
	0x000200F8, 0x00000010,

	/* %pos3 = OpLoad %vec3 %inPos */
	0x0004003D, 0x0000000A, 0x00000011, 0x00000003,
	/* %col4 = OpLoad %vec4 %inColor */
	0x0004003D, 0x0000000B, 0x00000012, 0x00000004,

	/* %pos4 = OpCompositeConstruct %vec4 %pos3.x %pos3.y %pos3.z %float_1 */
	/* — need to extract x,y,z from vec3 first */
	/* %px = OpCompositeExtract %float %pos3 0 */
	0x00050051, 0x00000009, 0x00000013, 0x00000011, 0x00000000,
	/* %py = OpCompositeExtract %float %pos3 1 */
	0x00050051, 0x00000009, 0x00000014, 0x00000011, 0x00000001,
	/* %pz = OpCompositeExtract %float %pos3 2 */
	0x00050051, 0x00000009, 0x00000015, 0x00000011, 0x00000002,
	/* %pos4 = OpCompositeConstruct %vec4 %px %py %pz %float_1 */
	0x00070050, 0x0000000B, 0x00000016, 0x00000013, 0x00000014,
	0x00000015, 0x0000000F,

	/* OpStore %gl_Position %pos4 */
	0x0003003E, 0x00000006, 0x00000016,
	/* OpStore %outColor %col4 */
	0x0003003E, 0x00000005, 0x00000012,

	/* OpReturn */
	0x000100FD,
	/* OpFunctionEnd */
	0x00010038,
};

/* SPIR-V Fragment Shader — pass-through color */
static const uint32_t spirv_ps[] = {
	/* Header */
	0x07230203, /* SPIR-V magic */
	0x00010000, /* Version 1.0 */
	0x00000000, /* Generator */
	0x00000010, /* Bound = 16 */
	0x00000000, /* Schema */

	/* OpCapability Shader */
	0x00020011, 0x00000001,

	/* OpMemoryModel Logical GLSL450 */
	0x00030006, 0x00000001, 0x00000001,

	/* OpEntryPoint Fragment %main "main" %inColor %outColor */
	0x0008000F, 0x00000004, 0x00000002, 0x6e69616d, 0x00000000,
	0x00000003, 0x00000004,

	/* OpExecutionMode %main OriginUpperLeft */
	0x00030010, 0x00000002, 0x00000007,

	/* OpDecorate %inColor Location 0 */
	0x00040047, 0x00000003, 0x0000001E, 0x00000000,
	/* OpDecorate %outColor Location 0 */
	0x00040047, 0x00000004, 0x0000001E, 0x00000000,

	/* Types */
	/* %void = OpTypeVoid */
	0x00020013, 0x00000005,
	/* %func_void = OpTypeFunction %void */
	0x00030021, 0x00000006, 0x00000005,
	/* %float = OpTypeFloat 32 */
	0x00030016, 0x00000007, 0x00000020,
	/* %vec4 = OpTypeVector %float 4 */
	0x00040017, 0x00000008, 0x00000007, 0x00000004,
	/* %ptr_in_vec4 = OpTypePointer Input %vec4 */
	0x00040020, 0x00000009, 0x00000001, 0x00000008,
	/* %ptr_out_vec4 = OpTypePointer Output %vec4 */
	0x00040020, 0x0000000A, 0x00000003, 0x00000008,

	/* Variables */
	/* %inColor = OpVariable %ptr_in_vec4 Input */
	0x0004003B, 0x00000009, 0x00000003, 0x00000001,
	/* %outColor = OpVariable %ptr_out_vec4 Output */
	0x0004003B, 0x0000000A, 0x00000004, 0x00000003,

	/* Function */
	/* %main = OpFunction %void None %func_void */
	0x00050036, 0x00000005, 0x00000002, 0x00000000, 0x00000006,
	/* %entry = OpLabel */
	0x000200F8, 0x0000000B,

	/* %color = OpLoad %vec4 %inColor */
	0x0004003D, 0x00000008, 0x0000000C, 0x00000003,
	/* OpStore %outColor %color */
	0x0003003E, 0x00000004, 0x0000000C,

	/* OpReturn */
	0x000100FD,
	/* OpFunctionEnd */
	0x00010038,
};

/* ============================================================
 * vk_create_buffer — 범용 GPU 버퍼 생성
 * ============================================================ */
int vk_create_buffer(struct vk_backend *vk, struct vk_gpu_buffer *buf,
                     VkDeviceSize size, VkBufferUsageFlags usage,
                     VkMemoryPropertyFlags mem_flags)
{
	memset(buf, 0, sizeof(*buf));

	VkBufferCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	VkResult r = vk->CreateBuffer(vk->device, &ci, NULL, &buf->buffer);
	if (r != VK_SUCCESS) return -1;

	VkMemoryRequirements req;
	vk->GetBufferMemoryRequirements(vk->device, buf->buffer, &req);

	int mt = vk_find_memory_type(vk, req.memoryTypeBits, mem_flags);
	if (mt < 0) {
		vk->DestroyBuffer(vk->device, buf->buffer, NULL);
		return -1;
	}

	VkMemoryAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = (uint32_t)mt,
	};

	r = vk->AllocateMemory(vk->device, &ai, NULL, &buf->memory);
	if (r != VK_SUCCESS) {
		vk->DestroyBuffer(vk->device, buf->buffer, NULL);
		return -1;
	}

	vk->BindBufferMemory(vk->device, buf->buffer, buf->memory, 0);
	buf->size = size;
	return 0;
}

void vk_destroy_buffer(struct vk_backend *vk, struct vk_gpu_buffer *buf)
{
	if (buf->buffer)
		vk->DestroyBuffer(vk->device, buf->buffer, NULL);
	if (buf->memory)
		vk->FreeMemory(vk->device, buf->memory, NULL);
	memset(buf, 0, sizeof(*buf));
}

int vk_upload_buffer(struct vk_backend *vk, struct vk_gpu_buffer *buf,
                     const void *data, VkDeviceSize size)
{
	void *mapped;
	VkResult r = vk->MapMemory(vk->device, buf->memory, 0, size, 0, &mapped);
	if (r != VK_SUCCESS) return -1;
	memcpy(mapped, data, (size_t)size);
	vk->UnmapMemory(vk->device, buf->memory);
	return 0;
}

/* ============================================================
 * vk_create_default_pipeline
 * ============================================================
 *
 * 하드코딩된 SPIR-V VS/PS로 기본 파이프라인 생성.
 * Vertex input: float3 pos (offset 0) + float4 color (offset 12).
 * Stride = 28 bytes (3*4 + 4*4).
 */
int vk_create_default_pipeline(struct vk_backend *vk,
                               struct vk_render_target *rt,
                               VkPipeline *out_pipeline,
                               VkPipelineLayout *out_layout)
{
	VkResult r;

	/* Shader modules */
	VkShaderModuleCreateInfo vs_ci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(spirv_vs),
		.pCode = spirv_vs,
	};
	VkShaderModule vs_mod;
	r = vk->CreateShaderModule(vk->device, &vs_ci, NULL, &vs_mod);
	if (r != VK_SUCCESS) {
		fprintf(stderr, "[VK] VS ShaderModule failed: %d\n", r);
		return -1;
	}

	VkShaderModuleCreateInfo ps_ci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(spirv_ps),
		.pCode = spirv_ps,
	};
	VkShaderModule ps_mod;
	r = vk->CreateShaderModule(vk->device, &ps_ci, NULL, &ps_mod);
	if (r != VK_SUCCESS) {
		vk->DestroyShaderModule(vk->device, vs_mod, NULL);
		fprintf(stderr, "[VK] PS ShaderModule failed: %d\n", r);
		return -1;
	}

	/* Shader stages */
	VkPipelineShaderStageCreateInfo stages[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vs_mod,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = ps_mod,
			.pName = "main",
		},
	};

	/* Vertex input: float3 pos + float4 color = 28 bytes stride */
	VkVertexInputBindingDescription bind = {
		.binding = 0,
		.stride = 28, /* 3*4 + 4*4 */
		.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
	};

	VkVertexInputAttributeDescription attrs[2] = {
		{ .location = 0, .binding = 0,
		  .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 },
		{ .location = 1, .binding = 0,
		  .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 12 },
	};

	VkPipelineVertexInputStateCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &bind,
		.vertexAttributeDescriptionCount = 2,
		.pVertexAttributeDescriptions = attrs,
	};

	/* Input assembly */
	VkPipelineInputAssemblyStateCreateInfo ia = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};

	/* Viewport/scissor — dynamic */
	VkPipelineViewportStateCreateInfo vp = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};

	/* Rasterizer */
	VkPipelineRasterizationStateCreateInfo rs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_CLOCKWISE,
		.lineWidth = 1.0f,
	};

	/* Multisample */
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	/* Color blend — no blend, write all */
	VkPipelineColorBlendAttachmentState cba = {
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
		                  VK_COLOR_COMPONENT_G_BIT |
		                  VK_COLOR_COMPONENT_B_BIT |
		                  VK_COLOR_COMPONENT_A_BIT,
	};

	VkPipelineColorBlendStateCreateInfo cb = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &cba,
	};

	/* Dynamic state */
	VkDynamicState dyn_states[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};

	VkPipelineDynamicStateCreateInfo dyn = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = dyn_states,
	};

	/* Pipeline layout (빈 레이아웃) */
	VkPipelineLayoutCreateInfo pl_ci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	};

	r = vk->CreatePipelineLayout(vk->device, &pl_ci, NULL, out_layout);
	if (r != VK_SUCCESS) {
		vk->DestroyShaderModule(vk->device, vs_mod, NULL);
		vk->DestroyShaderModule(vk->device, ps_mod, NULL);
		return -1;
	}

	/* Graphics pipeline */
	VkGraphicsPipelineCreateInfo gp_ci = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &vi,
		.pInputAssemblyState = &ia,
		.pViewportState = &vp,
		.pRasterizationState = &rs,
		.pMultisampleState = &ms,
		.pColorBlendState = &cb,
		.pDynamicState = &dyn,
		.layout = *out_layout,
		.renderPass = rt->render_pass,
		.subpass = 0,
	};

	r = vk->CreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1,
					&gp_ci, NULL, out_pipeline);

	vk->DestroyShaderModule(vk->device, vs_mod, NULL);
	vk->DestroyShaderModule(vk->device, ps_mod, NULL);

	if (r != VK_SUCCESS) {
		fprintf(stderr, "[VK] CreateGraphicsPipelines failed: %d\n", r);
		vk->DestroyPipelineLayout(vk->device, *out_layout, NULL);
		return -1;
	}

	return 0;
}

/* ============================================================
 * vk_draw — GPU Draw 실행
 * ============================================================
 *
 * CommandBuffer 녹화:
 *   begin renderpass (clear color 유지) → bind pipeline →
 *   bind VB → set viewport/scissor → draw → end renderpass
 *   → submit → wait
 */
int vk_draw(struct vk_backend *vk, struct vk_render_target *rt,
            VkPipeline pipeline, VkPipelineLayout layout,
            struct vk_gpu_buffer *vb,
            uint32_t vertex_count, uint32_t vertex_stride,
            int width, int height)
{
	(void)layout; (void)vertex_stride;

	/* Command buffer 할당 */
	VkCommandBufferAllocateInfo cb_ai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = vk->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cmd;
	if (vk->AllocateCommandBuffers(vk->device, &cb_ai, &cmd) != VK_SUCCESS)
		return -1;

	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->BeginCommandBuffer(cmd, &bi);

	/* Render pass begin (clear values: color + depth) */
	VkClearValue clear_vals[2] = {
		{ .color = { .float32 = { 0, 0, 0, 1 } } },
		{ .depthStencil = { 1.0f, 0 } },
	};
	VkRenderPassBeginInfo rp_bi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rt->render_pass,
		.framebuffer = rt->framebuffer,
		.renderArea = { .offset = { 0, 0 },
				.extent = { rt->width, rt->height } },
		.clearValueCount = 2,
		.pClearValues = clear_vals,
	};
	vk->CmdBeginRenderPass(cmd, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

	/* Bind pipeline */
	vk->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

	/* Bind vertex buffer */
	VkDeviceSize offset = 0;
	vk->CmdBindVertexBuffers(cmd, 0, 1, &vb->buffer, &offset);

	/* Set viewport */
	VkViewport viewport = {
		.x = 0.0f, .y = 0.0f,
		.width = (float)width, .height = (float)height,
		.minDepth = 0.0f, .maxDepth = 1.0f,
	};
	vk->CmdSetViewport(cmd, 0, 1, &viewport);

	/* Set scissor */
	VkRect2D scissor = {
		.offset = { 0, 0 },
		.extent = { (uint32_t)width, (uint32_t)height },
	};
	vk->CmdSetScissor(cmd, 0, 1, &scissor);

	/* Draw */
	vk->CmdDraw(cmd, vertex_count, 1, 0, 0);

	/* End render pass */
	vk->CmdEndRenderPass(cmd);

	/* Submit */
	vk->EndCommandBuffer(cmd);

	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmd,
	};
	vk->QueueSubmit(vk->graphics_queue, 1, &si, VK_NULL_HANDLE);
	vk->QueueWaitIdle(vk->graphics_queue);

	vk->FreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
	return 0;
}

/* ============================================================
 * Class 44: 사용자 SPIR-V 파이프라인 생성
 * ============================================================
 *
 * DXBC→SPIR-V로 컴파일된 셰이더로 VkPipeline을 생성.
 * UBO(constant buffer) 지원: set 0, binding 0.
 * Depth test 지원: D32_SFLOAT LESS_OR_EQUAL.
 */

/* 파이프라인 캐시 */

struct vk_cached_pipeline *vk_cache_find(struct vk_pipeline_cache *cache,
                                         const uint32_t *vs, const uint32_t *ps,
                                         int depth_test)
{
	for (int i = 0; i < cache->count; i++) {
		struct vk_cached_pipeline *e = &cache->entries[i];
		if (e->vs_spirv == vs && e->ps_spirv == ps &&
		    e->depth_test == depth_test)
			return e;
	}
	return NULL;
}

struct vk_cached_pipeline *vk_cache_insert(struct vk_pipeline_cache *cache)
{
	if (cache->count >= VK_MAX_CACHED_PIPELINES)
		return NULL; /* 캐시 꽉 참 — 가장 단순한 전략 */
	return &cache->entries[cache->count++];
}

/* Descriptor Set 할당 */
int vk_alloc_descriptor_set(struct vk_backend *vk,
                            VkDescriptorSetLayout layout,
                            VkDescriptorPool pool,
                            VkDescriptorSet *out_set)
{
	VkDescriptorSetAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &layout,
	};
	return vk->AllocateDescriptorSets(vk->device, &ai, out_set)
		== VK_SUCCESS ? 0 : -1;
}

/* UBO descriptor 업데이트 */
void vk_update_ubo_descriptor(struct vk_backend *vk,
                              VkDescriptorSet ds,
                              struct vk_gpu_buffer *ubo)
{
	VkDescriptorBufferInfo buf_info = {
		.buffer = ubo->buffer,
		.offset = 0,
		.range = ubo->size,
	};
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = ds,
		.dstBinding = 0,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pBufferInfo = &buf_info,
	};
	vk->UpdateDescriptorSets(vk->device, 1, &write, 0, NULL);
}

/* 사용자 SPIR-V 파이프라인 */
int vk_create_user_pipeline(struct vk_backend *vk,
                            struct vk_render_target *rt,
                            const uint32_t *vs_spirv, size_t vs_size,
                            const uint32_t *ps_spirv, size_t ps_size,
                            uint32_t vertex_stride,
                            int num_attrs,
                            int has_ubo,
                            int depth_test,
                            VkPipeline *out_pipeline,
                            VkPipelineLayout *out_layout,
                            VkDescriptorSetLayout *out_ds_layout,
                            VkDescriptorPool *out_ds_pool)
{
	VkResult r;

	/* Shader modules */
	VkShaderModuleCreateInfo vs_ci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = vs_size,
		.pCode = vs_spirv,
	};
	VkShaderModule vs_mod;
	r = vk->CreateShaderModule(vk->device, &vs_ci, NULL, &vs_mod);
	if (r != VK_SUCCESS) {
		fprintf(stderr, "[VK] User VS ShaderModule failed: %d\n", r);
		return -1;
	}

	VkShaderModuleCreateInfo ps_ci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = ps_size,
		.pCode = ps_spirv,
	};
	VkShaderModule ps_mod;
	r = vk->CreateShaderModule(vk->device, &ps_ci, NULL, &ps_mod);
	if (r != VK_SUCCESS) {
		vk->DestroyShaderModule(vk->device, vs_mod, NULL);
		fprintf(stderr, "[VK] User PS ShaderModule failed: %d\n", r);
		return -1;
	}

	/* Shader stages */
	VkPipelineShaderStageCreateInfo stages[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vs_mod,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = ps_mod,
			.pName = "main",
		},
	};

	/* Vertex input — up to 3 attributes: pos(vec3), color(vec4), texcoord(vec2) */
	VkVertexInputBindingDescription bind = {
		.binding = 0,
		.stride = vertex_stride,
		.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
	};

	VkVertexInputAttributeDescription attrs[3];
	int attr_count = 0;
	uint32_t offset = 0;

	/* location 0: pos (float3) */
	attrs[attr_count].location = 0;
	attrs[attr_count].binding = 0;
	attrs[attr_count].format = VK_FORMAT_R32G32B32_SFLOAT;
	attrs[attr_count].offset = offset;
	offset += 12; attr_count++;

	if (num_attrs >= 2) {
		/* location 1: color (float4) */
		attrs[attr_count].location = 1;
		attrs[attr_count].binding = 0;
		attrs[attr_count].format = VK_FORMAT_R32G32B32A32_SFLOAT;
		attrs[attr_count].offset = offset;
		offset += 16; attr_count++;
	}

	if (num_attrs >= 3) {
		/* location 2: texcoord (float2) */
		attrs[attr_count].location = 2;
		attrs[attr_count].binding = 0;
		attrs[attr_count].format = VK_FORMAT_R32G32_SFLOAT;
		attrs[attr_count].offset = offset;
		attr_count++;
	}

	VkPipelineVertexInputStateCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &bind,
		.vertexAttributeDescriptionCount = (uint32_t)attr_count,
		.pVertexAttributeDescriptions = attrs,
	};

	/* Input assembly */
	VkPipelineInputAssemblyStateCreateInfo ia = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};

	/* Viewport/scissor — dynamic */
	VkPipelineViewportStateCreateInfo vp = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};

	/* Rasterizer */
	VkPipelineRasterizationStateCreateInfo rs_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_CLOCKWISE,
		.lineWidth = 1.0f,
	};

	/* Multisample */
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	/* Depth/stencil */
	VkPipelineDepthStencilStateCreateInfo ds = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = depth_test ? 1 : 0,
		.depthWriteEnable = depth_test ? 1 : 0,
		.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
	};

	/* Color blend — no blend, write all */
	VkPipelineColorBlendAttachmentState cba = {
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
		                  VK_COLOR_COMPONENT_G_BIT |
		                  VK_COLOR_COMPONENT_B_BIT |
		                  VK_COLOR_COMPONENT_A_BIT,
	};

	VkPipelineColorBlendStateCreateInfo cb = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &cba,
	};

	/* Dynamic state */
	VkDynamicState dyn_states[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};

	VkPipelineDynamicStateCreateInfo dyn = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = dyn_states,
	};

	/* Descriptor set layout + pool (UBO at set=0, binding=0) */
	*out_ds_layout = VK_NULL_HANDLE;
	*out_ds_pool = VK_NULL_HANDLE;

	if (has_ubo) {
		VkDescriptorSetLayoutBinding ubo_bind = {
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		};
		VkDescriptorSetLayoutCreateInfo dsl_ci = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = 1,
			.pBindings = &ubo_bind,
		};
		r = vk->CreateDescriptorSetLayout(vk->device, &dsl_ci, NULL,
		                                  out_ds_layout);
		if (r != VK_SUCCESS) {
			vk->DestroyShaderModule(vk->device, vs_mod, NULL);
			vk->DestroyShaderModule(vk->device, ps_mod, NULL);
			return -1;
		}

		VkDescriptorPoolSize pool_size = {
			.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 8,
		};
		VkDescriptorPoolCreateInfo dp_ci = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = 8,
			.poolSizeCount = 1,
			.pPoolSizes = &pool_size,
		};
		r = vk->CreateDescriptorPool(vk->device, &dp_ci, NULL,
		                             out_ds_pool);
		if (r != VK_SUCCESS) {
			vk->DestroyDescriptorSetLayout(vk->device,
			                               *out_ds_layout, NULL);
			vk->DestroyShaderModule(vk->device, vs_mod, NULL);
			vk->DestroyShaderModule(vk->device, ps_mod, NULL);
			return -1;
		}
	}

	/* Pipeline layout */
	VkPipelineLayoutCreateInfo pl_ci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = has_ubo ? 1 : 0,
		.pSetLayouts = has_ubo ? out_ds_layout : NULL,
	};

	r = vk->CreatePipelineLayout(vk->device, &pl_ci, NULL, out_layout);
	if (r != VK_SUCCESS) {
		if (*out_ds_pool) vk->DestroyDescriptorPool(vk->device,
		                                            *out_ds_pool, NULL);
		if (*out_ds_layout) vk->DestroyDescriptorSetLayout(vk->device,
		                                                   *out_ds_layout, NULL);
		vk->DestroyShaderModule(vk->device, vs_mod, NULL);
		vk->DestroyShaderModule(vk->device, ps_mod, NULL);
		return -1;
	}

	/* Graphics pipeline */
	VkGraphicsPipelineCreateInfo gp_ci = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &vi,
		.pInputAssemblyState = &ia,
		.pViewportState = &vp,
		.pRasterizationState = &rs_state,
		.pMultisampleState = &ms,
		.pDepthStencilState = &ds,
		.pColorBlendState = &cb,
		.pDynamicState = &dyn,
		.layout = *out_layout,
		.renderPass = rt->render_pass,
		.subpass = 0,
	};

	r = vk->CreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1,
	                                &gp_ci, NULL, out_pipeline);

	vk->DestroyShaderModule(vk->device, vs_mod, NULL);
	vk->DestroyShaderModule(vk->device, ps_mod, NULL);

	if (r != VK_SUCCESS) {
		fprintf(stderr, "[VK] User CreateGraphicsPipelines failed: %d\n", r);
		vk->DestroyPipelineLayout(vk->device, *out_layout, NULL);
		if (*out_ds_pool) vk->DestroyDescriptorPool(vk->device,
		                                            *out_ds_pool, NULL);
		if (*out_ds_layout) vk->DestroyDescriptorSetLayout(vk->device,
		                                                   *out_ds_layout, NULL);
		return -1;
	}

	return 0;
}

/* ============================================================
 * vk_draw_full — GPU Draw (non-indexed) with descriptor set
 * ============================================================ */
int vk_draw_full(struct vk_backend *vk, struct vk_render_target *rt,
                 VkPipeline pipeline, VkPipelineLayout layout,
                 struct vk_gpu_buffer *vb, uint32_t vertex_count,
                 VkDescriptorSet ds,
                 int width, int height)
{
	VkCommandBufferAllocateInfo cb_ai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = vk->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cmd;
	if (vk->AllocateCommandBuffers(vk->device, &cb_ai, &cmd) != VK_SUCCESS)
		return -1;

	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->BeginCommandBuffer(cmd, &bi);

	VkClearValue clear_vals[2] = {
		{ .color = { .float32 = { 0, 0, 0, 1 } } },
		{ .depthStencil = { 1.0f, 0 } },
	};
	VkRenderPassBeginInfo rp_bi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rt->render_pass,
		.framebuffer = rt->framebuffer,
		.renderArea = { .offset = { 0, 0 },
				.extent = { rt->width, rt->height } },
		.clearValueCount = 2,
		.pClearValues = clear_vals,
	};
	vk->CmdBeginRenderPass(cmd, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

	vk->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

	if (ds)
		vk->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		                          layout, 0, 1, &ds, 0, NULL);

	VkDeviceSize vb_offset = 0;
	vk->CmdBindVertexBuffers(cmd, 0, 1, &vb->buffer, &vb_offset);

	VkViewport viewport = {
		.x = 0.0f, .y = 0.0f,
		.width = (float)width, .height = (float)height,
		.minDepth = 0.0f, .maxDepth = 1.0f,
	};
	vk->CmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor = {
		.offset = { 0, 0 },
		.extent = { (uint32_t)width, (uint32_t)height },
	};
	vk->CmdSetScissor(cmd, 0, 1, &scissor);

	vk->CmdDraw(cmd, vertex_count, 1, 0, 0);

	vk->CmdEndRenderPass(cmd);
	vk->EndCommandBuffer(cmd);

	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmd,
	};
	vk->QueueSubmit(vk->graphics_queue, 1, &si, VK_NULL_HANDLE);
	vk->QueueWaitIdle(vk->graphics_queue);

	vk->FreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
	return 0;
}

/* ============================================================
 * vk_draw_indexed — GPU DrawIndexed with descriptor set
 * ============================================================ */
int vk_draw_indexed(struct vk_backend *vk, struct vk_render_target *rt,
                    VkPipeline pipeline, VkPipelineLayout layout,
                    struct vk_gpu_buffer *vb,
                    struct vk_gpu_buffer *ib,
                    uint32_t index_count, VkIndexType index_type,
                    VkDescriptorSet ds,
                    int width, int height)
{
	VkCommandBufferAllocateInfo cb_ai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = vk->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cmd;
	if (vk->AllocateCommandBuffers(vk->device, &cb_ai, &cmd) != VK_SUCCESS)
		return -1;

	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->BeginCommandBuffer(cmd, &bi);

	VkClearValue clear_vals[2] = {
		{ .color = { .float32 = { 0, 0, 0, 1 } } },
		{ .depthStencil = { 1.0f, 0 } },
	};
	VkRenderPassBeginInfo rp_bi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rt->render_pass,
		.framebuffer = rt->framebuffer,
		.renderArea = { .offset = { 0, 0 },
				.extent = { rt->width, rt->height } },
		.clearValueCount = 2,
		.pClearValues = clear_vals,
	};
	vk->CmdBeginRenderPass(cmd, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

	vk->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

	if (ds)
		vk->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		                          layout, 0, 1, &ds, 0, NULL);

	VkDeviceSize vb_offset = 0;
	vk->CmdBindVertexBuffers(cmd, 0, 1, &vb->buffer, &vb_offset);
	vk->CmdBindIndexBuffer(cmd, ib->buffer, 0, index_type);

	VkViewport viewport = {
		.x = 0.0f, .y = 0.0f,
		.width = (float)width, .height = (float)height,
		.minDepth = 0.0f, .maxDepth = 1.0f,
	};
	vk->CmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor = {
		.offset = { 0, 0 },
		.extent = { (uint32_t)width, (uint32_t)height },
	};
	vk->CmdSetScissor(cmd, 0, 1, &scissor);

	vk->CmdDrawIndexed(cmd, index_count, 1, 0, 0, 0);

	vk->CmdEndRenderPass(cmd);
	vk->EndCommandBuffer(cmd);

	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmd,
	};
	vk->QueueSubmit(vk->graphics_queue, 1, &si, VK_NULL_HANDLE);
	vk->QueueWaitIdle(vk->graphics_queue);

	vk->FreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
	return 0;
}

#endif /* CITC_VULKAN_ENABLED */
