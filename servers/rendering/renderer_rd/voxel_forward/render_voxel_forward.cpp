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

RenderVoxelForward::RenderVoxelForward() {
	occupancy_uniform_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(OccupancyUniformData));
	OccupancyUniformData empty_occupancy = {};
	RD::get_singleton()->buffer_update(occupancy_uniform_buffer, 0, sizeof(OccupancyUniformData), &empty_occupancy);

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

	Vector<String> shadow_modes;
	shadow_modes.push_back("");
	shadow_atlas_shader.initialize(shadow_modes);
	shadow_atlas_shader_version = shadow_atlas_shader.version_create();
	shadow_atlas_pipeline = RD::get_singleton()->compute_pipeline_create(shadow_atlas_shader.version_get_shader(shadow_atlas_shader_version, 0));
	shadow_resolve_shader.initialize(shadow_modes);
	shadow_resolve_shader_version = shadow_resolve_shader.version_create();
	shadow_resolve_pipeline = RD::get_singleton()->compute_pipeline_create(shadow_resolve_shader.version_get_shader(shadow_resolve_shader_version, 0));
	indirect_inject_shader.initialize(shadow_modes);
	indirect_inject_shader_version = indirect_inject_shader.version_create();
	indirect_inject_pipeline = RD::get_singleton()->compute_pipeline_create(indirect_inject_shader.version_get_shader(indirect_inject_shader_version, 0));
	indirect_propagate_shader.initialize(shadow_modes);
	indirect_propagate_shader_version = indirect_propagate_shader.version_create();
	indirect_propagate_pipeline = RD::get_singleton()->compute_pipeline_create(indirect_propagate_shader.version_get_shader(indirect_propagate_shader_version, 0));
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
	}
	_free_indirect_light();
	if (shadow_mask_texture.is_valid() && RendererRD::TextureStorage::get_singleton() != nullptr) {
		RendererRD::TextureStorage::get_singleton()->texture_free(shadow_mask_texture);
	}
	if (shadow_atlas_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(shadow_atlas_pipeline);
	}
	if (shadow_resolve_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(shadow_resolve_pipeline);
	}
	if (indirect_inject_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(indirect_inject_pipeline);
	}
	if (indirect_propagate_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(indirect_propagate_pipeline);
	}
	if (occupancy_uniform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(occupancy_uniform_buffer);
	}
	shadow_atlas_shader.version_free(shadow_atlas_shader_version);
	shadow_resolve_shader.version_free(shadow_resolve_shader_version);
	indirect_inject_shader.version_free(indirect_inject_shader_version);
	indirect_propagate_shader.version_free(indirect_propagate_shader_version);
	for (uint32_t i = 0; i < 3; i++) {
		visibility_pipelines[i].clear();
	}
	visibility_shader.version_free(visibility_shader_version);
}

void RenderVoxelForward::_free_indirect_light() {
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
		}
		indirect_cascade_initialized[cascade] = false;
	}
	indirect_grid_resolution = 0;
	indirect_world_revision = UINT64_MAX;
}

