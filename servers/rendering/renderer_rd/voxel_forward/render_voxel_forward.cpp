/**************************************************************************/
/*  render_voxel_forward.cpp                                              */
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

#include "render_voxel_forward.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

namespace RendererSceneRenderImplementation {

RenderVoxelForward::RenderVoxelForward() : RenderForwardClustered(true, bool(GLOBAL_GET("rendering/voxel_forward/architectural_hit_buffer/exact_position_enabled"))) {
	occupancy_uniform_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(OccupancyUniformData));
	OccupancyUniformData empty_occupancy = {};
	RD::get_singleton()->buffer_update(occupancy_uniform_buffer, 0, sizeof(OccupancyUniformData), &empty_occupancy);

	Vector<String> shadow_modes;
	shadow_modes.push_back("");
	shadow_atlas_shader.initialize(shadow_modes);
	shadow_atlas_shader_version = shadow_atlas_shader.version_create();
	shadow_atlas_pipeline = RD::get_singleton()->compute_pipeline_create(shadow_atlas_shader.version_get_shader(shadow_atlas_shader_version, 0));
	shadow_atlas_scroll_shader.initialize(shadow_modes);
	shadow_atlas_scroll_shader_version = shadow_atlas_scroll_shader.version_create();
	shadow_atlas_scroll_pipeline = RD::get_singleton()->compute_pipeline_create(shadow_atlas_scroll_shader.version_get_shader(shadow_atlas_scroll_shader_version, 0));
	Vector<String> shadow_resolve_modes;
	shadow_resolve_modes.push_back("");
	shadow_resolve_modes.push_back("\n#define USE_VOXEL_HIT_PAYLOAD\n");
	shadow_resolve_shader.initialize(shadow_resolve_modes);
	shadow_resolve_shader_version = shadow_resolve_shader.version_create();
	shadow_resolve_legacy_pipeline = RD::get_singleton()->compute_pipeline_create(shadow_resolve_shader.version_get_shader(shadow_resolve_shader_version, 0));
	shadow_resolve_payload_pipeline = RD::get_singleton()->compute_pipeline_create(shadow_resolve_shader.version_get_shader(shadow_resolve_shader_version, 1));
	indirect_inject_shader.initialize(shadow_modes);
	indirect_inject_shader_version = indirect_inject_shader.version_create();
	indirect_inject_pipeline = RD::get_singleton()->compute_pipeline_create(indirect_inject_shader.version_get_shader(indirect_inject_shader_version, 0));
	indirect_propagate_shader.initialize(shadow_modes);
	indirect_propagate_shader_version = indirect_propagate_shader.version_create();
	indirect_propagate_pipeline = RD::get_singleton()->compute_pipeline_create(indirect_propagate_shader.version_get_shader(indirect_propagate_shader_version, 0));
	indirect_blend_shader.initialize(shadow_modes);
	indirect_blend_shader_version = indirect_blend_shader.version_create();
	indirect_blend_pipeline = RD::get_singleton()->compute_pipeline_create(indirect_blend_shader.version_get_shader(indirect_blend_shader_version, 0));
	reflection_color_inject_shader.initialize(shadow_modes);
	reflection_color_inject_shader_version = reflection_color_inject_shader.version_create();
	reflection_color_inject_pipeline = RD::get_singleton()->compute_pipeline_create(reflection_color_inject_shader.version_get_shader(reflection_color_inject_shader_version, 0));
	reflection_resolve_shader.initialize(shadow_modes);
	reflection_resolve_shader_version = reflection_resolve_shader.version_create();
	reflection_resolve_pipeline = RD::get_singleton()->compute_pipeline_create(reflection_resolve_shader.version_get_shader(reflection_resolve_shader_version, 0));
	reflection_uniform_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(ReflectionUniformData));
}

RenderVoxelForward::~RenderVoxelForward() {
	if (RendererRD::MaterialStorage::get_singleton() != nullptr) {
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_light_direction"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_ready"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_near"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_far"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_distant"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_ambient_color"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_ambient_energy"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_reflection"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_reflection_ready"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_reflection_intensity"), Variant());
	}
	_reset_shadow_atlas_state();
	_free_voxel_reflections();
	_free_indirect_light();
	if (shadow_mask_texture.is_valid() && RendererRD::TextureStorage::get_singleton() != nullptr) {
		RendererRD::TextureStorage::get_singleton()->texture_free(shadow_mask_texture);
	}
	if (shadow_atlas_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(shadow_atlas_pipeline);
	}
	if (shadow_atlas_scroll_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(shadow_atlas_scroll_pipeline);
	}
	if (shadow_atlas_scratch_rd.is_valid()) {
		RD::get_singleton()->free_rid(shadow_atlas_scratch_rd);
	}
	if (shadow_resolve_legacy_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(shadow_resolve_legacy_pipeline);
	}
	if (shadow_resolve_payload_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(shadow_resolve_payload_pipeline);
	}
	if (indirect_inject_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(indirect_inject_pipeline);
	}
	if (indirect_propagate_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(indirect_propagate_pipeline);
	}
	if (indirect_blend_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(indirect_blend_pipeline);
	}
	if (reflection_color_inject_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(reflection_color_inject_pipeline);
	}
	if (reflection_resolve_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(reflection_resolve_pipeline);
	}
	if (occupancy_uniform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(occupancy_uniform_buffer);
	}
	shadow_atlas_shader.version_free(shadow_atlas_shader_version);
	shadow_atlas_scroll_shader.version_free(shadow_atlas_scroll_shader_version);
	shadow_resolve_shader.version_free(shadow_resolve_shader_version);
	indirect_inject_shader.version_free(indirect_inject_shader_version);
	indirect_propagate_shader.version_free(indirect_propagate_shader_version);
	indirect_blend_shader.version_free(indirect_blend_shader_version);
	reflection_color_inject_shader.version_free(reflection_color_inject_shader_version);
	reflection_resolve_shader.version_free(reflection_resolve_shader_version);
	if (visibility_resources_initialized) {
		for (uint32_t i = 0; i < 3; i++) {
			visibility_pipelines[i].clear();
		}
		visibility_shader.version_free(visibility_shader_version);
	}
}

void RenderVoxelForward::_ensure_visibility_resources() {
	if (visibility_resources_initialized) {
		return;
	}
	Vector<String> modes;
	modes.push_back("");
	visibility_shader.initialize(modes);
	visibility_shader_version = visibility_shader.version_create();
	RID shader = visibility_shader.version_get_shader(visibility_shader_version, 0);

	RD::PipelineDepthStencilState depth_stencil;
	depth_stencil.enable_depth_test = true;
	depth_stencil.enable_depth_write = true;
	depth_stencil.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
	for (uint32_t i = 0; i < 3; i++) {
		// Forward Clustered keeps fixed color, specular, and velocity attachment
		// slots in its framebuffer descriptions even when optional RIDs are empty.
		visibility_pipelines[i].setup(shader, RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), depth_stencil, RD::PipelineColorBlendState::create_disabled(3), 0);
	}
	visibility_resources_initialized = true;
}

void RenderVoxelForward::_free_voxel_reflections() {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	if (reflection_texture.is_valid() && texture_storage != nullptr) {
		texture_storage->texture_free(reflection_texture);
		reflection_texture = RID();
	}
	reflection_source_rd = RID();
	reflection_screen_size = Size2i();
	for (uint32_t cascade = 0; cascade < REFLECTION_CASCADE_COUNT; cascade++) {
		if (reflection_color_grid_rd[cascade].is_valid()) {
			RD::get_singleton()->free_rid(reflection_color_grid_rd[cascade]);
			reflection_color_grid_rd[cascade] = RID();
		}
		reflection_color_grid_initialized[cascade] = false;
	}
	reflection_color_grid_resolution = 0;
	reflection_color_world_revision = UINT64_MAX;
	if (reflection_uniform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(reflection_uniform_buffer);
		reflection_uniform_buffer = RID();
	}
}

void RenderVoxelForward::_free_indirect_light() {
	_release_indirect_light_snapshot();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		if (indirect_grid_texture[cascade].is_valid() && texture_storage != nullptr) {
			const RID wrapped_rd = indirect_grid_rd[cascade][0];
			texture_storage->texture_free(indirect_grid_texture[cascade]);
			indirect_grid_texture[cascade] = RID();
			// texture_rd_initialize adopts an externally created RD texture. Keep an
			// explicit validity check here because backend teardown paths do not all
			// release that adopted texture through the wrapper.
			if (RD::get_singleton()->texture_is_valid(wrapped_rd)) {
				RD::get_singleton()->free_rid(wrapped_rd);
			}
			indirect_grid_rd[cascade][0] = RID();
		}
		for (uint32_t buffer = 0; buffer < 2; buffer++) {
			if (indirect_grid_rd[cascade][buffer].is_valid()) {
				RD::get_singleton()->free_rid(indirect_grid_rd[cascade][buffer]);
				indirect_grid_rd[cascade][buffer] = RID();
			}
			if (indirect_staging_grid_rd[cascade][buffer].is_valid()) {
				RD::get_singleton()->free_rid(indirect_staging_grid_rd[cascade][buffer]);
				indirect_staging_grid_rd[cascade][buffer] = RID();
			}
		}
		indirect_cascade_initialized[cascade] = false;
		indirect_staging_active[cascade] = false;
		indirect_staging_injected[cascade] = false;
		indirect_staging_partial[cascade] = false;
		indirect_staging_next_step[cascade] = 0;
		indirect_blend_active[cascade] = false;
		indirect_blend_has_history[cascade] = false;
		indirect_blend_step[cascade] = 0;
		indirect_blend_frame_count[cascade] = 0;
		indirect_active_converged[cascade] = false;
		indirect_active_definition[cascade] = IndirectCascadeDefinition();
		indirect_staging_definition[cascade] = IndirectCascadeDefinition();
		indirect_blend_definition[cascade] = IndirectCascadeDefinition();
	}
	indirect_grid_resolution = 0;
	indirect_world_revision = UINT64_MAX;
	indirect_schedule_cursor = 0;
	indirect_blend_schedule_cursor = 0;
}

void RenderVoxelForward::_release_indirect_light_snapshot() {
	if (indirect_staging_world_owned && RD::get_singleton() != nullptr) {
		if (indirect_staging_world.directory_buffer.is_valid()) {
			RD::get_singleton()->free_rid(indirect_staging_world.directory_buffer);
		}
		if (indirect_staging_world.brick_buffer.is_valid()) {
			RD::get_singleton()->free_rid(indirect_staging_world.brick_buffer);
		}
	}
	indirect_staging_world = VoxelForwardVolumeStorage::WorldOccupancy();
	indirect_staging_world_owned = false;
}

void RenderVoxelForward::_release_shadow_atlas_snapshot(ShadowAtlasBuildState &r_state) {
	if (r_state.owns_occupancy_snapshot && RD::get_singleton() != nullptr) {
		if (r_state.directory.is_valid()) {
			RD::get_singleton()->free_rid(r_state.directory);
		}
		if (r_state.bricks.is_valid()) {
			RD::get_singleton()->free_rid(r_state.bricks);
		}
	}
	r_state = ShadowAtlasBuildState();
}

void RenderVoxelForward::_reset_shadow_atlas_state() {
	_release_shadow_atlas_snapshot(shadow_atlas_staging);
	_release_shadow_atlas_snapshot(shadow_atlas_retired_snapshot);
	shadow_atlas_source_rd = RID();
	shadow_atlas_directory_rd = RID();
	shadow_atlas_bricks_rd = RID();
	shadow_atlas_world_revision = UINT64_MAX;
	shadow_atlas_center = Vector3();
	shadow_atlas_light_direction = Vector3();
	shadow_atlas_tangent = Vector3();
	shadow_atlas_bitangent = Vector3();
	shadow_atlas_resolution = 0;
	shadow_atlas_voxel_size = 0.0f;
	shadow_atlas_near_extent = 0.0f;
	shadow_atlas_far_extent = 0.0f;
	shadow_atlas_initialized = false;
	shadow_atlas_temporal_rebuild_in_progress = false;
	shadow_atlas_rebuild_next_texel = 0;
	shadow_atlas_rebuild_total_texels = 0;
	shadow_atlas_pending_world_revision = UINT64_MAX;
	shadow_atlas_pending_change_count = 0;
	shadow_atlas_pending_full_rebuild = false;
	shadow_atlas_pending_dirty_reliable = true;
	shadow_atlas_pending_dirty_bricks.clear();
}

