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
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_indirect_inject.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_indirect_propagate.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_shadow_atlas.glsl.gen.h"
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
		VoxelForwardVolumeStorage::Volume volume;
		Transform3D transform;
		AABB aabb;
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
	VoxelShadowAtlasShaderRD shadow_atlas_shader;
	RID shadow_atlas_shader_version;
	RID shadow_atlas_pipeline;
	VoxelShadowResolveShaderRD shadow_resolve_shader;
	RID shadow_resolve_shader_version;
	RID shadow_resolve_pipeline;
	VoxelIndirectInjectShaderRD indirect_inject_shader;
	RID indirect_inject_shader_version;
	RID indirect_inject_pipeline;
	VoxelIndirectPropagateShaderRD indirect_propagate_shader;
	RID indirect_propagate_shader_version;
	RID indirect_propagate_pipeline;
	RID shadow_mask_texture;
	RID shadow_mask_source_rd;
	RID shadow_atlas_source_rd;
	uint64_t shadow_atlas_world_revision = UINT64_MAX;
	Vector3 shadow_atlas_center;
	Vector3 shadow_atlas_light_direction;
	Vector3 shadow_atlas_tangent;
	Vector3 shadow_atlas_bitangent;
	uint32_t shadow_atlas_resolution = 0;
	float shadow_atlas_near_extent = 0.0f;
	float shadow_atlas_far_extent = 0.0f;
	bool shadow_atlas_initialized = false;
	static constexpr uint32_t INDIRECT_CASCADE_COUNT = 3;
	RID indirect_grid_rd[INDIRECT_CASCADE_COUNT][2];
	RID indirect_grid_texture[INDIRECT_CASCADE_COUNT];
	Vector3 indirect_grid_origin[INDIRECT_CASCADE_COUNT];
	float indirect_grid_cell_size[INDIRECT_CASCADE_COUNT] = {};
	bool indirect_cascade_initialized[INDIRECT_CASCADE_COUNT] = {};
	uint32_t indirect_grid_resolution = 0;
	uint64_t indirect_world_revision = UINT64_MAX;
	Vector3 indirect_light_direction;
	Color indirect_light_color;
	float indirect_light_energy = 0.0f;
	RID occupancy_uniform_buffer;
	RID bound_occupancy_directory;
	RID bound_occupancy_bricks;

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

	struct ShadowAtlasPushConstant {
		float world_origin_voxel_size[4];
		float atlas_center_depth[4];
		float tangent_near_extent[4];
		float bitangent_far_extent[4];
		int32_t atlas_directory_steps[4];
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
	};

	void _render_shadow_atlas(const RenderDataRD *p_render_data);
	void _render_indirect_light(const RenderDataRD *p_render_data, RID p_shadow_atlas, RID p_sampler, const Vector3 &p_light_direction, const Color &p_light_color, float p_light_energy);
	void _free_indirect_light();

protected:
	virtual void _add_voxel_occupancy_uniforms(Vector<RD::Uniform> &r_uniforms) override;
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