void RenderVoxelForward::_render_indirect_light(const RenderDataRD *p_render_data, RID p_shadow_atlas, RID p_sampler, const Vector3 &p_light_direction, const Color &p_light_color, float p_light_energy) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
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
		return;
	}

	const uint32_t resolution = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/resolution")), 24, 96));
	const float near_cell_size = MAX(world.voxel_size, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/near_cell_size")));
	const float far_cell_size = MAX(near_cell_size, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/far_cell_size")));
	const float distant_cell_size = MAX(far_cell_size, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/distant_cell_size")));
	const float cell_sizes[INDIRECT_CASCADE_COUNT] = { near_cell_size, far_cell_size, distant_cell_size };
	const int recenter_cells = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/recenter_cells")), 1, 16);
	const float recenter_hysteresis = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/recenter_hysteresis")), 0.55f, 0.95f);
	const Vector3 camera_position = p_render_data->scene_data->cam_transform.origin;
	Vector3 grid_origins[INDIRECT_CASCADE_COUNT];
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		const float snap = cell_sizes[cascade] * float(recenter_cells);
		Vector3 snapped_center;
		const bool can_reuse_center = indirect_cascade_initialized[cascade] && indirect_grid_resolution == resolution &&
				Math::is_equal_approx(indirect_grid_cell_size[cascade], cell_sizes[cascade]);
		if (can_reuse_center) {
			snapped_center = indirect_grid_origin[cascade] + Vector3(1, 1, 1) * (cell_sizes[cascade] * float(resolution) * 0.5f);
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
		RD::TextureFormat texture_format;
		texture_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		texture_format.width = resolution;
		texture_format.height = resolution;
		texture_format.depth = resolution;
		texture_format.texture_type = RD::TEXTURE_TYPE_3D;
		texture_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
		for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
			RID replacement_rd[2];
			for (uint32_t buffer = 0; buffer < 2; buffer++) {
				replacement_rd[buffer] = RD::get_singleton()->texture_create(texture_format, RD::TextureView());
				RD::get_singleton()->texture_clear(replacement_rd[buffer], Color(0, 0, 0, 0), 0, 1, 0, 1);
				RD::get_singleton()->set_resource_name(replacement_rd[buffer], vformat("Voxel Indirect Light Cascade %d Buffer %d", cascade, buffer));
			}
			if (indirect_grid_texture[cascade].is_valid()) {
				// Replace the resource behind the existing wrapper. Materials keep the
				// same public RID, so a live resolution edit cannot leave an old set bound.
				const RID previous_rd = indirect_grid_rd[cascade][0];
				RID replacement_texture = texture_storage->texture_allocate();
				texture_storage->texture_rd_initialize(replacement_texture, replacement_rd[0]);
				texture_storage->texture_replace(indirect_grid_texture[cascade], replacement_texture);
				if (RD::get_singleton()->texture_is_valid(previous_rd)) {
					RD::get_singleton()->free_rid(previous_rd);
				}
				if (indirect_grid_rd[cascade][1].is_valid()) {
					RD::get_singleton()->free_rid(indirect_grid_rd[cascade][1]);
				}
			} else {
				indirect_grid_texture[cascade] = texture_storage->texture_allocate();
				texture_storage->texture_rd_initialize(indirect_grid_texture[cascade], replacement_rd[0]);
			}
			indirect_grid_rd[cascade][0] = replacement_rd[0];
			indirect_grid_rd[cascade][1] = replacement_rd[1];
			indirect_cascade_initialized[cascade] = false;
		}
		indirect_grid_resolution = resolution;
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_near"), indirect_grid_texture[0]);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_far"), indirect_grid_texture[1]);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_distant"), indirect_grid_texture[2]);
	}

	const bool common_invalid = indirect_world_revision != world.revision ||
			!indirect_light_direction.is_equal_approx(p_light_direction) ||
			!indirect_light_color.is_equal_approx(p_light_color) ||
			!Math::is_equal_approx(indirect_light_energy, p_light_energy);
	bool any_invalid = common_invalid;
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		any_invalid = any_invalid || !indirect_cascade_initialized[cascade] ||
				!indirect_grid_origin[cascade].is_equal_approx(grid_origins[cascade]) ||
				!Math::is_equal_approx(indirect_grid_cell_size[cascade], cell_sizes[cascade]);
	}
	if (any_invalid) {
		const RID inject_shader_rid = indirect_inject_shader.version_get_shader(indirect_inject_shader_version, 0);
		const RID propagate_shader_rid = indirect_propagate_shader.version_get_shader(indirect_propagate_shader_version, 0);
		const int base_propagation_steps = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/propagation_steps")), 2, 16) & ~1;
		const float propagation_decay = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/propagation_decay")), 0.0f, 0.99f);
		const float shadow_bias = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/shadow_bias_voxels"))) * world.voxel_size;

		RENDER_TIMESTAMP("Voxel Indirect Light");
		RD::get_singleton()->draw_command_begin_label("Voxel Indirect Light");
		for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
			const bool cascade_invalid = common_invalid || !indirect_cascade_initialized[cascade] ||
					!indirect_grid_origin[cascade].is_equal_approx(grid_origins[cascade]) ||
					!Math::is_equal_approx(indirect_grid_cell_size[cascade], cell_sizes[cascade]);
			if (!cascade_invalid) {
				continue;
			}
			const int sample_radius = CLAMP(int(Math::ceil(cell_sizes[cascade] / world.voxel_size)), 1, 8);
			const int propagation_steps = MAX(2, (base_propagation_steps >> cascade)) & ~1;
			RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ indirect_grid_rd[cascade][0] }));
			RD::Uniform u_atlas(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ p_sampler, p_shadow_atlas }));
			RD::Uniform u_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ world.directory_buffer }));
			RD::Uniform u_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ world.brick_buffer }));
			const RID inject_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(inject_shader_rid, 0, u_output, u_atlas, u_directory, u_bricks);

			IndirectInjectPushConstant inject_push = {};
			inject_push.world_origin_voxel_size[0] = world.origin.x;
			inject_push.world_origin_voxel_size[1] = world.origin.y;
			inject_push.world_origin_voxel_size[2] = world.origin.z;
			inject_push.world_origin_voxel_size[3] = world.voxel_size;
			inject_push.grid_origin_cell_size[0] = grid_origins[cascade].x;
			inject_push.grid_origin_cell_size[1] = grid_origins[cascade].y;
			inject_push.grid_origin_cell_size[2] = grid_origins[cascade].z;
			inject_push.grid_origin_cell_size[3] = cell_sizes[cascade];
			inject_push.light_direction_energy[0] = p_light_direction.x;
			inject_push.light_direction_energy[1] = p_light_direction.y;
			inject_push.light_direction_energy[2] = p_light_direction.z;
			inject_push.light_direction_energy[3] = p_light_energy;
			inject_push.light_color_bias[0] = p_light_color.r;
			inject_push.light_color_bias[1] = p_light_color.g;
			inject_push.light_color_bias[2] = p_light_color.b;
			inject_push.light_color_bias[3] = shadow_bias;
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
			inject_push.grid_directory[1] = world.directory_mask;
			inject_push.grid_directory[2] = sample_radius;
			inject_push.grid_directory[3] = cascade;

			RD::ComputeListID inject_list = RD::get_singleton()->compute_list_begin();
			RD::get_singleton()->compute_list_bind_compute_pipeline(inject_list, indirect_inject_pipeline);
			RD::get_singleton()->compute_list_bind_uniform_set(inject_list, inject_uniform_set, 0);
			RD::get_singleton()->compute_list_set_push_constant(inject_list, &inject_push, sizeof(IndirectInjectPushConstant));
			RD::get_singleton()->compute_list_dispatch_threads(inject_list, resolution, resolution, resolution);
			RD::get_singleton()->compute_list_end();

			IndirectPropagatePushConstant propagate_push = {};
			propagate_push.world_origin_voxel_size[0] = world.origin.x;
			propagate_push.world_origin_voxel_size[1] = world.origin.y;
			propagate_push.world_origin_voxel_size[2] = world.origin.z;
			propagate_push.world_origin_voxel_size[3] = world.voxel_size;
			propagate_push.grid_origin_cell_size[0] = grid_origins[cascade].x;
			propagate_push.grid_origin_cell_size[1] = grid_origins[cascade].y;
			propagate_push.grid_origin_cell_size[2] = grid_origins[cascade].z;
			propagate_push.grid_origin_cell_size[3] = cell_sizes[cascade];
			propagate_push.grid_directory[0] = resolution;
			propagate_push.grid_directory[1] = world.directory_mask;
			propagate_push.propagation[0] = propagation_decay;
			for (int step = 0; step < propagation_steps; step++) {
				const uint32_t source = uint32_t(step) & 1u;
				const uint32_t destination = source ^ 1u;
				RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ p_sampler, indirect_grid_rd[cascade][source] }));
				RD::Uniform u_destination(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ indirect_grid_rd[cascade][destination] }));
				RD::Uniform u_propagate_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ world.directory_buffer }));
				RD::Uniform u_propagate_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ world.brick_buffer }));
				const RID propagate_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(propagate_shader_rid, 0, u_source, u_destination, u_propagate_directory, u_propagate_bricks);
				RD::ComputeListID propagate_list = RD::get_singleton()->compute_list_begin();
				RD::get_singleton()->compute_list_bind_compute_pipeline(propagate_list, indirect_propagate_pipeline);
				RD::get_singleton()->compute_list_bind_uniform_set(propagate_list, propagate_uniform_set, 0);
				RD::get_singleton()->compute_list_set_push_constant(propagate_list, &propagate_push, sizeof(IndirectPropagatePushConstant));
				RD::get_singleton()->compute_list_dispatch_threads(propagate_list, resolution, resolution, resolution);
				RD::get_singleton()->compute_list_end();
			}
			indirect_grid_origin[cascade] = grid_origins[cascade];
			indirect_grid_cell_size[cascade] = cell_sizes[cascade];
			indirect_cascade_initialized[cascade] = true;
		}
		RD::get_singleton()->draw_command_end_label();

		indirect_world_revision = world.revision;
		indirect_light_direction = p_light_direction;
		indirect_light_color = p_light_color;
		indirect_light_energy = p_light_energy;
		print_verbose(vformat("Voxel Forward indirect cascades updated: %d^3 x 3, %.2f / %.2f / %.2f m cells, revision %d.", resolution, near_cell_size, far_cell_size, distant_cell_size, world.revision));
	}

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