void RenderVoxelForward::_render_indirect_light(const RenderDataRD *p_render_data, RID p_shadow_atlas, RID p_sampler, const Vector3 &p_light_direction, const Color &p_light_color, float p_light_energy) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	const bool enabled = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/enabled"));
	if (!enabled || p_render_data == nullptr || p_render_data->scene_data == nullptr || !p_shadow_atlas.is_valid() || !p_sampler.is_valid() || !indirect_inject_pipeline.is_valid() || !indirect_propagate_pipeline.is_valid()) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), false);
		// Keep the cascade texture wrappers alive while disabled. Stable texture
		// RIDs keep material descriptor sets valid when GI is toggled or resized.
		return;
	}

	const VoxelForwardVolumeStorage::WorldOccupancy &world = volume_storage.get_world_occupancy();
	if (!world.directory_buffer.is_valid() || !world.brick_buffer.is_valid() || world.occupied_brick_count == 0 || world.voxel_size <= 0.0f) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), false);
		// If the world becomes empty, the retained textures no longer describe
		// a valid history for a later incremental insertion.
		indirect_world_revision = UINT64_MAX;
		for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
			indirect_staging_active[cascade] = false;
			indirect_staging_injected[cascade] = false;
			indirect_blend_active[cascade] = false;
		}
		_release_indirect_light_snapshot();
		return;
	}

	const uint32_t resolution = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/resolution")), 24, 96));
	const float near_cell_size = MAX(world.voxel_size, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/near_cell_size")));
	const float far_cell_size = MAX(near_cell_size, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/far_cell_size")));
	const float distant_cell_size = MAX(far_cell_size, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/distant_cell_size")));
	const float cell_sizes[INDIRECT_CASCADE_COUNT] = { near_cell_size, far_cell_size, distant_cell_size };
	const int recenter_cells = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/recenter_cells")), 1, 16);
	const float recenter_hysteresis = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/recenter_hysteresis")), 0.55f, 0.95f);
	const int base_propagation_steps = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/propagation_steps")), 2, 16) & ~1;
	const float propagation_decay = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/propagation_decay")), 0.0f, 0.99f);
	const float shadow_bias = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/shadow_bias_voxels"))) * world.voxel_size;
	const bool dirty_updates_enabled = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/dirty_updates_enabled"));
	const bool temporal_updates_enabled = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/temporal_updates_enabled"));
	const bool temporal_blend_enabled = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/temporal_blend_enabled")) && indirect_blend_pipeline.is_valid();
	const uint32_t temporal_blend_frames = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/temporal_blend_frames")), 1, 60));
	const uint32_t dispatch_budget = temporal_updates_enabled ? uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/dispatch_budget_per_frame")), 1, 16)) : 64u;
	const uint32_t blend_dispatch_budget = temporal_updates_enabled ? uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/blend_dispatch_budget_per_frame")), 1, int(INDIRECT_CASCADE_COUNT))) : INDIRECT_CASCADE_COUNT;
	const Vector3 camera_position = p_render_data->scene_data->cam_transform.origin;
	Vector3 grid_origins[INDIRECT_CASCADE_COUNT];
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		const float snap = cell_sizes[cascade] * float(recenter_cells);
		Vector3 snapped_center;
		const bool staging_layout_matches = indirect_staging_active[cascade] && indirect_grid_resolution == resolution &&
				Math::is_equal_approx(indirect_staging_definition[cascade].cell_size, cell_sizes[cascade]);
		const bool active_layout_matches = indirect_cascade_initialized[cascade] && indirect_grid_resolution == resolution &&
				Math::is_equal_approx(indirect_grid_cell_size[cascade], cell_sizes[cascade]);
		const bool can_reuse_center = staging_layout_matches || active_layout_matches;
		if (can_reuse_center) {
			const Vector3 reference_origin = staging_layout_matches ? indirect_staging_definition[cascade].origin : indirect_grid_origin[cascade];
			snapped_center = reference_origin + Vector3(1, 1, 1) * (cell_sizes[cascade] * float(resolution) * 0.5f);
			const float threshold = snap * recenter_hysteresis;
			for (int axis = 0; axis < 3; axis++) {
				const float offset = camera_position[axis] - snapped_center[axis];
				if (offset > threshold) {
					snapped_center[axis] += Math::ceil((offset - threshold) / snap) * snap;
				} else if (offset < -threshold) {
					snapped_center[axis] -= Math::ceil((-offset - threshold) / snap) * snap;
				}
			}
		} else {
			snapped_center = Vector3(
					Math::floor(camera_position.x / snap + 0.5f) * snap,
					Math::floor(camera_position.y / snap + 0.5f) * snap,
					Math::floor(camera_position.z / snap + 0.5f) * snap);
		}
		grid_origins[cascade] = snapped_center - Vector3(1, 1, 1) * (cell_sizes[cascade] * float(resolution) * 0.5f);
	}

	if (indirect_grid_resolution != resolution) {
		_release_indirect_light_snapshot();
		RD::TextureFormat texture_format;
		texture_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		texture_format.width = resolution;
		texture_format.height = resolution;
		texture_format.depth = resolution;
		texture_format.texture_type = RD::TEXTURE_TYPE_3D;
		texture_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
			RID replacement_rd[2];
			RID replacement_staging_rd[2];
			for (uint32_t buffer = 0; buffer < 2; buffer++) {
				replacement_rd[buffer] = RD::get_singleton()->texture_create(texture_format, RD::TextureView());
				replacement_staging_rd[buffer] = RD::get_singleton()->texture_create(texture_format, RD::TextureView());
				RD::get_singleton()->texture_clear(replacement_rd[buffer], Color(0, 0, 0, 0), 0, 1, 0, 1);
				RD::get_singleton()->texture_clear(replacement_staging_rd[buffer], Color(0, 0, 0, 0), 0, 1, 0, 1);
				RD::get_singleton()->set_resource_name(replacement_rd[buffer], vformat("Voxel Indirect Light Cascade %d Active Buffer %d", cascade, buffer));
				RD::get_singleton()->set_resource_name(replacement_staging_rd[buffer], vformat("Voxel Indirect Light Cascade %d Staging Buffer %d", cascade, buffer));
			}
			if (indirect_grid_texture[cascade].is_valid()) {
				// Replace the resource behind the existing wrapper. Materials keep the
				// same public RID, so a live resolution edit cannot leave an old set bound.
				RID previous_rd[2] = { indirect_grid_rd[cascade][0], indirect_grid_rd[cascade][1] };
				RID previous_staging_rd[2] = { indirect_staging_grid_rd[cascade][0], indirect_staging_grid_rd[cascade][1] };
				RID replacement_texture = texture_storage->texture_allocate();
				texture_storage->texture_rd_initialize(replacement_texture, replacement_rd[0]);
				texture_storage->texture_replace(indirect_grid_texture[cascade], replacement_texture);
				for (uint32_t buffer = 0; buffer < 2; buffer++) {
					if (RD::get_singleton()->texture_is_valid(previous_rd[buffer])) {
						RD::get_singleton()->free_rid(previous_rd[buffer]);
					}
					if (RD::get_singleton()->texture_is_valid(previous_staging_rd[buffer])) {
						RD::get_singleton()->free_rid(previous_staging_rd[buffer]);
					}
				}
			} else {
				indirect_grid_texture[cascade] = texture_storage->texture_allocate();
				texture_storage->texture_rd_initialize(indirect_grid_texture[cascade], replacement_rd[0]);
			}
			indirect_grid_rd[cascade][0] = replacement_rd[0];
			indirect_grid_rd[cascade][1] = replacement_rd[1];
			indirect_staging_grid_rd[cascade][0] = replacement_staging_rd[0];
			indirect_staging_grid_rd[cascade][1] = replacement_staging_rd[1];
			indirect_cascade_initialized[cascade] = false;
			indirect_staging_active[cascade] = false;
			indirect_staging_injected[cascade] = false;
			indirect_staging_next_step[cascade] = 0;
			indirect_blend_active[cascade] = false;
			indirect_blend_has_history[cascade] = false;
			indirect_blend_step[cascade] = 0;
			indirect_blend_frame_count[cascade] = 0;
			indirect_active_converged[cascade] = false;
			indirect_active_definition[cascade] = IndirectCascadeDefinition();
			indirect_staging_definition[cascade] = IndirectCascadeDefinition();
			indirect_blend_definition[cascade] = IndirectCascadeDefinition();
		}
		indirect_grid_resolution = resolution;
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_near"), indirect_grid_texture[0]);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_far"), indirect_grid_texture[1]);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_distant"), indirect_grid_texture[2]);
	}

	const RID inject_shader_rid = indirect_inject_shader.version_get_shader(indirect_inject_shader_version, 0);
	const RID propagate_shader_rid = indirect_propagate_shader.version_get_shader(indirect_propagate_shader_version, 0);
	const RID blend_shader_rid = indirect_blend_shader.version_get_shader(indirect_blend_shader_version, 0);
	static const char *cascade_names[INDIRECT_CASCADE_COUNT] = { "Near", "Far", "Distant" };
	auto definition_matches = [](const IndirectCascadeDefinition &p_a, const IndirectCascadeDefinition &p_b) {
		return p_a.origin.is_equal_approx(p_b.origin) && Math::is_equal_approx(p_a.cell_size, p_b.cell_size) &&
				p_a.world_revision == p_b.world_revision && p_a.light_direction.is_equal_approx(p_b.light_direction) &&
				p_a.light_color.is_equal_approx(p_b.light_color) && Math::is_equal_approx(p_a.light_energy, p_b.light_energy) &&
				p_a.propagation_steps == p_b.propagation_steps && Math::is_equal_approx(p_a.propagation_decay, p_b.propagation_decay) &&
				Math::is_equal_approx(p_a.shadow_bias, p_b.shadow_bias) && p_a.dirty_updates_enabled == p_b.dirty_updates_enabled;
	};
	auto definition_matches_except_world = [](const IndirectCascadeDefinition &p_a, const IndirectCascadeDefinition &p_b) {
		return p_a.origin.is_equal_approx(p_b.origin) && Math::is_equal_approx(p_a.cell_size, p_b.cell_size) &&
				p_a.light_direction.is_equal_approx(p_b.light_direction) && p_a.light_color.is_equal_approx(p_b.light_color) &&
				Math::is_equal_approx(p_a.light_energy, p_b.light_energy) && p_a.propagation_steps == p_b.propagation_steps &&
				Math::is_equal_approx(p_a.propagation_decay, p_b.propagation_decay) && Math::is_equal_approx(p_a.shadow_bias, p_b.shadow_bias) &&
				p_a.dirty_updates_enabled == p_b.dirty_updates_enabled;
	};
	bool staging_batch_active = false;
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		staging_batch_active = staging_batch_active || indirect_staging_active[cascade];
	}
	if (!staging_batch_active) {
		_release_indirect_light_snapshot();
	}

	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		IndirectCascadeDefinition desired;
		desired.origin = grid_origins[cascade];
		desired.cell_size = cell_sizes[cascade];
		desired.world_revision = world.revision;
		desired.light_direction = p_light_direction;
		desired.light_color = p_light_color;
		desired.light_energy = p_light_energy;
		desired.propagation_steps = MAX(2, (base_propagation_steps >> cascade)) & ~1;
		desired.propagation_decay = propagation_decay;
		desired.shadow_bias = shadow_bias;
		desired.dirty_updates_enabled = dirty_updates_enabled;

		if (indirect_blend_active[cascade]) {
			// Finish publishing the immutable completed target. Newer camera/world
			// definitions are picked up afterward instead of repeatedly cancelling
			// a blend that has already consumed generation work.
			continue;
		}

		if (indirect_cascade_initialized[cascade] && indirect_active_converged[cascade] && definition_matches(indirect_active_definition[cascade], desired)) {
			indirect_staging_active[cascade] = false;
			indirect_staging_injected[cascade] = false;
			continue;
		}
		if (indirect_staging_active[cascade]) {
			// A staging batch reads one immutable occupancy snapshot. Let it finish
			// coherently; a newer desired definition remains implicit in the current
			// live world and is scheduled after this batch publishes.
			continue;
		}

		if (!indirect_staging_world_owned) {
			if (!volume_storage.create_world_occupancy_snapshot(indirect_staging_world)) {
				continue;
			}
			indirect_staging_world_owned = true;
		}
		if (indirect_staging_world.revision != desired.world_revision) {
			// Other cascades are still finishing a coherent older generation.
			// Do not mix a newer definition with that snapshot.
			continue;
		}
		const VoxelForwardVolumeStorage::WorldOccupancy &build_world = indirect_staging_world;

		Vector3i dispatch_origin;
		Vector3i dispatch_size(resolution, resolution, resolution);
		bool partial_update = false;
		bool dirty_intersects = true;
		const bool can_use_dirty_update = indirect_cascade_initialized[cascade] && indirect_active_converged[cascade] && dirty_updates_enabled &&
				indirect_active_definition[cascade].world_revision != desired.world_revision &&
				indirect_active_definition[cascade].world_revision != UINT64_MAX &&
				indirect_active_definition[cascade].world_revision + 1u == desired.world_revision &&
				definition_matches_except_world(indirect_active_definition[cascade], desired) &&
				build_world.last_incremental_revision == build_world.revision && !build_world.last_dirty_bricks.is_empty();
		if (can_use_dirty_update) {
			const int padding_cells = desired.propagation_steps + 1;
			Vector3i dirty_begin(INT32_MAX, INT32_MAX, INT32_MAX);
			Vector3i dirty_end(INT32_MIN, INT32_MIN, INT32_MIN);
			for (const Vector3i &dirty_brick : build_world.last_dirty_bricks) {
				const Vector3 brick_world_begin = build_world.origin + Vector3(dirty_brick * 8) * build_world.voxel_size;
				const Vector3 brick_world_end = brick_world_begin + Vector3(8, 8, 8) * build_world.voxel_size;
				for (int axis = 0; axis < 3; axis++) {
					const int cell_begin = int(Math::floor((brick_world_begin[axis] - desired.origin[axis]) / desired.cell_size)) - padding_cells;
					const int cell_end = int(Math::ceil((brick_world_end[axis] - desired.origin[axis]) / desired.cell_size)) + padding_cells;
					dirty_begin[axis] = MIN(dirty_begin[axis], cell_begin);
					dirty_end[axis] = MAX(dirty_end[axis], cell_end);
				}
			}
			dirty_intersects = dirty_end.x > 0 && dirty_end.y > 0 && dirty_end.z > 0 &&
					dirty_begin.x < int(resolution) && dirty_begin.y < int(resolution) && dirty_begin.z < int(resolution);
			if (dirty_intersects) {
				for (int axis = 0; axis < 3; axis++) {
					dirty_begin[axis] = CLAMP(dirty_begin[axis], 0, int(resolution));
					dirty_end[axis] = CLAMP(dirty_end[axis], 0, int(resolution));
				}
				const Vector3i dirty_size = dirty_end - dirty_begin;
				const uint64_t dirty_cell_count = uint64_t(dirty_size.x) * uint64_t(dirty_size.y) * uint64_t(dirty_size.z);
				const uint64_t full_cell_count = uint64_t(resolution) * uint64_t(resolution) * uint64_t(resolution);
				if (dirty_cell_count * 4 < full_cell_count * 3) {
					dispatch_origin = dirty_begin;
					dispatch_size = dirty_size;
					partial_update = true;
				}
			}
		}
		if (can_use_dirty_update && !dirty_intersects) {
			indirect_active_definition[cascade] = desired;
			continue;
		}

		indirect_staging_definition[cascade] = desired;
		indirect_staging_dispatch_origin[cascade] = dispatch_origin;
		indirect_staging_dispatch_size[cascade] = dispatch_size;
		indirect_staging_next_step[cascade] = 0;
		indirect_staging_active[cascade] = true;
		indirect_staging_injected[cascade] = false;
		indirect_staging_partial[cascade] = partial_update;
	}

	bool has_staging_work = false;
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		has_staging_work = has_staging_work || indirect_staging_active[cascade];
	}
	uint32_t staging_dispatched = 0;
	if (has_staging_work) {
		RENDER_TIMESTAMP("Voxel Indirect Light");
		RD::get_singleton()->draw_command_begin_label("Voxel Indirect Light");
		// Injection and propagation own this budget. Temporal blending is a much
		// cheaper copy-like pass and has a separate bounded budget below, so a
		// completed cascade can no longer stall all remaining cascade generation.
		while (staging_dispatched < dispatch_budget) {
			uint32_t cascade = INDIRECT_CASCADE_COUNT;
			for (uint32_t probe = 0; probe < INDIRECT_CASCADE_COUNT; probe++) {
				const uint32_t candidate = (indirect_schedule_cursor + probe) % INDIRECT_CASCADE_COUNT;
				if (indirect_staging_active[candidate]) {
					cascade = candidate;
					break;
				}
			}
			if (cascade == INDIRECT_CASCADE_COUNT) {
				break;
			}
			indirect_schedule_cursor = (cascade + 1u) % INDIRECT_CASCADE_COUNT;
			const IndirectCascadeDefinition &build = indirect_staging_definition[cascade];
			const VoxelForwardVolumeStorage::WorldOccupancy &build_world = indirect_staging_world_owned ? indirect_staging_world : world;
			Vector3i dispatch_origin = indirect_staging_dispatch_origin[cascade];
			Vector3i dispatch_size = indirect_staging_dispatch_size[cascade];

			if (!indirect_staging_injected[cascade]) {
				if (indirect_staging_partial[cascade]) {
					const Vector3 copy_size(resolution, resolution, resolution);
					const Error copy_0 = RD::get_singleton()->texture_copy(indirect_grid_rd[cascade][0], indirect_staging_grid_rd[cascade][0], Vector3(), Vector3(), copy_size, 0, 0, 0, 0);
					const Error copy_1 = RD::get_singleton()->texture_copy(indirect_grid_rd[cascade][0], indirect_staging_grid_rd[cascade][1], Vector3(), Vector3(), copy_size, 0, 0, 0, 0);
					if (copy_0 != OK || copy_1 != OK) {
						indirect_staging_partial[cascade] = false;
						dispatch_origin = Vector3i();
						dispatch_size = Vector3i(resolution, resolution, resolution);
						indirect_staging_dispatch_origin[cascade] = dispatch_origin;
						indirect_staging_dispatch_size[cascade] = dispatch_size;
					}
				}

				RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ indirect_staging_grid_rd[cascade][0] }));
				RD::Uniform u_atlas(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ p_sampler, p_shadow_atlas }));
				RD::Uniform u_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ build_world.directory_buffer }));
				RD::Uniform u_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ build_world.brick_buffer }));
				const RID inject_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(inject_shader_rid, 0, u_output, u_atlas, u_directory, u_bricks);

				IndirectInjectPushConstant inject_push = {};
				inject_push.world_origin_voxel_size[0] = build_world.origin.x;
				inject_push.world_origin_voxel_size[1] = build_world.origin.y;
				inject_push.world_origin_voxel_size[2] = build_world.origin.z;
				inject_push.world_origin_voxel_size[3] = build_world.voxel_size;
				inject_push.grid_origin_cell_size[0] = build.origin.x;
				inject_push.grid_origin_cell_size[1] = build.origin.y;
				inject_push.grid_origin_cell_size[2] = build.origin.z;
				inject_push.grid_origin_cell_size[3] = build.cell_size;
				inject_push.light_direction_energy[0] = build.light_direction.x;
				inject_push.light_direction_energy[1] = build.light_direction.y;
				inject_push.light_direction_energy[2] = build.light_direction.z;
				inject_push.light_direction_energy[3] = build.light_energy;
				inject_push.light_color_bias[0] = build.light_color.r;
				inject_push.light_color_bias[1] = build.light_color.g;
				inject_push.light_color_bias[2] = build.light_color.b;
				inject_push.light_color_bias[3] = build.shadow_bias;
				inject_push.atlas_center_resolution[0] = shadow_atlas_center.x;
				inject_push.atlas_center_resolution[1] = shadow_atlas_center.y;
				inject_push.atlas_center_resolution[2] = shadow_atlas_center.z;
				inject_push.atlas_center_resolution[3] = shadow_atlas_resolution;
				inject_push.tangent_near_extent[0] = shadow_atlas_tangent.x;
				inject_push.tangent_near_extent[1] = shadow_atlas_tangent.y;
				inject_push.tangent_near_extent[2] = shadow_atlas_tangent.z;
				inject_push.tangent_near_extent[3] = shadow_atlas_near_extent;
				inject_push.bitangent_far_extent[0] = shadow_atlas_bitangent.x;
				inject_push.bitangent_far_extent[1] = shadow_atlas_bitangent.y;
				inject_push.bitangent_far_extent[2] = shadow_atlas_bitangent.z;
				inject_push.bitangent_far_extent[3] = shadow_atlas_far_extent;
				inject_push.grid_directory[0] = resolution;
				inject_push.grid_directory[1] = build_world.directory_mask;
				inject_push.grid_directory[2] = CLAMP(int(Math::ceil(build.cell_size / build_world.voxel_size)), 1, 8);
				inject_push.grid_directory[3] = dispatch_origin.x | (dispatch_origin.y << 8) | (dispatch_origin.z << 16);

				const String injection_label = vformat("Indirect %s Injection", cascade_names[cascade]);
				RENDER_TIMESTAMP(injection_label);
				RD::get_singleton()->draw_command_begin_label(injection_label.utf8().span());
				RD::ComputeListID inject_list = RD::get_singleton()->compute_list_begin();
				RD::get_singleton()->compute_list_bind_compute_pipeline(inject_list, indirect_inject_pipeline);
				RD::get_singleton()->compute_list_bind_uniform_set(inject_list, inject_uniform_set, 0);
				RD::get_singleton()->compute_list_set_push_constant(inject_list, &inject_push, sizeof(IndirectInjectPushConstant));
				RD::get_singleton()->compute_list_dispatch_threads(inject_list, dispatch_size.x, dispatch_size.y, dispatch_size.z);
				RD::get_singleton()->compute_list_end();
				RD::get_singleton()->draw_command_end_label();
				indirect_staging_injected[cascade] = true;
				staging_dispatched++;
				continue;
			}

			const uint32_t step = indirect_staging_next_step[cascade];
			const uint32_t source = step & 1u;
			const uint32_t destination = source ^ 1u;
			RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ p_sampler, indirect_staging_grid_rd[cascade][source] }));
			RD::Uniform u_destination(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ indirect_staging_grid_rd[cascade][destination] }));
			RD::Uniform u_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ build_world.directory_buffer }));
			RD::Uniform u_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ build_world.brick_buffer }));
			const RID propagate_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(propagate_shader_rid, 0, u_source, u_destination, u_directory, u_bricks);

			IndirectPropagatePushConstant propagate_push = {};
			propagate_push.world_origin_voxel_size[0] = build_world.origin.x;
			propagate_push.world_origin_voxel_size[1] = build_world.origin.y;
			propagate_push.world_origin_voxel_size[2] = build_world.origin.z;
			propagate_push.world_origin_voxel_size[3] = build_world.voxel_size;
			propagate_push.grid_origin_cell_size[0] = build.origin.x;
			propagate_push.grid_origin_cell_size[1] = build.origin.y;
			propagate_push.grid_origin_cell_size[2] = build.origin.z;
			propagate_push.grid_origin_cell_size[3] = build.cell_size;
			propagate_push.grid_directory[0] = resolution;
			propagate_push.grid_directory[1] = build_world.directory_mask;
			propagate_push.propagation[0] = build.propagation_decay;
			propagate_push.dispatch_origin[0] = dispatch_origin.x;
			propagate_push.dispatch_origin[1] = dispatch_origin.y;
			propagate_push.dispatch_origin[2] = dispatch_origin.z;

			const String propagation_label = vformat("Indirect %s Propagation", cascade_names[cascade]);
			RENDER_TIMESTAMP(propagation_label);
			RD::get_singleton()->draw_command_begin_label(propagation_label.utf8().span());
			RD::ComputeListID propagate_list = RD::get_singleton()->compute_list_begin();
			RD::get_singleton()->compute_list_bind_compute_pipeline(propagate_list, indirect_propagate_pipeline);
			RD::get_singleton()->compute_list_bind_uniform_set(propagate_list, propagate_uniform_set, 0);
			RD::get_singleton()->compute_list_set_push_constant(propagate_list, &propagate_push, sizeof(IndirectPropagatePushConstant));
			RD::get_singleton()->compute_list_dispatch_threads(propagate_list, dispatch_size.x, dispatch_size.y, dispatch_size.z);
			RD::get_singleton()->compute_list_end();
			RD::get_singleton()->draw_command_end_label();
			indirect_staging_next_step[cascade]++;
			staging_dispatched++;

			if (indirect_staging_next_step[cascade] == uint32_t(build.propagation_steps)) {
				// Every configured cascade pass count is even. The completed result is
				// therefore staging buffer 0 after 0->1, 1->0 ping-pong propagation.
				const String publish_label = vformat("Indirect Cascade Publish (%s)", cascade_names[cascade]);
				RENDER_TIMESTAMP(publish_label);
				RD::get_singleton()->draw_command_begin_label(publish_label.utf8().span());
				if (temporal_blend_enabled && temporal_blend_frames > 1u) {
					// The staging result is complete and immutable. Blend only its
					// updated region; all other active texels remain untouched.
					indirect_blend_definition[cascade] = build;
					indirect_blend_history_origin[cascade] = indirect_grid_origin[cascade];
					indirect_blend_history_cell_size[cascade] = indirect_grid_cell_size[cascade];
					indirect_blend_dispatch_origin[cascade] = indirect_staging_dispatch_origin[cascade];
					indirect_blend_dispatch_size[cascade] = indirect_staging_dispatch_size[cascade];
					indirect_blend_step[cascade] = 0;
					indirect_blend_frame_count[cascade] = temporal_blend_frames;
					indirect_blend_has_history[cascade] = indirect_cascade_initialized[cascade];
					indirect_blend_active[cascade] = true;
					indirect_staging_active[cascade] = false;
					indirect_staging_injected[cascade] = false;
					RD::get_singleton()->draw_command_end_label();
				} else {
					// Keep both the public wrapper RID and its underlying RD texture
					// fixed. Queue-ordered copying never exposes a partially written grid.
					const Vector3 copy_origin(indirect_staging_dispatch_origin[cascade]);
					const Vector3 copy_size(indirect_staging_dispatch_size[cascade]);
					const Error publish_error = RD::get_singleton()->texture_copy(indirect_staging_grid_rd[cascade][0], indirect_grid_rd[cascade][0], copy_origin, copy_origin, copy_size, 0, 0, 0, 0);
					RD::get_singleton()->draw_command_end_label();
					if (publish_error == OK) {
					indirect_active_definition[cascade] = build;
					indirect_grid_origin[cascade] = build.origin;
					indirect_grid_cell_size[cascade] = build.cell_size;
					indirect_cascade_initialized[cascade] = true;
					indirect_active_converged[cascade] = true;
					indirect_staging_active[cascade] = false;
					indirect_staging_injected[cascade] = false;
					} else {
					// Re-run only the deterministic final pass before retrying publish.
					// This avoids ever advancing beyond the configured even pass count.
					indirect_staging_next_step[cascade]--;
					}
				}
			}
		}
		RD::get_singleton()->draw_command_end_label();
	}
	bool staging_work_remaining = false;
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		staging_work_remaining = staging_work_remaining || indirect_staging_active[cascade];
	}
	if (!staging_work_remaining) {
		_release_indirect_light_snapshot();
	}

	// Publishing is a compute-side history operation. The fragment shader keeps
	// sampling one stable active texture and gains no branches, rays, or VGPRs.
	// At most one step per cascade is issued in a frame. Blending has its own
	// small budget so it cannot starve generation when the generation budget is
	// one, while still remaining strictly bounded.
	bool blended_this_frame[INDIRECT_CASCADE_COUNT] = {};
	uint32_t blend_dispatched = 0;
	while (blend_dispatched < blend_dispatch_budget) {
		uint32_t cascade = INDIRECT_CASCADE_COUNT;
		for (uint32_t probe = 0; probe < INDIRECT_CASCADE_COUNT; probe++) {
			const uint32_t candidate = (indirect_blend_schedule_cursor + probe) % INDIRECT_CASCADE_COUNT;
			if (indirect_blend_active[candidate] && !blended_this_frame[candidate]) {
				cascade = candidate;
				break;
			}
		}
		if (cascade == INDIRECT_CASCADE_COUNT) {
			break;
		}
		blended_this_frame[cascade] = true;
		indirect_blend_schedule_cursor = (cascade + 1u) % INDIRECT_CASCADE_COUNT;

		const IndirectCascadeDefinition &target = indirect_blend_definition[cascade];
		const Vector3i dispatch_origin = indirect_blend_dispatch_origin[cascade];
		const Vector3i dispatch_size = indirect_blend_dispatch_size[cascade];
		const uint32_t frame_count = MAX(1u, indirect_blend_frame_count[cascade]);
		const uint32_t step = MIN(indirect_blend_step[cascade], frame_count - 1u);
		const float blend_weight = 1.0f / float(frame_count - step);

		RD::Uniform u_history(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ p_sampler, indirect_grid_rd[cascade][0] }));
		RD::Uniform u_target(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ p_sampler, indirect_staging_grid_rd[cascade][0] }));
		RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ indirect_grid_rd[cascade][1] }));
		const RID blend_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(blend_shader_rid, 0, u_history, u_target, u_output);

		Color ambient_fallback = GLOBAL_GET("rendering/voxel_forward/ambient_light/color");
		float ambient_fallback_energy = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/ambient_light/energy")));
		if (p_render_data->environment.is_valid()) {
			ambient_fallback = environment_get_ambient_light(p_render_data->environment);
			ambient_fallback_energy = MAX(0.0f, environment_get_ambient_light_energy(p_render_data->environment));
		}

		IndirectBlendPushConstant blend_push = {};
		const Vector3 history_origin = indirect_blend_has_history[cascade] ? indirect_blend_history_origin[cascade] : target.origin;
		const float history_cell_size = indirect_blend_has_history[cascade] ? indirect_blend_history_cell_size[cascade] : target.cell_size;
		blend_push.history_origin_cell_size[0] = history_origin.x;
		blend_push.history_origin_cell_size[1] = history_origin.y;
		blend_push.history_origin_cell_size[2] = history_origin.z;
		blend_push.history_origin_cell_size[3] = history_cell_size;
		blend_push.target_origin_cell_size[0] = target.origin.x;
		blend_push.target_origin_cell_size[1] = target.origin.y;
		blend_push.target_origin_cell_size[2] = target.origin.z;
		blend_push.target_origin_cell_size[3] = target.cell_size;
		blend_push.fallback_radiance_blend[0] = ambient_fallback.r * ambient_fallback_energy;
		blend_push.fallback_radiance_blend[1] = ambient_fallback.g * ambient_fallback_energy;
		blend_push.fallback_radiance_blend[2] = ambient_fallback.b * ambient_fallback_energy;
		blend_push.fallback_radiance_blend[3] = blend_weight;
		blend_push.dispatch_origin_resolution[0] = dispatch_origin.x;
		blend_push.dispatch_origin_resolution[1] = dispatch_origin.y;
		blend_push.dispatch_origin_resolution[2] = dispatch_origin.z;
		blend_push.dispatch_origin_resolution[3] = resolution;
		blend_push.state[0] = indirect_blend_has_history[cascade] ? 1 : 0;

		const String blend_label = vformat("Indirect %s Temporal Blend %d/%d", cascade_names[cascade], step + 1u, frame_count);
		RENDER_TIMESTAMP(blend_label);
		RD::get_singleton()->draw_command_begin_label(blend_label.utf8().span());
		RD::ComputeListID blend_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(blend_list, indirect_blend_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(blend_list, blend_uniform_set, 0);
		RD::get_singleton()->compute_list_set_push_constant(blend_list, &blend_push, sizeof(IndirectBlendPushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(blend_list, dispatch_size.x, dispatch_size.y, dispatch_size.z);
		RD::get_singleton()->compute_list_end();

		const Vector3 copy_origin(dispatch_origin);
		const Vector3 copy_size(dispatch_size);
		const Error blend_copy_error = RD::get_singleton()->texture_copy(indirect_grid_rd[cascade][1], indirect_grid_rd[cascade][0], copy_origin, copy_origin, copy_size, 0, 0, 0, 0);
		RD::get_singleton()->draw_command_end_label();
		blend_dispatched++;
		if (blend_copy_error != OK) {
			continue;
		}

		indirect_active_definition[cascade] = target;
		indirect_grid_origin[cascade] = target.origin;
		indirect_grid_cell_size[cascade] = target.cell_size;
		indirect_cascade_initialized[cascade] = true;
		indirect_active_converged[cascade] = false;
		indirect_blend_has_history[cascade] = true;
		indirect_blend_history_origin[cascade] = target.origin;
		indirect_blend_history_cell_size[cascade] = target.cell_size;
		indirect_blend_step[cascade]++;
		if (indirect_blend_step[cascade] >= frame_count) {
			indirect_blend_active[cascade] = false;
			indirect_active_converged[cascade] = true;
		}
	}

	indirect_world_revision = world.revision;
	indirect_light_direction = p_light_direction;
	indirect_light_color = p_light_color;
	indirect_light_energy = p_light_energy;
	indirect_dirty_updates_enabled = dirty_updates_enabled;

	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_near_origin"), indirect_grid_origin[0]);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_far_origin"), indirect_grid_origin[1]);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_distant_origin"), indirect_grid_origin[2]);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_near_cell_size"), indirect_grid_cell_size[0]);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_far_cell_size"), indirect_grid_cell_size[1]);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_distant_cell_size"), indirect_grid_cell_size[2]);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_resolution"), int(indirect_grid_resolution));
	const float transition_cells = MAX(float(recenter_cells + 1), float(GLOBAL_GET("rendering/voxel_forward/indirect_light/transition_cells")));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_transition_cells"), transition_cells);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_intensity"), MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/intensity"))));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), indirect_cascade_initialized[0] && indirect_cascade_initialized[1] && indirect_cascade_initialized[2]);
}

