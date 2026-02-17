/*
 * vk_pipeline.h — Vulkan 그래픽스 파이프라인
 * ============================================
 *
 * VkBuffer(VB/IB) + VkPipeline + Draw 커맨드 녹화.
 *
 * Class 42: 기본 Draw (하드코딩 SPIR-V)
 * Class 44: 사용자 SPIR-V + CB + Depth 통합
 */

#ifndef CITC_VK_PIPELINE_H
#define CITC_VK_PIPELINE_H

#ifdef CITC_VULKAN_ENABLED

#include "vk_backend.h"

/* GPU 버퍼 (VB, IB, UBO 공통) */
struct vk_gpu_buffer {
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDeviceSize size;
};

/* 생성/파괴 */
int vk_create_buffer(struct vk_backend *vk, struct vk_gpu_buffer *buf,
                     VkDeviceSize size, VkBufferUsageFlags usage,
                     VkMemoryPropertyFlags mem_props);
void vk_destroy_buffer(struct vk_backend *vk, struct vk_gpu_buffer *buf);

/* CPU 데이터 → GPU 버퍼 업로드 */
int vk_upload_buffer(struct vk_backend *vk, struct vk_gpu_buffer *buf,
                     const void *data, VkDeviceSize size);

/* 기본 파이프라인 (하드코딩 SPIR-V, pos+color) */
int vk_create_default_pipeline(struct vk_backend *vk,
                               struct vk_render_target *rt,
                               VkPipeline *out_pipeline,
                               VkPipelineLayout *out_layout);

/* GPU Draw 실행 */
int vk_draw(struct vk_backend *vk, struct vk_render_target *rt,
            VkPipeline pipeline, VkPipelineLayout layout,
            struct vk_gpu_buffer *vb,
            uint32_t vertex_count, uint32_t vertex_stride,
            int width, int height);

/* ============================================================
 * Class 44: 사용자 SPIR-V 파이프라인 + UBO + Depth
 * ============================================================ */

/* 파이프라인 캐시 엔트리 (VS+PS SPIR-V 포인터 키) */
#define VK_MAX_CACHED_PIPELINES 16

struct vk_cached_pipeline {
	const uint32_t *vs_spirv;	/* 키 (포인터 비교) */
	const uint32_t *ps_spirv;
	int depth_test;
	VkPipeline pipeline;
	VkPipelineLayout layout;
	VkDescriptorSetLayout ds_layout;
	VkDescriptorPool ds_pool;
};

struct vk_pipeline_cache {
	struct vk_cached_pipeline entries[VK_MAX_CACHED_PIPELINES];
	int count;
};

/* 사용자 SPIR-V로 파이프라인 생성.
 * vs/ps SPIR-V, 정점 포맷(stride, attribute 수/형식),
 * depth test 여부, UBO 바인딩 여부 지정.
 * out_ds: descriptor set (UBO 바인딩이 있으면 할당) */
int vk_create_user_pipeline(struct vk_backend *vk,
                            struct vk_render_target *rt,
                            const uint32_t *vs_spirv, size_t vs_size,
                            const uint32_t *ps_spirv, size_t ps_size,
                            uint32_t vertex_stride,
                            int num_attrs,	/* 1-3: pos, color, texcoord */
                            int has_ubo,
                            int depth_test,
                            VkPipeline *out_pipeline,
                            VkPipelineLayout *out_layout,
                            VkDescriptorSetLayout *out_ds_layout,
                            VkDescriptorPool *out_ds_pool);

/* 파이프라인 캐시 lookup/insert */
struct vk_cached_pipeline *vk_cache_find(struct vk_pipeline_cache *cache,
                                         const uint32_t *vs, const uint32_t *ps,
                                         int depth_test);
struct vk_cached_pipeline *vk_cache_insert(struct vk_pipeline_cache *cache);

/* Descriptor Set 할당 + UBO 바인딩 업데이트 */
int vk_alloc_descriptor_set(struct vk_backend *vk,
                            VkDescriptorSetLayout layout,
                            VkDescriptorPool pool,
                            VkDescriptorSet *out_set);

void vk_update_ubo_descriptor(struct vk_backend *vk,
                              VkDescriptorSet ds,
                              struct vk_gpu_buffer *ubo);

/* GPU Draw (non-indexed) with full state */
int vk_draw_full(struct vk_backend *vk, struct vk_render_target *rt,
                 VkPipeline pipeline, VkPipelineLayout layout,
                 struct vk_gpu_buffer *vb, uint32_t vertex_count,
                 VkDescriptorSet ds,
                 int width, int height);

/* GPU DrawIndexed */
int vk_draw_indexed(struct vk_backend *vk, struct vk_render_target *rt,
                    VkPipeline pipeline, VkPipelineLayout layout,
                    struct vk_gpu_buffer *vb,
                    struct vk_gpu_buffer *ib,
                    uint32_t index_count, VkIndexType index_type,
                    VkDescriptorSet ds,
                    int width, int height);

#endif /* CITC_VULKAN_ENABLED */
#endif /* CITC_VK_PIPELINE_H */