void RenderVoxelForward::_add_voxel_occupancy_uniforms(Vector<RD::Uniform> &r_uniforms) {
	const VoxelForwardVolumeStorage::WorldOccupancy &world = volume_storage.get_world_occupancy();
	if (!world.directory_buffer.is_valid() || !world.brick_buffer.is_valid() || !occupancy_uniform_buffer.is_valid()) {
		RenderForwardClustered::_add_voxel_occupancy_uniforms(r_uniforms);
		return;
	}

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

bool RenderVoxelForward::_render_scene_custom_uses_resolved_depth() const {
	return bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/enabled"));
}

void RenderVoxelForward::_render_scene_custom_pre_opaque(RenderDataRD *p_render_data, bool p_depth_prepass) {
	if (!p_depth_prepass) {
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_ready"), false);
		if (shadow_mask_texture.is_valid()) {
			RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), Variant());
			RendererRD::TextureStorage::get_singleton()->texture_free(shadow_mask_texture);
			shadow_mask_texture = RID();
			shadow_mask_source_rd = RID();
		}
		shadow_atlas_initialized = false;
		shadow_atlas_source_rd = RID();
	} else {
		_render_shadow_atlas(p_render_data);
	}
}

void RenderVoxelForward::_render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) {
	const bool shadow_mask_enabled = bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/enabled"));
	volume_storage.update_world_occupancy(shadow_mask_enabled);
	const VoxelForwardVolumeStorage::WorldOccupancy &current_world = volume_storage.get_world_occupancy();
	if (current_world.directory_buffer != bound_occupancy_directory || current_world.brick_buffer != bound_occupancy_bricks) {
		bound_occupancy_directory = current_world.directory_buffer;
		bound_occupancy_bricks = current_world.brick_buffer;
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
	if (p_render_data != nullptr && p_render_data->instances != nullptr) {
		for (uint32_t i = 0; i < p_render_data->instances->size(); i++) {
			RenderGeometryInstance *instance = (*p_render_data->instances)[i];
			const VoxelForwardVolumeStorage::Volume *volume = volume_storage.get_volume(instance->get_base());
			if (volume == nullptr) {
				continue;
			}

			VisibleVolume visible;
			visible.volume = *volume;
			visible.transform = instance->get_transform();
			visible.aabb = instance->get_aabb();
			visible_volumes.push_back(visible);
		}
	}
	const uint32_t registered_volume_count = volume_storage.get_volume_count();
	const uint32_t visible_volume_count = visible_volumes.size();
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
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	auto disable_voxel_lighting = [material_storage]() {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_ready"), false);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), false);
	};
	if (!bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/enabled")) || p_render_data == nullptr || p_render_data->render_buffers.is_null() || p_render_data->scene_data == nullptr || p_render_data->scene_data->view_count != 1 || p_render_data->lights == nullptr || !shadow_atlas_pipeline.is_valid() || !shadow_resolve_pipeline.is_valid()) {
		disable_voxel_lighting();
		if (shadow_mask_texture.is_valid()) {
			RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), Variant());
			RendererRD::TextureStorage::get_singleton()->texture_free(shadow_mask_texture);
			shadow_mask_texture = RID();
			shadow_mask_source_rd = RID();
		}
		shadow_atlas_initialized = false;
		shadow_atlas_source_rd = RID();
		return;
	}
	const VoxelForwardVolumeStorage::WorldOccupancy &world = volume_storage.get_world_occupancy();
	if (!world.directory_buffer.is_valid() || !world.brick_buffer.is_valid() || !occupancy_uniform_buffer.is_valid() || world.occupied_brick_count == 0 || world.voxel_size <= 0.0f) {
		disable_voxel_lighting();
		shadow_atlas_initialized = false;
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
		shadow_atlas_initialized = false;
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
	const StringName atlas_texture_name = SNAME("occupancy_shadow_atlas");
	const StringName mask_texture_name = SNAME("occupancy_shadow_mask");
	if (render_buffers->has_texture(scope, atlas_texture_name) && shadow_atlas_resolution != 0 && shadow_atlas_resolution != atlas_resolution) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), Variant());
		if (shadow_mask_texture.is_valid()) {
			RendererRD::TextureStorage::get_singleton()->texture_free(shadow_mask_texture);
			shadow_mask_texture = RID();
		}
		render_buffers->clear_context(scope);
		shadow_mask_source_rd = RID();
		shadow_atlas_source_rd = RID();
		shadow_atlas_initialized = false;
	}
	if (!render_buffers->has_texture(scope, atlas_texture_name)) {
		render_buffers->create_texture(scope, atlas_texture_name, RD::DATA_FORMAT_R32_SFLOAT, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, Size2i(atlas_resolution * 2, atlas_resolution));
	}
	if (!render_buffers->has_texture(scope, mask_texture_name)) {
		render_buffers->create_texture(scope, mask_texture_name, RD::DATA_FORMAT_R8_UNORM, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, screen_size);
	}
	const RID atlas_output = render_buffers->get_texture(scope, atlas_texture_name);
	const RID mask_output = render_buffers->get_texture(scope, mask_texture_name);
	const RID depth = render_buffers->get_depth_texture();
	if (!atlas_output.is_valid() || !mask_output.is_valid() || !depth.is_valid()) {
		disable_voxel_lighting();
		return;
	}

	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
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
	const bool camera_outside_center = !shadow_atlas_initialized ||
			Math::abs(camera_from_center.dot(tangent)) > recenter_distance ||
			Math::abs(camera_from_center.dot(bitangent)) > recenter_distance ||
			Math::abs(camera_from_center.dot(light_direction)) > recenter_distance;
	const bool atlas_invalid = !shadow_atlas_initialized || shadow_atlas_source_rd != atlas_output ||
			shadow_atlas_world_revision != world.revision ||
			!shadow_atlas_light_direction.is_equal_approx(light_direction) ||
			shadow_atlas_resolution != atlas_resolution ||
			!Math::is_equal_approx(shadow_atlas_near_extent, near_extent) ||
			!Math::is_equal_approx(shadow_atlas_far_extent, far_extent) || camera_outside_center;

	if (atlas_invalid) {
		const float snap_size = MAX(world.voxel_size * 0.25f, near_extent * 2.0f / float(atlas_resolution));
		shadow_atlas_center = tangent * (Math::floor(camera_position.dot(tangent) / snap_size + 0.5f) * snap_size) +
				bitangent * (Math::floor(camera_position.dot(bitangent) / snap_size + 0.5f) * snap_size) +
				light_direction * (Math::floor(camera_position.dot(light_direction) / snap_size + 0.5f) * snap_size);
		shadow_atlas_light_direction = light_direction;
		shadow_atlas_tangent = tangent;
		shadow_atlas_bitangent = bitangent;
		shadow_atlas_world_revision = world.revision;
		shadow_atlas_resolution = atlas_resolution;
		shadow_atlas_near_extent = near_extent;
		shadow_atlas_far_extent = far_extent;
		shadow_atlas_source_rd = atlas_output;

		const RID atlas_shader_rid = shadow_atlas_shader.version_get_shader(shadow_atlas_shader_version, 0);
		RD::Uniform u_atlas_output(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ atlas_output }));
		RD::Uniform u_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ world.directory_buffer }));
		RD::Uniform u_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ world.brick_buffer }));
		const RID atlas_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(atlas_shader_rid, 0, u_atlas_output, u_directory, u_bricks);

		ShadowAtlasPushConstant atlas_push = {};
		atlas_push.world_origin_voxel_size[0] = world.origin.x;
		atlas_push.world_origin_voxel_size[1] = world.origin.y;
		atlas_push.world_origin_voxel_size[2] = world.origin.z;
		atlas_push.world_origin_voxel_size[3] = world.voxel_size;
		atlas_push.atlas_center_depth[0] = shadow_atlas_center.x;
		atlas_push.atlas_center_depth[1] = shadow_atlas_center.y;
		atlas_push.atlas_center_depth[2] = shadow_atlas_center.z;
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

		RENDER_TIMESTAMP("Voxel Light-Space Shadow Atlas");
		RD::get_singleton()->draw_command_begin_label("Voxel Light-Space Shadow Atlas");
		RD::ComputeListID atlas_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(atlas_list, shadow_atlas_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(atlas_list, atlas_uniform_set, 0);
		RD::get_singleton()->compute_list_set_push_constant(atlas_list, &atlas_push, sizeof(ShadowAtlasPushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(atlas_list, atlas_resolution * 2, atlas_resolution, 1);
		RD::get_singleton()->compute_list_end();
		RD::get_singleton()->draw_command_end_label();
		shadow_atlas_initialized = true;
		print_verbose(vformat("Voxel Forward light-space shadow atlas: %dx%d per cascade, near %.2f m, far %.2f m, center %s, revision %d.", atlas_resolution, atlas_resolution, near_extent, far_extent, shadow_atlas_center, world.revision));
	}

	const RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	_render_indirect_light(p_render_data, atlas_output, sampler, light_direction, light_color, light_energy);
	const RID resolve_shader_rid = shadow_resolve_shader.version_get_shader(shadow_resolve_shader_version, 0);
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, depth }));
	RD::Uniform u_atlas(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, atlas_output }));
	RD::Uniform u_mask(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ mask_output }));
	RD::Uniform u_occupancy(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, Vector<RID>({ occupancy_uniform_buffer }));
	RD::Uniform u_resolve_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ world.directory_buffer }));
	RD::Uniform u_resolve_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, Vector<RID>({ world.brick_buffer }));
	const RID resolve_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(resolve_shader_rid, 0, u_depth, u_atlas, u_mask, u_occupancy, u_resolve_directory, u_resolve_bricks);

	ShadowResolvePushConstant resolve_push = {};
	const Projection view_projection = p_render_data->scene_data->get_view_projection(0) * Projection(p_render_data->scene_data->cam_transform.affine_inverse());
	RendererRD::MaterialStorage::store_camera(view_projection.inverse(), resolve_push.inv_view_projection);
	resolve_push.atlas_center_voxel_size[0] = shadow_atlas_center.x;
	resolve_push.atlas_center_voxel_size[1] = shadow_atlas_center.y;
	resolve_push.atlas_center_voxel_size[2] = shadow_atlas_center.z;
	resolve_push.atlas_center_voxel_size[3] = world.voxel_size;
	resolve_push.tangent_near_extent[0] = shadow_atlas_tangent.x;
	resolve_push.tangent_near_extent[1] = shadow_atlas_tangent.y;
	resolve_push.tangent_near_extent[2] = shadow_atlas_tangent.z;
	resolve_push.tangent_near_extent[3] = near_extent;
	resolve_push.bitangent_far_extent[0] = shadow_atlas_bitangent.x;
	resolve_push.bitangent_far_extent[1] = shadow_atlas_bitangent.y;
	resolve_push.bitangent_far_extent[2] = shadow_atlas_bitangent.z;
	resolve_push.bitangent_far_extent[3] = far_extent;
	resolve_push.screen_atlas_filter[0] = screen_size.x;
	resolve_push.screen_atlas_filter[1] = screen_size.y;
	resolve_push.screen_atlas_filter[2] = atlas_resolution;
	const bool soft_shadow_enabled = int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_mode")) == 1;
	const uint32_t sample_count = soft_shadow_enabled ? uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_samples")), 1, 8)) : 1u;
	const uint32_t radius_q = uint32_t(CLAMP(Math::round(MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_radius_voxels"))) * 16.0f), 0.0f, 4095.0f));
	const uint32_t bias_q = uint32_t(CLAMP(Math::round(MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/atlas_bias_voxels"))) * 256.0f), 0.0f, 2047.0f));
	resolve_push.screen_atlas_filter[3] = int32_t(sample_count | (soft_shadow_enabled ? 0x80u : 0u) | (radius_q << 8u) | (bias_q << 20u));

	RENDER_TIMESTAMP("Voxel Shadow Atlas Resolve");
	RD::get_singleton()->draw_command_begin_label("Voxel Shadow Atlas Resolve");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, shadow_resolve_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, resolve_uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &resolve_push, sizeof(ShadowResolvePushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, screen_size.x, screen_size.y, 1);
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_light_direction"), light_direction);
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
		RID voxel_texture = texture_storage->texture_get_rd_texture(visible.volume.voxel_texture);
		RID brick_texture = texture_storage->texture_get_rd_texture(visible.volume.brick_texture);
		RID palette_texture = texture_storage->texture_get_rd_texture(visible.volume.palette_texture, true);
		if (!voxel_texture.is_valid() || !brick_texture.is_valid() || !palette_texture.is_valid()) {
			continue;
		}

		RD::Uniform u_voxels(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, voxel_texture }));
		RD::Uniform u_bricks(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, brick_texture }));
		RD::Uniform u_palette(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, palette_texture }));
		RID uniform_set = uniform_set_cache->get_cache(shader, 0, u_voxels, u_bricks, u_palette);

		const Transform3D voxel_to_world = visible.transform.scaled_local(Vector3(visible.volume.voxel_size, visible.volume.voxel_size, visible.volume.voxel_size));
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
		push_constant.volume_dimensions[0] = visible.volume.dimensions.x;
		push_constant.volume_dimensions[1] = visible.volume.dimensions.y;
		push_constant.volume_dimensions[2] = visible.volume.dimensions.z;

		RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set, 0);
		RD::get_singleton()->draw_list_set_push_constant(draw_list, &push_constant, sizeof(VisibilityPushConstant));
		RD::get_singleton()->draw_list_draw(draw_list, false, 1, 36);
	}

	RD::get_singleton()->draw_list_end();
	RD::get_singleton()->draw_command_end_label();
}

} // namespace RendererSceneRenderImplementation