void RenderVoxelForward::_render_voxel_reflections(const RenderDataRD *p_render_data) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	auto disable_reflections = [material_storage]() {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_reflection_ready"), false);
	};
	const bool enabled = bool(GLOBAL_GET("rendering/voxel_forward/reflections/enabled"));
	if (!enabled || p_render_data == nullptr || p_render_data->render_buffers.is_null() || p_render_data->scene_data == nullptr || p_render_data->scene_data->view_count != 1 || !reflection_color_inject_pipeline.is_valid() || !reflection_resolve_pipeline.is_valid() || !reflection_uniform_buffer.is_valid()) {
		disable_reflections();
		return;
	}
	const VoxelForwardVolumeStorage::WorldOccupancy &world = volume_storage.get_world_occupancy();
	if (!world.directory_buffer.is_valid() || !world.brick_buffer.is_valid() || world.occupied_brick_count == 0 || world.voxel_size <= 0.0f) {
		disable_reflections();
		return;
	}

	const uint32_t resolution = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/reflections/grid_resolution")), 32, 128));
	const float near_cell_size = MAX(world.voxel_size, float(GLOBAL_GET("rendering/voxel_forward/reflections/near_cell_size")));
	const float far_cell_size = MAX(near_cell_size, float(GLOBAL_GET("rendering/voxel_forward/reflections/far_cell_size")));
	const float distant_cell_size = MAX(far_cell_size, float(GLOBAL_GET("rendering/voxel_forward/reflections/distant_cell_size")));
	const float cell_sizes[REFLECTION_CASCADE_COUNT] = { near_cell_size, far_cell_size, distant_cell_size };
	const int recenter_cells = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/reflections/recenter_cells")), 1, 32);
	const Vector3 camera_position = p_render_data->scene_data->cam_transform.origin;
	Vector3 grid_origins[REFLECTION_CASCADE_COUNT];
	for (uint32_t cascade = 0; cascade < REFLECTION_CASCADE_COUNT; cascade++) {
		const float snap = cell_sizes[cascade] * float(recenter_cells);
		const float half_extent = cell_sizes[cascade] * float(resolution) * 0.5f;
		Vector3 snapped_center;
		for (int axis = 0; axis < 3; axis++) {
			snapped_center[axis] = world.origin[axis] + Math::floor((camera_position[axis] - world.origin[axis]) / snap + 0.5f) * snap;
		}
		grid_origins[cascade] = snapped_center - Vector3(half_extent, half_extent, half_extent);
	}

	if (reflection_color_grid_resolution != resolution) {
		RD::TextureFormat format;
		format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		format.width = resolution;
		format.height = resolution;
		format.depth = resolution;
		format.texture_type = RD::TEXTURE_TYPE_3D;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		for (uint32_t cascade = 0; cascade < REFLECTION_CASCADE_COUNT; cascade++) {
			if (reflection_color_grid_rd[cascade].is_valid()) {
				RD::get_singleton()->free_rid(reflection_color_grid_rd[cascade]);
			}
			reflection_color_grid_rd[cascade] = RD::get_singleton()->texture_create(format, RD::TextureView());
			RD::get_singleton()->set_resource_name(reflection_color_grid_rd[cascade], vformat("Voxel Reflection Color Cascade %d", cascade));
			reflection_color_grid_initialized[cascade] = false;
		}
		reflection_color_grid_resolution = resolution;
	}

	bool cascade_invalid[REFLECTION_CASCADE_COUNT] = {};
	bool any_invalid = reflection_color_world_revision != world.revision;
	for (uint32_t cascade = 0; cascade < REFLECTION_CASCADE_COUNT; cascade++) {
		cascade_invalid[cascade] = reflection_color_world_revision != world.revision || !reflection_color_grid_initialized[cascade] ||
				!reflection_color_grid_origin[cascade].is_equal_approx(grid_origins[cascade]) ||
				!Math::is_equal_approx(reflection_color_grid_cell_size[cascade], cell_sizes[cascade]);
		any_invalid = any_invalid || cascade_invalid[cascade];
	}

	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	const RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	if (any_invalid) {
		const RID inject_shader_rid = reflection_color_inject_shader.version_get_shader(reflection_color_inject_shader_version, 0);
		RENDER_TIMESTAMP("Voxel Reflection Color Clipmaps");
		RD::get_singleton()->draw_command_begin_label("Voxel Reflection Color Clipmaps");
		for (uint32_t cascade = 0; cascade < REFLECTION_CASCADE_COUNT; cascade++) {
			if (!cascade_invalid[cascade]) {
				continue;
			}
			RD::get_singleton()->texture_clear(reflection_color_grid_rd[cascade], Color(0, 0, 0, 0), 0, 1, 0, 1);
			const AABB grid_bounds(grid_origins[cascade], Vector3(1, 1, 1) * (cell_sizes[cascade] * float(resolution)));
			for (const KeyValue<RID, VoxelForwardVolumeStorage::Volume> &entry : volume_storage.get_volumes()) {
				const VoxelForwardVolumeStorage::Volume &volume = entry.value;
				RID voxel_texture = texture_storage->texture_get_rd_texture(volume.voxel_texture);
				RID brick_texture = texture_storage->texture_get_rd_texture(volume.brick_texture);
				RID palette_texture = texture_storage->texture_get_rd_texture(volume.palette_texture, true);
				if (!voxel_texture.is_valid() || !brick_texture.is_valid() || !palette_texture.is_valid()) {
					continue;
				}
				const AABB volume_bounds = volume.transform.xform(AABB(Vector3(), Vector3(volume.dimensions) * volume.voxel_size));
				const AABB intersection = volume_bounds.intersection(grid_bounds);
				if (!intersection.has_volume()) {
					continue;
				}
				Vector3i dispatch_min;
				Vector3i dispatch_max;
				for (int axis = 0; axis < 3; axis++) {
					dispatch_min[axis] = CLAMP(int(Math::floor((intersection.position[axis] - grid_origins[cascade][axis]) / cell_sizes[cascade])), 0, int(resolution));
					dispatch_max[axis] = CLAMP(int(Math::ceil((intersection.get_end()[axis] - grid_origins[cascade][axis]) / cell_sizes[cascade])), 0, int(resolution));
				}
				const Vector3i dispatch_size = dispatch_max - dispatch_min;
				if (dispatch_size.x <= 0 || dispatch_size.y <= 0 || dispatch_size.z <= 0) {
					continue;
				}

				RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ reflection_color_grid_rd[cascade] }));
				RD::Uniform u_voxels(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, voxel_texture }));
				RD::Uniform u_bricks(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, brick_texture }));
				RD::Uniform u_palette(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ sampler, palette_texture }));
				const RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(inject_shader_rid, 0, u_output, u_voxels, u_bricks, u_palette);

				ReflectionColorInjectPushConstant push = {};
				const Transform3D voxel_to_world = volume.transform.scaled_local(Vector3(volume.voxel_size, volume.voxel_size, volume.voxel_size));
				RendererRD::MaterialStorage::store_transform(voxel_to_world.affine_inverse(), push.world_to_voxel);
				push.grid_origin_cell_size[0] = grid_origins[cascade].x;
				push.grid_origin_cell_size[1] = grid_origins[cascade].y;
				push.grid_origin_cell_size[2] = grid_origins[cascade].z;
				push.grid_origin_cell_size[3] = cell_sizes[cascade];
				push.dispatch_origin[0] = dispatch_min.x;
				push.dispatch_origin[1] = dispatch_min.y;
				push.dispatch_origin[2] = dispatch_min.z;
				push.volume_dimensions[0] = volume.dimensions.x;
				push.volume_dimensions[1] = volume.dimensions.y;
				push.volume_dimensions[2] = volume.dimensions.z;
				push.grid_resolution[0] = resolution;
				push.grid_resolution[1] = resolution;
				push.grid_resolution[2] = resolution;

				RD::ComputeListID inject_list = RD::get_singleton()->compute_list_begin();
				RD::get_singleton()->compute_list_bind_compute_pipeline(inject_list, reflection_color_inject_pipeline);
				RD::get_singleton()->compute_list_bind_uniform_set(inject_list, uniform_set, 0);
				RD::get_singleton()->compute_list_set_push_constant(inject_list, &push, sizeof(ReflectionColorInjectPushConstant));
				RD::get_singleton()->compute_list_dispatch_threads(inject_list, dispatch_size.x, dispatch_size.y, dispatch_size.z);
				RD::get_singleton()->compute_list_end();
			}
			reflection_color_grid_origin[cascade] = grid_origins[cascade];
			reflection_color_grid_cell_size[cascade] = cell_sizes[cascade];
			reflection_color_grid_initialized[cascade] = true;
		}
		RD::get_singleton()->draw_command_end_label();
		reflection_color_world_revision = world.revision;
		print_verbose(vformat("Voxel Forward reflection color clipmaps updated: %d^3 x 3, %.2f / %.2f / %.2f m cells, revision %d.", resolution, near_cell_size, far_cell_size, distant_cell_size, world.revision));
	}

	Ref<RenderSceneBuffersRD> render_buffers = p_render_data->render_buffers;
	const Size2i internal_size = render_buffers->get_internal_size();
	const float resolution_scale = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/reflections/resolution_scale")), 0.25f, 1.0f);
	const Size2i screen_size(MAX(1, int(Math::ceil(internal_size.x * resolution_scale))), MAX(1, int(Math::ceil(internal_size.y * resolution_scale))));
	const StringName scope = SNAME("voxel_forward_reflection");
	const StringName texture_name = SNAME("reflection_radiance");
	if (render_buffers->has_texture(scope, texture_name) && reflection_screen_size != screen_size) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_reflection"), Variant());
		if (reflection_texture.is_valid()) {
			texture_storage->texture_free(reflection_texture);
			reflection_texture = RID();
		}
		render_buffers->clear_context(scope);
		reflection_source_rd = RID();
	}
	if (!render_buffers->has_texture(scope, texture_name)) {
		render_buffers->create_texture(scope, texture_name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, screen_size);
	}
	const RID output = render_buffers->get_texture(scope, texture_name);
	const RID depth = render_buffers->get_depth_texture();
	if (!output.is_valid() || !depth.is_valid()) {
		disable_reflections();
		return;
	}
	if (reflection_source_rd != output) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_reflection"), Variant());
		if (reflection_texture.is_valid()) {
			texture_storage->texture_free(reflection_texture);
		}
		reflection_texture = texture_storage->texture_allocate();
		texture_storage->texture_rd_initialize(reflection_texture, output);
		reflection_source_rd = output;
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_reflection"), reflection_texture);
	}
	reflection_screen_size = screen_size;

	ReflectionUniformData reflection_data = {};
	const Projection view_projection = p_render_data->scene_data->get_view_projection(0) * Projection(p_render_data->scene_data->cam_transform.affine_inverse());
	RendererRD::MaterialStorage::store_camera(view_projection.inverse(), reflection_data.inv_view_projection);
	reflection_data.world_origin_voxel_size[0] = world.origin.x;
	reflection_data.world_origin_voxel_size[1] = world.origin.y;
	reflection_data.world_origin_voxel_size[2] = world.origin.z;
	reflection_data.world_origin_voxel_size[3] = world.voxel_size;
	reflection_data.camera_position_max_distance[0] = camera_position.x;
	reflection_data.camera_position_max_distance[1] = camera_position.y;
	reflection_data.camera_position_max_distance[2] = camera_position.z;
	reflection_data.camera_position_max_distance[3] = MAX(world.voxel_size, float(GLOBAL_GET("rendering/voxel_forward/reflections/max_distance")));
	for (uint32_t cascade = 0; cascade < REFLECTION_CASCADE_COUNT; cascade++) {
		reflection_data.color_grid_origin_cell_size[cascade][0] = reflection_color_grid_origin[cascade].x;
		reflection_data.color_grid_origin_cell_size[cascade][1] = reflection_color_grid_origin[cascade].y;
		reflection_data.color_grid_origin_cell_size[cascade][2] = reflection_color_grid_origin[cascade].z;
		reflection_data.color_grid_origin_cell_size[cascade][3] = reflection_color_grid_cell_size[cascade];
		reflection_data.indirect_grid_origin_cell_size[cascade][0] = indirect_grid_origin[cascade].x;
		reflection_data.indirect_grid_origin_cell_size[cascade][1] = indirect_grid_origin[cascade].y;
		reflection_data.indirect_grid_origin_cell_size[cascade][2] = indirect_grid_origin[cascade].z;
		reflection_data.indirect_grid_origin_cell_size[cascade][3] = indirect_grid_cell_size[cascade];
	}
	Color ambient_color = GLOBAL_GET("rendering/voxel_forward/ambient_light/color");
	float ambient_energy = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/ambient_light/energy")));
	if (p_render_data->environment.is_valid()) {
		ambient_color = environment_get_ambient_light(p_render_data->environment);
		ambient_energy = MAX(0.0f, environment_get_ambient_light_energy(p_render_data->environment));
	}
	reflection_data.ambient_color_energy[0] = ambient_color.r;
	reflection_data.ambient_color_energy[1] = ambient_color.g;
	reflection_data.ambient_color_energy[2] = ambient_color.b;
	reflection_data.ambient_color_energy[3] = ambient_energy;
	reflection_data.screen_grid_steps[0] = screen_size.x;
	reflection_data.screen_grid_steps[1] = screen_size.y;
	reflection_data.screen_grid_steps[2] = resolution;
	reflection_data.screen_grid_steps[3] = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/reflections/max_steps")), 1, 4096);
	reflection_data.state[0] = world.directory_mask;
	reflection_data.state[1] = indirect_cascade_initialized[0] && indirect_cascade_initialized[1] && indirect_cascade_initialized[2] ? 1 : 0;
	reflection_data.state[2] = indirect_grid_resolution;
	RD::get_singleton()->buffer_update(reflection_uniform_buffer, 0, sizeof(ReflectionUniformData), &reflection_data);

	RID indirect_textures[INDIRECT_CASCADE_COUNT];
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		indirect_textures[cascade] = indirect_grid_rd[cascade][0].is_valid() ? indirect_grid_rd[cascade][0] : reflection_color_grid_rd[cascade];
	}
	const RID resolve_shader_rid = reflection_resolve_shader.version_get_shader(reflection_resolve_shader_version, 0);
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, depth }));
	RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ output }));
	RD::Uniform u_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ world.directory_buffer }));
	RD::Uniform u_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ world.brick_buffer }));
	RD::Uniform u_color_near(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ sampler, reflection_color_grid_rd[0] }));
	RD::Uniform u_color_far(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ sampler, reflection_color_grid_rd[1] }));
	RD::Uniform u_color_distant(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ sampler, reflection_color_grid_rd[2] }));
	RD::Uniform u_indirect_near(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ sampler, indirect_textures[0] }));
	RD::Uniform u_indirect_far(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ sampler, indirect_textures[1] }));
	RD::Uniform u_indirect_distant(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ sampler, indirect_textures[2] }));
	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 10, Vector<RID>({ reflection_uniform_buffer }));
	const RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(resolve_shader_rid, 0, u_depth, u_output, u_directory, u_bricks, u_color_near, u_color_far, u_color_distant, u_indirect_near, u_indirect_far, u_indirect_distant, u_params);

	RENDER_TIMESTAMP("Voxel Face Reflections");
	RD::get_singleton()->draw_command_begin_label("Voxel Face Reflections");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, reflection_resolve_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, screen_size.x, screen_size.y, 1);
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_reflection_intensity"), MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/reflections/intensity"))));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_reflection_ready"), true);
}

