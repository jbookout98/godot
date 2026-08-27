/**************************************************************************/
/*  render_voxel_forward.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_indirect_blend.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_indirect_inject.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_indirect_propagate.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_outline.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_reflection_color_inject.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_reflection_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_shadow_atlas.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_shadow_atlas_scroll.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_shadow_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_visibility.glsl.gen.h"
#include "voxel_forward_volume_storage.h"

namespace RendererSceneRenderImplementation {

// Voxel Forward intentionally starts as a behavior-preserving Forward+
// renderer. Voxel-owned visibility and lighting passes will be introduced
// behind this type one milestone at a time, keeping stock Forward+ available
// as a known-good fallback throughout development.
class RenderVoxelForward : public RenderForwardClustered {
private:
	struct VisibleVolume {
		RID voxel_texture;
		RID brick_texture;
		RID palette_texture;
		Transform3D transform;
		Vector3i dimensions;
		float voxel_size = 0.1f;
	};

	VoxelForwardVolumeStorage volume_storage;
	Vector<VisibleVolume> visible_volumes;
	uint32_t last_registered_volume_count = UINT32_MAX;
	uint32_t last_visible_volume_count = UINT32_MAX;
	uint64_t last_occupancy_gpu_bytes = UINT64_MAX;
	uint64_t last_world_occupancy_revision = UINT64_MAX;
	VoxelVisibilityShaderRD visibility_shader;
	RID visibility_shader_version;
	PipelineCacheRD visibility_pipelines[3];
	bool visibility_resources_initialized = false;
	VoxelOutlineShaderRD outline_shader;
	RID outline_shader_version;
	PipelineCacheRD outline_pipelines[RD::TEXTURE_SAMPLES_MAX][3];
	bool outline_resources_initialized = false;
	VoxelShadowAtlasShaderRD shadow_atlas_shader;
	RID shadow_atlas_shader_version;
	RID shadow_atlas_pipeline;
	VoxelShadowAtlasScrollShaderRD shadow_atlas_scroll_shader;
	RID shadow_atlas_scroll_shader_version;
	RID shadow_atlas_scroll_pipeline;
	VoxelShadowResolveShaderRD shadow_resolve_shader;
	RID shadow_resolve_shader_version;
	RID shadow_resolve_legacy_pipeline;
	RID shadow_resolve_payload_pipeline;
	VoxelIndirectInjectShaderRD indirect_inject_shader;
	RID indirect_inject_shader_version;
	RID indirect_inject_pipeline;
	VoxelIndirectPropagateShaderRD indirect_propagate_shader;
	RID indirect_propagate_shader_version;
	RID indirect_propagate_pipeline;
	VoxelIndirectBlendShaderRD indirect_blend_shader;
	RID indirect_blend_shader_version;
	RID indirect_blend_pipeline;
	VoxelReflectionColorInjectShaderRD reflection_color_inject_shader;
	RID reflection_color_inject_shader_version;
	RID reflection_color_inject_pipeline;
	VoxelReflectionResolveShaderRD reflection_resolve_shader;
	RID reflection_resolve_shader_version;
	RID reflection_resolve_pipeline;
	RID shadow_mask_texture;
	RID shadow_mask_source_rd;
	RID shadow_atlas_source_rd;
	RID shadow_atlas_scratch_rd;
	RID shadow_atlas_scratch_source_rd;
	RID shadow_atlas_directory_rd;
	RID shadow_atlas_bricks_rd;
	uint32_t shadow_atlas_scratch_resolution = 0;
	uint64_t shadow_atlas_world_revision = UINT64_MAX;
	Vector3 shadow_atlas_center;
	Vector3 shadow_atlas_light_direction;
	Vector3 shadow_atlas_tangent;
	Vector3 shadow_atlas_bitangent;
	uint32_t shadow_atlas_resolution = 0;
	float shadow_atlas_voxel_size = 0.0f;
	float shadow_atlas_near_extent = 0.0f;
	float shadow_atlas_far_extent = 0.0f;
	bool shadow_atlas_initialized = false;
	bool shadow_atlas_incremental_enabled = true;
	uint64_t shadow_atlas_full_rebuild_count = 0;
	uint64_t shadow_atlas_incremental_update_count = 0;
	uint64_t shadow_atlas_incremental_scroll_count = 0;

	struct ShadowAtlasBuildState {
		RID texture;
		RID directory;
		RID bricks;
		bool owns_occupancy_snapshot = false;
		uint64_t world_revision = UINT64_MAX;
		Vector3 world_origin;
		Vector3 center;
		Vector3 light_direction;
		Vector3 tangent;
		Vector3 bitangent;
		uint32_t resolution = 0;
		uint32_t directory_mask = 0;
		int32_t max_steps = 0;
		float voxel_size = 0.0f;
		float near_extent = 0.0f;
		float far_extent = 0.0f;
	};

	ShadowAtlasBuildState shadow_atlas_staging;
	ShadowAtlasBuildState shadow_atlas_retired_snapshot;
	uint32_t shadow_atlas_active_slot = 0;
	uint32_t shadow_atlas_allocated_resolution = 0;
	bool shadow_atlas_temporal_rebuild_in_progress = false;
	bool shadow_atlas_force_full_rebuild_latched = false;
	uint64_t shadow_atlas_rebuild_next_texel = 0;
	uint64_t shadow_atlas_rebuild_total_texels = 0;
	uint64_t shadow_atlas_temporal_rebuild_count = 0;
	uint64_t shadow_atlas_temporal_publish_count = 0;
	uint64_t shadow_atlas_temporal_restart_count = 0;
	uint64_t shadow_atlas_pending_world_revision = UINT64_MAX;
	uint64_t shadow_atlas_pending_change_count = 0;
	bool shadow_atlas_pending_full_rebuild = false;
	bool shadow_atlas_pending_dirty_reliable = true;
	HashSet<Vector3i> shadow_atlas_pending_dirty_bricks;
	static constexpr uint32_t INDIRECT_CASCADE_COUNT = 3;
	RID indirect_grid_rd[INDIRECT_CASCADE_COUNT][2];
	RID indirect_staging_grid_rd[INDIRECT_CASCADE_COUNT][2];
	RID indirect_grid_texture[INDIRECT_CASCADE_COUNT];
	VoxelForwardVolumeStorage::WorldOccupancy indirect_staging_world;
	bool indirect_staging_world_owned = false;
	Vector3 indirect_grid_origin[INDIRECT_CASCADE_COUNT];
	float indirect_grid_cell_size[INDIRECT_CASCADE_COUNT] = {};
	bool indirect_cascade_initialized[INDIRECT_CASCADE_COUNT] = {};

	struct IndirectCascadeDefinition {
		Vector3 origin;
		float cell_size = 0.0f;
		uint64_t world_revision = UINT64_MAX;
		Vector3 light_direction;
		Color light_color;
		float light_energy = 0.0f;
		int propagation_steps = 0;
		float propagation_decay = 0.0f;
		float shadow_bias = 0.0f;
		bool dirty_updates_enabled = true;
	};

	IndirectCascadeDefinition indirect_active_definition[INDIRECT_CASCADE_COUNT];
	IndirectCascadeDefinition indirect_staging_definition[INDIRECT_CASCADE_COUNT];
	Vector3i indirect_staging_dispatch_origin[INDIRECT_CASCADE_COUNT];
	Vector3i indirect_staging_dispatch_size[INDIRECT_CASCADE_COUNT];
	uint32_t indirect_staging_next_step[INDIRECT_CASCADE_COUNT] = {};
	bool indirect_staging_active[INDIRECT_CASCADE_COUNT] = {};
	bool indirect_staging_injected[INDIRECT_CASCADE_COUNT] = {};
	bool indirect_staging_partial[INDIRECT_CASCADE_COUNT] = {};
	bool indirect_staging_batch_classified = false;
	bool indirect_staging_batch_low_latency = false;
	uint64_t indirect_staging_dirty_cell_count = 0;
	uint64_t indirect_staging_cell_pass_workload = 0;
	uint32_t indirect_staging_dispatch_count = 0;
	uint64_t indirect_render_frame_index = 0;
	uint64_t indirect_staging_batch_detected_frame = 0;
	uint64_t indirect_staging_detected_frame[INDIRECT_CASCADE_COUNT] = {};
	uint64_t indirect_low_latency_update_count = 0;
	uint64_t indirect_temporal_update_count = 0;
	uint32_t indirect_last_publication_frames = 0;
	IndirectCascadeDefinition indirect_blend_definition[INDIRECT_CASCADE_COUNT];
	Vector3 indirect_blend_history_origin[INDIRECT_CASCADE_COUNT];
	float indirect_blend_history_cell_size[INDIRECT_CASCADE_COUNT] = {};
	Vector3i indirect_blend_dispatch_origin[INDIRECT_CASCADE_COUNT];
	Vector3i indirect_blend_dispatch_size[INDIRECT_CASCADE_COUNT];
	uint32_t indirect_blend_step[INDIRECT_CASCADE_COUNT] = {};
	uint32_t indirect_blend_frame_count[INDIRECT_CASCADE_COUNT] = {};
	uint64_t indirect_blend_detected_frame[INDIRECT_CASCADE_COUNT] = {};
	bool indirect_blend_active[INDIRECT_CASCADE_COUNT] = {};
	bool indirect_blend_has_history[INDIRECT_CASCADE_COUNT] = {};
	bool indirect_active_converged[INDIRECT_CASCADE_COUNT] = {};
	uint32_t indirect_schedule_cursor = 0;
	uint32_t indirect_blend_schedule_cursor = 0;
	uint32_t indirect_grid_resolution = 0;
	uint64_t indirect_world_revision = UINT64_MAX;
	Vector3 indirect_light_direction;
	Color indirect_light_color;
	float indirect_light_energy = 0.0f;
	bool indirect_dirty_updates_enabled = true;
	static constexpr uint32_t REFLECTION_CASCADE_COUNT = 3;
	RID reflection_color_grid_rd[REFLECTION_CASCADE_COUNT];
	Vector3 reflection_color_grid_origin[REFLECTION_CASCADE_COUNT];
	float reflection_color_grid_cell_size[REFLECTION_CASCADE_COUNT] = {};
	bool reflection_color_grid_initialized[REFLECTION_CASCADE_COUNT] = {};
	uint32_t reflection_color_grid_resolution = 0;
	uint64_t reflection_color_world_revision = UINT64_MAX;
	RID reflection_texture;
	RID reflection_source_rd;
	RID reflection_uniform_buffer;
	Size2i reflection_screen_size;
	RID occupancy_uniform_buffer;
	RID bound_occupancy_directory;
	RID bound_occupancy_bricks;
	uint64_t bound_batch_texture_revision = 0;

	struct OccupancyUniformData {
		float world_origin_voxel_size[4];
		int32_t directory_steps[4];
		float limits[4];
	};

	struct VisibilityPushConstant {
		float model_view_projection[16];
		float camera_local[4];
		int32_t volume_dimensions[4];
	};

	struct OutlinePushConstant {
		int32_t screen_instances[4];
		int32_t instance_layout[4];
		float thresholds[4];
	};

	struct ShadowAtlasPushConstant {
		float world_origin_voxel_size[4];
		float atlas_center_depth[4];
		float tangent_near_extent[4];
		float bitangent_far_extent[4];
		int32_t atlas_directory_steps[4];
		int32_t dispatch_rect[4];
	};

	struct ShadowAtlasScrollPushConstant {
		int32_t atlas_size[4];
		int32_t cascade_shifts[4];
	};

	struct ShadowResolvePushConstant {
		float inv_view_projection[16];
		float atlas_center_voxel_size[4];
		float tangent_near_extent[4];
		float bitangent_far_extent[4];
		int32_t screen_atlas_filter[4];
	};

	struct IndirectInjectPushConstant {
		float world_origin_voxel_size[4];
		float grid_origin_cell_size[4];
		float light_direction_energy[4];
		float light_color_bias[4];
		float atlas_center_resolution[4];
		float tangent_near_extent[4];
		float bitangent_far_extent[4];
		int32_t grid_directory[4];
	};

	struct IndirectPropagatePushConstant {
		float world_origin_voxel_size[4];
		float grid_origin_cell_size[4];
		int32_t grid_directory[4];
		float propagation[4];
		int32_t dispatch_origin[4];
	};

	struct IndirectBlendPushConstant {
		float history_origin_cell_size[4];
		float target_origin_cell_size[4];
		float fallback_radiance_blend[4];
		int32_t dispatch_origin_resolution[4];
		int32_t state[4];
	};

	struct ReflectionColorInjectPushConstant {
		float world_to_voxel[16];
		float grid_origin_cell_size[4];
		int32_t dispatch_origin[4];
		int32_t volume_dimensions[4];
		int32_t grid_resolution[4];
	};

	struct ReflectionUniformData {
		float inv_view_projection[16];
		float world_origin_voxel_size[4];
		float camera_position_max_distance[4];
		float color_grid_origin_cell_size[REFLECTION_CASCADE_COUNT][4];
		float indirect_grid_origin_cell_size[INDIRECT_CASCADE_COUNT][4];
		float ambient_color_energy[4];
		int32_t screen_grid_steps[4];
		int32_t state[4];
	};

	void _render_shadow_atlas(const RenderDataRD *p_render_data);
	void _reset_shadow_atlas_state();
	void _release_shadow_atlas_snapshot(ShadowAtlasBuildState &r_state);
	void _render_indirect_light(const RenderDataRD *p_render_data, RID p_shadow_atlas, RID p_sampler, const Vector3 &p_light_direction, const Color &p_light_color, float p_light_energy);
	void _release_indirect_light_snapshot();
	void _free_indirect_light();
	void _render_voxel_reflections(const RenderDataRD *p_render_data);
	void _free_voxel_reflections();
	void _ensure_visibility_resources();
	void _ensure_outline_resources();
	void _render_voxel_outline(RenderDataRD *p_render_data, RID p_framebuffer, uint32_t p_color_attachment_count);

protected:
	virtual void _add_voxel_occupancy_uniforms(Vector<RD::Uniform> &r_uniforms) override;
	virtual void _fill_voxel_instance_data(RID p_base, VoxelInstanceData &r_instance_data) const override;
	virtual bool _render_scene_custom_uses_resolved_depth() const override;
	virtual void _render_scene_custom_pre_opaque(RenderDataRD *p_render_data, bool p_depth_prepass) override;
	virtual void _render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) override;
	virtual void _render_buffers_debug_draw(const RenderDataRD *p_render_data) override;
	virtual void _render_scene_custom_opaque(RenderDataRD *p_render_data, RID p_framebuffer, uint32_t p_color_pass_flags, uint32_t p_color_attachment_count, bool p_depth_prepass) override;

public:
	RenderVoxelForward();
	~RenderVoxelForward();
};

} // namespace RendererSceneRenderImplementation