void RenderVoxelForward::_add_voxel_occupancy_uniforms(Vector<RD::Uniform> &r_uniforms) {
	const VoxelForwardVolumeStorage::WorldOccupancy &world = volume_storage.get_world_occupancy();
	if (!world.directory_buffer.is_valid() || !world.brick_buffer.is_valid() || !occupancy_uniform_buffer.is_valid()) {
		for (uint32_t binding = 21; binding <= 22; binding++) {
			RD::Uniform dummy_buffer;
			dummy_buffer.binding = binding;
			dummy_buffer.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			dummy_buffer.append_id(_get_default_vec4_xform_buffer());
			r_uniforms.push_back(dummy_buffer);
		}
		RD::Uniform dummy_params;
		dummy_params.binding = 23;
		dummy_params.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		dummy_params.append_id(sdfgi_get_ubo());
		r_uniforms.push_back(dummy_params);
	} else {
		RD::Uniform u_directory;
		u_directory.binding = 21;
		u_directory.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u_directory.append_id(world.directory_buffer);
		r_uniforms.push_back(u_directory);

		RD::Uniform u_bricks;
		u_bricks.binding = 22;
		u_bricks.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u_bricks.append_id(world.brick_buffer);
		r_uniforms.push_back(u_bricks);

		RD::Uniform u_params;
		u_params.binding = 23;
		u_params.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u_params.append_id(occupancy_uniform_buffer);
		r_uniforms.push_back(u_params);
	}

	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	const RID default_3d = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_BLACK);
	const Vector<RID> *tables[3] = {
		&volume_storage.get_batch_voxel_textures(),
		&volume_storage.get_batch_brick_textures(),
		&volume_storage.get_batch_neighbor_textures(),
	};
	for (uint32_t table = 0; table < 3; table++) {
		RD::Uniform texture_array;
		texture_array.binding = 24 + table;
		texture_array.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		for (uint32_t index = 0; index < VoxelForwardVolumeStorage::MAX_BATCH_TEXTURES; index++) {
			const RID texture = (*tables[table])[index];
			const RID rd_texture = texture.is_valid() ? texture_storage->texture_get_rd_texture(texture) : RID();
			texture_array.append_id(rd_texture.is_valid() ? rd_texture : default_3d);
		}
		r_uniforms.push_back(texture_array);
	}

}

void RenderVoxelForward::_fill_voxel_instance_data(RID p_base, VoxelInstanceData &r_instance_data) const {
	const VoxelForwardVolumeStorage::Volume *volume = volume_storage.get_volume(p_base);
	if (volume == nullptr || volume->batch_texture_index == VoxelForwardVolumeStorage::INVALID_BATCH_TEXTURE_INDEX) {
		return;
	}
	for (int axis = 0; axis < 3; axis++) {
		r_instance_data.volume_dims_resource[axis] = volume->dimensions[axis];
		r_instance_data.brick_dims_neighbor_mask[axis] = volume->brick_dimensions[axis];
		r_instance_data.atlas_dims_diagonal_mask[axis] = volume->atlas_brick_dimensions[axis];
	}
	r_instance_data.volume_dims_resource[3] = int32_t(volume->batch_texture_index);
	r_instance_data.brick_dims_neighbor_mask[3] = int32_t(volume->neighbor_mask);
	r_instance_data.atlas_dims_diagonal_mask[3] = int32_t(volume->neighbor_diagonal_mask);
	r_instance_data.voxel_size_pad[0] = volume->voxel_size;
}

bool RenderVoxelForward::_render_scene_custom_uses_resolved_depth() const {
	return bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/enabled")) || bool(GLOBAL_GET("rendering/voxel_forward/reflections/enabled"));
}

void RenderVoxelForward::_render_scene_custom_pre_opaque(RenderDataRD *p_render_data, bool p_depth_prepass) {
	if (!p_depth_prepass) {
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_ready"), false);
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_reflection_ready"), false);
		if (shadow_mask_texture.is_valid()) {
			RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), Variant());
			RendererRD::TextureStorage::get_singleton()->texture_free(shadow_mask_texture);
			shadow_mask_texture = RID();
			shadow_mask_source_rd = RID();
		}
		_reset_shadow_atlas_state();
	} else {
		_render_shadow_atlas(p_render_data);
		_render_voxel_reflections(p_render_data);
	}
}

void RenderVoxelForward::_render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	Color ambient_color = GLOBAL_GET("rendering/voxel_forward/ambient_light/color");
	float ambient_energy = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/ambient_light/energy")));
	if (p_render_data != nullptr && p_render_data->environment.is_valid()) {
		// Voxel Forward owns ambient evaluation, but follows the active
		// WorldEnvironment's color and energy controls.
		ambient_color = environment_get_ambient_light(p_render_data->environment);
		ambient_energy = MAX(0.0f, environment_get_ambient_light_energy(p_render_data->environment));
	}
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ambient_color"), ambient_color);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ambient_energy"), ambient_energy);
	const bool shadow_mask_enabled = bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/enabled"));
	const bool reflection_enabled = bool(GLOBAL_GET("rendering/voxel_forward/reflections/enabled"));
	volume_storage.update_world_occupancy(shadow_mask_enabled || reflection_enabled);
	const VoxelForwardVolumeStorage::WorldOccupancy &current_world = volume_storage.get_world_occupancy();
	if (current_world.directory_buffer != bound_occupancy_directory || current_world.brick_buffer != bound_occupancy_bricks) {
		bound_occupancy_directory = current_world.directory_buffer;
		bound_occupancy_bricks = current_world.brick_buffer;
		base_uniforms_changed();
	}
	if (bound_batch_texture_revision != volume_storage.get_batch_texture_revision()) {
		bound_batch_texture_revision = volume_storage.get_batch_texture_revision();
		base_uniforms_changed();
	}
	OccupancyUniformData occupancy_data = {};
	occupancy_data.world_origin_voxel_size[0] = current_world.origin.x;
	occupancy_data.world_origin_voxel_size[1] = current_world.origin.y;
	occupancy_data.world_origin_voxel_size[2] = current_world.origin.z;
	occupancy_data.world_origin_voxel_size[3] = current_world.voxel_size;
	occupancy_data.directory_steps[0] = current_world.directory_mask;
	occupancy_data.directory_steps[1] = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/max_steps")), 1, 4096);
	occupancy_data.directory_steps[2] = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/max_local_lights")), 0, 32);
	occupancy_data.directory_steps[3] = shadow_mask_enabled && current_world.directory_buffer.is_valid() && current_world.brick_buffer.is_valid() && current_world.occupied_brick_count > 0 ? 1 : 0;
	occupancy_data.limits[0] = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/max_distance")));
	occupancy_data.limits[1] = int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_mode")) == 1 ? 1.0f : 0.0f;
	// Four- and eight-sample modes use centered rings. One sample retains the
	// hard-shadow fast path while keeping a single quality control.
	occupancy_data.limits[2] = occupancy_data.limits[1] > 0.5f ?
			CLAMP(int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_samples")), 1, 8) :
			1.0f;
	occupancy_data.limits[3] = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_radius_voxels")));
	RD::get_singleton()->buffer_update(occupancy_uniform_buffer, 0, sizeof(OccupancyUniformData), &occupancy_data);
	visible_volumes.clear();
	const bool custom_visibility_enabled = bool(GLOBAL_GET("rendering/voxel_forward/experimental_custom_visibility"));
	const bool inspect_visible_volumes = custom_visibility_enabled || is_print_verbose_enabled();
	uint32_t visible_volume_count = 0;
	if (inspect_visible_volumes && p_render_data != nullptr && p_render_data->instances != nullptr) {
		for (uint32_t i = 0; i < p_render_data->instances->size(); i++) {
			RenderGeometryInstance *instance = (*p_render_data->instances)[i];
			const VoxelForwardVolumeStorage::Volume *volume = volume_storage.get_volume(instance->get_base());
			if (volume == nullptr) {
				continue;
			}
			visible_volume_count++;
			if (!custom_visibility_enabled) {
				continue;
			}

			VisibleVolume visible;
			visible.voxel_texture = volume->voxel_texture;
			visible.brick_texture = volume->brick_texture;
			visible.palette_texture = volume->palette_texture;
			visible.transform = instance->get_transform();
			visible.dimensions = volume->dimensions;
			visible.voxel_size = volume->voxel_size;
			visible_volumes.push_back(visible);
		}
	}
	const uint32_t registered_volume_count = volume_storage.get_volume_count();
	const uint64_t occupancy_gpu_bytes = volume_storage.get_occupancy_gpu_bytes();
	const VoxelForwardVolumeStorage::WorldOccupancy &world_occupancy = volume_storage.get_world_occupancy();
	if (registered_volume_count != last_registered_volume_count || visible_volume_count != last_visible_volume_count || occupancy_gpu_bytes != last_occupancy_gpu_bytes || world_occupancy.revision != last_world_occupancy_revision) {
		print_verbose(vformat("Voxel Forward: %d registered voxel volumes, %d visible, %.2f MiB occupancy; world table %d bricks (%d mixed, %d tombstones), max probe %d, %d incompatible; last update %d dirty bricks / %.2f KiB, %d incremental / %d full.", registered_volume_count, visible_volume_count, double(occupancy_gpu_bytes) / (1024.0 * 1024.0), world_occupancy.occupied_brick_count, world_occupancy.mixed_brick_count, world_occupancy.tombstone_count, world_occupancy.max_probe_count, world_occupancy.incompatible_volume_count, world_occupancy.last_dirty_brick_count, double(world_occupancy.last_uploaded_bytes) / 1024.0, world_occupancy.incremental_update_count, world_occupancy.full_rebuild_count));
		last_registered_volume_count = registered_volume_count;
		last_visible_volume_count = visible_volume_count;
		last_occupancy_gpu_bytes = occupancy_gpu_bytes;
		last_world_occupancy_revision = world_occupancy.revision;
	}

	RenderForwardClustered::_render_scene(p_render_data, p_default_bg_color);
}

void RenderVoxelForward::_render_shadow_atlas(const RenderDataRD *p_render_data) {
	// Snapshots retired after last frame's publish can be released now. Rendering
	// Device defers the underlying destruction until submitted GPU work is done.
	_release_shadow_atlas_snapshot(shadow_atlas_retired_snapshot);
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	auto disable_voxel_lighting = [material_storage]() {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_ready"), false);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), false);
	};
	if (!bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/enabled")) || p_render_data == nullptr || p_render_data->render_buffers.is_null() || p_render_data->scene_data == nullptr || p_render_data->scene_data->view_count != 1 || p_render_data->lights == nullptr || !shadow_atlas_pipeline.is_valid() || !shadow_resolve_legacy_pipeline.is_valid() || !shadow_resolve_payload_pipeline.is_valid()) {
		disable_voxel_lighting();
		if (shadow_mask_texture.is_valid()) {
			RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), Variant());
			RendererRD::TextureStorage::get_singleton()->texture_free(shadow_mask_texture);
			shadow_mask_texture = RID();
			shadow_mask_source_rd = RID();
		}
		_reset_shadow_atlas_state();
		return;
	}
	const VoxelForwardVolumeStorage::WorldOccupancy &world = volume_storage.get_world_occupancy();
	if (!world.directory_buffer.is_valid() || !world.brick_buffer.is_valid() || !occupancy_uniform_buffer.is_valid() || world.occupied_brick_count == 0 || world.voxel_size <= 0.0f) {
		disable_voxel_lighting();
		_reset_shadow_atlas_state();
		return;
	}

	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	Vector3 light_direction;
	Color light_color;
	float light_energy = 0.0f;
	bool found_directional_light = false;
	for (uint32_t i = 0; i < p_render_data->lights->size(); i++) {
		const RID light_instance = (*p_render_data->lights)[i];
		if (light_storage->light_instance_get_type(light_instance) != RSE::LIGHT_DIRECTIONAL) {
			continue;
		}
		// DirectionalLight3D propagates along local -Z. Occupancy traces from the
		// receiver back toward the source, so it follows local +Z.
		light_direction = light_storage->light_instance_get_base_transform(light_instance).basis.xform(Vector3(0, 0, 1)).normalized();
		const RID light = light_storage->light_instance_get_base_light(light_instance);
		light_color = light_storage->light_get_color(light).srgb_to_linear();
		light_energy = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ENERGY) * light_storage->light_get_param(light, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
		found_directional_light = true;
		break;
	}
	if (!found_directional_light) {
		disable_voxel_lighting();
		_reset_shadow_atlas_state();
		return;
	}

	Ref<RenderSceneBuffersRD> render_buffers = p_render_data->render_buffers;
	const Size2i internal_size = render_buffers->get_internal_size();
	const float resolution_scale = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/resolution_scale")), 0.25f, 1.0f);
	const Size2i screen_size(MAX(1, int(Math::ceil(internal_size.x * resolution_scale))), MAX(1, int(Math::ceil(internal_size.y * resolution_scale))));
	const uint32_t atlas_resolution = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/atlas_resolution")), 128, 2048));
	const float far_extent = MAX(world.voxel_size * 8.0f, float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/max_distance")));
	const float near_extent = far_extent * CLAMP(float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/atlas_near_extent_ratio")), 0.05f, 0.75f);
	const StringName scope = SNAME("voxel_forward");
	const StringName atlas_texture_names[2] = { SNAME("occupancy_shadow_atlas_a"), SNAME("occupancy_shadow_atlas_b") };
	const StringName mask_texture_name = SNAME("occupancy_shadow_mask");
	if (shadow_atlas_allocated_resolution != 0 && shadow_atlas_allocated_resolution != atlas_resolution) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), Variant());
		if (shadow_mask_texture.is_valid()) {
			RendererRD::TextureStorage::get_singleton()->texture_free(shadow_mask_texture);
			shadow_mask_texture = RID();
		}
		render_buffers->clear_context(scope);
		if (shadow_atlas_scratch_rd.is_valid()) {
			RD::get_singleton()->free_rid(shadow_atlas_scratch_rd);
			shadow_atlas_scratch_rd = RID();
		}
		shadow_mask_source_rd = RID();
		shadow_atlas_source_rd = RID();
		shadow_atlas_scratch_source_rd = RID();
		shadow_atlas_scratch_resolution = 0;
		_release_shadow_atlas_snapshot(shadow_atlas_staging);
		shadow_atlas_temporal_rebuild_in_progress = false;
		shadow_atlas_rebuild_next_texel = 0;
		shadow_atlas_rebuild_total_texels = 0;
		shadow_atlas_active_slot = 0;
		shadow_atlas_initialized = false;
	}
	for (uint32_t slot = 0; slot < 2; slot++) {
		if (!render_buffers->has_texture(scope, atlas_texture_names[slot])) {
			render_buffers->create_texture(scope, atlas_texture_names[slot], RD::DATA_FORMAT_R32_SFLOAT, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT, RD::TEXTURE_SAMPLES_1, Size2i(atlas_resolution * 2, atlas_resolution));
		}
	}
	shadow_atlas_allocated_resolution = atlas_resolution;
	if (!render_buffers->has_texture(scope, mask_texture_name)) {
		render_buffers->create_texture(scope, mask_texture_name, RD::DATA_FORMAT_R8_UNORM, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, screen_size);
	}
	RID atlas_slots[2] = {
		render_buffers->get_texture(scope, atlas_texture_names[0]),
		render_buffers->get_texture(scope, atlas_texture_names[1])
	};
	RID atlas_output = atlas_slots[shadow_atlas_active_slot];
	RID atlas_staging_output = atlas_slots[1u - shadow_atlas_active_slot];
	const RID mask_output = render_buffers->get_texture(scope, mask_texture_name);
	const RID depth = render_buffers->get_depth_texture();
	RD *rd = RD::get_singleton();
	if (rd == nullptr || !atlas_output.is_valid() || !rd->texture_is_valid(atlas_output) ||
			!atlas_staging_output.is_valid() || !rd->texture_is_valid(atlas_staging_output) ||
			!mask_output.is_valid() || !rd->texture_is_valid(mask_output) ||
			!depth.is_valid() || !rd->texture_is_valid(depth)) {
		disable_voxel_lighting();
		return;
	}

	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	const RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	if (shadow_mask_source_rd != mask_output) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), Variant());
		if (shadow_mask_texture.is_valid()) {
			texture_storage->texture_free(shadow_mask_texture);
		}
		shadow_mask_texture = texture_storage->texture_allocate();
		texture_storage->texture_rd_initialize(shadow_mask_texture, mask_output);
		shadow_mask_source_rd = mask_output;
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), shadow_mask_texture);
	}

	Vector3 reference_axis = Math::abs(light_direction.y) < 0.95f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
	const Vector3 tangent = reference_axis.cross(light_direction).normalized();
	const Vector3 bitangent = light_direction.cross(tangent).normalized();
	const Vector3 camera_position = p_render_data->scene_data->cam_transform.origin;
	const Vector3 camera_from_center = camera_position - shadow_atlas_center;
	const float recenter_distance = near_extent * CLAMP(float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/atlas_recenter_ratio")), 0.05f, 0.75f);
	const bool tangent_outside_center = !shadow_atlas_initialized || Math::abs(camera_from_center.dot(tangent)) > recenter_distance;
	const bool bitangent_outside_center = !shadow_atlas_initialized || Math::abs(camera_from_center.dot(bitangent)) > recenter_distance;
	const bool depth_outside_center = !shadow_atlas_initialized || Math::abs(camera_from_center.dot(light_direction)) > recenter_distance;
	const bool camera_outside_center = tangent_outside_center || bitangent_outside_center || depth_outside_center;
	const bool incremental_enabled = bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/incremental_atlas_updates"));
	const bool dirty_region_updates_enabled = bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/dirty_region_updates_enabled"));
	const bool incremental_mode_changed = shadow_atlas_initialized && shadow_atlas_incremental_enabled != incremental_enabled;
	const bool world_invalid = shadow_atlas_world_revision != world.revision;
	// Occupancy buffer replacement always advances world.revision. Comparing live
	// buffer RIDs here would incorrectly invalidate an atlas built from a private
	// immutable snapshot of the same revision.
	const bool source_invalid = shadow_atlas_source_rd != atlas_output;
	const bool light_invalid = !shadow_atlas_light_direction.is_equal_approx(light_direction) ||
			!shadow_atlas_tangent.is_equal_approx(tangent) || !shadow_atlas_bitangent.is_equal_approx(bitangent);
	const bool layout_invalid = shadow_atlas_resolution != atlas_resolution ||
			!Math::is_equal_approx(shadow_atlas_near_extent, near_extent) ||
			!Math::is_equal_approx(shadow_atlas_far_extent, far_extent);
	const bool atlas_invalid = !shadow_atlas_initialized || source_invalid || world_invalid || light_invalid || layout_invalid || camera_outside_center || incremental_mode_changed;
	const int32_t max_shadow_steps = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/max_steps")), 1, 4096);
	const bool temporal_rebuild_enabled = bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/temporal_rebuild_enabled"));
	const bool force_full_rebuild = bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/force_full_rebuild"));
	const bool force_full_rebuild_requested = force_full_rebuild && !shadow_atlas_force_full_rebuild_latched;
	shadow_atlas_force_full_rebuild_latched = force_full_rebuild;
	if (!temporal_rebuild_enabled && shadow_atlas_temporal_rebuild_in_progress) {
		_release_shadow_atlas_snapshot(shadow_atlas_staging);
		shadow_atlas_temporal_rebuild_in_progress = false;
		shadow_atlas_rebuild_next_texel = 0;
		shadow_atlas_rebuild_total_texels = 0;
	}

	// Snap the target independently from the staging state. Camera changes that
	// arrive during a rebuild are coalesced and picked up after the current
	// complete atlas is published instead of continually restarting partial work.
	const float planar_snap_size = MAX(world.voxel_size * 0.25f, far_extent * 2.0f / float(atlas_resolution));
	const float depth_snap_size = MAX(world.voxel_size * 0.25f, near_extent * 2.0f / float(atlas_resolution));
	const float requested_tangent = tangent_outside_center ? Math::floor(camera_position.dot(tangent) / planar_snap_size + 0.5f) * planar_snap_size : shadow_atlas_center.dot(tangent);
	const float requested_bitangent = bitangent_outside_center ? Math::floor(camera_position.dot(bitangent) / planar_snap_size + 0.5f) * planar_snap_size : shadow_atlas_center.dot(bitangent);
	const float requested_depth = depth_outside_center ? Math::floor(camera_position.dot(light_direction) / depth_snap_size + 0.5f) * depth_snap_size : shadow_atlas_center.dot(light_direction);
	const Vector3 requested_center = tangent * requested_tangent + bitangent * requested_bitangent + light_direction * requested_depth;
	const bool direct_reliable_world_delta = !world_invalid ||
			(shadow_atlas_world_revision != UINT64_MAX && shadow_atlas_world_revision + 1 == world.revision &&
					world.last_incremental_revision == world.revision && !world.last_dirty_bricks.is_empty());
	const bool coalesced_reliable_world_delta = world_invalid && shadow_atlas_pending_world_revision == world.revision &&
			shadow_atlas_pending_dirty_reliable && !shadow_atlas_pending_dirty_bricks.is_empty();
	const bool reliable_world_delta = direct_reliable_world_delta || coalesced_reliable_world_delta;
	const bool can_attempt_dirty_region_update = dirty_region_updates_enabled && world_invalid && reliable_world_delta && !camera_outside_center;
	const bool can_attempt_camera_scroll = incremental_enabled && camera_outside_center && shadow_atlas_scroll_pipeline.is_valid();
	const bool can_attempt_ordinary_incremental = shadow_atlas_initialized && !source_invalid && !light_invalid && !layout_invalid && !incremental_mode_changed &&
			(can_attempt_dirty_region_update || can_attempt_camera_scroll);
	const bool temporal_path_required = shadow_atlas_temporal_rebuild_in_progress || shadow_atlas_pending_full_rebuild || (atlas_invalid && !can_attempt_ordinary_incremental);

	if (temporal_rebuild_enabled && temporal_path_required) {
		// Render-buffer contexts may replace their textures without changing atlas
		// dimensions (for example, switching from the editor viewport to Play).
		// Unlike a world revision, a dead staging image cannot be allowed to finish.
		// Abort only this invalid-resource build and immediately take a new coherent
		// snapshot targeting the replacement texture.
		if (shadow_atlas_temporal_rebuild_in_progress &&
				(shadow_atlas_staging.texture != atlas_staging_output || !shadow_atlas_staging.texture.is_valid() || !rd->texture_is_valid(shadow_atlas_staging.texture))) {
			const uint64_t abandoned_texels = shadow_atlas_rebuild_next_texel;
			_release_shadow_atlas_snapshot(shadow_atlas_staging);
			shadow_atlas_temporal_rebuild_in_progress = false;
			shadow_atlas_rebuild_next_texel = 0;
			shadow_atlas_rebuild_total_texels = 0;
			shadow_atlas_pending_full_rebuild = true;
			shadow_atlas_temporal_restart_count++;
			print_verbose(vformat("Voxel Forward shadow atlas temporal rebuild aborted after %d texels: staging render-buffer texture was replaced; restart #%d will use the current texture.", abandoned_texels, shadow_atlas_temporal_restart_count));
		}
		if (shadow_atlas_temporal_rebuild_in_progress) {
			if (shadow_atlas_staging.world_revision != world.revision && shadow_atlas_pending_world_revision != world.revision) {
				const uint64_t expected_revision = shadow_atlas_pending_world_revision == UINT64_MAX ? shadow_atlas_staging.world_revision + 1 : shadow_atlas_pending_world_revision + 1;
				if (world.revision != expected_revision || world.last_incremental_revision != world.revision || world.last_dirty_bricks.is_empty()) {
					shadow_atlas_pending_dirty_reliable = false;
				}
				for (const Vector3i &dirty_brick : world.last_dirty_bricks) {
					shadow_atlas_pending_dirty_bricks.insert(dirty_brick);
				}
				shadow_atlas_pending_world_revision = world.revision;
				shadow_atlas_pending_change_count++;
			}
			const bool staging_definition_changed =
					shadow_atlas_staging.texture != atlas_staging_output ||
					shadow_atlas_staging.world_revision != world.revision ||
					shadow_atlas_staging.world_origin != world.origin ||
					!Math::is_equal_approx(shadow_atlas_staging.voxel_size, world.voxel_size) ||
					!shadow_atlas_staging.center.is_equal_approx(requested_center) ||
					!shadow_atlas_staging.light_direction.is_equal_approx(light_direction) ||
					!shadow_atlas_staging.tangent.is_equal_approx(tangent) ||
					!shadow_atlas_staging.bitangent.is_equal_approx(bitangent) ||
					shadow_atlas_staging.resolution != atlas_resolution ||
					shadow_atlas_staging.directory_mask != world.directory_mask ||
					shadow_atlas_staging.max_steps != max_shadow_steps ||
					!Math::is_equal_approx(shadow_atlas_staging.near_extent, near_extent) ||
					!Math::is_equal_approx(shadow_atlas_staging.far_extent, far_extent);
			if (staging_definition_changed || force_full_rebuild_requested) {
				if (shadow_atlas_pending_change_count == 0) {
					shadow_atlas_pending_change_count++;
				}
				const bool incompatible_pending_change = force_full_rebuild_requested ||
						shadow_atlas_staging.texture != atlas_staging_output ||
						shadow_atlas_staging.world_origin != world.origin ||
						!Math::is_equal_approx(shadow_atlas_staging.voxel_size, world.voxel_size) ||
						!shadow_atlas_staging.light_direction.is_equal_approx(light_direction) ||
						!shadow_atlas_staging.tangent.is_equal_approx(tangent) ||
						!shadow_atlas_staging.bitangent.is_equal_approx(bitangent) ||
						shadow_atlas_staging.resolution != atlas_resolution ||
						shadow_atlas_staging.directory_mask != world.directory_mask ||
						shadow_atlas_staging.max_steps != max_shadow_steps ||
						!Math::is_equal_approx(shadow_atlas_staging.near_extent, near_extent) ||
						!Math::is_equal_approx(shadow_atlas_staging.far_extent, far_extent);
				shadow_atlas_pending_full_rebuild = shadow_atlas_pending_full_rebuild || incompatible_pending_change;
			}
		}

		// Never restart an active build. It reads private occupancy buffers and is
		// therefore a coherent, immutable definition even if the live world moves.
		const bool begin_rebuild = !shadow_atlas_temporal_rebuild_in_progress && (atlas_invalid || force_full_rebuild_requested || shadow_atlas_pending_full_rebuild);
		if (begin_rebuild) {
			String rebuild_reason;
			if (force_full_rebuild_requested) {
				rebuild_reason = "explicit full-rebuild request";
			} else if (!shadow_atlas_initialized) {
				rebuild_reason = "initialization";
			} else if (source_invalid) {
				rebuild_reason = "atlas or occupancy resources changed";
			} else if (world_invalid) {
				rebuild_reason = "world occupancy revision changed";
			} else if (light_invalid) {
				rebuild_reason = "light-space basis changed";
			} else if (layout_invalid) {
				rebuild_reason = "atlas layout changed";
			} else if (incremental_mode_changed) {
				rebuild_reason = "incremental setting changed";
			} else {
				rebuild_reason = "camera recentered";
			}

			VoxelForwardVolumeStorage::WorldOccupancy snapshot;
			if (!volume_storage.create_world_occupancy_snapshot(snapshot)) {
				print_verbose(vformat("Voxel Forward shadow atlas temporal rebuild deferred: occupancy snapshot allocation failed at revision %d; previous complete atlas retained.", world.revision));
			} else {
				_release_shadow_atlas_snapshot(shadow_atlas_staging);
				shadow_atlas_staging.texture = atlas_staging_output;
				shadow_atlas_staging.directory = snapshot.directory_buffer;
				shadow_atlas_staging.bricks = snapshot.brick_buffer;
				shadow_atlas_staging.owns_occupancy_snapshot = true;
				shadow_atlas_staging.world_revision = snapshot.revision;
				shadow_atlas_staging.world_origin = snapshot.origin;
				shadow_atlas_staging.center = requested_center;
				shadow_atlas_staging.light_direction = light_direction;
				shadow_atlas_staging.tangent = tangent;
				shadow_atlas_staging.bitangent = bitangent;
				shadow_atlas_staging.resolution = atlas_resolution;
				shadow_atlas_staging.directory_mask = snapshot.directory_mask;
				shadow_atlas_staging.max_steps = max_shadow_steps;
				shadow_atlas_staging.voxel_size = snapshot.voxel_size;
				shadow_atlas_staging.near_extent = near_extent;
				shadow_atlas_staging.far_extent = far_extent;
				shadow_atlas_rebuild_next_texel = 0;
				shadow_atlas_rebuild_total_texels = uint64_t(atlas_resolution) * uint64_t(atlas_resolution) * 2u;
				shadow_atlas_temporal_rebuild_in_progress = true;
				shadow_atlas_temporal_rebuild_count++;
				shadow_atlas_pending_world_revision = UINT64_MAX;
				shadow_atlas_pending_change_count = 0;
				shadow_atlas_pending_full_rebuild = false;
				shadow_atlas_pending_dirty_reliable = true;
				shadow_atlas_pending_dirty_bricks.clear();
				print_verbose(vformat("Voxel Forward shadow atlas temporal rebuild #%d started: %s, %d texels, center %s, immutable revision %d.", shadow_atlas_temporal_rebuild_count, rebuild_reason, shadow_atlas_rebuild_total_texels, requested_center, snapshot.revision));
			}
		}

		if (shadow_atlas_temporal_rebuild_in_progress) {
			const uint64_t configured_budget = uint64_t(MAX(64, int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/rebuild_texel_budget"))));
			// The project setting is the explicit work ceiling. The previous hidden
			// 1,024-texel cap made every larger configured value ineffective and made
			// the default 512 atlas require 512 frames to publish. Align down to whole
			// workgroups so each frame remains deterministic and bounded.
			static constexpr uint64_t WORKGROUP_TEXELS = 8u * 8u;
			const uint64_t frame_budget = MAX(WORKGROUP_TEXELS, configured_budget / WORKGROUP_TEXELS * WORKGROUP_TEXELS);
			uint64_t frame_remaining = MIN(frame_budget, shadow_atlas_rebuild_total_texels - shadow_atlas_rebuild_next_texel);
			const uint32_t atlas_width = shadow_atlas_staging.resolution * 2u;
			const uint32_t workgroups_per_row = atlas_width / 8u;
			const RID atlas_shader_rid = shadow_atlas_shader.version_get_shader(shadow_atlas_shader_version, 0);
			RD::Uniform u_staging_output(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ shadow_atlas_staging.texture }));
			RD::Uniform u_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ shadow_atlas_staging.directory }));
			RD::Uniform u_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ shadow_atlas_staging.bricks }));
			const RID staging_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(atlas_shader_rid, 0, u_staging_output, u_directory, u_bricks);

			ShadowAtlasPushConstant atlas_push = {};
			atlas_push.world_origin_voxel_size[0] = shadow_atlas_staging.world_origin.x;
			atlas_push.world_origin_voxel_size[1] = shadow_atlas_staging.world_origin.y;
			atlas_push.world_origin_voxel_size[2] = shadow_atlas_staging.world_origin.z;
			atlas_push.world_origin_voxel_size[3] = shadow_atlas_staging.voxel_size;
			atlas_push.atlas_center_depth[0] = shadow_atlas_staging.center.x;
			atlas_push.atlas_center_depth[1] = shadow_atlas_staging.center.y;
			atlas_push.atlas_center_depth[2] = shadow_atlas_staging.center.z;
			atlas_push.atlas_center_depth[3] = shadow_atlas_staging.far_extent;
			atlas_push.tangent_near_extent[0] = shadow_atlas_staging.tangent.x;
			atlas_push.tangent_near_extent[1] = shadow_atlas_staging.tangent.y;
			atlas_push.tangent_near_extent[2] = shadow_atlas_staging.tangent.z;
			atlas_push.tangent_near_extent[3] = shadow_atlas_staging.near_extent;
			atlas_push.bitangent_far_extent[0] = shadow_atlas_staging.bitangent.x;
			atlas_push.bitangent_far_extent[1] = shadow_atlas_staging.bitangent.y;
			atlas_push.bitangent_far_extent[2] = shadow_atlas_staging.bitangent.z;
			atlas_push.bitangent_far_extent[3] = shadow_atlas_staging.far_extent;
			atlas_push.atlas_directory_steps[0] = shadow_atlas_staging.resolution;
			atlas_push.atlas_directory_steps[1] = 2;
			atlas_push.atlas_directory_steps[2] = shadow_atlas_staging.directory_mask;
			atlas_push.atlas_directory_steps[3] = shadow_atlas_staging.max_steps;

			RENDER_TIMESTAMP("Shadow Atlas Temporal Rebuild");
			RD::get_singleton()->draw_command_begin_label("Shadow Atlas Temporal Rebuild");
			RD::ComputeListID atlas_list = RD::get_singleton()->compute_list_begin();
			RD::get_singleton()->compute_list_bind_compute_pipeline(atlas_list, shadow_atlas_pipeline);
			RD::get_singleton()->compute_list_bind_uniform_set(atlas_list, staging_uniform_set, 0);
			while (frame_remaining > 0) {
				const uint64_t workgroup_index = shadow_atlas_rebuild_next_texel / WORKGROUP_TEXELS;
				const uint32_t workgroup_row = uint32_t(workgroup_index / workgroups_per_row);
				const uint32_t workgroup_column = uint32_t(workgroup_index % workgroups_per_row);
				const uint32_t remaining_workgroups_in_row = workgroups_per_row - workgroup_column;
				const uint32_t dispatched_workgroups = uint32_t(MIN(frame_remaining / WORKGROUP_TEXELS, uint64_t(remaining_workgroups_in_row)));
				const uint32_t rect_width = dispatched_workgroups * 8u;
				const uint32_t rect_height = 8u;
				atlas_push.dispatch_rect[0] = workgroup_column * 8u;
				atlas_push.dispatch_rect[1] = workgroup_row * 8u;
				atlas_push.dispatch_rect[2] = rect_width;
				atlas_push.dispatch_rect[3] = rect_height;
				RD::get_singleton()->compute_list_set_push_constant(atlas_list, &atlas_push, sizeof(ShadowAtlasPushConstant));
				RD::get_singleton()->compute_list_dispatch_threads(atlas_list, rect_width, rect_height, 1);
				const uint64_t dispatched_texels = uint64_t(dispatched_workgroups) * WORKGROUP_TEXELS;
				shadow_atlas_rebuild_next_texel += dispatched_texels;
				frame_remaining -= dispatched_texels;
			}
			RD::get_singleton()->compute_list_end();
			RD::get_singleton()->draw_command_end_label();

			if (shadow_atlas_rebuild_next_texel == shadow_atlas_rebuild_total_texels) {
				const uint64_t completed_texels = shadow_atlas_rebuild_total_texels;
				RENDER_TIMESTAMP("Shadow Atlas Publish");
				RD::get_singleton()->draw_command_begin_label("Shadow Atlas Publish");
				shadow_atlas_active_slot = 1u - shadow_atlas_active_slot;
				atlas_output = shadow_atlas_staging.texture;
				atlas_staging_output = atlas_slots[1u - shadow_atlas_active_slot];
				shadow_atlas_source_rd = atlas_output;
				// The published texture records the snapshot revision. Live buffer RIDs
				// are bookkeeping only; newer revisions remain invalid and trigger one
				// coherent follow-up update on the next frame.
				shadow_atlas_directory_rd = world.directory_buffer;
				shadow_atlas_bricks_rd = world.brick_buffer;
				shadow_atlas_world_revision = shadow_atlas_staging.world_revision;
				shadow_atlas_center = shadow_atlas_staging.center;
				shadow_atlas_light_direction = shadow_atlas_staging.light_direction;
				shadow_atlas_tangent = shadow_atlas_staging.tangent;
				shadow_atlas_bitangent = shadow_atlas_staging.bitangent;
				shadow_atlas_resolution = shadow_atlas_staging.resolution;
				shadow_atlas_voxel_size = shadow_atlas_staging.voxel_size;
				shadow_atlas_near_extent = shadow_atlas_staging.near_extent;
				shadow_atlas_far_extent = shadow_atlas_staging.far_extent;
				shadow_atlas_incremental_enabled = incremental_enabled;
				shadow_atlas_initialized = true;
				shadow_atlas_temporal_rebuild_in_progress = false;
				shadow_atlas_rebuild_next_texel = 0;
				shadow_atlas_rebuild_total_texels = 0;
				shadow_atlas_retired_snapshot = shadow_atlas_staging;
				shadow_atlas_staging = ShadowAtlasBuildState();
				shadow_atlas_temporal_publish_count++;
				RD::get_singleton()->draw_command_end_label();
				print_verbose(vformat("Voxel Forward shadow atlas temporal publish #%d completed from rebuild #%d; active slot %d, center %s, exact revision %d, %d texels processed, latest revision %d, %d pending changes / %d pending dirty regions, %d restarts.", shadow_atlas_temporal_publish_count, shadow_atlas_temporal_rebuild_count, shadow_atlas_active_slot, shadow_atlas_center, shadow_atlas_world_revision, completed_texels, world.revision, shadow_atlas_pending_change_count, shadow_atlas_pending_dirty_bricks.size(), shadow_atlas_temporal_restart_count));
			}
		}
	} else if (atlas_invalid || force_full_rebuild_requested) {

		// Snapping planar movement to the far-cascade texel grid makes the default
		// near/far ratio an exact integer shift in both cascades. Axes that did not
		// cross their hysteresis threshold retain their old coordinate, especially
		// the depth axis whose movement changes the traced ray interval.
		const Vector3 center_delta = requested_center - shadow_atlas_center;
		bool use_incremental = !force_full_rebuild_requested && shadow_atlas_initialized && !source_invalid && !light_invalid && !layout_invalid && !incremental_mode_changed &&
			(can_attempt_dirty_region_update || can_attempt_camera_scroll);
		String fallback_reason;
		if (force_full_rebuild_requested) {
			fallback_reason = "explicit full-rebuild request";
		} else if (!incremental_enabled && !can_attempt_dirty_region_update) {
			fallback_reason = "camera scrolling disabled and no reliable dirty world region is available";
		} else if (!shadow_atlas_initialized) {
			fallback_reason = "initialization";
		} else if (source_invalid) {
			fallback_reason = "atlas resource changed";
		} else if (light_invalid) {
			fallback_reason = "light-space basis changed";
		} else if (layout_invalid) {
			fallback_reason = "atlas layout changed";
		} else if (incremental_mode_changed) {
			fallback_reason = "incremental update setting changed";
		} else if (camera_outside_center && !shadow_atlas_scroll_pipeline.is_valid()) {
			fallback_reason = "scroll pipeline unavailable";
		}

		if (use_incremental && !reliable_world_delta) {
			use_incremental = false;
			fallback_reason = "world revision has no complete dirty region";
		}

		Vector2i cascade_shifts[2];
		if (use_incremental && camera_outside_center) {
			const float light_axis_delta = center_delta.dot(light_direction);
			const float depth_tolerance = MAX(0.000001f, world.voxel_size * 0.0001f);
			if (Math::abs(light_axis_delta) > depth_tolerance) {
				use_incremental = false;
				fallback_reason = "light-axis center movement changes the traced interval";
			}
			const float cascade_extents[2] = { near_extent, far_extent };
			for (uint32_t cascade = 0; cascade < 2 && use_incremental; cascade++) {
				const float texel_world_size = 2.0f * cascade_extents[cascade] / float(atlas_resolution);
				const float shift_x_float = center_delta.dot(tangent) / texel_world_size;
				const float shift_y_float = center_delta.dot(bitangent) / texel_world_size;
				const int shift_x = int(Math::round(shift_x_float));
				const int shift_y = int(Math::round(shift_y_float));
				if (Math::abs(shift_x_float - float(shift_x)) > 0.0001f || Math::abs(shift_y_float - float(shift_y)) > 0.0001f) {
					use_incremental = false;
					fallback_reason = vformat("cascade %d movement is not an integer texel shift", cascade);
					break;
				}
				if (Math::abs(shift_x) >= int(atlas_resolution) || Math::abs(shift_y) >= int(atlas_resolution)) {
					use_incremental = false;
					fallback_reason = vformat("cascade %d has no reusable overlap", cascade);
					break;
				}
				cascade_shifts[cascade] = Vector2i(shift_x, shift_y);
			}
		}

		Vector<Rect2i> update_rects;
		auto add_update_rect = [&update_rects, atlas_resolution](const Rect2i &p_rect) {
			const Rect2i atlas_bounds(0, 0, int(atlas_resolution) * 2, int(atlas_resolution));
			Rect2i merged_rect = p_rect.intersection(atlas_bounds);
			if (!merged_rect.has_area()) {
				return;
			}
			bool merged = true;
			while (merged) {
				merged = false;
				for (int i = 0; i < update_rects.size(); i++) {
					if (merged_rect.intersects(update_rects[i])) {
						merged_rect = merged_rect.merge(update_rects[i]);
						update_rects.remove_at(i);
						merged = true;
						break;
					}
				}
			}
			update_rects.push_back(merged_rect);
		};

		bool scroll_required = false;
		uint64_t dirty_rectangles_processed = 0;
		if (use_incremental) {
			for (uint32_t cascade = 0; cascade < 2; cascade++) {
				const int tile_x = int(cascade * atlas_resolution);
				const int shift_x = cascade_shifts[cascade].x;
				const int shift_y = cascade_shifts[cascade].y;
				scroll_required = scroll_required || shift_x != 0 || shift_y != 0;
				if (shift_y > 0) {
					add_update_rect(Rect2i(tile_x, int(atlas_resolution) - shift_y, int(atlas_resolution), shift_y));
				} else if (shift_y < 0) {
					add_update_rect(Rect2i(tile_x, 0, int(atlas_resolution), -shift_y));
				}
				const int valid_y_begin = MAX(0, -shift_y);
				const int valid_y_end = MIN(int(atlas_resolution), int(atlas_resolution) - shift_y);
				if (shift_x > 0 && valid_y_end > valid_y_begin) {
					add_update_rect(Rect2i(tile_x + int(atlas_resolution) - shift_x, valid_y_begin, shift_x, valid_y_end - valid_y_begin));
				} else if (shift_x < 0 && valid_y_end > valid_y_begin) {
					add_update_rect(Rect2i(tile_x, valid_y_begin, -shift_x, valid_y_end - valid_y_begin));
				}
			}

			if (world_invalid) {
				const float cascade_extents[2] = { near_extent, far_extent };
				const HashSet<Vector3i> *coalesced_dirty_bricks = coalesced_reliable_world_delta ? &shadow_atlas_pending_dirty_bricks : nullptr;
				const Vector<Vector3i> &latest_dirty_bricks = world.last_dirty_bricks;
				auto project_dirty_brick = [&](const Vector3i &dirty_brick) {
					const Vector3 brick_begin = world.origin + Vector3(dirty_brick * 8) * world.voxel_size;
					const Vector3 brick_size = Vector3(8, 8, 8) * world.voxel_size;
					for (uint32_t cascade = 0; cascade < 2; cascade++) {
						float minimum_u = 1e30f;
						float maximum_u = -1e30f;
						float minimum_v = 1e30f;
						float maximum_v = -1e30f;
						for (int corner = 0; corner < 8; corner++) {
							const Vector3 world_corner = brick_begin + Vector3((corner & 1) ? brick_size.x : 0.0f, (corner & 2) ? brick_size.y : 0.0f, (corner & 4) ? brick_size.z : 0.0f);
							const Vector3 relative = world_corner - requested_center;
							const float u = relative.dot(tangent);
							const float v = relative.dot(bitangent);
							minimum_u = MIN(minimum_u, u);
							maximum_u = MAX(maximum_u, u);
							minimum_v = MIN(minimum_v, v);
							maximum_v = MAX(maximum_v, v);
						}
						const float extent = cascade_extents[cascade];
						const float minimum_pixel_x = (minimum_u / (2.0f * extent) + 0.5f) * float(atlas_resolution) - 0.5f;
						const float maximum_pixel_x = (maximum_u / (2.0f * extent) + 0.5f) * float(atlas_resolution) - 0.5f;
						const float minimum_pixel_y = (minimum_v / (2.0f * extent) + 0.5f) * float(atlas_resolution) - 0.5f;
						const float maximum_pixel_y = (maximum_v / (2.0f * extent) + 0.5f) * float(atlas_resolution) - 0.5f;
						const int pixel_begin_x = MAX(0, int(Math::floor(minimum_pixel_x)) - 1);
						const int pixel_end_x = MIN(int(atlas_resolution), int(Math::ceil(maximum_pixel_x)) + 2);
						const int pixel_begin_y = MAX(0, int(Math::floor(minimum_pixel_y)) - 1);
						const int pixel_end_y = MIN(int(atlas_resolution), int(Math::ceil(maximum_pixel_y)) + 2);
						if (pixel_end_x > pixel_begin_x && pixel_end_y > pixel_begin_y) {
							dirty_rectangles_processed++;
							add_update_rect(Rect2i(int(cascade * atlas_resolution) + pixel_begin_x, pixel_begin_y, pixel_end_x - pixel_begin_x, pixel_end_y - pixel_begin_y));
						}
					}
				};
				if (coalesced_dirty_bricks != nullptr) {
					for (const Vector3i &dirty_brick : *coalesced_dirty_bricks) {
						project_dirty_brick(dirty_brick);
					}
				} else {
					for (const Vector3i &dirty_brick : latest_dirty_bricks) {
						project_dirty_brick(dirty_brick);
					}
				}
			}

			uint64_t retraced_texels = 0;
			for (const Rect2i &rect : update_rects) {
				retraced_texels += uint64_t(rect.size.x) * uint64_t(rect.size.y);
			}
			const uint64_t full_texels = uint64_t(atlas_resolution) * uint64_t(atlas_resolution) * 2u;
			const uint64_t incremental_texel_budget = uint64_t(MAX(64, int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/rebuild_texel_budget"))));
			if (temporal_rebuild_enabled && retraced_texels > incremental_texel_budget) {
				// An ordinary incremental update publishes atomically through scratch,
				// so it cannot be split across frames. Defer oversized regions to the
				// already bounded temporal path rather than creating a surprise spike.
				use_incremental = false;
				fallback_reason = vformat("incremental update exceeds the %d-texel frame budget", incremental_texel_budget);
			} else if (retraced_texels * 4u >= full_texels * 3u) {
				use_incremental = false;
				fallback_reason = "incremental rectangles cover at least 75 percent of the atlas";
			}
		}

		const RID atlas_shader_rid = shadow_atlas_shader.version_get_shader(shadow_atlas_shader_version, 0);
		RD::Uniform u_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ world.directory_buffer }));
		RD::Uniform u_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ world.brick_buffer }));

		ShadowAtlasPushConstant atlas_push = {};
		atlas_push.world_origin_voxel_size[0] = world.origin.x;
		atlas_push.world_origin_voxel_size[1] = world.origin.y;
		atlas_push.world_origin_voxel_size[2] = world.origin.z;
		atlas_push.world_origin_voxel_size[3] = world.voxel_size;
		atlas_push.atlas_center_depth[0] = requested_center.x;
		atlas_push.atlas_center_depth[1] = requested_center.y;
		atlas_push.atlas_center_depth[2] = requested_center.z;
		atlas_push.atlas_center_depth[3] = far_extent;
		atlas_push.tangent_near_extent[0] = tangent.x;
		atlas_push.tangent_near_extent[1] = tangent.y;
		atlas_push.tangent_near_extent[2] = tangent.z;
		atlas_push.tangent_near_extent[3] = near_extent;
		atlas_push.bitangent_far_extent[0] = bitangent.x;
		atlas_push.bitangent_far_extent[1] = bitangent.y;
		atlas_push.bitangent_far_extent[2] = bitangent.z;
		atlas_push.bitangent_far_extent[3] = far_extent;
		atlas_push.atlas_directory_steps[0] = atlas_resolution;
		atlas_push.atlas_directory_steps[1] = 2;
		atlas_push.atlas_directory_steps[2] = world.directory_mask;
		atlas_push.atlas_directory_steps[3] = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/max_steps")), 1, 4096);

		if (use_incremental && (!update_rects.is_empty() || scroll_required)) {
			if (!shadow_atlas_scratch_rd.is_valid() || shadow_atlas_scratch_resolution != atlas_resolution || shadow_atlas_scratch_source_rd != atlas_output) {
				if (shadow_atlas_scratch_rd.is_valid()) {
					RD::get_singleton()->free_rid(shadow_atlas_scratch_rd);
				}
				RD::TextureFormat scratch_format;
				scratch_format.format = RD::DATA_FORMAT_R32_SFLOAT;
				scratch_format.width = atlas_resolution * 2;
				scratch_format.height = atlas_resolution;
				scratch_format.texture_type = RD::TEXTURE_TYPE_2D;
				scratch_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
				shadow_atlas_scratch_rd = RD::get_singleton()->texture_create(scratch_format, RD::TextureView());
				shadow_atlas_scratch_resolution = shadow_atlas_scratch_rd.is_valid() ? atlas_resolution : 0;
				shadow_atlas_scratch_source_rd = shadow_atlas_scratch_rd.is_valid() ? atlas_output : RID();
				if (shadow_atlas_scratch_rd.is_valid()) {
					RD::get_singleton()->set_resource_name(shadow_atlas_scratch_rd, "Voxel Shadow Atlas Scroll Scratch");
				}
			}
			if (!shadow_atlas_scratch_rd.is_valid()) {
				use_incremental = false;
				fallback_reason = "scratch atlas allocation failed";
			}
		}

		RENDER_TIMESTAMP("Voxel Light-Space Shadow Atlas");
		RD::get_singleton()->draw_command_begin_label("Voxel Light-Space Shadow Atlas");
		uint64_t retraced_texels = 0;
		bool update_complete = false;
		if (use_incremental && update_rects.is_empty() && !scroll_required) {
			update_complete = true;
		} else if (use_incremental) {
			bool scratch_initialized = scroll_required;
			if (!scroll_required) {
				scratch_initialized = RD::get_singleton()->texture_copy(atlas_output, shadow_atlas_scratch_rd, Vector3(), Vector3(), Vector3(atlas_resolution * 2, atlas_resolution, 1), 0, 0, 0, 0) == OK;
				if (!scratch_initialized) {
					fallback_reason = "copying the live atlas to scratch failed";
				}
			}
			if (scratch_initialized) {
				RD::ComputeListID atlas_list = RD::get_singleton()->compute_list_begin();
				if (scroll_required) {
					const RID scroll_shader_rid = shadow_atlas_scroll_shader.version_get_shader(shadow_atlas_scroll_shader_version, 0);
					RD::Uniform u_scroll_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, atlas_output }));
					RD::Uniform u_scroll_destination(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ shadow_atlas_scratch_rd }));
					const RID scroll_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(scroll_shader_rid, 0, u_scroll_source, u_scroll_destination);
					ShadowAtlasScrollPushConstant scroll_push = {};
					scroll_push.atlas_size[0] = atlas_resolution;
					scroll_push.atlas_size[1] = 2;
					scroll_push.cascade_shifts[0] = cascade_shifts[0].x;
					scroll_push.cascade_shifts[1] = cascade_shifts[0].y;
					scroll_push.cascade_shifts[2] = cascade_shifts[1].x;
					scroll_push.cascade_shifts[3] = cascade_shifts[1].y;
					RD::get_singleton()->compute_list_bind_compute_pipeline(atlas_list, shadow_atlas_scroll_pipeline);
					RD::get_singleton()->compute_list_bind_uniform_set(atlas_list, scroll_uniform_set, 0);
					RD::get_singleton()->compute_list_set_push_constant(atlas_list, &scroll_push, sizeof(ShadowAtlasScrollPushConstant));
					RD::get_singleton()->compute_list_dispatch_threads(atlas_list, atlas_resolution * 2, atlas_resolution, 1);
					if (!update_rects.is_empty()) {
						RD::get_singleton()->compute_list_add_barrier(atlas_list);
					}
				}
				if (!update_rects.is_empty()) {
					RD::Uniform u_scratch_output(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ shadow_atlas_scratch_rd }));
					const RID scratch_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(atlas_shader_rid, 0, u_scratch_output, u_directory, u_bricks);
					RD::get_singleton()->compute_list_bind_compute_pipeline(atlas_list, shadow_atlas_pipeline);
					RD::get_singleton()->compute_list_bind_uniform_set(atlas_list, scratch_uniform_set, 0);
					for (const Rect2i &rect : update_rects) {
						atlas_push.dispatch_rect[0] = rect.position.x;
						atlas_push.dispatch_rect[1] = rect.position.y;
						atlas_push.dispatch_rect[2] = rect.size.x;
						atlas_push.dispatch_rect[3] = rect.size.y;
						RD::get_singleton()->compute_list_set_push_constant(atlas_list, &atlas_push, sizeof(ShadowAtlasPushConstant));
						RD::get_singleton()->compute_list_dispatch_threads(atlas_list, rect.size.x, rect.size.y, 1);
						retraced_texels += uint64_t(rect.size.x) * uint64_t(rect.size.y);
					}
				}
				RD::get_singleton()->compute_list_end();
				update_complete = RD::get_singleton()->texture_copy(shadow_atlas_scratch_rd, atlas_output, Vector3(), Vector3(), Vector3(atlas_resolution * 2, atlas_resolution, 1), 0, 0, 0, 0) == OK;
				if (!update_complete) {
					fallback_reason = "publishing the completed scratch atlas failed";
				}
			}
			if (!update_complete) {
				use_incremental = false;
			}
		}

		if (!use_incremental && temporal_rebuild_enabled) {
			// The incremental proof failed after detailed rectangle/scroll checks.
			// Keep the complete active atlas and let the next frame start a bounded,
			// immutable temporal rebuild instead of issuing one unbounded dispatch.
			shadow_atlas_pending_full_rebuild = true;
			shadow_atlas_pending_change_count++;
			shadow_atlas_pending_world_revision = world.revision;
			print_verbose(vformat("Voxel Forward shadow atlas incremental update deferred to temporal rebuild: %s, %d dirty regions, latest revision %d.", fallback_reason, shadow_atlas_pending_dirty_bricks.size(), world.revision));
		} else if (!use_incremental) {
			RD::Uniform u_atlas_output(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ atlas_output }));
			const RID atlas_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(atlas_shader_rid, 0, u_atlas_output, u_directory, u_bricks);
			atlas_push.dispatch_rect[0] = 0;
			atlas_push.dispatch_rect[1] = 0;
			atlas_push.dispatch_rect[2] = atlas_resolution * 2;
			atlas_push.dispatch_rect[3] = atlas_resolution;
			RD::ComputeListID atlas_list = RD::get_singleton()->compute_list_begin();
			RD::get_singleton()->compute_list_bind_compute_pipeline(atlas_list, shadow_atlas_pipeline);
			RD::get_singleton()->compute_list_bind_uniform_set(atlas_list, atlas_uniform_set, 0);
			RD::get_singleton()->compute_list_set_push_constant(atlas_list, &atlas_push, sizeof(ShadowAtlasPushConstant));
			RD::get_singleton()->compute_list_dispatch_threads(atlas_list, atlas_resolution * 2, atlas_resolution, 1);
			RD::get_singleton()->compute_list_end();
			update_complete = true;
			retraced_texels = uint64_t(atlas_resolution) * uint64_t(atlas_resolution) * 2u;
			shadow_atlas_full_rebuild_count++;
			print_verbose(vformat("Voxel Forward shadow atlas full rebuild #%d: %s, %d texels, center %s, revision %d.", shadow_atlas_full_rebuild_count, fallback_reason, retraced_texels, requested_center, world.revision));
		} else {
			shadow_atlas_incremental_update_count++;
			if (scroll_required) {
				shadow_atlas_incremental_scroll_count++;
			}
			print_verbose(vformat("Voxel Forward shadow atlas incremental update #%d (scroll #%d): near shift %s, far shift %s, %d dirty rectangles / %d merged retrace rectangles / %d retraced texels, center %s, revision %d.", shadow_atlas_incremental_update_count, shadow_atlas_incremental_scroll_count, cascade_shifts[0], cascade_shifts[1], dirty_rectangles_processed, update_rects.size(), retraced_texels, requested_center, world.revision));
		}
		RD::get_singleton()->draw_command_end_label();

		if (update_complete) {
			shadow_atlas_center = requested_center;
			shadow_atlas_light_direction = light_direction;
			shadow_atlas_tangent = tangent;
			shadow_atlas_bitangent = bitangent;
			shadow_atlas_world_revision = world.revision;
			shadow_atlas_resolution = atlas_resolution;
			shadow_atlas_voxel_size = world.voxel_size;
			shadow_atlas_near_extent = near_extent;
			shadow_atlas_far_extent = far_extent;
			shadow_atlas_source_rd = atlas_output;
			shadow_atlas_directory_rd = world.directory_buffer;
			shadow_atlas_bricks_rd = world.brick_buffer;
			shadow_atlas_incremental_enabled = incremental_enabled;
			shadow_atlas_pending_world_revision = UINT64_MAX;
			shadow_atlas_pending_change_count = 0;
			shadow_atlas_pending_full_rebuild = false;
			shadow_atlas_pending_dirty_reliable = true;
			shadow_atlas_pending_dirty_bricks.clear();
		}
		shadow_atlas_initialized = true;
	}

	// A newly allocated or resized pair has no complete texture to expose until
	// its first temporal rebuild publishes. Stale but complete atlases remain
	// available while ordinary camera/world invalidations are processed.
	if (!shadow_atlas_initialized || !shadow_atlas_source_rd.is_valid() || shadow_atlas_source_rd != atlas_slots[shadow_atlas_active_slot]) {
		disable_voxel_lighting();
		return;
	}
	atlas_output = shadow_atlas_source_rd;
	_render_indirect_light(p_render_data, atlas_output, sampler, shadow_atlas_light_direction, light_color, light_energy);
	Ref<RenderBufferDataForwardClustered> forward_buffers;
	if (render_buffers->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		forward_buffers = render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	}
	// The shared occupancy atlas currently accepts positive, unrotated,
	// grid-aligned volumes only. If any registered volume is incompatible, keep
	// the legacy resolver for the frame so its established fallback behavior is
	// preserved. The normal all-compatible path compiles out every hash lookup.
	const bool use_hit_payload = bool(GLOBAL_GET("rendering/voxel_forward/architectural_hit_buffer/enabled")) && world.incompatible_volume_count == 0 && forward_buffers.is_valid() && forward_buffers->has_voxel_hit();
	const uint32_t resolve_mode = use_hit_payload ? 1u : 0u;
	const RID resolve_shader_rid = shadow_resolve_shader.version_get_shader(shadow_resolve_shader_version, resolve_mode);
	const RID resolve_pipeline = use_hit_payload ? shadow_resolve_payload_pipeline : shadow_resolve_legacy_pipeline;
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, depth }));
	RD::Uniform u_atlas(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, atlas_output }));
	RD::Uniform u_mask(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ mask_output }));
	RD::Uniform u_occupancy(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, Vector<RID>({ occupancy_uniform_buffer }));
	RID resolve_uniform_set;
	if (use_hit_payload) {
		RD::Uniform u_hit_payload(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ sampler, forward_buffers->get_voxel_hit() }));
		resolve_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(resolve_shader_rid, 0, u_depth, u_atlas, u_mask, u_occupancy, u_hit_payload);
	} else {
		RD::Uniform u_resolve_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ world.directory_buffer }));
		RD::Uniform u_resolve_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, Vector<RID>({ world.brick_buffer }));
		resolve_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(resolve_shader_rid, 0, u_depth, u_atlas, u_mask, u_occupancy, u_resolve_directory, u_resolve_bricks);
	}

	ShadowResolvePushConstant resolve_push = {};
	const Projection view_projection = p_render_data->scene_data->get_view_projection(0) * Projection(p_render_data->scene_data->cam_transform.affine_inverse());
	RendererRD::MaterialStorage::store_camera(view_projection.inverse(), resolve_push.inv_view_projection);
	resolve_push.atlas_center_voxel_size[0] = shadow_atlas_center.x;
	resolve_push.atlas_center_voxel_size[1] = shadow_atlas_center.y;
	resolve_push.atlas_center_voxel_size[2] = shadow_atlas_center.z;
	resolve_push.atlas_center_voxel_size[3] = shadow_atlas_voxel_size;
	resolve_push.tangent_near_extent[0] = shadow_atlas_tangent.x;
	resolve_push.tangent_near_extent[1] = shadow_atlas_tangent.y;
	resolve_push.tangent_near_extent[2] = shadow_atlas_tangent.z;
	resolve_push.tangent_near_extent[3] = shadow_atlas_near_extent;
	resolve_push.bitangent_far_extent[0] = shadow_atlas_bitangent.x;
	resolve_push.bitangent_far_extent[1] = shadow_atlas_bitangent.y;
	resolve_push.bitangent_far_extent[2] = shadow_atlas_bitangent.z;
	resolve_push.bitangent_far_extent[3] = shadow_atlas_far_extent;
	resolve_push.screen_atlas_filter[0] = screen_size.x;
	resolve_push.screen_atlas_filter[1] = screen_size.y;
	resolve_push.screen_atlas_filter[2] = shadow_atlas_resolution;
	const bool soft_shadow_enabled = int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_mode")) == 1;
	const uint32_t sample_count = soft_shadow_enabled ? uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_samples")), 1, 8)) : 1u;
	const uint32_t radius_q = uint32_t(CLAMP(Math::round(MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_radius_voxels"))) * 16.0f), 0.0f, 4095.0f));
	const uint32_t bias_q = uint32_t(CLAMP(Math::round(MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/atlas_bias_voxels"))) * 256.0f), 0.0f, 2047.0f));
	resolve_push.screen_atlas_filter[3] = int32_t(sample_count | (soft_shadow_enabled ? 0x80u : 0u) | (radius_q << 8u) | (bias_q << 20u));

	RENDER_TIMESTAMP("Shadow Mask Resolve");
	RD::get_singleton()->draw_command_begin_label("Shadow Mask Resolve");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, resolve_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, resolve_uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &resolve_push, sizeof(ShadowResolvePushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, screen_size.x, screen_size.y, 1);
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_light_direction"), shadow_atlas_light_direction);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_ready"), true);
}

void RenderVoxelForward::_render_buffers_debug_draw(const RenderDataRD *p_render_data) {
	RenderForwardClustered::_render_buffers_debug_draw(p_render_data);
	if (!bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/debug_view")) || p_render_data == nullptr || p_render_data->render_buffers.is_null()) {
		return;
	}
	Ref<RenderSceneBuffersRD> render_buffers = p_render_data->render_buffers;
	const StringName scope = SNAME("voxel_forward");
	const StringName texture_name = SNAME("occupancy_shadow_mask");
	if (!render_buffers->has_texture(scope, texture_name)) {
		return;
	}
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	const RID render_target = render_buffers->get_render_target();
	const Size2i target_size = texture_storage->render_target_get_size(render_target);
	copy_effects->copy_to_fb_rect(render_buffers->get_texture(scope, texture_name), texture_storage->render_target_get_rd_framebuffer(render_target), Rect2i(Point2i(), target_size), false, true, false, false, RID(), false, true);
}

void RenderVoxelForward::_render_scene_custom_opaque(RenderDataRD *p_render_data, RID p_framebuffer, uint32_t p_color_pass_flags, uint32_t p_color_attachment_count, bool p_depth_prepass) {
	if (!bool(GLOBAL_GET("rendering/voxel_forward/experimental_custom_visibility")) || visible_volumes.is_empty() || p_render_data->scene_data->cam_orthogonal || p_render_data->scene_data->view_count != 1 || p_color_attachment_count < 1 || p_color_attachment_count > 3) {
		return;
	}
	_ensure_visibility_resources();

	RD::get_singleton()->draw_command_begin_label("Voxel Forward Opaque");
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader = visibility_shader.version_get_shader(visibility_shader_version, 0);
	RID pipeline = visibility_pipelines[p_color_attachment_count - 1].get_render_pipeline(RD::INVALID_ID, RD::get_singleton()->framebuffer_get_format(p_framebuffer));
	if (!pipeline.is_valid()) {
		RD::get_singleton()->draw_command_end_label();
		return;
	}
	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 1.0f, 0u, p_render_data->render_region);
	RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, pipeline);

	for (const VisibleVolume &visible : visible_volumes) {
		RID voxel_texture = texture_storage->texture_get_rd_texture(visible.voxel_texture);
		RID brick_texture = texture_storage->texture_get_rd_texture(visible.brick_texture);
		RID palette_texture = texture_storage->texture_get_rd_texture(visible.palette_texture, true);
		if (!voxel_texture.is_valid() || !brick_texture.is_valid() || !palette_texture.is_valid()) {
			continue;
		}

		RD::Uniform u_voxels(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, voxel_texture }));
		RD::Uniform u_bricks(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, brick_texture }));
		RD::Uniform u_palette(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, palette_texture }));
		RID uniform_set = uniform_set_cache->get_cache(shader, 0, u_voxels, u_bricks, u_palette);

		const Transform3D voxel_to_world = visible.transform.scaled_local(Vector3(visible.voxel_size, visible.voxel_size, visible.voxel_size));
		const Transform3D view_model = p_render_data->scene_data->cam_transform.affine_inverse() * voxel_to_world;
		Projection depth_correction;
		depth_correction.set_depth_correction(true);
		const Projection model_view_projection = (depth_correction * p_render_data->scene_data->cam_projection) * Projection(view_model);
		const Vector3 camera_local = voxel_to_world.affine_inverse().xform(p_render_data->scene_data->cam_transform.origin);
		VisibilityPushConstant push_constant = {};
		RendererRD::MaterialStorage::store_camera(model_view_projection, push_constant.model_view_projection);
		push_constant.camera_local[0] = camera_local.x;
		push_constant.camera_local[1] = camera_local.y;
		push_constant.camera_local[2] = camera_local.z;
		push_constant.volume_dimensions[0] = visible.dimensions.x;
		push_constant.volume_dimensions[1] = visible.dimensions.y;
		push_constant.volume_dimensions[2] = visible.dimensions.z;

		RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set, 0);
		RD::get_singleton()->draw_list_set_push_constant(draw_list, &push_constant, sizeof(VisibilityPushConstant));
		RD::get_singleton()->draw_list_draw(draw_list, false, 1, 36);
	}

	RD::get_singleton()->draw_list_end();
	RD::get_singleton()->draw_command_end_label();
}

} // namespace RendererSceneRenderImplementation
