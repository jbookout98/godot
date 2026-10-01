/**************************************************************************/
/*  render_voxel_forward.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
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
#include "core/object/callable_mp.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

namespace RendererSceneRenderImplementation {
RenderVoxelForward *RenderVoxelForward::bake_renderer = nullptr;

class IndirectBoundaryReadbackReceiver : public Object {
	RenderVoxelForward *owner = nullptr;

public:
	explicit IndirectBoundaryReadbackReceiver(RenderVoxelForward *p_owner) : owner(p_owner) {}

	void completed(const PackedByteArray &p_data, uint32_t p_cascade, uint64_t p_world_revision) {
		if (owner != nullptr) {
			owner->_indirect_boundary_delta_readback(p_data, p_cascade, p_world_revision);
		}
	}

	void detach() {
		owner = nullptr;
	}
};

RenderVoxelForward::RenderVoxelForward() : RenderForwardClustered(true, true) {
	bake_renderer = this;
	indirect_boundary_readback_receiver = memnew(IndirectBoundaryReadbackReceiver(this));
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
	local_shadow_resolve_shader.initialize(shadow_modes);
	local_shadow_resolve_shader_version = local_shadow_resolve_shader.version_create();
	local_shadow_resolve_pipeline = RD::get_singleton()->compute_pipeline_create(local_shadow_resolve_shader.version_get_shader(local_shadow_resolve_shader_version, 0));
	local_shadow_uniform_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(LocalShadowUniformData));
	local_shadow_disabled_uniform_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(LocalShadowUniformData));
	LocalShadowUniformData empty_local_shadows = {};
	RD::get_singleton()->buffer_update(local_shadow_uniform_buffer, 0, sizeof(LocalShadowUniformData), &empty_local_shadows);
	RD::get_singleton()->buffer_update(local_shadow_disabled_uniform_buffer, 0, sizeof(LocalShadowUniformData), &empty_local_shadows);
	indirect_inject_shader.initialize(shadow_modes);
	indirect_inject_shader_version = indirect_inject_shader.version_create();
	indirect_inject_pipeline = RD::get_singleton()->compute_pipeline_create(indirect_inject_shader.version_get_shader(indirect_inject_shader_version, 0));
	indirect_propagate_shader.initialize(shadow_modes);
	indirect_propagate_shader_version = indirect_propagate_shader.version_create();
	indirect_propagate_pipeline = RD::get_singleton()->compute_pipeline_create(indirect_propagate_shader.version_get_shader(indirect_propagate_shader_version, 0));
	indirect_blend_shader.initialize(shadow_modes);
	indirect_blend_shader_version = indirect_blend_shader.version_create();
	indirect_blend_pipeline = RD::get_singleton()->compute_pipeline_create(indirect_blend_shader.version_get_shader(indirect_blend_shader_version, 0));
	indirect_boundary_delta_shader.initialize(shadow_modes);
	indirect_boundary_delta_shader_version = indirect_boundary_delta_shader.version_create();
	indirect_boundary_delta_pipeline = RD::get_singleton()->compute_pipeline_create(indirect_boundary_delta_shader.version_get_shader(indirect_boundary_delta_shader_version, 0));
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		indirect_boundary_delta_buffer[cascade] = RD::get_singleton()->storage_buffer_create(sizeof(uint32_t));
		RD::get_singleton()->set_resource_name(indirect_boundary_delta_buffer[cascade], vformat("Voxel Indirect Boundary Delta %d", cascade));
	}
	Vector<String> ddgi_trace_modes;
	ddgi_trace_modes.push_back(is_using_radiance_octmap_array() ? "\n#define USE_RADIANCE_OCTMAP_ARRAY\n" : "");
	ddgi_trace_shader.initialize(ddgi_trace_modes);
	ddgi_trace_shader_version = ddgi_trace_shader.version_create();
	ddgi_trace_pipeline = RD::get_singleton()->compute_pipeline_create(ddgi_trace_shader.version_get_shader(ddgi_trace_shader_version, 0));
	ddgi_activate_shader.initialize(shadow_modes);
	ddgi_activate_shader_version = ddgi_activate_shader.version_create();
	ddgi_activate_pipeline = RD::get_singleton()->compute_pipeline_create(ddgi_activate_shader.version_get_shader(ddgi_activate_shader_version, 0));
	ddgi_record_scroll_shader.initialize(shadow_modes);
	ddgi_record_scroll_shader_version = ddgi_record_scroll_shader.version_create();
	ddgi_record_scroll_pipeline = RD::get_singleton()->compute_pipeline_create(ddgi_record_scroll_shader.version_get_shader(ddgi_record_scroll_shader_version, 0));
	ddgi_integrate_shader.initialize(shadow_modes);
	ddgi_integrate_shader_version = ddgi_integrate_shader.version_create();
	ddgi_integrate_pipeline = RD::get_singleton()->compute_pipeline_create(ddgi_integrate_shader.version_get_shader(ddgi_integrate_shader_version, 0));
	ddgi_resolve_shader.initialize(shadow_modes);
	ddgi_resolve_shader_version = ddgi_resolve_shader.version_create();
	ddgi_resolve_pipeline = RD::get_singleton()->compute_pipeline_create(ddgi_resolve_shader.version_get_shader(ddgi_resolve_shader_version, 0));
	ddgi_temporal_shader.initialize(shadow_modes);
	ddgi_temporal_shader_version = ddgi_temporal_shader.version_create();
	ddgi_temporal_pipeline = RD::get_singleton()->compute_pipeline_create(ddgi_temporal_shader.version_get_shader(ddgi_temporal_shader_version, 0));
	// Parameter uploads are frame-ringed. Rewriting one uniform buffer while a
	// previous frame still reads it forces the Vulkan backend to serialize the
	// transfer and shader-read usages.
	for (uint32_t slot = 0; slot < DDGI_FRAME_RESOURCE_COUNT; slot++) {
		ddgi_state.ddgi_resolve_uniform_buffer[slot] = RD::get_singleton()->uniform_buffer_create(sizeof(DdgiResolveUniformData));
		ddgi_state.ddgi_temporal_uniform_buffer[slot] = RD::get_singleton()->uniform_buffer_create(sizeof(DdgiTemporalUniformData));
	}
	restir_temporal_shader.initialize(shadow_modes);
	restir_temporal_shader_version = restir_temporal_shader.version_create();
	restir_temporal_pipeline = RD::get_singleton()->compute_pipeline_create(restir_temporal_shader.version_get_shader(restir_temporal_shader_version, 0));
	restir_spatial_shader.initialize(shadow_modes);
	restir_spatial_shader_version = restir_spatial_shader.version_create();
	restir_spatial_pipeline = RD::get_singleton()->compute_pipeline_create(restir_spatial_shader.version_get_shader(restir_spatial_shader_version, 0));
	restir_denoise_shader.initialize(shadow_modes);
	restir_denoise_shader_version = restir_denoise_shader.version_create();
	restir_denoise_pipeline = RD::get_singleton()->compute_pipeline_create(restir_denoise_shader.version_get_shader(restir_denoise_shader_version, 0));
	restir_uniform_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(RestirUniformData));
	restir_dirty_brick_buffer = RD::get_singleton()->storage_buffer_create(sizeof(RestirDirtyBrickData) * RESTIR_MAX_DIRTY_BRICKS);
	reflection_color_inject_shader.initialize(shadow_modes);
	reflection_color_inject_shader_version = reflection_color_inject_shader.version_create();
	reflection_color_inject_pipeline = RD::get_singleton()->compute_pipeline_create(reflection_color_inject_shader.version_get_shader(reflection_color_inject_shader_version, 0));
	reflection_resolve_shader.initialize(shadow_modes);
	reflection_resolve_shader_version = reflection_resolve_shader.version_create();
	reflection_resolve_pipeline = RD::get_singleton()->compute_pipeline_create(reflection_resolve_shader.version_get_shader(reflection_resolve_shader_version, 0));
	ddgi_state.reflection_uniform_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(ReflectionUniformData));
}

RenderVoxelForward::~RenderVoxelForward() {
	bake_renderer = nullptr;
	if (ddgi_seed_version.is_valid()) {
		ddgi_seed_shader.version_free(ddgi_seed_version);
	}
	for (KeyValue<RID, DdgiWorldState> &entry : ddgi_worlds) {
		SWAP(ddgi_state, entry.value);
		_free_world_lighting();
		SWAP(ddgi_state, entry.value);
	}
	ddgi_worlds.clear();
	if (indirect_boundary_readback_receiver != nullptr) {
		indirect_boundary_readback_receiver->detach();
		memdelete(indirect_boundary_readback_receiver);
		indirect_boundary_readback_receiver = nullptr;
	}
	if (RendererRD::MaterialStorage::get_singleton() != nullptr) {
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_light_direction"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_ready"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_near"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_far"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_distant"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_restir_gi"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_restir_ready"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_ready"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_resolve"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_resolve_ready"), Variant());
		static const StringName ddgi_texture_names[] = {
			SNAME("voxel_forward_ddgi_irradiance_lod0"), SNAME("voxel_forward_ddgi_irradiance_lod1"), SNAME("voxel_forward_ddgi_irradiance_lod2"), SNAME("voxel_forward_ddgi_irradiance_lod3"),
			SNAME("voxel_forward_ddgi_depth_lod0"), SNAME("voxel_forward_ddgi_depth_lod1"), SNAME("voxel_forward_ddgi_depth_lod2"), SNAME("voxel_forward_ddgi_depth_lod3"),
			SNAME("voxel_forward_ddgi_metadata_lod0"), SNAME("voxel_forward_ddgi_metadata_lod1"), SNAME("voxel_forward_ddgi_metadata_lod2"), SNAME("voxel_forward_ddgi_metadata_lod3")
		};
		for (const StringName &name : ddgi_texture_names) {
			RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(name, Variant());
		}
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_ambient_color"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_ambient_energy"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_shadow_fill_strength"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_shadow_fill_tint"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_shadow_fill_reach"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_color_saturation"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_toon_enabled"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_toon_band_count"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_toon_band_softness"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_toon_band_range"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_toon_specular_enabled"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_toon_specular_threshold"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_toon_specular_softness"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_toon_specular_strength"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_reflection"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_reflection_ready"), Variant());
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_reflection_intensity"), Variant());
	}
	_reset_shadow_atlas_state();
	_free_local_shadows();
	_free_voxel_reflections();
	_free_ddgi();
	_free_dynamic_voxel_lighting_scene();
	_free_restir_gi();
	_free_indirect_light();
	if (ddgi_state.shadow_mask_texture.is_valid() && RendererRD::TextureStorage::get_singleton() != nullptr) {
		RendererRD::TextureStorage::get_singleton()->texture_free(ddgi_state.shadow_mask_texture);
	}
	if (shadow_atlas_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(shadow_atlas_pipeline);
	}
	if (shadow_atlas_scroll_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(shadow_atlas_scroll_pipeline);
	}
	if (ddgi_state.shadow_atlas_scratch_rd.is_valid()) {
		RD::get_singleton()->free_rid(ddgi_state.shadow_atlas_scratch_rd);
	}
	if (shadow_resolve_legacy_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(shadow_resolve_legacy_pipeline);
	}
	if (shadow_resolve_payload_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(shadow_resolve_payload_pipeline);
	}
	if (local_shadow_resolve_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(local_shadow_resolve_pipeline);
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
	if (indirect_boundary_delta_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(indirect_boundary_delta_pipeline);
	}
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		if (indirect_boundary_delta_buffer[cascade].is_valid()) {
			RD::get_singleton()->free_rid(indirect_boundary_delta_buffer[cascade]);
		}
	}
	if (ddgi_trace_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(ddgi_trace_pipeline);
	}
	if (ddgi_activate_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(ddgi_activate_pipeline);
	}
	if (ddgi_record_scroll_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(ddgi_record_scroll_pipeline);
	}
	if (ddgi_integrate_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(ddgi_integrate_pipeline);
	}
	if (ddgi_resolve_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(ddgi_resolve_pipeline);
	}
	for (RID &buffer : ddgi_state.ddgi_resolve_uniform_buffer) {
		if (buffer.is_valid()) {
			RD::get_singleton()->free_rid(buffer);
		}
	}
	if (ddgi_temporal_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(ddgi_temporal_pipeline);
	}
	for (RID &buffer : ddgi_state.ddgi_temporal_uniform_buffer) {
		if (buffer.is_valid()) {
			RD::get_singleton()->free_rid(buffer);
		}
	}
	if (restir_temporal_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(restir_temporal_pipeline);
	}
	if (restir_spatial_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(restir_spatial_pipeline);
	}
	if (restir_denoise_pipeline.is_valid()) {
		RD::get_singleton()->free_rid(restir_denoise_pipeline);
	}
	if (restir_uniform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(restir_uniform_buffer);
	}
	if (restir_dirty_brick_buffer.is_valid()) {
		RD::get_singleton()->free_rid(restir_dirty_brick_buffer);
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
	local_shadow_resolve_shader.version_free(local_shadow_resolve_shader_version);
	indirect_inject_shader.version_free(indirect_inject_shader_version);
	indirect_propagate_shader.version_free(indirect_propagate_shader_version);
	indirect_blend_shader.version_free(indirect_blend_shader_version);
	indirect_boundary_delta_shader.version_free(indirect_boundary_delta_shader_version);
	ddgi_trace_shader.version_free(ddgi_trace_shader_version);
	ddgi_activate_shader.version_free(ddgi_activate_shader_version);
	ddgi_record_scroll_shader.version_free(ddgi_record_scroll_shader_version);
	ddgi_integrate_shader.version_free(ddgi_integrate_shader_version);
	ddgi_resolve_shader.version_free(ddgi_resolve_shader_version);
	ddgi_temporal_shader.version_free(ddgi_temporal_shader_version);
	restir_temporal_shader.version_free(restir_temporal_shader_version);
	restir_spatial_shader.version_free(restir_spatial_shader_version);
	restir_denoise_shader.version_free(restir_denoise_shader_version);
	reflection_color_inject_shader.version_free(reflection_color_inject_shader_version);
	reflection_resolve_shader.version_free(reflection_resolve_shader_version);
	if (visibility_resources_initialized) {
		for (uint32_t i = 0; i < 3; i++) {
			visibility_pipelines[i].clear();
		}
		visibility_shader.version_free(visibility_shader_version);
	}
	if (outline_resources_initialized) {
		for (uint32_t samples = 0; samples < RD::TEXTURE_SAMPLES_MAX; samples++) {
			for (uint32_t attachment = 0; attachment < 3; attachment++) {
				outline_pipelines[samples][attachment].clear();
			}
		}
		outline_shader.version_free(outline_shader_version);
	}
	if (ddgi_debug_resources_initialized) {
		for (uint32_t samples = 0; samples < RD::TEXTURE_SAMPLES_MAX; samples++) {
			for (uint32_t attachment = 0; attachment < 3; attachment++) {
				ddgi_debug_overlay_pipelines[samples][attachment].clear();
				ddgi_debug_depth_pipelines[samples][attachment].clear();
			}
		}
		ddgi_debug_shader.version_free(ddgi_debug_shader_version);
	}
}

void RenderVoxelForward::_free_local_shadows() {
	local_shadow_mask_rd = RID();
	local_shadow_screen_size = Size2i();
	local_shadow_layer_count = 0;
	local_shadow_masks_ready = false;
	if (local_shadow_uniform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(local_shadow_uniform_buffer);
		local_shadow_uniform_buffer = RID();
	}
	if (local_shadow_disabled_uniform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(local_shadow_disabled_uniform_buffer);
		local_shadow_disabled_uniform_buffer = RID();
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

void RenderVoxelForward::_ensure_outline_resources() {
	if (outline_resources_initialized) {
		return;
	}
	Vector<String> modes;
	modes.push_back("");
	Vector<uint64_t> dynamic_buffers;
	dynamic_buffers.push_back(ShaderRD::DynamicBuffer::encode(0, 2));
	outline_shader.initialize(modes, String(), Vector<RD::PipelineImmutableSampler>(), dynamic_buffers);
	outline_shader_version = outline_shader.version_create();
	RID shader = outline_shader.version_get_shader(outline_shader_version, 0);

	RD::PipelineColorBlendState blend_state;
	RD::PipelineColorBlendState::Attachment color_blend;
	color_blend.enable_blend = true;
	color_blend.src_color_blend_factor = RD::BLEND_FACTOR_SRC_ALPHA;
	color_blend.dst_color_blend_factor = RD::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	color_blend.src_alpha_blend_factor = RD::BLEND_FACTOR_ONE;
	color_blend.dst_alpha_blend_factor = RD::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_state.attachments.push_back(color_blend);
	for (uint32_t attachment = 1; attachment < 3; attachment++) {
		RD::PipelineColorBlendState::Attachment disabled;
		disabled.write_r = false;
		disabled.write_g = false;
		disabled.write_b = false;
		disabled.write_a = false;
		blend_state.attachments.push_back(disabled);
	}

	for (uint32_t samples = 0; samples < RD::TEXTURE_SAMPLES_MAX; samples++) {
		RD::PipelineMultisampleState multisample;
		multisample.sample_count = RD::TextureSamples(samples);
		for (uint32_t attachment = 0; attachment < 3; attachment++) {
			outline_pipelines[samples][attachment].setup(shader, RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), multisample, RD::PipelineDepthStencilState(), blend_state, 0);
		}
	}
	outline_resources_initialized = true;
}

void RenderVoxelForward::_ensure_ddgi_debug_resources() {
	if (ddgi_debug_resources_initialized) {
		return;
	}
	Vector<String> modes;
	modes.push_back("");
	ddgi_debug_shader.initialize(modes);
	ddgi_debug_shader_version = ddgi_debug_shader.version_create();
	RID shader = ddgi_debug_shader.version_get_shader(ddgi_debug_shader_version, 0);
	RD::PipelineColorBlendState blend_state;
	RD::PipelineColorBlendState::Attachment color_blend;
	color_blend.enable_blend = true;
	color_blend.src_color_blend_factor = RD::BLEND_FACTOR_SRC_ALPHA;
	color_blend.dst_color_blend_factor = RD::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	color_blend.src_alpha_blend_factor = RD::BLEND_FACTOR_ONE;
	color_blend.dst_alpha_blend_factor = RD::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_state.attachments.push_back(color_blend);
	for (uint32_t attachment = 1; attachment < 3; attachment++) {
		RD::PipelineColorBlendState::Attachment disabled;
		disabled.write_r = false;
		disabled.write_g = false;
		disabled.write_b = false;
		disabled.write_a = false;
		blend_state.attachments.push_back(disabled);
	}
	RD::PipelineDepthStencilState depth_state;
	depth_state.enable_depth_test = true;
	depth_state.enable_depth_write = false;
	depth_state.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
	for (uint32_t samples = 0; samples < RD::TEXTURE_SAMPLES_MAX; samples++) {
		RD::PipelineMultisampleState multisample;
		multisample.sample_count = RD::TextureSamples(samples);
		for (uint32_t attachment = 0; attachment < 3; attachment++) {
			ddgi_debug_overlay_pipelines[samples][attachment].setup(shader, RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), multisample, RD::PipelineDepthStencilState(), blend_state, 0);
			ddgi_debug_depth_pipelines[samples][attachment].setup(shader, RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), multisample, depth_state, blend_state, 0);
		}
	}
	ddgi_debug_resources_initialized = true;
}

void RenderVoxelForward::_free_voxel_reflections() {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	if (ddgi_state.reflection_texture.is_valid() && texture_storage != nullptr) {
		texture_storage->texture_free(ddgi_state.reflection_texture);
		ddgi_state.reflection_texture = RID();
	}
	ddgi_state.reflection_source_rd = RID();
	ddgi_state.reflection_screen_size = Size2i();
	for (uint32_t cascade = 0; cascade < REFLECTION_CASCADE_COUNT; cascade++) {
		if (ddgi_state.reflection_color_grid_rd[cascade].is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.reflection_color_grid_rd[cascade]);
			ddgi_state.reflection_color_grid_rd[cascade] = RID();
		}
		if (ddgi_state.reflection_material_grid_rd[cascade].is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.reflection_material_grid_rd[cascade]);
			ddgi_state.reflection_material_grid_rd[cascade] = RID();
		}
		ddgi_state.reflection_color_grid_initialized[cascade] = false;
	}
	ddgi_state.reflection_color_grid_resolution = 0;
	ddgi_state.reflection_color_world_revision = UINT64_MAX;
	ddgi_state.reflection_color_shading_revision = UINT64_MAX;
	ddgi_state.reflection_resolve_cache_valid = false;
	ddgi_state.reflection_last_output = RID();
	ddgi_state.reflection_last_world_revision = UINT64_MAX;
	ddgi_state.reflection_last_color_world_revision = UINT64_MAX;
	ddgi_state.reflection_last_indirect_low_latency_update_count = UINT64_MAX;
	ddgi_state.reflection_last_indirect_update_count = UINT64_MAX;
	if (ddgi_state.reflection_uniform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(ddgi_state.reflection_uniform_buffer);
		ddgi_state.reflection_uniform_buffer = RID();
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
		if (indirect_injection_grid_rd[cascade].is_valid()) {
			RD::get_singleton()->free_rid(indirect_injection_grid_rd[cascade]);
			indirect_injection_grid_rd[cascade] = RID();
		}
		indirect_cascade_initialized[cascade] = false;
		indirect_staging_active[cascade] = false;
		indirect_staging_injected[cascade] = false;
		indirect_staging_partial[cascade] = false;
		indirect_staging_scrolled[cascade] = false;
		indirect_staging_scroll_shift[cascade] = Vector3i();
		indirect_staging_adaptive[cascade] = false;
		indirect_staging_boundary_waiting[cascade] = false;
		indirect_staging_boundary_ready[cascade] = false;
		indirect_staging_boundary_delta[cascade] = 0.0f;
		indirect_staging_expansion_count[cascade] = 0;
		indirect_staging_boundary_revision[cascade] = UINT64_MAX;
		indirect_staging_detected_frame[cascade] = 0;
		indirect_staging_next_step[cascade] = 0;
		indirect_blend_active[cascade] = false;
		indirect_blend_has_history[cascade] = false;
		indirect_blend_step[cascade] = 0;
		indirect_blend_frame_count[cascade] = 0;
		indirect_blend_detected_frame[cascade] = 0;
		indirect_active_converged[cascade] = false;
		indirect_active_definition[cascade] = IndirectCascadeDefinition();
		indirect_staging_definition[cascade] = IndirectCascadeDefinition();
		indirect_blend_definition[cascade] = IndirectCascadeDefinition();
	}
	if (indirect_color_grid_uniform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(indirect_color_grid_uniform_buffer);
		indirect_color_grid_uniform_buffer = RID();
	}
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		if (indirect_parent_grid_uniform_buffer[cascade].is_valid()) {
			RD::get_singleton()->free_rid(indirect_parent_grid_uniform_buffer[cascade]);
			indirect_parent_grid_uniform_buffer[cascade] = RID();
		}
	}
	indirect_grid_resolution = 0;
	indirect_world_revision = UINT64_MAX;
	indirect_schedule_cursor = 0;
	indirect_blend_schedule_cursor = 0;
	indirect_staging_batch_classified = false;
	indirect_staging_batch_low_latency = false;
	indirect_staging_dirty_cell_count = 0;
	indirect_staging_cell_pass_workload = 0;
	indirect_staging_dispatch_count = 0;
	indirect_render_frame_index = 0;
	indirect_staging_batch_detected_frame = 0;
	indirect_low_latency_update_count = 0;
	indirect_temporal_update_count = 0;
	indirect_last_publication_frames = 0;
	indirect_last_invalidated_cell_count = 0;
	indirect_last_convergence_expansions = 0;
	indirect_last_boundary_delta = 0.0f;
}

void RenderVoxelForward::_free_ddgi() {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	if (ddgi_state.ddgi_resolve_output_texture.is_valid() && texture_storage != nullptr) {
		texture_storage->texture_free(ddgi_state.ddgi_resolve_output_texture);
	}
	ddgi_state.ddgi_resolve_output_texture = RID();
	ddgi_state.ddgi_resolve_output_rd = RID();
	ddgi_state.ddgi_published_output_rd = RID();
	for (uint32_t slot = 0; slot < 2; slot++) {
		ddgi_state.ddgi_temporal_output_rd[slot] = RID();
		ddgi_state.ddgi_temporal_face_rd[slot] = RID();
	}
	ddgi_state.ddgi_temporal_slot = 0;
	ddgi_state.ddgi_temporal_history_valid = false;
	ddgi_state.ddgi_temporal_debug_mode = -1;
	ddgi_state.ddgi_resolve_screen_size = Size2i();
	for (uint32_t cascade = 0; cascade < DDGI_LOD_COUNT; cascade++) {
		if (ddgi_state.ddgi_cascades[cascade].irradiance_texture.is_valid() && texture_storage != nullptr) {
			const RID adopted = ddgi_state.ddgi_cascades[cascade].irradiance_rd;
			texture_storage->texture_free(ddgi_state.ddgi_cascades[cascade].irradiance_texture);
			ddgi_state.ddgi_cascades[cascade].irradiance_texture = RID();
			if (RD::get_singleton()->texture_is_valid(adopted)) {
				RD::get_singleton()->free_rid(adopted);
			}
			ddgi_state.ddgi_cascades[cascade].irradiance_rd = RID();
		}
		if (ddgi_state.ddgi_cascades[cascade].visibility_texture.is_valid() && texture_storage != nullptr) {
			const RID adopted = ddgi_state.ddgi_cascades[cascade].visibility_rd;
			texture_storage->texture_free(ddgi_state.ddgi_cascades[cascade].visibility_texture);
			ddgi_state.ddgi_cascades[cascade].visibility_texture = RID();
			if (RD::get_singleton()->texture_is_valid(adopted)) {
				RD::get_singleton()->free_rid(adopted);
			}
			ddgi_state.ddgi_cascades[cascade].visibility_rd = RID();
		}
		if (ddgi_state.ddgi_cascades[cascade].metadata_texture.is_valid() && texture_storage != nullptr) {
			const RID adopted = ddgi_state.ddgi_cascades[cascade].metadata_rd;
			texture_storage->texture_free(ddgi_state.ddgi_cascades[cascade].metadata_texture);
			ddgi_state.ddgi_cascades[cascade].metadata_texture = RID();
			if (RD::get_singleton()->texture_is_valid(adopted)) {
				RD::get_singleton()->free_rid(adopted);
			}
			ddgi_state.ddgi_cascades[cascade].metadata_rd = RID();
		}
		if (ddgi_state.ddgi_cascades[cascade].irradiance_rd.is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.ddgi_cascades[cascade].irradiance_rd);
		}
		if (ddgi_state.ddgi_cascades[cascade].visibility_rd.is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.ddgi_cascades[cascade].visibility_rd);
		}
		if (ddgi_state.ddgi_cascades[cascade].metadata_rd.is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.ddgi_cascades[cascade].metadata_rd);
		}
		if (ddgi_state.ddgi_cascades[cascade].probe_records.is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.ddgi_cascades[cascade].probe_records);
		}
		if (ddgi_state.ddgi_cascades[cascade].probe_upload_records.is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.ddgi_cascades[cascade].probe_upload_records);
		}
		ddgi_state.ddgi_cascades[cascade].irradiance_rd = RID();
		ddgi_state.ddgi_cascades[cascade].visibility_rd = RID();
		ddgi_state.ddgi_cascades[cascade].metadata_rd = RID();
		ddgi_state.ddgi_cascades[cascade].probe_records = RID();
		ddgi_state.ddgi_cascades[cascade].probe_upload_records = RID();
		ddgi_state.ddgi_cascades[cascade].origin = Vector3();
		ddgi_state.ddgi_cascades[cascade].logical_origin = Vector3i();
		ddgi_state.ddgi_cascades[cascade].phase_offset = Vector3i();
		ddgi_state.ddgi_cascades[cascade].cell_size = Vector3();
		ddgi_state.ddgi_cascades[cascade].initialized = false;
		ddgi_state.ddgi_cascades[cascade].occupancy_revision = 0;
		ddgi_state.ddgi_cascades[cascade].lighting_revision = 0;
		ddgi_state.ddgi_cascades[cascade].active_update_cursor = 0;
		ddgi_state.ddgi_cascades[cascade].urgent_update_cursor = 0;
		ddgi_state.ddgi_cascades[cascade].edit_boost_updates_remaining = 0;
		if (ddgi_state.ddgi_cascades[cascade].probe_index_buffer.is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.ddgi_cascades[cascade].probe_index_buffer);
			ddgi_state.ddgi_cascades[cascade].probe_index_buffer = RID();
		}
		if (ddgi_state.ddgi_cascades[cascade].probe_counter_buffer.is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.ddgi_cascades[cascade].probe_counter_buffer);
			ddgi_state.ddgi_cascades[cascade].probe_counter_buffer = RID();
		}
		if (ddgi_state.ddgi_cascades[cascade].probe_dispatch_buffer.is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.ddgi_cascades[cascade].probe_dispatch_buffer);
			ddgi_state.ddgi_cascades[cascade].probe_dispatch_buffer = RID();
		}
		if (ddgi_state.ddgi_cascades[cascade].dynamic_bin_buffer.is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.ddgi_cascades[cascade].dynamic_bin_buffer);
			ddgi_state.ddgi_cascades[cascade].dynamic_bin_buffer = RID();
		}
		ddgi_state.ddgi_cascades[cascade].classify_uniform_set = RID();
		ddgi_state.ddgi_cascades[cascade].integrate_uniform_set = RID();
		for (uint32_t slot = 0; slot < DDGI_FRAME_RESOURCE_COUNT; slot++) {
			ddgi_state.ddgi_cascades[cascade].trace_uniform_set[slot] = RID();
			ddgi_state.ddgi_cascades[cascade].trace_shadow[slot] = RID();
			ddgi_state.ddgi_cascades[cascade].trace_environment[slot] = RID();
			ddgi_state.ddgi_cascades[cascade].trace_sampler[slot] = RID();
			ddgi_state.ddgi_cascades[cascade].trace_sky_sampler[slot] = RID();
			ddgi_state.ddgi_cascades[cascade].trace_color_grid[slot] = RID();
		}
		ddgi_state.ddgi_cascades[cascade].dynamic_bin_capacity = 0;
		ddgi_state.ddgi_cascades[cascade].probe_index_capacity = 0;
		ddgi_state.ddgi_cascades[cascade].dynamic_bin_hash = 0;
		ddgi_state.ddgi_cascades[cascade].dynamic_source_hash = 0;
		ddgi_state.ddgi_cascades[cascade].activation_dirty = true;
	}
	if (ddgi_state.ddgi_surfel_buffer.is_valid()) {
		RD::get_singleton()->free_rid(ddgi_state.ddgi_surfel_buffer);
		ddgi_state.ddgi_surfel_buffer = RID();
	}
	if (ddgi_state.ddgi_local_light_buffer.is_valid()) {
		RD::get_singleton()->free_rid(ddgi_state.ddgi_local_light_buffer);
		ddgi_state.ddgi_local_light_buffer = RID();
	}
	ddgi_state.ddgi_local_light_buffer_capacity = 0;
	ddgi_state.ddgi_local_light_count = 0;
	ddgi_state.ddgi_local_light_overflow_count = 0;
	ddgi_state.ddgi_local_light_upload_hash = 0;
	ddgi_state.ddgi_local_light_cache.clear();
	for (RID &buffer : ddgi_state.ddgi_trace_uniform_buffer) {
		if (buffer.is_valid()) {
			RD::get_singleton()->free_rid(buffer);
			buffer = RID();
		}
	}
	ddgi_state.ddgi_probe_resolution = 0;
	ddgi_state.ddgi_irradiance_atlas_width = 0;
	ddgi_state.ddgi_irradiance_atlas_height = 0;
	ddgi_state.ddgi_visibility_atlas_width = 0;
	ddgi_state.ddgi_visibility_atlas_height = 0;
	ddgi_state.ddgi_surfel_capacity = 0;
	ddgi_state.ddgi_schedule_cursor = 0;
	ddgi_state.ddgi_world_revision = UINT64_MAX;
	ddgi_state.ddgi_placement_revision = UINT64_MAX;
	ddgi_state.ddgi_page_publication_serial = UINT64_MAX;
	ddgi_state.ddgi_incremental_edit_pending = false;
	ddgi_state.ddgi_frame_index = 0;
	ddgi_state.ddgi_lighting_revision = 1;
	ddgi_state.ddgi_light_direction = Vector3();
	ddgi_state.ddgi_light_color = Color();
	ddgi_state.ddgi_light_energy = 0.0f;
	ddgi_state.ddgi_light_valid = false;
	ddgi_state.ddgi_resident_cache_hits = 0;
	ddgi_state.ddgi_resident_empty_slots = 0;
	ddgi_state.ddgi_camera_placement_work_count = 0;
	ddgi_state.ddgi_camera_upload_bytes = 0;
	ddgi_state.ddgi_new_plane_upload_count = 0;
}

void RenderVoxelForward::_free_dynamic_voxel_lighting_scene() {
	RID *buffers[] = {
		&ddgi_state.ddgi_dynamic_volume_buffer,
		&ddgi_state.ddgi_dynamic_bvh_buffer,
		&ddgi_state.ddgi_dynamic_directory_buffer,
		&ddgi_state.ddgi_dynamic_brick_buffer,
	};
	if (RD::get_singleton() != nullptr) {
		for (RID *buffer : buffers) {
			if (buffer->is_valid()) {
				RD::get_singleton()->free_rid(*buffer);
				*buffer = RID();
			}
		}
	}
	ddgi_state.ddgi_dynamic_volume_buffer_capacity = 0;
	ddgi_state.ddgi_dynamic_bvh_buffer_capacity = 0;
	ddgi_state.ddgi_dynamic_directory_buffer_capacity = 0;
	ddgi_state.ddgi_dynamic_brick_buffer_capacity = 0;
	ddgi_state.ddgi_dynamic_volume_count = 0;
	ddgi_state.ddgi_dynamic_bvh_node_count = 0;
	ddgi_state.ddgi_dynamic_content_cache.clear();
	ddgi_state.ddgi_dynamic_current_bounds.clear();
	for (Vector<AABB> &dirty_bounds : ddgi_state.ddgi_dynamic_dirty_bounds) {
		dirty_bounds.clear();
	}
}

void RenderVoxelForward::_update_dynamic_voxel_lighting_scene(const VoxelForwardVolumeStorage::WorldOccupancy &p_world) {
	RD *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}

	struct SourceVolume {
		const VoxelForwardVolumeStorage::Volume *volume = nullptr;
		_FORCE_INLINE_ bool operator<(const SourceVolume &p_other) const { return volume->base < p_other.volume->base; }
	};
	Vector<const VoxelForwardVolumeStorage::Volume *> non_world_volumes;
	volume_storage->get_non_world_occupancy_volumes(non_world_volumes);
	Vector<SourceVolume> sources;
	for (const VoxelForwardVolumeStorage::Volume *volume : non_world_volumes) {
		if (volume == nullptr || volume->voxel_size <= 0.0f || volume->dimensions.x <= 0 || volume->dimensions.y <= 0 || volume->dimensions.z <= 0 ||
				volume->brick_dimensions.x <= 0 || volume->brick_dimensions.y <= 0 || volume->brick_dimensions.z <= 0) {
			continue;
		}
		const uint64_t expected_directory_bytes = uint64_t(volume->brick_dimensions.x) * uint64_t(volume->brick_dimensions.y) * uint64_t(volume->brick_dimensions.z) * sizeof(uint32_t);
		if (expected_directory_bytes > uint64_t(volume->occupancy_directory_cpu.size()) || (volume->occupancy_bricks_cpu.size() % 4) != 0) {
			ERR_PRINT_ONCE(vformat("Voxel Forward dynamic lighting rejected voxel volume %s because its sparse occupancy payload is malformed.", volume->base));
			continue;
		}
		const Basis &basis = volume->transform.basis;
		const float scale = basis.get_column(0).length();
		const Vector3 axis0 = scale > 0.000001f ? basis.get_column(0) / scale : Vector3();
		const Vector3 axis1 = scale > 0.000001f ? basis.get_column(1) / scale : Vector3();
		const Vector3 axis2 = scale > 0.000001f ? basis.get_column(2) / scale : Vector3();
		const bool supported_transform = scale > 0.000001f &&
				Math::is_equal_approx(basis.get_column(1).length(), scale) && Math::is_equal_approx(basis.get_column(2).length(), scale) &&
				Math::abs(axis0.dot(axis1)) < 0.0001f && Math::abs(axis0.dot(axis2)) < 0.0001f && Math::abs(axis1.dot(axis2)) < 0.0001f;
		if (!supported_transform) {
			ERR_PRINT_ONCE(vformat("Voxel Forward dynamic lighting rejected voxel volume %s because non-uniform scale or shear is unsupported.", volume->base));
			continue;
		}
		SourceVolume source;
		source.volume = volume;
		sources.push_back(source);
	}
	sources.sort();

	bool layout_changed = sources.size() != ddgi_state.ddgi_dynamic_content_cache.size();
	if (!layout_changed) {
		for (int index = 0; index < sources.size(); index++) {
			const VoxelForwardVolumeStorage::Volume &volume = *sources[index].volume;
			const DdgiDynamicContentCacheEntry &cached = ddgi_state.ddgi_dynamic_content_cache[index];
			if (cached.base != volume.base || cached.directory_size_bytes != uint32_t(volume.occupancy_directory_cpu.size()) || cached.brick_size_bytes != uint32_t(volume.occupancy_bricks_cpu.size())) {
				layout_changed = true;
				break;
			}
		}
	}

	Vector<DdgiDynamicContentCacheEntry> previous_cache = ddgi_state.ddgi_dynamic_content_cache;
	PackedByteArray directory_upload;
	PackedByteArray brick_upload;
	if (layout_changed) {
		ddgi_state.ddgi_dynamic_content_cache.clear();
		for (const SourceVolume &source : sources) {
			const VoxelForwardVolumeStorage::Volume &volume = *source.volume;
			DdgiDynamicContentCacheEntry entry;
			entry.base = volume.base;
			entry.revision = volume.revision;
			entry.directory_offset_words = directory_upload.size() / 4;
			entry.directory_size_bytes = volume.occupancy_directory_cpu.size();
			entry.brick_offset_words = brick_upload.size() / 4;
			entry.brick_size_bytes = volume.occupancy_bricks_cpu.size();
			entry.previous_transform = volume.transform;
			entry.previous_bounds = volume.transform.xform(AABB(Vector3(), Vector3(volume.dimensions) * volume.voxel_size));
			directory_upload.append_array(volume.occupancy_directory_cpu);
			brick_upload.append_array(volume.occupancy_bricks_cpu);
			ddgi_state.ddgi_dynamic_content_cache.push_back(entry);
		}
	}

	auto ensure_buffer = [rd](RID &r_buffer, uint32_t &r_capacity, uint32_t p_required) {
		const uint32_t required = MAX(16u, p_required);
		if (!r_buffer.is_valid() || r_capacity < required) {
			if (r_buffer.is_valid()) {
				rd->free_rid(r_buffer);
			}
			r_buffer = rd->storage_buffer_create(required);
			r_capacity = required;
			return true;
		}
		return false;
	};

	const uint32_t directory_bytes = layout_changed ? uint32_t(directory_upload.size()) : (ddgi_state.ddgi_dynamic_content_cache.is_empty() ? 0u : ddgi_state.ddgi_dynamic_content_cache[ddgi_state.ddgi_dynamic_content_cache.size() - 1].directory_offset_words * 4u + ddgi_state.ddgi_dynamic_content_cache[ddgi_state.ddgi_dynamic_content_cache.size() - 1].directory_size_bytes);
	const uint32_t brick_bytes = layout_changed ? uint32_t(brick_upload.size()) : (ddgi_state.ddgi_dynamic_content_cache.is_empty() ? 0u : ddgi_state.ddgi_dynamic_content_cache[ddgi_state.ddgi_dynamic_content_cache.size() - 1].brick_offset_words * 4u + ddgi_state.ddgi_dynamic_content_cache[ddgi_state.ddgi_dynamic_content_cache.size() - 1].brick_size_bytes);
	if (layout_changed) {
		print_verbose(vformat("Voxel Forward dynamic lighting scene: %d local volume(s), %.2f KiB directory, %.2f KiB occupancy bricks; sparse content remains resident across transform-only updates.", sources.size(), double(directory_bytes) / 1024.0, double(brick_bytes) / 1024.0));
	}
	const bool directory_reallocated = ensure_buffer(ddgi_state.ddgi_dynamic_directory_buffer, ddgi_state.ddgi_dynamic_directory_buffer_capacity, directory_bytes);
	const bool brick_reallocated = ensure_buffer(ddgi_state.ddgi_dynamic_brick_buffer, ddgi_state.ddgi_dynamic_brick_buffer_capacity, brick_bytes);
	if (layout_changed) {
		if (!directory_upload.is_empty()) {
			rd->buffer_update(ddgi_state.ddgi_dynamic_directory_buffer, 0, directory_upload.size(), directory_upload.ptr());
			ddgi_state.ddgi_dynamic_uploaded_bytes += directory_upload.size();
		}
		if (!brick_upload.is_empty()) {
			rd->buffer_update(ddgi_state.ddgi_dynamic_brick_buffer, 0, brick_upload.size(), brick_upload.ptr());
			ddgi_state.ddgi_dynamic_uploaded_bytes += brick_upload.size();
		}
	} else {
		for (int index = 0; index < sources.size(); index++) {
			const VoxelForwardVolumeStorage::Volume &volume = *sources[index].volume;
			DdgiDynamicContentCacheEntry &cached = ddgi_state.ddgi_dynamic_content_cache.write[index];
			if (cached.revision != volume.revision || directory_reallocated || brick_reallocated) {
				if (!volume.occupancy_directory_cpu.is_empty()) {
					rd->buffer_update(ddgi_state.ddgi_dynamic_directory_buffer, cached.directory_offset_words * 4u, cached.directory_size_bytes, volume.occupancy_directory_cpu.ptr());
					ddgi_state.ddgi_dynamic_uploaded_bytes += cached.directory_size_bytes;
				}
				if (!volume.occupancy_bricks_cpu.is_empty()) {
					rd->buffer_update(ddgi_state.ddgi_dynamic_brick_buffer, cached.brick_offset_words * 4u, cached.brick_size_bytes, volume.occupancy_bricks_cpu.ptr());
					ddgi_state.ddgi_dynamic_uploaded_bytes += cached.brick_size_bytes;
				}
				cached.revision = volume.revision;
			}
		}
	}

	Vector<DdgiDynamicVolumeGpuData> gpu_volumes;
	Vector<AABB> current_bounds;
	auto append_ddgi_dirty_region = [&](const AABB &p_bounds) {
		for (uint32_t lod = 0; lod < DDGI_LOD_COUNT; lod++) {
			const float spacing = float(p_world.ddgi_cell_spacing_voxels[lod]);
			const Vector3 cell_world = Vector3(spacing, spacing, spacing) * p_world.voxel_size;
			const float surface_bias = MIN(cell_world.x, MIN(cell_world.y, cell_world.z)) * 0.75f;
			const Vector3 influence = cell_world + Vector3(surface_bias, surface_bias, surface_bias);
			ddgi_state.ddgi_dynamic_dirty_bounds[lod].push_back(AABB(p_bounds.position - influence, p_bounds.size + influence * 2.0f));
		}
	};
	gpu_volumes.resize(sources.size());
	current_bounds.resize(sources.size());
	bool scene_changed = layout_changed;
	for (int index = 0; index < sources.size(); index++) {
		const VoxelForwardVolumeStorage::Volume &volume = *sources[index].volume;
		DdgiDynamicContentCacheEntry &cached = ddgi_state.ddgi_dynamic_content_cache.write[index];
		DdgiDynamicVolumeGpuData &gpu = gpu_volumes.write[index];
		memset(&gpu, 0, sizeof(gpu));
		const Transform3D world_to_local = volume.transform.affine_inverse();
		for (int row = 0; row < 3; row++) {
			for (int column = 0; column < 3; column++) {
				gpu.world_to_voxel[row][column] = world_to_local.basis[row][column] / volume.voxel_size;
			}
			gpu.world_to_voxel[row][3] = world_to_local.origin[row] / volume.voxel_size;
		}
		const Basis normal_to_world = volume.transform.basis.inverse().transposed();
		for (int row = 0; row < 3; row++) {
			for (int column = 0; column < 3; column++) {
				gpu.normal_to_world[row][column] = normal_to_world[row][column];
			}
		}
		const AABB bounds = volume.transform.xform(AABB(Vector3(), Vector3(volume.dimensions) * volume.voxel_size));
		current_bounds.write[index] = bounds;
		for (int axis = 0; axis < 3; axis++) {
			gpu.bounds_min[axis] = bounds.position[axis];
			gpu.bounds_max[axis] = (bounds.position + bounds.size)[axis];
			gpu.dimensions_directory_offset[axis] = volume.dimensions[axis];
			gpu.brick_dimensions_brick_offset[axis] = volume.brick_dimensions[axis];
		}
		gpu.dimensions_directory_offset[3] = cached.directory_offset_words;
		gpu.brick_dimensions_brick_offset[3] = cached.brick_offset_words;
		gpu.storage[0] = cached.directory_size_bytes / 4u;
		gpu.storage[1] = cached.brick_size_bytes / 4u;
		gpu.storage[2] = uint32_t(volume.revision);
		gpu.storage[3] = 1u;

		const bool revision_changed = !layout_changed && index < previous_cache.size() && previous_cache[index].revision != volume.revision;
		const bool transform_changed = cached.previous_transform != volume.transform;
		if (layout_changed || transform_changed) {
			const AABB swept = cached.previous_bounds.has_volume() ? cached.previous_bounds.merge(bounds) : bounds;
			append_ddgi_dirty_region(swept);
		}
		scene_changed = scene_changed || transform_changed || revision_changed;
		cached.previous_transform = volume.transform;
		cached.previous_bounds = bounds;
		cached.revision = volume.revision;
	}
	if (layout_changed) {
		for (const DdgiDynamicContentCacheEntry &old_entry : previous_cache) {
			bool retained = false;
			for (const DdgiDynamicContentCacheEntry &new_entry : ddgi_state.ddgi_dynamic_content_cache) {
				if (old_entry.base == new_entry.base) {
					retained = true;
					break;
				}
			}
			if (!retained && old_entry.previous_bounds.has_volume()) {
				append_ddgi_dirty_region(old_entry.previous_bounds);
			}
		}
	}
	Vector<AABB> lighting_dirty_bounds;
	volume_storage->consume_lighting_dirty_bounds(lighting_dirty_bounds);
	for (const AABB &dirty : lighting_dirty_bounds) {
		append_ddgi_dirty_region(dirty);
	}
	scene_changed = scene_changed || !lighting_dirty_bounds.is_empty();

	struct MortonVolume {
		uint32_t code = 0;
		uint32_t volume_index = 0;
		_FORCE_INLINE_ bool operator<(const MortonVolume &p_other) const { return code == p_other.code ? volume_index < p_other.volume_index : code < p_other.code; }
	};
	auto spread_bits = [](uint32_t value) {
		value &= 0x000003ffu;
		value = (value | (value << 16u)) & 0x030000ffu;
		value = (value | (value << 8u)) & 0x0300f00fu;
		value = (value | (value << 4u)) & 0x030c30c3u;
		value = (value | (value << 2u)) & 0x09249249u;
		return value;
	};
	Vector<MortonVolume> morton_volumes;
	if (!current_bounds.is_empty()) {
		AABB scene_bounds = current_bounds[0];
		for (int index = 1; index < current_bounds.size(); index++) {
			scene_bounds.merge_with(current_bounds[index]);
		}
		const Vector3 scene_size = scene_bounds.size;
		morton_volumes.resize(current_bounds.size());
		for (int index = 0; index < current_bounds.size(); index++) {
			const Vector3 center = current_bounds[index].get_center();
			Vector3 unit;
			for (int axis = 0; axis < 3; axis++) {
				unit[axis] = scene_size[axis] > 0.000001f ? CLAMP((center[axis] - scene_bounds.position[axis]) / scene_size[axis], 0.0f, 1.0f) : 0.5f;
			}
			const uint32_t x = uint32_t(Math::round(unit.x * 1023.0f));
			const uint32_t y = uint32_t(Math::round(unit.y * 1023.0f));
			const uint32_t z = uint32_t(Math::round(unit.z * 1023.0f));
			morton_volumes.write[index].code = spread_bits(x) | (spread_bits(y) << 1u) | (spread_bits(z) << 2u);
			morton_volumes.write[index].volume_index = index;
		}
		morton_volumes.sort();
	}

	Vector<DdgiDynamicBvhNodeGpuData> gpu_nodes;
	auto build_node = [&](auto &&self, uint32_t begin, uint32_t end) -> uint32_t {
		const uint32_t node_index = gpu_nodes.size();
		gpu_nodes.push_back(DdgiDynamicBvhNodeGpuData());
		DdgiDynamicBvhNodeGpuData node = {};
		AABB bounds = current_bounds[morton_volumes[begin].volume_index];
		for (uint32_t index = begin + 1u; index < end; index++) {
			bounds.merge_with(current_bounds[morton_volumes[index].volume_index]);
		}
		for (int axis = 0; axis < 3; axis++) {
			node.bounds_min[axis] = bounds.position[axis];
			node.bounds_max[axis] = (bounds.position + bounds.size)[axis];
		}
		if (end - begin == 1u) {
			node.children[0] = morton_volumes[begin].volume_index;
			node.children[2] = 1u;
		} else {
			const uint32_t middle = begin + (end - begin) / 2u;
			node.children[0] = self(self, begin, middle);
			node.children[1] = self(self, middle, end);
		}
		gpu_nodes.write[node_index] = node;
		return node_index;
	};
	uint32_t root_node = 0;
	if (!morton_volumes.is_empty()) {
		root_node = build_node(build_node, 0u, morton_volumes.size());
	}

	const uint32_t volume_upload_size = 16u + gpu_volumes.size() * sizeof(DdgiDynamicVolumeGpuData);
	const uint32_t bvh_upload_size = 16u + gpu_nodes.size() * sizeof(DdgiDynamicBvhNodeGpuData);
	ensure_buffer(ddgi_state.ddgi_dynamic_volume_buffer, ddgi_state.ddgi_dynamic_volume_buffer_capacity, volume_upload_size);
	ensure_buffer(ddgi_state.ddgi_dynamic_bvh_buffer, ddgi_state.ddgi_dynamic_bvh_buffer_capacity, bvh_upload_size);
	PackedByteArray volume_bytes;
	volume_bytes.resize(volume_upload_size);
	memset(volume_bytes.ptrw(), 0, volume_bytes.size());
	uint32_t *volume_header = reinterpret_cast<uint32_t *>(volume_bytes.ptrw());
	volume_header[0] = gpu_volumes.size();
	volume_header[1] = gpu_nodes.size();
	volume_header[2] = uint32_t(ddgi_state.ddgi_dynamic_scene_revision + (scene_changed ? 1u : 0u));
	volume_header[3] = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/max_trace_steps")), 32, 4096));
	if (!gpu_volumes.is_empty()) {
		memcpy(volume_bytes.ptrw() + 16, gpu_volumes.ptr(), gpu_volumes.size() * sizeof(DdgiDynamicVolumeGpuData));
	}
	rd->buffer_update(ddgi_state.ddgi_dynamic_volume_buffer, 0, volume_bytes.size(), volume_bytes.ptr());
	PackedByteArray bvh_bytes;
	bvh_bytes.resize(bvh_upload_size);
	memset(bvh_bytes.ptrw(), 0, bvh_bytes.size());
	uint32_t *bvh_header = reinterpret_cast<uint32_t *>(bvh_bytes.ptrw());
	bvh_header[0] = root_node;
	bvh_header[1] = gpu_nodes.size();
	if (!gpu_nodes.is_empty()) {
		memcpy(bvh_bytes.ptrw() + 16, gpu_nodes.ptr(), gpu_nodes.size() * sizeof(DdgiDynamicBvhNodeGpuData));
	}
	rd->buffer_update(ddgi_state.ddgi_dynamic_bvh_buffer, 0, bvh_bytes.size(), bvh_bytes.ptr());
	ddgi_state.ddgi_dynamic_uploaded_bytes += volume_bytes.size() + bvh_bytes.size();
	ddgi_state.ddgi_dynamic_volume_count = gpu_volumes.size();
	ddgi_state.ddgi_dynamic_bvh_node_count = gpu_nodes.size();
	ddgi_state.ddgi_dynamic_current_bounds = current_bounds;
	if (scene_changed) {
		ddgi_state.ddgi_dynamic_scene_revision++;
	}
}

void RenderVoxelForward::_free_restir_gi() {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	if (material_storage != nullptr) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_restir_gi"), Variant());
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_restir_ready"), false);
	}
	if (restir_output_texture.is_valid() && texture_storage != nullptr) {
		texture_storage->texture_free(restir_output_texture);
	}
	restir_output_texture = RID();
	restir_output_rd = RID();
	for (uint32_t slot = 0; slot < 2; slot++) {
		if (restir_temporal_reservoir[slot].is_valid()) {
			RD::get_singleton()->free_rid(restir_temporal_reservoir[slot]);
			restir_temporal_reservoir[slot] = RID();
		}
		if (restir_spatial_reservoir[slot].is_valid()) {
			RD::get_singleton()->free_rid(restir_spatial_reservoir[slot]);
			restir_spatial_reservoir[slot] = RID();
		}
	}
	restir_screen_size = Size2i();
	restir_frame_index = 0;
	restir_history_slot = 0;
	restir_world_revision = UINT64_MAX;
	restir_shading_revision = UINT64_MAX;
	restir_history_valid = false;
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
	_release_shadow_atlas_snapshot(ddgi_state.shadow_atlas_staging);
	_release_shadow_atlas_snapshot(ddgi_state.shadow_atlas_retired_snapshot);
	ddgi_state.shadow_atlas_source_rd = RID();
	ddgi_state.shadow_atlas_directory_rd = RID();
	ddgi_state.shadow_atlas_bricks_rd = RID();
	ddgi_state.shadow_atlas_world_revision = UINT64_MAX;
	ddgi_state.shadow_atlas_center = Vector3();
	ddgi_state.shadow_atlas_light_direction = Vector3();
	ddgi_state.shadow_atlas_tangent = Vector3();
	ddgi_state.shadow_atlas_bitangent = Vector3();
	ddgi_state.shadow_atlas_resolution = 0;
	ddgi_state.shadow_atlas_voxel_size = 0.0f;
	ddgi_state.shadow_atlas_near_extent = 0.0f;
	ddgi_state.shadow_atlas_far_extent = 0.0f;
	ddgi_state.shadow_atlas_initialized = false;
	ddgi_state.shadow_atlas_temporal_rebuild_in_progress = false;
	ddgi_state.shadow_atlas_rebuild_next_texel = 0;
	ddgi_state.shadow_atlas_rebuild_total_texels = 0;
	ddgi_state.shadow_atlas_pending_world_revision = UINT64_MAX;
	ddgi_state.shadow_atlas_pending_change_count = 0;
	ddgi_state.shadow_atlas_pending_full_rebuild = false;
	ddgi_state.shadow_atlas_pending_dirty_reliable = true;
	ddgi_state.shadow_atlas_pending_dirty_bricks.clear();
}

void RenderVoxelForward::_indirect_boundary_delta_readback(const PackedByteArray &p_data, uint32_t p_cascade, uint64_t p_world_revision) {
	if (p_cascade >= INDIRECT_CASCADE_COUNT || p_data.size() < int(sizeof(uint32_t))) {
		return;
	}
	if (!indirect_staging_active[p_cascade] || !indirect_staging_boundary_waiting[p_cascade] ||
			indirect_staging_boundary_revision[p_cascade] != p_world_revision ||
			indirect_staging_definition[p_cascade].world_revision != p_world_revision) {
		return;
	}
	uint32_t delta_bits = 0;
	memcpy(&delta_bits, p_data.ptr(), sizeof(uint32_t));
	float delta = 0.0f;
	memcpy(&delta, &delta_bits, sizeof(float));
	indirect_staging_boundary_delta[p_cascade] = Math::is_finite(delta) ? MAX(delta, 0.0f) : 1e20f;
	indirect_staging_boundary_waiting[p_cascade] = false;
	indirect_staging_boundary_ready[p_cascade] = true;
}

void RenderVoxelForward::_render_indirect_light(const RenderDataRD *p_render_data, RID p_shadow_atlas, RID p_sampler, const Vector3 &p_light_direction, const Color &p_light_color, float p_light_energy) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	const bool enabled = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/enabled"));
	const int backend = enabled ? CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/backend")), 1, 2) : 0;
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_backend"), backend);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_debug_mode"), backend == 1 ? int(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/debug_mode")) : 0);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_corner_ao_strength"), CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/voxel_gi/corner_occlusion_strength")), 0.0f, 1.0f));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_corner_ao_tint"), Color(GLOBAL_GET("rendering/voxel_forward/indirect_light/voxel_gi/corner_occlusion_tint")));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_shadow_fill_strength"), MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/shadow_fill_strength"))));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_shadow_fill_tint"), Color(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/shadow_fill_tint")));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_shadow_fill_reach"), CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/shadow_fill_reach")), 0.0f, 1.0f));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_color_saturation"), CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/color_saturation")), 0.0f, 2.0f));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_toon_enabled"), bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/toon_enabled")));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_toon_band_count"), CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/toon_band_count")), 0, 16));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_toon_band_softness"), CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/toon_band_softness")), 0.0f, 1.0f));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_toon_band_range"), MAX(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/toon_band_range")), 0.01f));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_toon_specular_enabled"), bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/toon_specular_enabled")));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_toon_specular_threshold"), CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/toon_specular_threshold")), 0.0f, 1.0f));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_toon_specular_softness"), CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/toon_specular_softness")), 0.0f, 1.0f));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_toon_specular_strength"), MAX(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/toon_specular_strength")), 0.0f));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_debug_mode"), backend == 2 ? int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/debug_mode")) : 0);
	if (backend == 0) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), false);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_ready"), false);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_restir_ready"), false);
		return;
	}
	if (backend == 2) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_restir_ready"), false);
		_render_ddgi_indirect_light(p_render_data, p_shadow_atlas, p_sampler, p_light_direction, p_light_color, p_light_energy);
		return;
	}
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_ready"), false);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), false);
	// Backend 1 is dispatched independently in the pre-opaque hook. It must not
	// depend on the directional shadow atlas being enabled or fully published.
}

void RenderVoxelForward::_ddgi_upload_resident_probe_records(uint32_t p_lod, uint32_t p_resolution, const Vector3i &p_previous_origin, bool p_had_history, bool p_force_full, bool p_refresh_published_pages, bool p_camera_shift, const VoxelForwardVolumeStorage::WorldOccupancy &p_world) {
	ERR_FAIL_INDEX(p_lod, DDGI_LOD_COUNT);
	const uint32_t total_probes = p_resolution * p_resolution * p_resolution;
	struct RecordUpdate {
		uint32_t slot = 0;
		DdgiGpuProbeRecord record = {};
		_FORCE_INLINE_ bool operator<(const RecordUpdate &p_other) const { return slot < p_other.slot; }
	};
	Vector<RecordUpdate> updates;
	updates.reserve(p_force_full ? total_probes : p_resolution * p_resolution * 3u);
	HashMap<Vector3i, uint64_t> published_page_masks;
	if (p_refresh_published_pages) {
		const Vector<Vector3i> &pages = p_world.ddgi_last_published_pages[p_lod];
		const Vector<uint64_t> &masks = p_world.ddgi_last_published_invalidation_masks[p_lod];
		for (int index = 0; index < pages.size(); index++) {
			published_page_masks.insert(pages[index], index < masks.size() ? masks[index] : UINT64_MAX);
		}
	}
	auto floor_divide = [](int p_value, int p_divisor) {
		int quotient = p_value / p_divisor;
		if (p_value < 0 && p_value % p_divisor != 0) {
			quotient--;
		}
		return quotient;
	};
	uint64_t cache_hits = 0;
	uint64_t empty_slots = 0;
	for (uint32_t index = 0; index < total_probes; index++) {
		const Vector3i local(
				int(index % p_resolution),
				int((index / p_resolution) % p_resolution),
				int(index / (p_resolution * p_resolution)));
		const Vector3i logical_cell = ddgi_state.ddgi_cascades[p_lod].logical_origin + local;
		const Vector3i previous_local = logical_cell - p_previous_origin;
		const bool retained_history = p_had_history && previous_local.x >= 0 && previous_local.y >= 0 && previous_local.z >= 0 &&
				previous_local.x < int(p_resolution) && previous_local.y < int(p_resolution) && previous_local.z < int(p_resolution);
		const Vector3i page_coordinate(floor_divide(logical_cell.x, VoxelForwardVolumeStorage::DDGI_PLACEMENT_PAGE_SIZE), floor_divide(logical_cell.y, VoxelForwardVolumeStorage::DDGI_PLACEMENT_PAGE_SIZE), floor_divide(logical_cell.z, VoxelForwardVolumeStorage::DDGI_PLACEMENT_PAGE_SIZE));
		const uint64_t *published_mask = published_page_masks.getptr(page_coordinate);
		const Vector3i page_local = logical_cell - page_coordinate * VoxelForwardVolumeStorage::DDGI_PLACEMENT_PAGE_SIZE;
		const uint32_t page_local_index = uint32_t(page_local.x + page_local.y * VoxelForwardVolumeStorage::DDGI_PLACEMENT_PAGE_SIZE + page_local.z * VoxelForwardVolumeStorage::DDGI_PLACEMENT_PAGE_SIZE * VoxelForwardVolumeStorage::DDGI_PLACEMENT_PAGE_SIZE);
		const bool probe_was_invalidated = p_refresh_published_pages && published_mask != nullptr && ((*published_mask & (uint64_t(1) << page_local_index)) != 0);
		if (!p_force_full && retained_history && !probe_was_invalidated) {
			continue;
		}
		auto positive_mod = [p_resolution](int p_value) {
			const int divisor = int(p_resolution);
			const int remainder = p_value % divisor;
			return remainder < 0 ? remainder + divisor : remainder;
		};
		const Vector3i physical(positive_mod(logical_cell.x), positive_mod(logical_cell.y), positive_mod(logical_cell.z));
		RecordUpdate update;
		update.slot = uint32_t(physical.x + physical.y * int(p_resolution) + physical.z * int(p_resolution * p_resolution));
		DdgiGpuProbeRecord &record = update.record;
		record.logical_cell_lod[0] = logical_cell.x;
		record.logical_cell_lod[1] = logical_cell.y;
		record.logical_cell_lod[2] = logical_cell.z;
		record.logical_cell_lod[3] = p_lod;
		record.physical_position_valid[0] = 0.0f;
		record.physical_position_valid[1] = 0.0f;
		record.physical_position_valid[2] = 0.0f;
		record.physical_position_valid[3] = 0.0f;
		record.state_revision_frame_flags[0] = 0u;
		record.state_revision_frame_flags[1] = uint32_t(p_world.revision);
		record.state_revision_frame_flags[2] = 0u;
		record.state_revision_frame_flags[3] = 0u;

		VoxelForwardVolumeStorage::DdgiProbePlacement placement;
		if (!volume_storage->get_ddgi_probe_placement(p_lod, logical_cell, placement)) {
			empty_slots++;
			updates.push_back(update);
			continue;
		}
		cache_hits++;
		record.physical_position_valid[0] = placement.position.x;
		record.physical_position_valid[1] = placement.position.y;
		record.physical_position_valid[2] = placement.position.z;
		record.physical_position_valid[3] = placement.valid ? 1.0f : 0.0f;
		record.state_revision_frame_flags[3] = placement.flags;
		if (!placement.valid) {
			updates.push_back(update);
			continue;
		}
		record.state_revision_frame_flags[0] = placement.initial_state;
		updates.push_back(update);
	}
	if (updates.is_empty()) {
		return;
	}
	updates.sort();
	Vector<uint32_t> changed_slots;
	changed_slots.resize(updates.size());
	uint32_t update_begin = 0;
	while (update_begin < uint32_t(updates.size())) {
		uint32_t update_end = update_begin + 1;
		while (update_end < uint32_t(updates.size()) && updates[update_end].slot == updates[update_end - 1].slot + 1u) {
			update_end++;
		}
		Vector<DdgiGpuProbeRecord> run;
		run.resize(update_end - update_begin);
		for (uint32_t run_index = update_begin; run_index < update_end; run_index++) {
			run.write[run_index - update_begin] = updates[run_index].record;
			changed_slots.write[run_index] = updates[run_index].slot;
		}
		RD::get_singleton()->buffer_update(ddgi_state.ddgi_cascades[p_lod].probe_upload_records, updates[update_begin].slot * DDGI_PROBE_RECORD_SIZE, run.size() * DDGI_PROBE_RECORD_SIZE, run.ptr());
		update_begin = update_end;
	}
	RD::get_singleton()->buffer_update(ddgi_state.ddgi_cascades[p_lod].probe_index_buffer, 0, changed_slots.size() * sizeof(uint32_t), changed_slots.ptr());

	// Merge staged placement into GPU-owned authoritative records. Matching
	// logical identities and positions keep their atlas history and become Dirty;
	// exposed, removed, or relocated probes take the uploaded New/Off state.
	const RID record_shader_rid = ddgi_record_scroll_shader.version_get_shader(ddgi_record_scroll_shader_version, 0);
	RD::Uniform records(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ ddgi_state.ddgi_cascades[p_lod].probe_records }));
	RD::Uniform uploads(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ ddgi_state.ddgi_cascades[p_lod].probe_upload_records }));
	RD::Uniform changed(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ ddgi_state.ddgi_cascades[p_lod].probe_index_buffer }));
	RD::Uniform metadata(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ ddgi_state.ddgi_cascades[p_lod].metadata_rd }));
	const RID record_set = UniformSetCacheRD::get_singleton()->get_cache(record_shader_rid, 0, records, uploads, changed, metadata);
	DdgiRecordScrollPushConstant push = {};
	push.grid_resolution_lod[0] = p_resolution;
	push.grid_resolution_lod[1] = updates.size();
	push.grid_resolution_lod[3] = p_lod;
	RD::ComputeListID list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(list, ddgi_record_scroll_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(list, record_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(list, &push, sizeof(push));
	RD::get_singleton()->compute_list_dispatch_threads(list, updates.size(), 1, 1);
	RD::get_singleton()->compute_list_end();
	Vector<DdgiGpuProbeRecord> seed_records;
	seed_records.resize(updates.size());
	for (int i = 0; i < updates.size(); i++) {
		seed_records.write[i] = updates[i].record;
	}
	_seed_baked_records(p_lod, changed_slots, seed_records);
	ddgi_state.ddgi_resident_cache_hits += cache_hits;
	ddgi_state.ddgi_resident_empty_slots += empty_slots;
	ddgi_state.ddgi_cascades[p_lod].activation_dirty = true;
	if (!p_force_full && p_camera_shift) {
		ddgi_state.ddgi_camera_upload_bytes += uint64_t(updates.size()) * (DDGI_PROBE_RECORD_SIZE + sizeof(uint32_t));
		ddgi_state.ddgi_new_plane_upload_count += MAX(1u, uint32_t(updates.size()) / MAX(1u, p_resolution * p_resolution));
	}
}

void RenderVoxelForward::_render_ddgi_indirect_light(const RenderDataRD *p_render_data, RID p_shadow_atlas, RID p_sampler, const Vector3 &p_light_direction, const Color &p_light_color, float p_light_energy) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	Color environment_color;
	float environment_energy = 1.0f;
	int32_t environment_mode = 0;
	RID environment_sky;
	RID environment_radiance = texture_storage->texture_rd_get_default(is_using_radiance_octmap_array() ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	Basis environment_sky_orientation;
	Quaternion inverse_sky_orientation;
	float sky_border_size = 0.0f;
	if (p_render_data != nullptr) {
		if (p_render_data->environment.is_valid()) {
			environment_energy = MAX(0.0f, environment_get_bg_energy_multiplier(p_render_data->environment));
			switch (environment_get_background(p_render_data->environment)) {
				case RSE::ENV_BG_CLEAR_COLOR: {
					environment_color = RSG::texture_storage->get_default_clear_color().srgb_to_linear();
					environment_mode = 1;
				} break;
				case RSE::ENV_BG_COLOR: {
					environment_color = environment_get_bg_color(p_render_data->environment);
					environment_mode = 1;
				} break;
				case RSE::ENV_BG_SKY: {
					environment_sky = environment_get_sky(p_render_data->environment);
					if (environment_sky.is_valid()) {
						const RID radiance = sky.sky_get_radiance_texture_rd(environment_sky);
						if (radiance.is_valid()) {
							environment_radiance = radiance;
							environment_sky_orientation = environment_get_sky_orientation(p_render_data->environment);
							inverse_sky_orientation = environment_sky_orientation.get_quaternion().inverse();
							sky_border_size = sky.sky_get_uv_border_size(environment_sky);
							environment_mode = 2;
						}
					}
				} break;
				default:
					break;
			}
		} else {
			environment_color = RSG::texture_storage->get_default_clear_color().srgb_to_linear();
			environment_mode = 1;
		}
	}
	const uint32_t requested_resolution = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/spatial_cascade_resolution")), 8, 32));
	const VoxelForwardVolumeStorage::WorldOccupancy &world = volume_storage->get_world_occupancy();
	RD *rd = RD::get_singleton();
	const bool resources_ready = rd != nullptr && p_render_data != nullptr && p_render_data->scene_data != nullptr && p_shadow_atlas.is_valid() && rd->texture_is_valid(p_shadow_atlas) && p_sampler.is_valid() &&
			ddgi_activate_pipeline.is_valid() && ddgi_record_scroll_pipeline.is_valid() && ddgi_trace_pipeline.is_valid() && ddgi_integrate_pipeline.is_valid() &&
			world.directory_buffer.is_valid() && world.brick_buffer.is_valid() && world.occupied_brick_count > 0 && world.voxel_size > 0.0f &&
			ddgi_state.ddgi_dynamic_volume_buffer.is_valid() && ddgi_state.ddgi_dynamic_bvh_buffer.is_valid() && ddgi_state.ddgi_dynamic_directory_buffer.is_valid() && ddgi_state.ddgi_dynamic_brick_buffer.is_valid();
	if (!resources_ready) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_ready"), false);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), false);
		return;
	}
	for (uint32_t index = 0; index < REFLECTION_CASCADE_COUNT; index++) {
		if (!ddgi_state.reflection_color_grid_rd[index].is_valid() || !ddgi_state.reflection_color_grid_initialized[index]) {
			material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_ready"), false);
			material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), false);
			return;
		}
	}

	const bool environment_changed = !ddgi_state.ddgi_environment_valid || ddgi_state.ddgi_environment_mode != environment_mode || ddgi_state.ddgi_sky != environment_sky ||
			!ddgi_state.ddgi_environment_color.is_equal_approx(environment_color) || !Math::is_equal_approx(ddgi_state.ddgi_environment_energy, environment_energy) ||
			!ddgi_state.ddgi_sky_orientation.is_equal_approx(environment_sky_orientation);
	const bool lighting_changed = !ddgi_state.ddgi_light_valid || ddgi_state.ddgi_light_direction.dot(p_light_direction) < 0.9999f ||
			!ddgi_state.ddgi_light_color.is_equal_approx(p_light_color) || !Math::is_equal_approx(ddgi_state.ddgi_light_energy, p_light_energy) || environment_changed;
	if (lighting_changed) {
		ddgi_state.ddgi_lighting_revision++;
		ddgi_state.ddgi_light_direction = p_light_direction;
		ddgi_state.ddgi_light_color = p_light_color;
		ddgi_state.ddgi_light_energy = p_light_energy;
		ddgi_state.ddgi_light_valid = true;
		ddgi_state.ddgi_sky = environment_sky;
		ddgi_state.ddgi_sky_orientation = environment_sky_orientation;
		ddgi_state.ddgi_environment_color = environment_color;
		ddgi_state.ddgi_environment_energy = environment_energy;
		ddgi_state.ddgi_environment_mode = environment_mode;
		ddgi_state.ddgi_environment_valid = true;
		for (DdgiCascade &cascade : ddgi_state.ddgi_cascades) {
			cascade.activation_dirty = true;
		}
	}
	ddgi_state.ddgi_frame_index++;
	const uint32_t ddgi_frame_resource = uint32_t(ddgi_state.ddgi_frame_index % DDGI_FRAME_RESOURCE_COUNT);
	const uint32_t resolution = requested_resolution;
	const uint32_t ray_count = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/rays_per_probe")), 8, 64));
	const uint32_t convergence_ray_count = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/convergence_rays_per_probe")), 8, 64));
	const uint32_t convergence_updates = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/convergence_updates")), 5, 32));
	const float dirty_target_residual = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/dirty_target_residual")), 0.001f, 0.5f);
	const uint32_t ray_stride = MAX(ray_count, convergence_ray_count);
	// Mature probes are immutable until a scene or lighting revision makes them
	// dirty again. Optional maintenance is useful for deliberately untracked
	// external inputs, but zero is the stable and cheapest production default.
	const uint32_t probes_per_frame = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/probes_per_frame")), 0, 512));
	const uint32_t urgent_probes_per_frame = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/urgent_probes_per_frame")), 1, 512));
	const uint32_t camera_probes_per_frame = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/camera_probes_per_frame")), 1, 512));
	const uint32_t lighting_probes_per_frame = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/lighting_probes_per_frame")), 1, 512));
	const float irradiance_hysteresis = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/irradiance_hysteresis")), 0.0f, 0.999f);
	const float visibility_hysteresis = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/visibility_hysteresis")), 0.0f, 0.999f);
	// The user specifies how much stale history may remain after the requested
	// number of valid updates. Deriving hysteresis makes that contract exact and
	// avoids the former 0.85^5 = 44% residual which made shadows arrive slowly.
	const float dirty_hysteresis = Math::pow(dirty_target_residual, 1.0f / float(convergence_updates));
	const float dirty_irradiance_hysteresis = dirty_hysteresis;
	const float dirty_visibility_hysteresis = dirty_hysteresis;
	const float bounce_feedback = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/bounce_feedback")), 0.0f, 0.95f);
	const uint32_t max_trace_steps = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/max_trace_steps")), 32, 4096));
	const uint32_t irradiance_atlas_width = resolution * resolution * DDGI_IRRADIANCE_TILE_SIZE;
	const uint32_t irradiance_atlas_height = resolution * DDGI_IRRADIANCE_TILE_SIZE;
	const uint32_t visibility_atlas_width = resolution * resolution * DDGI_VISIBILITY_TILE_SIZE;
	const uint32_t visibility_atlas_height = resolution * DDGI_VISIBILITY_TILE_SIZE;
	const uint32_t total_probes = resolution * resolution * resolution;
	const uint32_t maximum_scheduled_probes = MIN(total_probes, probes_per_frame + camera_probes_per_frame + MAX(urgent_probes_per_frame, lighting_probes_per_frame));

	bool resources_reallocated = false;
	if (ddgi_state.ddgi_probe_resolution != resolution) {
		_free_ddgi();
		RD::TextureFormat irradiance_format;
		irradiance_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		irradiance_format.width = irradiance_atlas_width;
		irradiance_format.height = irradiance_atlas_height;
		irradiance_format.texture_type = RD::TEXTURE_TYPE_2D;
		irradiance_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		RD::TextureFormat depth_format = irradiance_format;
		// Mean depth, second moment, conservative upper depth, and directional
		// confidence. Half precision is sufficient for the configured trace range.
		depth_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		depth_format.width = visibility_atlas_width;
		depth_format.height = visibility_atlas_height;
		RD::TextureFormat metadata_format = irradiance_format;
		metadata_format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		metadata_format.width = resolution * resolution;
		metadata_format.height = resolution;
		for (uint32_t lod = 0; lod < DDGI_LOD_COUNT; lod++) {
			ddgi_state.ddgi_cascades[lod].irradiance_rd = RD::get_singleton()->texture_create(irradiance_format, RD::TextureView());
			ddgi_state.ddgi_cascades[lod].visibility_rd = RD::get_singleton()->texture_create(depth_format, RD::TextureView());
			ddgi_state.ddgi_cascades[lod].metadata_rd = RD::get_singleton()->texture_create(metadata_format, RD::TextureView());
			RD::get_singleton()->texture_clear(ddgi_state.ddgi_cascades[lod].irradiance_rd, Color(0, 0, 0, 0), 0, 1, 0, 1);
			RD::get_singleton()->texture_clear(ddgi_state.ddgi_cascades[lod].visibility_rd, Color(0, 0, 0, 0), 0, 1, 0, 1);
			RD::get_singleton()->texture_clear(ddgi_state.ddgi_cascades[lod].metadata_rd, Color(0, 0, 0, 0), 0, 1, 0, 1);
			ddgi_state.ddgi_cascades[lod].irradiance_texture = texture_storage->texture_allocate();
			texture_storage->texture_rd_initialize(ddgi_state.ddgi_cascades[lod].irradiance_texture, ddgi_state.ddgi_cascades[lod].irradiance_rd);
			ddgi_state.ddgi_cascades[lod].visibility_texture = texture_storage->texture_allocate();
			texture_storage->texture_rd_initialize(ddgi_state.ddgi_cascades[lod].visibility_texture, ddgi_state.ddgi_cascades[lod].visibility_rd);
			ddgi_state.ddgi_cascades[lod].metadata_texture = texture_storage->texture_allocate();
			texture_storage->texture_rd_initialize(ddgi_state.ddgi_cascades[lod].metadata_texture, ddgi_state.ddgi_cascades[lod].metadata_rd);
			ddgi_state.ddgi_cascades[lod].probe_records = RD::get_singleton()->storage_buffer_create(total_probes * DDGI_PROBE_RECORD_SIZE);
			ddgi_state.ddgi_cascades[lod].probe_upload_records = RD::get_singleton()->storage_buffer_create(total_probes * DDGI_PROBE_RECORD_SIZE);
			RD::get_singleton()->buffer_clear(ddgi_state.ddgi_cascades[lod].probe_records, 0, total_probes * DDGI_PROBE_RECORD_SIZE);
			RD::get_singleton()->buffer_clear(ddgi_state.ddgi_cascades[lod].probe_upload_records, 0, total_probes * DDGI_PROBE_RECORD_SIZE);
		}
		ddgi_state.ddgi_probe_resolution = resolution;
		ddgi_state.ddgi_irradiance_atlas_width = irradiance_atlas_width;
		ddgi_state.ddgi_irradiance_atlas_height = irradiance_atlas_height;
		ddgi_state.ddgi_visibility_atlas_width = visibility_atlas_width;
		ddgi_state.ddgi_visibility_atlas_height = visibility_atlas_height;
		resources_reallocated = true;
		print_verbose(vformat("Voxel Forward DDGI allocated four circular LOD windows with %dx%dx%d probes each.", resolution, resolution, resolution));
	}

	const uint32_t required_surfel_count = maximum_scheduled_probes * ray_stride;
	// Each lifecycle category owns a fixed GPU segment. Classification can then
	// append all categories in one pass without ordering-dependent prefix passes.
	for (uint32_t cascade = 0; cascade < DDGI_LOD_COUNT; cascade++) {
		const uint32_t required_index_capacity = total_probes * 3u;
		if (!ddgi_state.ddgi_cascades[cascade].probe_index_buffer.is_valid() || ddgi_state.ddgi_cascades[cascade].probe_index_capacity < required_index_capacity) {
			if (ddgi_state.ddgi_cascades[cascade].probe_index_buffer.is_valid()) {
				RD::get_singleton()->free_rid(ddgi_state.ddgi_cascades[cascade].probe_index_buffer);
			}
			ddgi_state.ddgi_cascades[cascade].probe_index_buffer = RD::get_singleton()->storage_buffer_create(required_index_capacity * sizeof(uint32_t));
			ddgi_state.ddgi_cascades[cascade].probe_index_capacity = required_index_capacity;
		}
		if (!ddgi_state.ddgi_cascades[cascade].probe_counter_buffer.is_valid()) {
			ddgi_state.ddgi_cascades[cascade].probe_counter_buffer = RD::get_singleton()->storage_buffer_create(6 * sizeof(uint32_t));
		}
		if (!ddgi_state.ddgi_cascades[cascade].probe_dispatch_buffer.is_valid()) {
			ddgi_state.ddgi_cascades[cascade].probe_dispatch_buffer = RD::get_singleton()->storage_buffer_create(8 * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		}
	}
	if (!ddgi_state.ddgi_surfel_buffer.is_valid() || ddgi_state.ddgi_surfel_capacity < required_surfel_count) {
		if (ddgi_state.ddgi_surfel_buffer.is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.ddgi_surfel_buffer);
		}
		ddgi_state.ddgi_surfel_buffer = RD::get_singleton()->storage_buffer_create(required_surfel_count * 48u);
		ddgi_state.ddgi_surfel_capacity = required_surfel_count;
	}
	for (RID &buffer : ddgi_state.ddgi_trace_uniform_buffer) {
		if (!buffer.is_valid()) {
			buffer = RD::get_singleton()->uniform_buffer_create(sizeof(DdgiTraceUniformData));
		}
	}
	// Resolution changes free all DDGI-owned buffers above. Build the bounded
	// local-light buffer only after that lifecycle is complete so binding 26 can
	// never reference a RID invalidated during the same frame.
	_update_ddgi_local_lights(p_render_data);
	_invalidate_changed_bake();
	if (!ddgi_state.ddgi_local_light_buffer.is_valid()) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_ready"), false);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_resolve_ready"), false);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), false);
		return;
	}

	// Favor the near cascade without increasing total ray work. LOD 0 receives
	// half of the update frames; progressively coarser coverage shares the rest.
	static constexpr uint32_t lod_schedule[] = { 0, 1, 0, 2, 0, 3 };
	const uint32_t lod = lod_schedule[ddgi_state.ddgi_schedule_cursor++ % (sizeof(lod_schedule) / sizeof(lod_schedule[0]))];
	Vector<float> &aabb_upload = ddgi_state.ddgi_dynamic_aabb_upload;
	aabb_upload.clear();
	if (p_render_data->instances != nullptr) {
		const uint32_t instance_limit = MIN(uint32_t(p_render_data->instances->size()), 256u);
		for (uint32_t index = 0; index < instance_limit; index++) {
			RenderGeometryInstance *instance = (*p_render_data->instances)[index];
			if (volume_storage->get_volume(instance->get_base()) != nullptr) {
				continue;
			}
			const AABB bounds = instance->get_transform().xform(instance->get_aabb());
			const Vector3 end = bounds.position + bounds.size;
			aabb_upload.push_back(bounds.position.x);
			aabb_upload.push_back(bounds.position.y);
			aabb_upload.push_back(bounds.position.z);
			aabb_upload.push_back(1.0f);
			aabb_upload.push_back(end.x);
			aabb_upload.push_back(end.y);
			aabb_upload.push_back(end.z);
			aabb_upload.push_back(1.0f);
		}
	}
	for (const AABB &bounds : ddgi_state.ddgi_dynamic_current_bounds) {
		const Vector3 end = bounds.position + bounds.size;
		aabb_upload.push_back(bounds.position.x);
		aabb_upload.push_back(bounds.position.y);
		aabb_upload.push_back(bounds.position.z);
		aabb_upload.push_back(1.0f);
		aabb_upload.push_back(end.x);
		aabb_upload.push_back(end.y);
		aabb_upload.push_back(end.z);
		aabb_upload.push_back(1.0f);
	}
	for (const AABB &bounds : ddgi_state.ddgi_dynamic_dirty_bounds[lod]) {
		const Vector3 end = bounds.position + bounds.size;
		aabb_upload.push_back(bounds.position.x);
		aabb_upload.push_back(bounds.position.y);
		aabb_upload.push_back(bounds.position.z);
		aabb_upload.push_back(2.0f);
		aabb_upload.push_back(end.x);
		aabb_upload.push_back(end.y);
		aabb_upload.push_back(end.z);
		aabb_upload.push_back(2.0f);
	}
	const uint32_t aabb_count = aabb_upload.size() / 8;

	const Vector3 camera_position = p_render_data->scene_data->cam_transform.origin;
	bool residency_dirty[DDGI_LOD_COUNT] = { resources_reallocated, resources_reallocated, resources_reallocated, resources_reallocated };
	bool camera_shift[DDGI_LOD_COUNT] = {};
	Vector3i previous_origin[DDGI_LOD_COUNT];
	bool had_history[DDGI_LOD_COUNT] = {};
	for (uint32_t lod = 0; lod < DDGI_LOD_COUNT; lod++) {
		previous_origin[lod] = ddgi_state.ddgi_cascades[lod].logical_origin;
		had_history[lod] = ddgi_state.ddgi_cascades[lod].initialized;
		const int spacing = int(world.ddgi_cell_spacing_voxels[lod]);
		const Vector3i cell_voxels(spacing, spacing, spacing);
		const Vector3 cell_world = Vector3(cell_voxels) * world.voxel_size;
		const bool layout_changed = !ddgi_state.ddgi_cascades[lod].cell_size.is_equal_approx(cell_world) ||
				!ddgi_state.ddgi_cascades[lod].origin.is_equal_approx(world.origin + Vector3(previous_origin[lod]) * cell_world);
		if (layout_changed) {
			residency_dirty[lod] = true;
			ddgi_state.ddgi_temporal_history_valid = false;
		}
		const Vector3 camera_voxel = (camera_position - world.origin) / world.voxel_size;
		const Vector3 camera_cell_position = camera_voxel / Vector3(cell_voxels);
		Vector3i snapped_center_cell;
		// Make LOD ownership a pure function of camera position. A hysteretic
		// center has different origins on the outward and return paths, which makes
		// identical viewpoints select different cascades and visibly pulse.
		for (int axis = 0; axis < 3; axis++) {
			snapped_center_cell[axis] = int(Math::floor(camera_cell_position[axis] + 0.5f));
		}
		const Vector3i desired_origin_cell = snapped_center_cell - Vector3i(resolution / 2, resolution / 2, resolution / 2);
		const Vector3 desired_origin = world.origin + Vector3(desired_origin_cell * cell_voxels) * world.voxel_size;
		camera_shift[lod] = ddgi_state.ddgi_cascades[lod].initialized && ddgi_state.ddgi_cascades[lod].logical_origin != desired_origin_cell;
		residency_dirty[lod] = residency_dirty[lod] || ddgi_state.ddgi_cascades[lod].logical_origin != desired_origin_cell;
		if (layout_changed || (ddgi_state.ddgi_cascades[lod].initialized && ddgi_state.ddgi_cascades[lod].logical_origin != desired_origin_cell)) {
			const Vector3i shift = desired_origin_cell - ddgi_state.ddgi_cascades[lod].logical_origin;
			if (layout_changed || Math::abs(shift.x) >= int(resolution) || Math::abs(shift.y) >= int(resolution) || Math::abs(shift.z) >= int(resolution)) {
				RD::get_singleton()->texture_clear(ddgi_state.ddgi_cascades[lod].irradiance_rd, Color(0, 0, 0, 0), 0, 1, 0, 1);
				RD::get_singleton()->texture_clear(ddgi_state.ddgi_cascades[lod].visibility_rd, Color(0, 0, 0, 0), 0, 1, 0, 1);
				RD::get_singleton()->texture_clear(ddgi_state.ddgi_cascades[lod].metadata_rd, Color(0, 0, 0, 0), 0, 1, 0, 1);
				RD::get_singleton()->buffer_clear(ddgi_state.ddgi_cascades[lod].probe_records, 0, total_probes * DDGI_PROBE_RECORD_SIZE);
				ddgi_state.ddgi_cascades[lod].initialized = false;
				had_history[lod] = false;
				ddgi_state.ddgi_cascades[lod].active_update_cursor = 0;
				ddgi_state.ddgi_cascades[lod].urgent_update_cursor = 0;
			}
		}
		ddgi_state.ddgi_cascades[lod].logical_origin = desired_origin_cell;
		ddgi_state.ddgi_cascades[lod].phase_offset = Vector3i(
				((desired_origin_cell.x % int(resolution)) + int(resolution)) % int(resolution),
				((desired_origin_cell.y % int(resolution)) + int(resolution)) % int(resolution),
				((desired_origin_cell.z % int(resolution)) + int(resolution)) % int(resolution));
		ddgi_state.ddgi_cascades[lod].origin = desired_origin;
		ddgi_state.ddgi_cascades[lod].cell_size = cell_world;
	}

	const bool placement_changed = ddgi_state.ddgi_placement_revision != world.ddgi_probe_cache_revision;
	const bool page_publication_changed = ddgi_state.ddgi_page_publication_serial != world.ddgi_page_publication_serial;
	const bool incremental_edit_started = ddgi_state.ddgi_world_revision != UINT64_MAX && ddgi_state.ddgi_world_revision != world.revision && world.last_incremental_revision == world.revision;
	if (incremental_edit_started) {
		ddgi_state.ddgi_incremental_edit_pending = true;
	}
	const bool incremental_edit_published = ddgi_state.ddgi_incremental_edit_pending && page_publication_changed;
	if (incremental_edit_published && world.ddgi_pending_page_count == 0) {
		ddgi_state.ddgi_incremental_edit_pending = false;
	}
	// A world revision is not a DDGI-wide invalidation. Placement publication
	// already identifies the affected sparse pages; unrelated probes retain
	// their authoritative records and atlas history.
	for (uint32_t lod_index = 0; lod_index < DDGI_LOD_COUNT; lod_index++) {
		const BakedWorld *baked = baked_worlds.getptr(lighting_scenario);
		const bool initial_seed = baked && !baked->invalidated && !(baked->initial_lods & (1u << lod_index)) && world.ddgi_pending_page_count == 0;
		if (residency_dirty[lod_index] || placement_changed || page_publication_changed || initial_seed) {
			const bool force_full = resources_reallocated || !had_history[lod_index] || initial_seed;
			_ddgi_upload_resident_probe_records(lod_index, resolution, previous_origin[lod_index], had_history[lod_index], force_full, page_publication_changed, camera_shift[lod_index], world);
		}
	}
	ddgi_state.ddgi_world_revision = world.revision;
	ddgi_state.ddgi_placement_revision = world.ddgi_probe_cache_revision;
	ddgi_state.ddgi_page_publication_serial = world.ddgi_page_publication_serial;

	// Hash the compact source AABB stream and the scheduled circular-grid origin.
	// Ordinary frames only inspect this small stream; the O(probes + overlapped
	// cells) CPU binning work runs solely when geometry, edits, or residency move.
	uint64_t dynamic_source_hash = 1469598103934665603ull;
	const uint8_t *aabb_bytes = reinterpret_cast<const uint8_t *>(aabb_upload.ptr());
	for (uint32_t byte = 0; byte < uint32_t(aabb_upload.size() * sizeof(float)); byte++) {
		dynamic_source_hash ^= uint64_t(aabb_bytes[byte]);
		dynamic_source_hash *= 1099511628211ull;
	}
	const Vector3i source_grid_origin = ddgi_state.ddgi_cascades[lod].logical_origin;
	const int32_t source_identity[4] = { source_grid_origin.x, source_grid_origin.y, source_grid_origin.z, int32_t(resolution) };
	const uint8_t *identity_bytes = reinterpret_cast<const uint8_t *>(source_identity);
	for (uint32_t byte = 0; byte < sizeof(source_identity); byte++) {
		dynamic_source_hash ^= uint64_t(identity_bytes[byte]);
		dynamic_source_hash *= 1099511628211ull;
	}
	bool dynamic_buffer_reallocated = false;
	if (!ddgi_state.ddgi_cascades[lod].dynamic_bin_buffer.is_valid() || ddgi_state.ddgi_cascades[lod].dynamic_bin_capacity < total_probes) {
		if (ddgi_state.ddgi_cascades[lod].dynamic_bin_buffer.is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.ddgi_cascades[lod].dynamic_bin_buffer);
		}
		ddgi_state.ddgi_cascades[lod].dynamic_bin_buffer = RD::get_singleton()->storage_buffer_create(total_probes * sizeof(uint32_t));
		ddgi_state.ddgi_cascades[lod].dynamic_bin_capacity = total_probes;
		dynamic_buffer_reallocated = true;
	}
	const bool dynamic_bins_changed = dynamic_buffer_reallocated || ddgi_state.ddgi_cascades[lod].dynamic_source_hash != dynamic_source_hash;
	if (dynamic_bins_changed) {
		Vector<uint32_t> dynamic_probe_mask;
		dynamic_probe_mask.resize(total_probes);
		dynamic_probe_mask.fill(0u);
		for (uint32_t aabb = 0; aabb < aabb_count; aabb++) {
			const uint32_t mask_bits = uint32_t(aabb_upload[aabb * 8u + 3u]);
			const Vector3 begin(aabb_upload[aabb * 8u], aabb_upload[aabb * 8u + 1u], aabb_upload[aabb * 8u + 2u]);
			const Vector3 end(aabb_upload[aabb * 8u + 4u], aabb_upload[aabb * 8u + 5u], aabb_upload[aabb * 8u + 6u]);
			const Vector3 expanded_begin = begin - ddgi_state.ddgi_cascades[lod].cell_size;
			const Vector3 expanded_end = end + ddgi_state.ddgi_cascades[lod].cell_size;
			Vector3i first = Vector3i(Math::floor((expanded_begin.x - world.origin.x) / ddgi_state.ddgi_cascades[lod].cell_size.x), Math::floor((expanded_begin.y - world.origin.y) / ddgi_state.ddgi_cascades[lod].cell_size.y), Math::floor((expanded_begin.z - world.origin.z) / ddgi_state.ddgi_cascades[lod].cell_size.z));
			Vector3i last = Vector3i(Math::floor((expanded_end.x - world.origin.x) / ddgi_state.ddgi_cascades[lod].cell_size.x), Math::floor((expanded_end.y - world.origin.y) / ddgi_state.ddgi_cascades[lod].cell_size.y), Math::floor((expanded_end.z - world.origin.z) / ddgi_state.ddgi_cascades[lod].cell_size.z));
			first = first.clamp(ddgi_state.ddgi_cascades[lod].logical_origin, ddgi_state.ddgi_cascades[lod].logical_origin + Vector3i(resolution - 1, resolution - 1, resolution - 1));
			last = last.clamp(ddgi_state.ddgi_cascades[lod].logical_origin, ddgi_state.ddgi_cascades[lod].logical_origin + Vector3i(resolution - 1, resolution - 1, resolution - 1));
			for (int z = first.z; z <= last.z; z++) {
				for (int y = first.y; y <= last.y; y++) {
					for (int x = first.x; x <= last.x; x++) {
						const int px = ((x % int(resolution)) + int(resolution)) % int(resolution);
						const int py = ((y % int(resolution)) + int(resolution)) % int(resolution);
						const int pz = ((z % int(resolution)) + int(resolution)) % int(resolution);
						const uint32_t physical_index = px + py * resolution + pz * resolution * resolution;
						dynamic_probe_mask.write[physical_index] |= mask_bits;
					}
				}
			}
		}
		uint64_t dynamic_hash = 1469598103934665603ull;
		for (uint32_t index = 0; index < total_probes; index++) {
			if (dynamic_probe_mask[index] != 0u) {
				dynamic_hash ^= uint64_t(index + 1u) | (uint64_t(dynamic_probe_mask[index]) << 32u);
				dynamic_hash *= 1099511628211ull;
			}
		}
		RD::get_singleton()->buffer_update(ddgi_state.ddgi_cascades[lod].dynamic_bin_buffer, 0, total_probes * sizeof(uint32_t), dynamic_probe_mask.ptr());
		ddgi_state.ddgi_cascades[lod].dynamic_bin_hash = dynamic_hash;
		ddgi_state.ddgi_cascades[lod].dynamic_source_hash = dynamic_source_hash;
	}
	const bool selected_lighting_changed = ddgi_state.ddgi_cascades[lod].lighting_revision != ddgi_state.ddgi_lighting_revision;
	const uint32_t dirty_probe_budget = selected_lighting_changed ? lighting_probes_per_frame : urgent_probes_per_frame;
	if (ddgi_state.ddgi_cascades[lod].activation_dirty || dynamic_bins_changed) {
		RENDER_TIMESTAMP("DDGI Probe Classification");
		RD::get_singleton()->buffer_clear(ddgi_state.ddgi_cascades[lod].probe_counter_buffer, 0, 6 * sizeof(uint32_t));
		DdgiClassifyUniformData classify = {};
		classify.world_origin_voxel_size[0] = world.origin.x;
		classify.world_origin_voxel_size[1] = world.origin.y;
		classify.world_origin_voxel_size[2] = world.origin.z;
		classify.world_origin_voxel_size[3] = world.voxel_size;
		classify.grid_origin_lod[0] = ddgi_state.ddgi_cascades[lod].logical_origin.x;
		classify.grid_origin_lod[1] = ddgi_state.ddgi_cascades[lod].logical_origin.y;
		classify.grid_origin_lod[2] = ddgi_state.ddgi_cascades[lod].logical_origin.z;
		classify.grid_origin_lod[3] = lod;
		classify.grid_resolution_phase[0] = resolution;
		classify.cell_size_voxels[0] = world.ddgi_cell_spacing_voxels[lod];
		classify.cell_size_voxels[1] = world.ddgi_cell_spacing_voxels[lod];
		classify.cell_size_voxels[2] = world.ddgi_cell_spacing_voxels[lod];
		classify.directory_revision[0] = world.directory_mask;
		classify.directory_revision[1] = world.max_probe_count;
		classify.directory_revision[2] = uint32_t(world.revision);
		classify.directory_revision[3] = ray_stride;
		classify.schedule[0] = probes_per_frame;
		classify.schedule[1] = total_probes;
		classify.schedule[2] = dirty_probe_budget;
		classify.schedule[3] = camera_probes_per_frame;
		classify.changes[0] = selected_lighting_changed ? 1u : 0u;
		classify.changes[1] = 0u;
		classify.changes[2] = ddgi_state.ddgi_dynamic_dirty_bounds[lod].is_empty() ? 0u : 1u;

		const RID activate_shader_rid = ddgi_activate_shader.version_get_shader(ddgi_activate_shader_version, 0);
		RID &activate_set = ddgi_state.ddgi_cascades[lod].classify_uniform_set;
		if (!activate_set.is_valid() || !RD::get_singleton()->uniform_set_is_valid(activate_set)) {
			RD::Uniform a_records(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ ddgi_state.ddgi_cascades[lod].probe_records }));
			RD::Uniform a_indices(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ ddgi_state.ddgi_cascades[lod].probe_index_buffer }));
			RD::Uniform a_counters(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ ddgi_state.ddgi_cascades[lod].probe_counter_buffer }));
			RD::Uniform a_aabbs(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ ddgi_state.ddgi_cascades[lod].dynamic_bin_buffer }));
			RD::Uniform a_metadata(RD::UNIFORM_TYPE_IMAGE, 4, Vector<RID>({ ddgi_state.ddgi_cascades[lod].metadata_rd }));
			RD::Uniform a_dispatch(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, Vector<RID>({ ddgi_state.ddgi_cascades[lod].probe_dispatch_buffer }));
			activate_set = UniformSetCacheRD::get_singleton()->get_cache(activate_shader_rid, 0, a_records, a_indices, a_counters, a_aabbs, a_metadata, a_dispatch);
		}
		RD::get_singleton()->draw_command_begin_label("DDGI GPU Worklist Build");
		RD::ComputeListID activate_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(activate_list, ddgi_activate_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(activate_list, activate_set, 0);
		classify.grid_resolution_phase[3] = 0;
		RD::get_singleton()->compute_list_set_push_constant(activate_list, &classify, sizeof(classify));
		RD::get_singleton()->compute_list_dispatch_threads(activate_list, total_probes, 1, 1);
		RD::get_singleton()->compute_list_add_barrier(activate_list);
		classify.grid_resolution_phase[3] = 1;
		RD::get_singleton()->compute_list_set_push_constant(activate_list, &classify, sizeof(classify));
		RD::get_singleton()->compute_list_dispatch_threads(activate_list, 1, 1, 1);
		RD::get_singleton()->compute_list_end();
		RD::get_singleton()->draw_command_end_label();
		ddgi_state.ddgi_cascades[lod].activation_dirty = false;
		ddgi_state.ddgi_cascades[lod].lighting_revision = ddgi_state.ddgi_lighting_revision;
		ddgi_state.ddgi_dynamic_dirty_bounds[lod].clear();
	}

	const uint32_t color_lod = MIN(lod, REFLECTION_CASCADE_COUNT - 1);
	// Each lifecycle owns an explicit bounded budget. This prevents camera-exposed
	// probes from consuming steady maintenance and lets lighting changes converge
	// quickly without raising ordinary-frame cost.
	DdgiTraceUniformData trace = {};
	trace.world_origin_voxel_size[0] = world.origin.x;
	trace.world_origin_voxel_size[1] = world.origin.y;
	trace.world_origin_voxel_size[2] = world.origin.z;
	trace.world_origin_voxel_size[3] = world.voxel_size;
	trace.grid_origin_max_distance[0] = ddgi_state.ddgi_cascades[lod].origin.x;
	trace.grid_origin_max_distance[1] = ddgi_state.ddgi_cascades[lod].origin.y;
	trace.grid_origin_max_distance[2] = ddgi_state.ddgi_cascades[lod].origin.z;
	trace.grid_origin_max_distance[3] = float(max_trace_steps) * world.voxel_size;
	trace.cell_size_lod[0] = ddgi_state.ddgi_cascades[lod].cell_size.x;
	trace.cell_size_lod[1] = ddgi_state.ddgi_cascades[lod].cell_size.y;
	trace.cell_size_lod[2] = ddgi_state.ddgi_cascades[lod].cell_size.z;
	trace.cell_size_lod[3] = lod;
	trace.color_origin_cell_size[0] = ddgi_state.reflection_color_grid_origin[color_lod].x;
	trace.color_origin_cell_size[1] = ddgi_state.reflection_color_grid_origin[color_lod].y;
	trace.color_origin_cell_size[2] = ddgi_state.reflection_color_grid_origin[color_lod].z;
	trace.color_origin_cell_size[3] = ddgi_state.reflection_color_grid_cell_size[color_lod];
	trace.light_direction_energy[0] = p_light_direction.x;
	trace.light_direction_energy[1] = p_light_direction.y;
	trace.light_direction_energy[2] = p_light_direction.z;
	trace.light_direction_energy[3] = p_light_energy;
	trace.light_color_bounce[0] = p_light_color.r;
	trace.light_color_bounce[1] = p_light_color.g;
	trace.light_color_bounce[2] = p_light_color.b;
	trace.light_color_bounce[3] = bounce_feedback;
	trace.sky_color_energy[0] = environment_color.r;
	trace.sky_color_energy[1] = environment_color.g;
	trace.sky_color_energy[2] = environment_color.b;
	trace.sky_color_energy[3] = environment_energy;
	trace.sky_orientation[0] = inverse_sky_orientation.x;
	trace.sky_orientation[1] = inverse_sky_orientation.y;
	trace.sky_orientation[2] = inverse_sky_orientation.z;
	trace.sky_orientation[3] = inverse_sky_orientation.w;
	trace.sky_border_mode[0] = sky_border_size;
	trace.sky_border_mode[1] = 1.0f - sky_border_size * 2.0f;
	trace.sky_border_mode[2] = float(environment_mode);
	trace.atlas_center_resolution[0] = ddgi_state.shadow_atlas_center.x;
	trace.atlas_center_resolution[1] = ddgi_state.shadow_atlas_center.y;
	trace.atlas_center_resolution[2] = ddgi_state.shadow_atlas_center.z;
	trace.atlas_center_resolution[3] = ddgi_state.shadow_atlas_resolution;
	trace.tangent_near_extent[0] = ddgi_state.shadow_atlas_tangent.x;
	trace.tangent_near_extent[1] = ddgi_state.shadow_atlas_tangent.y;
	trace.tangent_near_extent[2] = ddgi_state.shadow_atlas_tangent.z;
	trace.tangent_near_extent[3] = ddgi_state.shadow_atlas_near_extent;
	trace.bitangent_far_extent[0] = ddgi_state.shadow_atlas_bitangent.x;
	trace.bitangent_far_extent[1] = ddgi_state.shadow_atlas_bitangent.y;
	trace.bitangent_far_extent[2] = ddgi_state.shadow_atlas_bitangent.z;
	trace.bitangent_far_extent[3] = ddgi_state.shadow_atlas_far_extent;
	trace.probe_grid[0] = resolution;
	trace.probe_grid[1] = ray_stride;
	trace.probe_grid[2] = 0;
	trace.probe_grid[3] = 0;
	trace.logical_origin[0] = ddgi_state.ddgi_cascades[lod].logical_origin.x;
	trace.logical_origin[1] = ddgi_state.ddgi_cascades[lod].logical_origin.y;
	trace.logical_origin[2] = ddgi_state.ddgi_cascades[lod].logical_origin.z;
	trace.logical_origin[3] = lod;
	trace.directory_trace[0] = world.directory_mask;
	trace.directory_trace[1] = world.max_probe_count;
	trace.directory_trace[2] = max_trace_steps;
	trace.directory_trace[3] = ddgi_state.reflection_color_grid_resolution;
	trace.atlas_layout[0] = DDGI_IRRADIANCE_TILE_SIZE;
	trace.atlas_layout[1] = irradiance_atlas_width;
	trace.atlas_layout[2] = irradiance_atlas_height;
	trace.atlas_layout[3] = DDGI_IRRADIANCE_INTERIOR_SIZE;
	trace.visibility_layout[0] = DDGI_VISIBILITY_TILE_SIZE;
	trace.visibility_layout[1] = visibility_atlas_width;
	trace.visibility_layout[2] = visibility_atlas_height;
	trace.visibility_layout[3] = DDGI_VISIBILITY_INTERIOR_SIZE;
	trace.update_state[0] = ray_count;
	trace.update_state[1] = convergence_ray_count;
	trace.update_state[2] = ddgi_state.ddgi_cascades[lod].active_update_cursor;
	trace.update_state[3] = ddgi_state.ddgi_cascades[lod].urgent_update_cursor;
	trace.scheduling[0] = probes_per_frame;
	trace.scheduling[1] = dirty_probe_budget;
	trace.scheduling[2] = camera_probes_per_frame;
	trace.query_settings[0] = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/self_shadow_bias")), 0.0f, 2.0f);
	trace.query_settings[1] = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/view_bias")), 0.0f, 1.0f);
	trace.query_settings[2] = ddgi_state.shadow_atlas_initialized && ddgi_state.shadow_atlas_source_rd.is_valid() && rd->texture_is_valid(ddgi_state.shadow_atlas_source_rd) && p_shadow_atlas == ddgi_state.shadow_atlas_source_rd ? 1.0f : 0.0f;
	trace.query_settings[3] = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/lod_transition")), 0.01f, 0.5f);
	trace.camera_position_lod_transition[0] = camera_position.x;
	trace.camera_position_lod_transition[1] = camera_position.y;
	trace.camera_position_lod_transition[2] = camera_position.z;
	trace.camera_position_lod_transition[3] = trace.query_settings[3];
	for (uint32_t cascade = 0; cascade < DDGI_LOD_COUNT; cascade++) {
		trace.cascade_origin[cascade][0] = ddgi_state.ddgi_cascades[cascade].origin.x;
		trace.cascade_origin[cascade][1] = ddgi_state.ddgi_cascades[cascade].origin.y;
		trace.cascade_origin[cascade][2] = ddgi_state.ddgi_cascades[cascade].origin.z;
		trace.cascade_cell_size[cascade][0] = ddgi_state.ddgi_cascades[cascade].cell_size.x;
		trace.cascade_cell_size[cascade][1] = ddgi_state.ddgi_cascades[cascade].cell_size.y;
		trace.cascade_cell_size[cascade][2] = ddgi_state.ddgi_cascades[cascade].cell_size.z;
		trace.cascade_cell_size[cascade][3] = float(cascade);
		trace.cascade_logical_origin[cascade][0] = ddgi_state.ddgi_cascades[cascade].logical_origin.x;
		trace.cascade_logical_origin[cascade][1] = ddgi_state.ddgi_cascades[cascade].logical_origin.y;
		trace.cascade_logical_origin[cascade][2] = ddgi_state.ddgi_cascades[cascade].logical_origin.z;
		trace.cascade_logical_origin[cascade][3] = cascade;
	}
	trace.local_lights[0] = ddgi_state.ddgi_local_light_count;
	trace.local_lights[1] = ddgi_state.ddgi_local_light_overflow_count;
	RD::get_singleton()->buffer_update(ddgi_state.ddgi_trace_uniform_buffer[ddgi_frame_resource], 0, sizeof(trace), &trace);

	const RID trace_shader_rid = ddgi_trace_shader.version_get_shader(ddgi_trace_shader_version, 0);
	const RID sky_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID &trace_set = ddgi_state.ddgi_cascades[lod].trace_uniform_set[ddgi_frame_resource];
	const bool trace_bindings_changed = ddgi_state.ddgi_cascades[lod].trace_shadow[ddgi_frame_resource] != p_shadow_atlas ||
			ddgi_state.ddgi_cascades[lod].trace_environment[ddgi_frame_resource] != environment_radiance ||
			ddgi_state.ddgi_cascades[lod].trace_sampler[ddgi_frame_resource] != p_sampler ||
			ddgi_state.ddgi_cascades[lod].trace_sky_sampler[ddgi_frame_resource] != sky_sampler ||
			ddgi_state.ddgi_cascades[lod].trace_color_grid[ddgi_frame_resource] != ddgi_state.reflection_color_grid_rd[color_lod];
	if (trace_bindings_changed || !trace_set.is_valid() || !RD::get_singleton()->uniform_set_is_valid(trace_set)) {
		RD::Uniform t_surfels(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ ddgi_state.ddgi_surfel_buffer }));
		RD::Uniform t_indices(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ ddgi_state.ddgi_cascades[lod].probe_index_buffer }));
		RD::Uniform t_records(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ ddgi_state.ddgi_cascades[lod].probe_records }));
		RD::Uniform t_counters(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ ddgi_state.ddgi_cascades[lod].probe_counter_buffer }));
		RD::Uniform t_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ world.directory_buffer }));
		RD::Uniform t_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, Vector<RID>({ world.brick_buffer }));
		RD::Uniform t_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ p_sampler, ddgi_state.reflection_color_grid_rd[color_lod] }));
		RD::Uniform t_shadow(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ p_sampler, p_shadow_atlas }));
		RD::Uniform t_irradiance0(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ p_sampler, ddgi_state.ddgi_cascades[0].irradiance_rd }));
		RD::Uniform t_visibility0(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ p_sampler, ddgi_state.ddgi_cascades[0].visibility_rd }));
		RD::Uniform t_metadata0(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, Vector<RID>({ p_sampler, ddgi_state.ddgi_cascades[0].metadata_rd }));
		RD::Uniform t_irradiance1(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 11, Vector<RID>({ p_sampler, ddgi_state.ddgi_cascades[1].irradiance_rd }));
		RD::Uniform t_visibility1(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 12, Vector<RID>({ p_sampler, ddgi_state.ddgi_cascades[1].visibility_rd }));
		RD::Uniform t_metadata1(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 13, Vector<RID>({ p_sampler, ddgi_state.ddgi_cascades[1].metadata_rd }));
		RD::Uniform t_irradiance2(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 14, Vector<RID>({ p_sampler, ddgi_state.ddgi_cascades[2].irradiance_rd }));
		RD::Uniform t_visibility2(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 15, Vector<RID>({ p_sampler, ddgi_state.ddgi_cascades[2].visibility_rd }));
		RD::Uniform t_metadata2(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 16, Vector<RID>({ p_sampler, ddgi_state.ddgi_cascades[2].metadata_rd }));
		RD::Uniform t_irradiance3(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 17, Vector<RID>({ p_sampler, ddgi_state.ddgi_cascades[3].irradiance_rd }));
		RD::Uniform t_visibility3(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 18, Vector<RID>({ p_sampler, ddgi_state.ddgi_cascades[3].visibility_rd }));
		RD::Uniform t_metadata3(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 19, Vector<RID>({ p_sampler, ddgi_state.ddgi_cascades[3].metadata_rd }));
		RD::Uniform t_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 20, Vector<RID>({ ddgi_state.ddgi_trace_uniform_buffer[ddgi_frame_resource] }));
		RD::Uniform t_sky(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 21, Vector<RID>({ sky_sampler, environment_radiance }));
		RD::Uniform t_dynamic_volumes(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, Vector<RID>({ ddgi_state.ddgi_dynamic_volume_buffer }));
		RD::Uniform t_dynamic_bvh(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, Vector<RID>({ ddgi_state.ddgi_dynamic_bvh_buffer }));
		RD::Uniform t_dynamic_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, Vector<RID>({ ddgi_state.ddgi_dynamic_directory_buffer }));
		RD::Uniform t_dynamic_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25, Vector<RID>({ ddgi_state.ddgi_dynamic_brick_buffer }));
		RD::Uniform t_local_lights(RD::UNIFORM_TYPE_STORAGE_BUFFER, 26, Vector<RID>({ ddgi_state.ddgi_local_light_buffer }));
		trace_set = UniformSetCacheRD::get_singleton()->get_cache(trace_shader_rid, 0, t_surfels, t_indices, t_records, t_counters, t_directory, t_bricks, t_color, t_shadow, t_irradiance0, t_visibility0, t_metadata0, t_irradiance1, t_visibility1, t_metadata1, t_irradiance2, t_visibility2, t_metadata2, t_irradiance3, t_visibility3, t_metadata3, t_params, t_sky, t_dynamic_volumes, t_dynamic_bvh, t_dynamic_directory, t_dynamic_bricks, t_local_lights);
		ddgi_state.ddgi_cascades[lod].trace_shadow[ddgi_frame_resource] = p_shadow_atlas;
		ddgi_state.ddgi_cascades[lod].trace_environment[ddgi_frame_resource] = environment_radiance;
		ddgi_state.ddgi_cascades[lod].trace_sampler[ddgi_frame_resource] = p_sampler;
		ddgi_state.ddgi_cascades[lod].trace_sky_sampler[ddgi_frame_resource] = sky_sampler;
		ddgi_state.ddgi_cascades[lod].trace_color_grid[ddgi_frame_resource] = ddgi_state.reflection_color_grid_rd[color_lod];
	}
	const RID integrate_shader_rid = ddgi_integrate_shader.version_get_shader(ddgi_integrate_shader_version, 0);
	RID &integrate_set = ddgi_state.ddgi_cascades[lod].integrate_uniform_set;
	if (!integrate_set.is_valid() || !RD::get_singleton()->uniform_set_is_valid(integrate_set)) {
		RD::Uniform i_surfels(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ ddgi_state.ddgi_surfel_buffer }));
		RD::Uniform i_indices(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ ddgi_state.ddgi_cascades[lod].probe_index_buffer }));
		RD::Uniform i_counters(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ ddgi_state.ddgi_cascades[lod].probe_counter_buffer }));
		RD::Uniform i_records(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ ddgi_state.ddgi_cascades[lod].probe_records }));
		RD::Uniform i_irradiance(RD::UNIFORM_TYPE_IMAGE, 4, Vector<RID>({ ddgi_state.ddgi_cascades[lod].irradiance_rd }));
		RD::Uniform i_depth(RD::UNIFORM_TYPE_IMAGE, 5, Vector<RID>({ ddgi_state.ddgi_cascades[lod].visibility_rd }));
		RD::Uniform i_metadata(RD::UNIFORM_TYPE_IMAGE, 6, Vector<RID>({ ddgi_state.ddgi_cascades[lod].metadata_rd }));
		integrate_set = UniformSetCacheRD::get_singleton()->get_cache(integrate_shader_rid, 0, i_surfels, i_indices, i_counters, i_records, i_irradiance, i_depth, i_metadata);
	}
	DdgiIntegratePushConstant integrate = {};
	integrate.probe_grid[0] = resolution;
	integrate.probe_grid[1] = ray_stride;
	integrate.probe_grid[2] = 0;
	integrate.probe_grid[3] = 0;
	integrate.irradiance_layout[0] = DDGI_IRRADIANCE_TILE_SIZE;
	integrate.irradiance_layout[1] = irradiance_atlas_width;
	integrate.irradiance_layout[2] = irradiance_atlas_height;
	integrate.irradiance_layout[3] = DDGI_IRRADIANCE_INTERIOR_SIZE;
	integrate.visibility_layout[0] = DDGI_VISIBILITY_TILE_SIZE;
	integrate.visibility_layout[1] = visibility_atlas_width;
	integrate.visibility_layout[2] = visibility_atlas_height;
	integrate.visibility_layout[3] = DDGI_VISIBILITY_INTERIOR_SIZE;
	integrate.update_state[0] = ray_count;
	integrate.update_state[1] = convergence_ray_count;
	integrate.update_state[2] = ddgi_state.ddgi_cascades[lod].active_update_cursor;
	integrate.update_state[3] = ddgi_state.ddgi_cascades[lod].urgent_update_cursor;
	integrate.scheduling[0] = probes_per_frame;
	integrate.scheduling[1] = dirty_probe_budget;
	integrate.scheduling[2] = camera_probes_per_frame;
	integrate.temporal[0] = irradiance_hysteresis;
	integrate.temporal[1] = visibility_hysteresis;
	integrate.temporal[2] = world.voxel_size;
	// Camera-centered edge weighting and per-probe confidence keep exposed planes
	// behind the parent cascade while the bounded scheduler matures their data.
	// Rohacek depth rejection: a visibility sample beyond the probe cage
	// diagonal cannot occlude a query inside that cage. Account for the bounded
	// 45% relocation on both sides of a cage.
	integrate.temporal[3] = ddgi_state.ddgi_cascades[lod].cell_size.length() * 1.9f;
	integrate.dirty_temporal[0] = dirty_irradiance_hysteresis;
	integrate.dirty_temporal[1] = dirty_visibility_hysteresis;
	integrate.dirty_temporal[2] = float(convergence_updates);
	RENDER_TIMESTAMP("DDGI Probe Trace and Integrate");
	RD::ComputeListID integrate_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(integrate_list, ddgi_trace_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(integrate_list, trace_set, 0);
	RD::get_singleton()->compute_list_dispatch_indirect(integrate_list, ddgi_state.ddgi_cascades[lod].probe_dispatch_buffer, 0);
	RD::get_singleton()->compute_list_add_barrier(integrate_list);
	RD::get_singleton()->compute_list_bind_compute_pipeline(integrate_list, ddgi_integrate_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(integrate_list, integrate_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(integrate_list, &integrate, sizeof(integrate));
	RD::get_singleton()->compute_list_dispatch_indirect(integrate_list, ddgi_state.ddgi_cascades[lod].probe_dispatch_buffer, 4 * sizeof(uint32_t));
	RD::get_singleton()->compute_list_end();
	RENDER_TIMESTAMP("DDGI Probe Update Complete");
	ddgi_state.ddgi_cascades[lod].active_update_cursor = (ddgi_state.ddgi_cascades[lod].active_update_cursor + MAX(probes_per_frame, camera_probes_per_frame)) % MAX(total_probes, 1u);
	ddgi_state.ddgi_cascades[lod].urgent_update_cursor += dirty_probe_budget;
	// Integration changes lifecycle state (new/dirty/retiring to mature/inactive).
	// Rebuild the compacted category prefixes before the next selection pass so
	// their counters always describe the records referenced by each prefix.
	ddgi_state.ddgi_cascades[lod].activation_dirty = true;
	ddgi_state.ddgi_cascades[lod].initialized = true;
	ddgi_state.ddgi_cascades[lod].occupancy_revision = world.revision;
	static const StringName irradiance_names[DDGI_LOD_COUNT] = { SNAME("voxel_forward_ddgi_irradiance_lod0"), SNAME("voxel_forward_ddgi_irradiance_lod1"), SNAME("voxel_forward_ddgi_irradiance_lod2"), SNAME("voxel_forward_ddgi_irradiance_lod3") };
	static const StringName depth_names[DDGI_LOD_COUNT] = { SNAME("voxel_forward_ddgi_depth_lod0"), SNAME("voxel_forward_ddgi_depth_lod1"), SNAME("voxel_forward_ddgi_depth_lod2"), SNAME("voxel_forward_ddgi_depth_lod3") };
	static const StringName metadata_names[DDGI_LOD_COUNT] = { SNAME("voxel_forward_ddgi_metadata_lod0"), SNAME("voxel_forward_ddgi_metadata_lod1"), SNAME("voxel_forward_ddgi_metadata_lod2"), SNAME("voxel_forward_ddgi_metadata_lod3") };
	static const StringName origin_names[DDGI_LOD_COUNT] = { SNAME("voxel_forward_ddgi_origin_lod0"), SNAME("voxel_forward_ddgi_origin_lod1"), SNAME("voxel_forward_ddgi_origin_lod2"), SNAME("voxel_forward_ddgi_origin_lod3") };
	static const StringName cell_names[DDGI_LOD_COUNT] = { SNAME("voxel_forward_ddgi_cell_size_lod0"), SNAME("voxel_forward_ddgi_cell_size_lod1"), SNAME("voxel_forward_ddgi_cell_size_lod2"), SNAME("voxel_forward_ddgi_cell_size_lod3") };
	static const StringName phase_names[DDGI_LOD_COUNT] = { SNAME("voxel_forward_ddgi_phase_lod0"), SNAME("voxel_forward_ddgi_phase_lod1"), SNAME("voxel_forward_ddgi_phase_lod2"), SNAME("voxel_forward_ddgi_phase_lod3") };
	static const StringName logical_origin_names[DDGI_LOD_COUNT] = { SNAME("voxel_forward_ddgi_logical_origin_lod0"), SNAME("voxel_forward_ddgi_logical_origin_lod1"), SNAME("voxel_forward_ddgi_logical_origin_lod2"), SNAME("voxel_forward_ddgi_logical_origin_lod3") };
	bool any_ready = false;
	for (uint32_t index = 0; index < DDGI_LOD_COUNT; index++) {
		if (resources_reallocated || ddgi_context_changed) {
			material_storage->global_shader_parameter_set_override(irradiance_names[index], ddgi_state.ddgi_cascades[index].irradiance_texture);
			material_storage->global_shader_parameter_set_override(depth_names[index], ddgi_state.ddgi_cascades[index].visibility_texture);
			material_storage->global_shader_parameter_set_override(metadata_names[index], ddgi_state.ddgi_cascades[index].metadata_texture);
		}
		if (residency_dirty[index] || ddgi_context_changed) {
			material_storage->global_shader_parameter_set_override(origin_names[index], ddgi_state.ddgi_cascades[index].origin);
			material_storage->global_shader_parameter_set_override(cell_names[index], ddgi_state.ddgi_cascades[index].cell_size);
			material_storage->global_shader_parameter_set_override(phase_names[index], ddgi_state.ddgi_cascades[index].phase_offset);
			material_storage->global_shader_parameter_set_override(logical_origin_names[index], ddgi_state.ddgi_cascades[index].logical_origin);
		}
		any_ready = any_ready || ddgi_state.ddgi_cascades[index].initialized;
	}
	if (resources_reallocated) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_probe_resolution"), int(resolution));
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_irradiance_atlas_size"), Vector2(irradiance_atlas_width, irradiance_atlas_height));
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_visibility_atlas_size"), Vector2(visibility_atlas_width, visibility_atlas_height));
	}
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_self_shadow_bias"), CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/self_shadow_bias")), 0.0f, 2.0f));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_view_bias"), CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/view_bias")), 0.0f, 1.0f));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_lod_transition"), CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/lod_transition")), 0.01f, 0.5f));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_camera_position"), camera_position);
	// Both indirect backends use the same artist-facing intensity. The legacy
	// path publishes it below, but backend 2 returns before reaching that code.
	// Publish it here as well or valid DDGI irradiance is multiplied by the
	// registered global default of zero in the voxel material.
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_intensity"), MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/intensity"))));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_ready"), any_ready);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), false);
	if ((ddgi_state.ddgi_frame_index % 600u) == 0u) {
		print_verbose(vformat("Voxel Forward DDGI placement revision %d: pages=%d/%d/%d/%d, probes=%d/%d/%d/%d, pending=%d, last page work %.2f ms (%d probes), resident hits=%d, empty slots=%d, budgets steady/edit/camera/light=%d/%d/%d/%d, local lights=%d (overflow %d), camera placement work=%d, camera uploads=%d bytes in %d exposed plane(s).", world.ddgi_probe_cache_revision, world.ddgi_cached_page_count[0], world.ddgi_cached_page_count[1], world.ddgi_cached_page_count[2], world.ddgi_cached_page_count[3], world.ddgi_cached_probe_count[0], world.ddgi_cached_probe_count[1], world.ddgi_cached_probe_count[2], world.ddgi_cached_probe_count[3], world.ddgi_pending_page_count, double(world.ddgi_last_probe_bake_usec) / 1000.0, world.ddgi_last_dirty_probe_cell_count, ddgi_state.ddgi_resident_cache_hits, ddgi_state.ddgi_resident_empty_slots, probes_per_frame, urgent_probes_per_frame, camera_probes_per_frame, lighting_probes_per_frame, ddgi_state.ddgi_local_light_count, ddgi_state.ddgi_local_light_overflow_count, ddgi_state.ddgi_camera_placement_work_count, ddgi_state.ddgi_camera_upload_bytes, ddgi_state.ddgi_new_plane_upload_count));
	}
}

void RenderVoxelForward::_render_restir_gi(const RenderDataRD *p_render_data, RID p_sampler, const Vector3 &p_light_direction, const Color &p_light_color, float p_light_energy) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	auto disable_restir = [material_storage]() {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_restir_ready"), false);
	};
	const VoxelForwardVolumeStorage::WorldOccupancy &world = volume_storage->get_world_occupancy();
	if (p_render_data == nullptr || p_render_data->render_buffers.is_null() || p_render_data->scene_data == nullptr ||
			p_render_data->scene_data->view_count != 1 || !p_sampler.is_valid() || !restir_temporal_pipeline.is_valid() ||
			!restir_spatial_pipeline.is_valid() || !restir_denoise_pipeline.is_valid() || !restir_uniform_buffer.is_valid() || !restir_dirty_brick_buffer.is_valid() ||
			!world.directory_buffer.is_valid() || !world.brick_buffer.is_valid() || world.occupied_brick_count == 0 ||
			world.voxel_size <= 0.0f) {
		disable_restir();
		restir_history_valid = false;
		return;
	}
	for (uint32_t cascade = 0; cascade < REFLECTION_CASCADE_COUNT; cascade++) {
		if (!ddgi_state.reflection_color_grid_rd[cascade].is_valid() || !ddgi_state.reflection_color_grid_initialized[cascade]) {
			disable_restir();
			restir_history_valid = false;
			return;
		}
	}

	Ref<RenderSceneBuffersRD> render_buffers = p_render_data->render_buffers;
	Ref<RenderBufferDataForwardClustered> forward_buffers;
	if (render_buffers->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		forward_buffers = render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	}
	if (forward_buffers.is_null() || !forward_buffers->has_voxel_hit()) {
		disable_restir();
		restir_history_valid = false;
		return;
	}

	const Size2i internal_size = render_buffers->get_internal_size();
	const float resolution_scale = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/resolution_scale")), 0.25f, 1.0f);
	const Size2i screen_size(MAX(1, int(Math::ceil(internal_size.x * resolution_scale))), MAX(1, int(Math::ceil(internal_size.y * resolution_scale))));
	const StringName scope = SNAME("voxel_forward_restir");
	const StringName texture_name = SNAME("indirect_radiance");
	if (restir_screen_size != screen_size) {
		_free_restir_gi();
		if (render_buffers->has_texture(scope, texture_name)) {
			render_buffers->clear_context(scope);
		}
	}
	if (!render_buffers->has_texture(scope, texture_name)) {
		render_buffers->create_texture(scope, texture_name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, screen_size);
	}
	const RID output = render_buffers->get_texture(scope, texture_name);
	const RID depth = render_buffers->get_depth_texture();
	const RID hit_payload = forward_buffers->get_voxel_hit();
	if (!output.is_valid() || !depth.is_valid() || !hit_payload.is_valid()) {
		disable_restir();
		restir_history_valid = false;
		return;
	}
	if (restir_output_rd != output) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_restir_gi"), Variant());
		if (restir_output_texture.is_valid()) {
			texture_storage->texture_free(restir_output_texture);
		}
		restir_output_texture = texture_storage->texture_allocate();
		texture_storage->texture_rd_initialize(restir_output_texture, output);
		restir_output_rd = output;
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_restir_gi"), restir_output_texture);
		restir_history_valid = false;
	}

	const uint64_t reservoir_count = uint64_t(screen_size.x) * uint64_t(screen_size.y);
	const uint64_t reservoir_bytes = reservoir_count * RESTIR_RESERVOIR_SIZE;
	if (reservoir_bytes > UINT32_MAX) {
		ERR_PRINT_ONCE("ReSTIR GI reservoir allocation exceeds RenderingDevice's 32-bit buffer limit.");
		disable_restir();
		return;
	}
	if (!restir_temporal_reservoir[0].is_valid()) {
		for (uint32_t slot = 0; slot < 2; slot++) {
			restir_temporal_reservoir[slot] = RD::get_singleton()->storage_buffer_create(uint32_t(reservoir_bytes));
			restir_spatial_reservoir[slot] = RD::get_singleton()->storage_buffer_create(uint32_t(reservoir_bytes));
			RD::get_singleton()->set_resource_name(restir_temporal_reservoir[slot], vformat("ReSTIR GI Temporal Reservoir %d", slot));
			RD::get_singleton()->set_resource_name(restir_spatial_reservoir[slot], vformat("ReSTIR GI Spatial Reservoir %d", slot));
		}
		restir_history_slot = 0;
		restir_history_valid = false;
		print_verbose(vformat("ReSTIR GI allocation: %dx%d, four %.2f MiB reservoir buffers, %.2f MiB total.", screen_size.x, screen_size.y, double(reservoir_bytes) / (1024.0 * 1024.0), double(reservoir_bytes * 4u) / (1024.0 * 1024.0)));
	}
	restir_screen_size = screen_size;

	bool global_invalidation = restir_shading_revision != UINT64_MAX && restir_shading_revision != volume_storage->get_shading_texture_revision();
	Vector<RestirDirtyBrickData> dirty_upload;
	if (restir_world_revision != UINT64_MAX && restir_world_revision != world.revision) {
		const bool incremental_change = world.last_incremental_revision == world.revision && !world.last_dirty_bricks.is_empty() && world.last_dirty_bricks.size() <= int(RESTIR_MAX_DIRTY_BRICKS);
		if (incremental_change) {
			dirty_upload.resize(world.last_dirty_bricks.size());
			for (int index = 0; index < world.last_dirty_bricks.size(); index++) {
				const Vector3i brick = world.last_dirty_bricks[index];
				dirty_upload.write[index].coordinate[0] = brick.x;
				dirty_upload.write[index].coordinate[1] = brick.y;
				dirty_upload.write[index].coordinate[2] = brick.z;
			}
		} else {
			global_invalidation = true;
		}
	}
	if (!dirty_upload.is_empty()) {
		RD::get_singleton()->buffer_update(restir_dirty_brick_buffer, 0, dirty_upload.size() * sizeof(RestirDirtyBrickData), dirty_upload.ptr());
	}

	const Transform3D &camera_transform = p_render_data->scene_data->cam_transform;
	const Transform3D &previous_camera_transform = p_render_data->scene_data->prev_cam_transform;
	const float camera_cut_distance = MAX(world.voxel_size, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/camera_cut_distance")));
	const float camera_cut_angle = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/camera_cut_angle_degrees")), 1.0f, 180.0f);
	const Vector3 camera_forward = -camera_transform.basis.get_column(2).normalized();
	const Vector3 previous_camera_forward = -previous_camera_transform.basis.get_column(2).normalized();
	const bool camera_cut = camera_transform.origin.distance_to(previous_camera_transform.origin) > camera_cut_distance ||
			camera_forward.dot(previous_camera_forward) < Math::cos(Math::deg_to_rad(camera_cut_angle));
	const bool light_changed = restir_frame_index > 0 && (restir_light_direction.dot(p_light_direction) < 0.9999f || !restir_light_color.is_equal_approx(p_light_color) || !Math::is_equal_approx(restir_light_energy, p_light_energy));
	if (camera_cut || light_changed || global_invalidation) {
		restir_history_valid = false;
	}

	RestirUniformData uniform_data = {};
	const Projection view_projection = p_render_data->scene_data->get_view_projection(0) * Projection(camera_transform.affine_inverse());
	Projection previous_correction;
	previous_correction.set_depth_correction(true);
	const Projection previous_view_projection = (previous_correction * p_render_data->scene_data->prev_cam_projection) * Projection(previous_camera_transform.affine_inverse());
	RendererRD::MaterialStorage::store_camera(view_projection.inverse(), uniform_data.inv_view_projection);
	RendererRD::MaterialStorage::store_camera(previous_view_projection, uniform_data.previous_view_projection);
	uniform_data.world_origin_voxel_size[0] = world.origin.x;
	uniform_data.world_origin_voxel_size[1] = world.origin.y;
	uniform_data.world_origin_voxel_size[2] = world.origin.z;
	uniform_data.world_origin_voxel_size[3] = world.voxel_size;
	uniform_data.light_direction_energy[0] = p_light_direction.x;
	uniform_data.light_direction_energy[1] = p_light_direction.y;
	uniform_data.light_direction_energy[2] = p_light_direction.z;
	uniform_data.light_direction_energy[3] = MAX(0.0f, p_light_energy);
	uniform_data.light_color_intensity[0] = p_light_color.r;
	uniform_data.light_color_intensity[1] = p_light_color.g;
	uniform_data.light_color_intensity[2] = p_light_color.b;
	uniform_data.light_color_intensity[3] = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/intensity")));
	Color ambient_color = GLOBAL_GET("rendering/voxel_forward/ambient_light/color");
	float ambient_energy = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/ambient_light/energy")));
	if (p_render_data->environment.is_valid()) {
		ambient_color = environment_get_ambient_light(p_render_data->environment);
		ambient_energy = MAX(0.0f, environment_get_ambient_light_energy(p_render_data->environment));
	}
	uniform_data.ambient_color_energy[0] = ambient_color.r;
	uniform_data.ambient_color_energy[1] = ambient_color.g;
	uniform_data.ambient_color_energy[2] = ambient_color.b;
	uniform_data.ambient_color_energy[3] = ambient_energy;
	for (uint32_t cascade = 0; cascade < REFLECTION_CASCADE_COUNT; cascade++) {
		uniform_data.color_grid_origin_cell_size[cascade][0] = ddgi_state.reflection_color_grid_origin[cascade].x;
		uniform_data.color_grid_origin_cell_size[cascade][1] = ddgi_state.reflection_color_grid_origin[cascade].y;
		uniform_data.color_grid_origin_cell_size[cascade][2] = ddgi_state.reflection_color_grid_origin[cascade].z;
		uniform_data.color_grid_origin_cell_size[cascade][3] = ddgi_state.reflection_color_grid_cell_size[cascade];
	}
	uniform_data.screen[0] = screen_size.x;
	uniform_data.screen[1] = screen_size.y;
	uniform_data.screen[2] = internal_size.x;
	uniform_data.screen[3] = internal_size.y;
	uniform_data.trace[0] = world.directory_mask;
	uniform_data.trace[1] = world.max_probe_count;
	uniform_data.trace[2] = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/max_trace_steps")), 16, 4096);
	uniform_data.trace[3] = ddgi_state.reflection_color_grid_resolution;
	const uint32_t debug_mode = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/debug_mode")), 0, 5));
	uniform_data.state[0] = restir_frame_index;
	uniform_data.state[1] = restir_history_valid ? 1u : 0u;
	uniform_data.state[2] = uint32_t(world.revision);
	const uint32_t denoise_history_frames = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/denoise_history_frames")), 1, 64));
	uniform_data.state[3] = (global_invalidation ? 1u : 0u) | (debug_mode << 1u) | (denoise_history_frames << 9u) | (uint32_t(dirty_upload.size()) << 16u);
	uniform_data.reuse[0] = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/spatial_neighbors")), 0, 32);
	uniform_data.reuse[1] = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/spatial_radius")), 1, 128);
	uniform_data.reuse[2] = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/max_temporal_samples")), 1, 128);
	uniform_data.reuse[3] = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/max_spatial_samples")), 1, 4096);
	uniform_data.limits[0] = MAX(world.voxel_size, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/max_distance")));
	uniform_data.limits[1] = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/depth_tolerance_voxels")), 0.01f, 8.0f);
	uniform_data.limits[2] = 1e-5f;
	uniform_data.limits[3] = MAX(1.0f, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/radiance_clamp")));
	uniform_data.validation[0] = Math::cos(Math::deg_to_rad(CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/normal_tolerance_degrees")), 0.0f, 89.0f)));
	uniform_data.validation[1] = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/validation_interval")), 1.0f, 120.0f);
	uniform_data.validation[2] = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/radiance_tolerance")), 0.0f, 10.0f);
	uniform_data.validation[3] = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/bias_mode")), 0.0f, 1.0f);
	RD::get_singleton()->buffer_update(restir_uniform_buffer, 0, sizeof(RestirUniformData), &uniform_data);

	const uint32_t write_slot = restir_history_slot ^ 1u;
	const RID temporal_shader_rid = restir_temporal_shader.version_get_shader(restir_temporal_shader_version, 0);
	RD::Uniform temporal_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ p_sampler, depth }));
	RD::Uniform temporal_hit(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ p_sampler, hit_payload }));
	RD::Uniform temporal_history(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ restir_temporal_reservoir[restir_history_slot] }));
	RD::Uniform temporal_output(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ restir_temporal_reservoir[write_slot] }));
	RD::Uniform temporal_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ world.directory_buffer }));
	RD::Uniform temporal_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, Vector<RID>({ world.brick_buffer }));
	RD::Uniform temporal_color_near(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ p_sampler, ddgi_state.reflection_color_grid_rd[0] }));
	RD::Uniform temporal_color_far(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ p_sampler, ddgi_state.reflection_color_grid_rd[1] }));
	RD::Uniform temporal_color_distant(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ p_sampler, ddgi_state.reflection_color_grid_rd[2] }));
	RD::Uniform temporal_dirty(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, Vector<RID>({ restir_dirty_brick_buffer }));
	RD::Uniform temporal_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 10, Vector<RID>({ restir_uniform_buffer }));
	const RID temporal_set = UniformSetCacheRD::get_singleton()->get_cache(temporal_shader_rid, 0, temporal_depth, temporal_hit, temporal_history, temporal_output, temporal_directory, temporal_bricks, temporal_color_near, temporal_color_far, temporal_color_distant, temporal_dirty, temporal_params);

	const RID spatial_shader_rid = restir_spatial_shader.version_get_shader(restir_spatial_shader_version, 0);
	RD::Uniform spatial_temporal(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ restir_temporal_reservoir[write_slot] }));
	RD::Uniform spatial_output_buffer(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ restir_spatial_reservoir[write_slot] }));
	RD::Uniform spatial_output_image(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ output }));
	RD::Uniform spatial_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ world.directory_buffer }));
	RD::Uniform spatial_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ world.brick_buffer }));
	RD::Uniform spatial_color_near(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ p_sampler, ddgi_state.reflection_color_grid_rd[0] }));
	RD::Uniform spatial_color_far(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ p_sampler, ddgi_state.reflection_color_grid_rd[1] }));
	RD::Uniform spatial_color_distant(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ p_sampler, ddgi_state.reflection_color_grid_rd[2] }));
	RD::Uniform spatial_history(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, Vector<RID>({ restir_spatial_reservoir[restir_history_slot] }));
	RD::Uniform spatial_dirty(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, Vector<RID>({ restir_dirty_brick_buffer }));
	RD::Uniform spatial_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 10, Vector<RID>({ restir_uniform_buffer }));
	const RID spatial_set = UniformSetCacheRD::get_singleton()->get_cache(spatial_shader_rid, 0, spatial_temporal, spatial_output_buffer, spatial_output_image, spatial_directory, spatial_bricks, spatial_color_near, spatial_color_far, spatial_color_distant, spatial_history, spatial_dirty, spatial_params);

	const RID denoise_shader_rid = restir_denoise_shader.version_get_shader(restir_denoise_shader_version, 0);
	RD::Uniform denoise_reservoirs(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ restir_spatial_reservoir[write_slot] }));
	RD::Uniform denoise_output(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ output }));
	RD::Uniform denoise_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 2, Vector<RID>({ restir_uniform_buffer }));
	const RID denoise_set = UniformSetCacheRD::get_singleton()->get_cache(denoise_shader_rid, 0, denoise_reservoirs, denoise_output, denoise_params);

	RENDER_TIMESTAMP("ReSTIR GI Temporal");
	RD::get_singleton()->draw_command_begin_label("ReSTIR GI");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, restir_temporal_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, temporal_set, 0);
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, screen_size.x, screen_size.y, 1);
	RD::get_singleton()->compute_list_add_barrier(compute_list);
	RENDER_TIMESTAMP("ReSTIR GI Spatial");
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, restir_spatial_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, spatial_set, 0);
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, screen_size.x, screen_size.y, 1);
	RD::get_singleton()->compute_list_add_barrier(compute_list);
	RENDER_TIMESTAMP("ReSTIR GI Voxel-Face Denoise");
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, restir_denoise_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, denoise_set, 0);
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, screen_size.x, screen_size.y, 1);
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();

	restir_history_slot = write_slot;
	restir_history_valid = true;
	restir_world_revision = world.revision;
	restir_shading_revision = volume_storage->get_shading_texture_revision();
	restir_light_direction = p_light_direction;
	restir_light_color = p_light_color;
	restir_light_energy = p_light_energy;
	restir_frame_index++;
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_restir_ready"), true);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_intensity"), MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/intensity"))));
}

void RenderVoxelForward::_render_restir_gi_for_scene(const RenderDataRD *p_render_data) {
	if (!bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/enabled")) || int(GLOBAL_GET("rendering/voxel_forward/indirect_light/backend")) != 1 || p_render_data == nullptr || p_render_data->scene_data == nullptr) {
		return;
	}
	Vector3 light_direction(0.0, 1.0, 0.0);
	Color light_color;
	float light_energy = 0.0f;
	if (p_render_data->lights != nullptr) {
		RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
		for (uint32_t i = 0; i < p_render_data->lights->size(); i++) {
			const RID light_instance = (*p_render_data->lights)[i];
			if (light_storage->light_instance_get_type(light_instance) != RSE::LIGHT_DIRECTIONAL) {
				continue;
			}
			light_direction = light_storage->light_instance_get_base_transform(light_instance).basis.xform(Vector3(0, 0, 1)).normalized();
			const RID light = light_storage->light_instance_get_base_light(light_instance);
			light_color = light_storage->light_get_color(light).srgb_to_linear();
			light_energy = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ENERGY) * light_storage->light_get_param(light, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
			break;
		}
	}

	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	const RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	_render_restir_gi(p_render_data, sampler, light_direction, light_color, light_energy);
}

void RenderVoxelForward::_update_ddgi_local_lights(const RenderDataRD *p_render_data) {
	Vector<DdgiLocalLightGpuData> upload;
	HashMap<RID, DdgiLocalLightCacheEntry> current_cache;
	const bool enabled = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/local_lights_enabled"));
	const uint32_t maximum_lights = enabled ? uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/max_local_lights")), 0, 32)) : 0u;
	uint32_t eligible_count = 0;
	if (p_render_data != nullptr && p_render_data->lights != nullptr && maximum_lights > 0) {
		RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
		for (uint32_t index = 0; index < p_render_data->lights->size(); index++) {
			const RID light_instance = (*p_render_data->lights)[index];
			const RSE::LightType type = light_storage->light_instance_get_type(light_instance);
			if (type != RSE::LIGHT_OMNI && type != RSE::LIGHT_AREA) {
				continue;
			}
			const RID light = light_storage->light_instance_get_base_light(light_instance);
			const float range = MAX(light_storage->light_get_param(light, RSE::LIGHT_PARAM_RANGE), 0.001f);
			const float energy = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ENERGY) * light_storage->light_get_param(light, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
			if (energy <= 0.0f || light_storage->light_is_negative(light)) {
				continue;
			}
			eligible_count++;
			if (upload.size() >= int(maximum_lights)) {
				continue;
			}
			const Transform3D transform = light_storage->light_instance_get_base_transform(light_instance);
			DdgiLocalLightGpuData local = {};
			local.position_range[0] = transform.origin.x;
			local.position_range[1] = transform.origin.y;
			local.position_range[2] = transform.origin.z;
			local.position_range[3] = range;
			const Vector3 direction = transform.basis.xform(Vector3(0, 0, -1)).normalized();
			local.direction_type[0] = direction.x;
			local.direction_type[1] = direction.y;
			local.direction_type[2] = direction.z;
			local.direction_type[3] = type == RSE::LIGHT_AREA ? 2.0f : 1.0f;
			const Color color = light_storage->light_get_color(light).srgb_to_linear();
			local.color_energy[0] = color.r;
			local.color_energy[1] = color.g;
			local.color_energy[2] = color.b;
			local.color_energy[3] = energy;
			local.attenuation_source_size[0] = MAX(light_storage->light_get_param(light, RSE::LIGHT_PARAM_ATTENUATION), 0.0f);
			local.attenuation_source_size[1] = light_storage->light_has_shadow(light) ? 1.0f : 0.0f;
			if (type == RSE::LIGHT_AREA) {
				const Vector2 area_size = light_storage->light_area_get_size(light);
				local.attenuation_source_size[2] = MAX(area_size.x, 0.0f);
				local.attenuation_source_size[3] = MAX(area_size.y, 0.0f);
			} else {
				local.attenuation_source_size[2] = MAX(light_storage->light_get_param(light, RSE::LIGHT_PARAM_SIZE), 0.0f);
			}
			upload.push_back(local);

			DdgiLocalLightCacheEntry cache;
			cache.influence_bounds = AABB(transform.origin - Vector3(range, range, range), Vector3(range * 2.0f, range * 2.0f, range * 2.0f));
			cache.transform = transform;
			cache.version = light_storage->light_get_version(light);
			current_cache.insert(light_instance, cache);
			const DdgiLocalLightCacheEntry *previous = ddgi_state.ddgi_local_light_cache.getptr(light_instance);
			if (previous == nullptr || previous->version != cache.version || previous->transform != cache.transform) {
				for (uint32_t lod = 0; lod < DDGI_LOD_COUNT; lod++) {
					if (previous != nullptr) {
						ddgi_state.ddgi_dynamic_dirty_bounds[lod].push_back(previous->influence_bounds);
					}
					ddgi_state.ddgi_dynamic_dirty_bounds[lod].push_back(cache.influence_bounds);
				}
			}
		}
	}
	for (const KeyValue<RID, DdgiLocalLightCacheEntry> &previous : ddgi_state.ddgi_local_light_cache) {
		if (!current_cache.has(previous.key)) {
			for (uint32_t lod = 0; lod < DDGI_LOD_COUNT; lod++) {
				ddgi_state.ddgi_dynamic_dirty_bounds[lod].push_back(previous.value.influence_bounds);
			}
		}
	}
	ddgi_state.ddgi_local_light_cache = current_cache;
	ddgi_state.ddgi_local_light_count = upload.size();
	ddgi_state.ddgi_local_light_overflow_count = eligible_count > ddgi_state.ddgi_local_light_count ? eligible_count - ddgi_state.ddgi_local_light_count : 0u;
	uint64_t upload_hash = 1469598103934665603ull;
	const uint8_t *upload_bytes = reinterpret_cast<const uint8_t *>(upload.ptr());
	for (uint32_t byte = 0; byte < uint32_t(upload.size() * sizeof(DdgiLocalLightGpuData)); byte++) {
		upload_hash ^= uint64_t(upload_bytes[byte]);
		upload_hash *= 1099511628211ull;
	}
	upload_hash ^= uint64_t(ddgi_state.ddgi_local_light_count);
	upload_hash *= 1099511628211ull;
	const uint32_t required_capacity = MAX(ddgi_state.ddgi_local_light_count, 1u);
	bool buffer_reallocated = false;
	if (!ddgi_state.ddgi_local_light_buffer.is_valid() || ddgi_state.ddgi_local_light_buffer_capacity < required_capacity) {
		if (ddgi_state.ddgi_local_light_buffer.is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.ddgi_local_light_buffer);
		}
		ddgi_state.ddgi_local_light_buffer = RD::get_singleton()->storage_buffer_create(required_capacity * sizeof(DdgiLocalLightGpuData));
		ddgi_state.ddgi_local_light_buffer_capacity = required_capacity;
		RD::get_singleton()->set_resource_name(ddgi_state.ddgi_local_light_buffer, "Voxel DDGI Local Lights");
		buffer_reallocated = true;
	}
	if (buffer_reallocated || ddgi_state.ddgi_local_light_upload_hash != upload_hash) {
		if (!upload.is_empty()) {
			RD::get_singleton()->buffer_update(ddgi_state.ddgi_local_light_buffer, 0, upload.size() * sizeof(DdgiLocalLightGpuData), upload.ptr());
		} else {
			DdgiLocalLightGpuData empty = {};
			RD::get_singleton()->buffer_update(ddgi_state.ddgi_local_light_buffer, 0, sizeof(empty), &empty);
		}
		ddgi_state.ddgi_local_light_upload_hash = upload_hash;
	}
}

void RenderVoxelForward::_render_ddgi_gi_for_scene(const RenderDataRD *p_render_data) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	if (!bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/enabled")) || int(GLOBAL_GET("rendering/voxel_forward/indirect_light/backend")) != 2 || p_render_data == nullptr || p_render_data->scene_data == nullptr) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_ready"), false);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_resolve_ready"), false);
		return;
	}
	Vector3 light_direction(0.0, 1.0, 0.0);
	Color light_color;
	float light_energy = 0.0f;
	if (p_render_data->lights != nullptr) {
		RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
		for (uint32_t i = 0; i < p_render_data->lights->size(); i++) {
			const RID light_instance = (*p_render_data->lights)[i];
			if (light_storage->light_instance_get_type(light_instance) != RSE::LIGHT_DIRECTIONAL) {
				continue;
			}
			light_direction = light_storage->light_instance_get_base_transform(light_instance).basis.xform(Vector3(0, 0, 1)).normalized();
			const RID light = light_storage->light_instance_get_base_light(light_instance);
			light_color = light_storage->light_get_color(light).srgb_to_linear();
			light_energy = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ENERGY) * light_storage->light_get_param(light, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
			break;
		}
	}

	// Probe evolution is independent from the optional shadow-atlas lifecycle.
	// Until a complete atlas exists, the default texture only keeps the descriptor
	// valid; the trace shader resolves direct visibility against voxel occupancy.
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	RD *rd = RD::get_singleton();
	const bool shadow_valid = rd != nullptr && ddgi_state.shadow_atlas_initialized && ddgi_state.shadow_atlas_source_rd.is_valid() && rd->texture_is_valid(ddgi_state.shadow_atlas_source_rd);
	const RID shadow_texture = shadow_valid ? ddgi_state.shadow_atlas_source_rd : texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	if (!shadow_texture.is_valid() || rd == nullptr || !rd->texture_is_valid(shadow_texture)) {
		ERR_PRINT_ONCE("Voxel Forward DDGI has neither a live shadow atlas nor a descriptor-safe fallback texture.");
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_ready"), false);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_resolve_ready"), false);
		return;
	}
	if (shadow_valid) {
		light_direction = ddgi_state.shadow_atlas_light_direction;
	}
	const RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	_render_indirect_light(p_render_data, shadow_texture, sampler, light_direction, light_color, light_energy);
	_render_ddgi_resolve(p_render_data, sampler);
}

void RenderVoxelForward::_render_ddgi_resolve(const RenderDataRD *p_render_data, RID p_sampler) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	const uint32_t frame_resource = uint32_t(ddgi_state.ddgi_frame_index % DDGI_FRAME_RESOURCE_COUNT);
	auto disable_resolve = [material_storage]() {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_resolve_ready"), false);
	};
	if (p_render_data == nullptr || p_render_data->render_buffers.is_null() || p_render_data->scene_data == nullptr ||
			p_render_data->scene_data->view_count != 1 || !p_sampler.is_valid() || !ddgi_resolve_pipeline.is_valid() ||
			!ddgi_state.ddgi_resolve_uniform_buffer[frame_resource].is_valid() || !ddgi_temporal_pipeline.is_valid() || !ddgi_state.ddgi_temporal_uniform_buffer[frame_resource].is_valid() || ddgi_state.ddgi_probe_resolution < 2 ||
			int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/debug_mode")) == 15) {
		disable_resolve();
		return;
	}
	for (uint32_t lod = 0; lod < DDGI_LOD_COUNT; lod++) {
		if (!ddgi_state.ddgi_cascades[lod].initialized || !ddgi_state.ddgi_cascades[lod].irradiance_rd.is_valid() ||
				!ddgi_state.ddgi_cascades[lod].visibility_rd.is_valid() || !ddgi_state.ddgi_cascades[lod].metadata_rd.is_valid()) {
			disable_resolve();
			return;
		}
	}

	Ref<RenderSceneBuffersRD> render_buffers = p_render_data->render_buffers;
	Ref<RenderBufferDataForwardClustered> forward_buffers;
	if (render_buffers->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		forward_buffers = render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	}
	if (forward_buffers.is_null() || !forward_buffers->has_voxel_hit() || !forward_buffers->has_voxel_hit_position()) {
		disable_resolve();
		return;
	}

	const Size2i screen_size = render_buffers->get_internal_size();
	const StringName scope = SNAME("voxel_forward_ddgi_resolve");
	const StringName raw_texture_name = SNAME("raw_indirect_radiance");
	const StringName stable_published_name = SNAME("published_indirect");
	static const StringName published_names[2] = { SNAME("published_indirect_0"), SNAME("published_indirect_1") };
	static const StringName face_history_names[2] = { SNAME("face_history_0"), SNAME("face_history_1") };
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	if (ddgi_state.ddgi_resolve_screen_size != screen_size) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_resolve"), Variant());
		if (ddgi_state.ddgi_resolve_output_texture.is_valid()) {
			texture_storage->texture_free(ddgi_state.ddgi_resolve_output_texture);
		}
		ddgi_state.ddgi_resolve_output_texture = RID();
		ddgi_state.ddgi_resolve_output_rd = RID();
		ddgi_state.ddgi_published_output_rd = RID();
		for (uint32_t slot = 0; slot < 2; slot++) {
			ddgi_state.ddgi_temporal_output_rd[slot] = RID();
			ddgi_state.ddgi_temporal_face_rd[slot] = RID();
		}
		ddgi_state.ddgi_temporal_slot = 0;
		ddgi_state.ddgi_temporal_history_valid = false;
		if (render_buffers->has_texture(scope, raw_texture_name)) {
			render_buffers->clear_context(scope);
		}
	}
	if (!render_buffers->has_texture(scope, raw_texture_name)) {
		render_buffers->create_texture(scope, raw_texture_name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, screen_size);
		render_buffers->create_texture(scope, stable_published_name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, screen_size);
		for (uint32_t slot = 0; slot < 2; slot++) {
			render_buffers->create_texture(scope, published_names[slot], RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, screen_size);
			render_buffers->create_texture(scope, face_history_names[slot], RD::DATA_FORMAT_R32G32B32A32_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, screen_size);
		}
	}
	const RID output = render_buffers->get_texture(scope, raw_texture_name);
	const RID face_data = forward_buffers->get_voxel_hit_position();
	const RID hit_payload = forward_buffers->get_voxel_hit();
	if (!output.is_valid() || !face_data.is_valid() || !hit_payload.is_valid()) {
		disable_resolve();
		return;
	}
	if (ddgi_state.ddgi_resolve_output_rd != output) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_resolve"), Variant());
		if (ddgi_state.ddgi_resolve_output_texture.is_valid()) {
			texture_storage->texture_free(ddgi_state.ddgi_resolve_output_texture);
		}
		ddgi_state.ddgi_resolve_output_rd = output;
		ddgi_state.ddgi_published_output_rd = render_buffers->get_texture(scope, stable_published_name);
		for (uint32_t slot = 0; slot < 2; slot++) {
			ddgi_state.ddgi_temporal_output_rd[slot] = render_buffers->get_texture(scope, published_names[slot]);
			ddgi_state.ddgi_temporal_face_rd[slot] = render_buffers->get_texture(scope, face_history_names[slot]);
		}
		ddgi_state.ddgi_resolve_output_texture = texture_storage->texture_allocate();
		texture_storage->texture_rd_initialize(ddgi_state.ddgi_resolve_output_texture, ddgi_state.ddgi_published_output_rd);
		// The material-visible RID remains stable. Only temporal history alternates
		// internally, so ordinary frames never requeue every voxel material.
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_resolve"), ddgi_state.ddgi_resolve_output_texture);
	}
	if (!ddgi_state.ddgi_published_output_rd.is_valid()) {
		disable_resolve();
		return;
	}
	if (ddgi_context_changed) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_resolve"), ddgi_state.ddgi_resolve_output_texture);
	}
	for (uint32_t slot = 0; slot < 2; slot++) {
		if (!ddgi_state.ddgi_temporal_output_rd[slot].is_valid() || !ddgi_state.ddgi_temporal_face_rd[slot].is_valid()) {
			disable_resolve();
			return;
		}
	}
	ddgi_state.ddgi_resolve_screen_size = screen_size;

	DdgiResolveUniformData uniform_data = {};
	for (uint32_t lod = 0; lod < DDGI_LOD_COUNT; lod++) {
		uniform_data.origins[lod][0] = ddgi_state.ddgi_cascades[lod].origin.x;
		uniform_data.origins[lod][1] = ddgi_state.ddgi_cascades[lod].origin.y;
		uniform_data.origins[lod][2] = ddgi_state.ddgi_cascades[lod].origin.z;
		uniform_data.cell_sizes[lod][0] = ddgi_state.ddgi_cascades[lod].cell_size.x;
		uniform_data.cell_sizes[lod][1] = ddgi_state.ddgi_cascades[lod].cell_size.y;
		uniform_data.cell_sizes[lod][2] = ddgi_state.ddgi_cascades[lod].cell_size.z;
		uniform_data.phase_offsets[lod][0] = ddgi_state.ddgi_cascades[lod].phase_offset.x;
		uniform_data.phase_offsets[lod][1] = ddgi_state.ddgi_cascades[lod].phase_offset.y;
		uniform_data.phase_offsets[lod][2] = ddgi_state.ddgi_cascades[lod].phase_offset.z;
		uniform_data.logical_origins[lod][0] = ddgi_state.ddgi_cascades[lod].logical_origin.x;
		uniform_data.logical_origins[lod][1] = ddgi_state.ddgi_cascades[lod].logical_origin.y;
		uniform_data.logical_origins[lod][2] = ddgi_state.ddgi_cascades[lod].logical_origin.z;
	}
	const Vector3 camera_position = p_render_data->scene_data->cam_transform.origin;
	uniform_data.camera_irradiance_size[0] = camera_position.x;
	uniform_data.camera_irradiance_size[1] = camera_position.y;
	uniform_data.camera_irradiance_size[2] = camera_position.z;
	uniform_data.atlas_sizes[0] = ddgi_state.ddgi_irradiance_atlas_width;
	uniform_data.atlas_sizes[1] = ddgi_state.ddgi_irradiance_atlas_height;
	uniform_data.atlas_sizes[2] = ddgi_state.ddgi_visibility_atlas_width;
	uniform_data.atlas_sizes[3] = ddgi_state.ddgi_visibility_atlas_height;
	uniform_data.tuning[0] = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/self_shadow_bias")), 0.0f, 2.0f);
	uniform_data.tuning[1] = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/view_bias")), 0.0f, 1.0f);
	uniform_data.tuning[2] = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/lod_transition")), 0.01f, 0.5f);
	uniform_data.screen_resolution_debug[0] = screen_size.x;
	uniform_data.screen_resolution_debug[1] = screen_size.y;
	uniform_data.screen_resolution_debug[2] = ddgi_state.ddgi_probe_resolution;
	const int32_t debug_mode = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/debug_mode")), 0, 18);
	uniform_data.screen_resolution_debug[3] = debug_mode;
	RD::get_singleton()->buffer_update(ddgi_state.ddgi_resolve_uniform_buffer[frame_resource], 0, sizeof(uniform_data), &uniform_data);

	const RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID shader = ddgi_resolve_shader.version_get_shader(ddgi_resolve_shader_version, 0);
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ p_sampler, face_data })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ p_sampler, hit_payload })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ output })));
	for (uint32_t lod = 0; lod < DDGI_LOD_COUNT; lod++) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3 + lod, Vector<RID>({ linear_sampler, ddgi_state.ddgi_cascades[lod].irradiance_rd })));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7 + lod, Vector<RID>({ linear_sampler, ddgi_state.ddgi_cascades[lod].visibility_rd })));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 11 + lod, Vector<RID>({ p_sampler, ddgi_state.ddgi_cascades[lod].metadata_rd })));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 15, Vector<RID>({ ddgi_state.ddgi_resolve_uniform_buffer[frame_resource] })));
	const RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache_vec(shader, 0, uniforms);
	if (!uniform_set.is_valid()) {
		disable_resolve();
		return;
	}

	if (ddgi_state.ddgi_temporal_debug_mode != debug_mode) {
		ddgi_state.ddgi_temporal_debug_mode = debug_mode;
		ddgi_state.ddgi_temporal_history_valid = false;
	}
	const uint32_t read_slot = ddgi_state.ddgi_temporal_slot;
	const uint32_t write_slot = read_slot ^ 1u;
	DdgiTemporalUniformData temporal_data = {};
	Projection previous_correction;
	previous_correction.set_depth_correction(true);
	const Projection previous_view_projection = (previous_correction * p_render_data->scene_data->prev_cam_projection) * Projection(p_render_data->scene_data->prev_cam_transform.affine_inverse());
	RendererRD::MaterialStorage::store_camera(previous_view_projection, temporal_data.previous_view_projection);
	temporal_data.screen_history[0] = screen_size.x;
	temporal_data.screen_history[1] = screen_size.y;
	temporal_data.screen_history[2] = ddgi_state.ddgi_temporal_history_valid ? 1 : 0;
	const float publication_fade_seconds = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/publication_fade_seconds")), 0.02f, 2.0f);
	const float frame_seconds = CLAMP(float(time_step), 1.0f / 240.0f, 0.1f);
	// Reach 95% of the new field in the requested duration. Debug views bypass
	// temporal publication so their diagnostics always describe the live data.
	temporal_data.temporal[0] = debug_mode == 0 ? 1.0f - Math::exp(Math::log(0.05f) * frame_seconds / publication_fade_seconds) : 1.0f;
	const VoxelForwardVolumeStorage::WorldOccupancy &world = volume_storage->get_world_occupancy();
	temporal_data.temporal[1] = MAX(world.voxel_size * 0.15f, 0.0001f);
	temporal_data.temporal[2] = 0.995f;
	RD::get_singleton()->buffer_update(ddgi_state.ddgi_temporal_uniform_buffer[frame_resource], 0, sizeof(temporal_data), &temporal_data);

	const RID temporal_shader = ddgi_temporal_shader.version_get_shader(ddgi_temporal_shader_version, 0);
	LocalVector<RD::Uniform> temporal_uniforms;
	temporal_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ p_sampler, output })));
	temporal_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ p_sampler, face_data })));
	temporal_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ p_sampler, ddgi_state.ddgi_temporal_output_rd[read_slot] })));
	temporal_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ p_sampler, ddgi_state.ddgi_temporal_face_rd[read_slot] })));
	temporal_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, Vector<RID>({ ddgi_state.ddgi_temporal_output_rd[write_slot] })));
	temporal_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, Vector<RID>({ ddgi_state.ddgi_temporal_face_rd[write_slot] })));
	temporal_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, Vector<RID>({ ddgi_state.ddgi_published_output_rd })));
	temporal_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 7, Vector<RID>({ ddgi_state.ddgi_temporal_uniform_buffer[frame_resource] })));
	const RID temporal_set = UniformSetCacheRD::get_singleton()->get_cache_vec(temporal_shader, 0, temporal_uniforms);
	if (!temporal_set.is_valid()) {
		disable_resolve();
		ddgi_state.ddgi_temporal_history_valid = false;
		return;
	}

	RENDER_TIMESTAMP("DDGI Screen Resolve");
	RD::get_singleton()->draw_command_begin_label("DDGI Screen Resolve");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, ddgi_resolve_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, screen_size.x, screen_size.y, 1);
	RD::get_singleton()->compute_list_add_barrier(compute_list);
	RENDER_TIMESTAMP("DDGI Temporal Publication");
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, ddgi_temporal_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, temporal_set, 0);
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, screen_size.x, screen_size.y, 1);
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();
	ddgi_state.ddgi_temporal_slot = write_slot;
	ddgi_state.ddgi_temporal_history_valid = true;
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_resolve_ready"), true);
}

void RenderVoxelForward::_render_voxel_gi(const RenderDataRD *p_render_data, RID p_shadow_atlas, RID p_sampler, const Vector3 &p_light_direction, const Color &p_light_color, float p_light_energy) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	const bool enabled = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/enabled"));
	if (!enabled || p_render_data == nullptr || p_render_data->scene_data == nullptr || !p_shadow_atlas.is_valid() || !p_sampler.is_valid() || !indirect_inject_pipeline.is_valid() || !indirect_propagate_pipeline.is_valid()) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), false);
		// Keep the cascade texture wrappers alive while disabled. Stable texture
		// RIDs keep material descriptor sets valid when GI is toggled or resized.
		return;
	}

	const VoxelForwardVolumeStorage::WorldOccupancy &world = volume_storage->get_world_occupancy();
	if (!world.directory_buffer.is_valid() || !world.brick_buffer.is_valid() || world.occupied_brick_count == 0 || world.voxel_size <= 0.0f) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), false);
		// If the world becomes empty, the retained textures no longer describe
		// a valid history for a later incremental insertion.
		indirect_world_revision = UINT64_MAX;
		for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
			indirect_staging_active[cascade] = false;
			indirect_staging_injected[cascade] = false;
			indirect_staging_scrolled[cascade] = false;
			indirect_staging_scroll_shift[cascade] = Vector3i();
			indirect_staging_boundary_waiting[cascade] = false;
			indirect_staging_boundary_ready[cascade] = false;
			indirect_blend_active[cascade] = false;
		}
		_release_indirect_light_snapshot();
		return;
	}
	for (uint32_t cascade = 0; cascade < REFLECTION_CASCADE_COUNT; cascade++) {
		if (!ddgi_state.reflection_color_grid_rd[cascade].is_valid() || !ddgi_state.reflection_material_grid_rd[cascade].is_valid() || !ddgi_state.reflection_color_grid_initialized[cascade]) {
			material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), false);
			return;
		}
	}
	// Never finish or blend an obsolete edit generation. The last coherent
	// active field remains visible while a fresh staging snapshot is scheduled.
	if (indirect_staging_world_owned && indirect_staging_world.revision != world.revision) {
		for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
			indirect_staging_active[cascade] = false;
			indirect_staging_injected[cascade] = false;
			indirect_staging_scrolled[cascade] = false;
			indirect_staging_scroll_shift[cascade] = Vector3i();
			indirect_staging_boundary_waiting[cascade] = false;
			indirect_staging_boundary_ready[cascade] = false;
			indirect_blend_active[cascade] = false;
		}
		indirect_staging_batch_classified = false;
		_release_indirect_light_snapshot();
	}
	indirect_render_frame_index++;

	const uint32_t resolution = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/resolution")), 24, 96));
	const float near_cell_size = MAX(world.voxel_size, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/near_cell_size")));
	const float far_cell_size = MAX(near_cell_size, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/far_cell_size")));
	const float distant_cell_size = MAX(far_cell_size, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/distant_cell_size")));
	const float cell_sizes[INDIRECT_CASCADE_COUNT] = { near_cell_size, far_cell_size, distant_cell_size };
	const int recenter_cells = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/recenter_cells")), 1, 16);
	const float recenter_hysteresis = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/recenter_hysteresis")), 0.55f, 0.95f);
	const int base_propagation_steps = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/propagation_steps")), 2, 16) & ~1;
	const float propagation_decay = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/propagation_decay")), 0.0f, 0.99f);
	const float invalidation_epsilon = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/invalidation_epsilon")), 0.001f, 0.25f);
	const float shadow_bias = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/shadow_bias_voxels"))) * world.voxel_size;
	const bool dirty_updates_enabled = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/dirty_updates_enabled"));
	const bool low_latency_dirty_updates_enabled = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/low_latency_dirty_updates_enabled"));
	const uint64_t dirty_cell_pass_budget = uint64_t(MAX(1, int(GLOBAL_GET("rendering/voxel_forward/indirect_light/dirty_cell_pass_budget_per_frame"))));
	const bool temporal_updates_enabled = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/temporal_updates_enabled"));
	const bool temporal_blend_enabled = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/temporal_blend_enabled")) && indirect_blend_pipeline.is_valid();
	const uint32_t temporal_blend_frames = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/temporal_blend_frames")), 1, 60));
	const uint32_t dispatch_budget = temporal_updates_enabled ? uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/dispatch_budget_per_frame")), 1, 16)) : 64u;
	const uint32_t blend_dispatch_budget = temporal_updates_enabled ? uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/blend_dispatch_budget_per_frame")), 1, int(INDIRECT_CASCADE_COUNT))) : INDIRECT_CASCADE_COUNT;
	const Vector3 camera_position = p_render_data->scene_data->cam_transform.origin;
	Vector3 grid_origins[INDIRECT_CASCADE_COUNT];
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		const float snap = cell_sizes[cascade] * float(recenter_cells);
		const float half_extent = cell_sizes[cascade] * float(resolution) * 0.5f;
		Vector3 snapped_center;
		if (indirect_cascade_initialized[cascade]) {
			// Keep the active clipmap fixed while the camera remains in its central
			// hysteresis region. The setting previously existed only in the UI: origins
			// still snapped every recenter_cells, causing constant rebuilds and visible
			// history lag during ordinary walking.
			snapped_center = indirect_active_definition[cascade].origin + Vector3(half_extent, half_extent, half_extent);
			const float threshold = MAX(snap, half_extent * recenter_hysteresis);
			for (int axis = 0; axis < 3; axis++) {
				const float offset = camera_position[axis] - snapped_center[axis];
				if (offset > threshold) {
					const int steps = MAX(1, int(Math::ceil((offset - threshold) / snap)));
					snapped_center[axis] += float(steps) * snap;
				} else if (offset < -threshold) {
					const int steps = MAX(1, int(Math::ceil((-threshold - offset) / snap)));
					snapped_center[axis] -= float(steps) * snap;
				}
			}
		} else {
			for (int axis = 0; axis < 3; axis++) {
				snapped_center[axis] = world.origin[axis] + Math::floor((camera_position[axis] - world.origin[axis]) / snap + 0.5f) * snap;
			}
		}
		grid_origins[cascade] = snapped_center - Vector3(half_extent, half_extent, half_extent);
	}

	if (indirect_grid_resolution != resolution) {
		_release_indirect_light_snapshot();
		RD::TextureFormat texture_format;
		texture_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		texture_format.width = resolution * INDIRECT_DIRECTION_COUNT;
		texture_format.height = resolution;
		texture_format.depth = resolution;
		texture_format.texture_type = RD::TEXTURE_TYPE_3D;
		texture_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
			RID replacement_rd[2];
			RID replacement_staging_rd[2];
			RID replacement_injection_rd = RD::get_singleton()->texture_create(texture_format, RD::TextureView());
			RD::get_singleton()->texture_clear(replacement_injection_rd, Color(0, 0, 0, 0), 0, 1, 0, 1);
			RD::get_singleton()->set_resource_name(replacement_injection_rd, vformat("Voxel GI Cascade %d Injection and Face Transmittance", cascade));
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
				RID previous_injection_rd = indirect_injection_grid_rd[cascade];
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
				if (RD::get_singleton()->texture_is_valid(previous_injection_rd)) {
					RD::get_singleton()->free_rid(previous_injection_rd);
				}
			} else {
				indirect_grid_texture[cascade] = texture_storage->texture_allocate();
				texture_storage->texture_rd_initialize(indirect_grid_texture[cascade], replacement_rd[0]);
			}
			indirect_grid_rd[cascade][0] = replacement_rd[0];
			indirect_grid_rd[cascade][1] = replacement_rd[1];
			indirect_staging_grid_rd[cascade][0] = replacement_staging_rd[0];
			indirect_staging_grid_rd[cascade][1] = replacement_staging_rd[1];
			indirect_injection_grid_rd[cascade] = replacement_injection_rd;
			indirect_cascade_initialized[cascade] = false;
			indirect_staging_active[cascade] = false;
			indirect_staging_injected[cascade] = false;
			indirect_staging_partial[cascade] = false;
			indirect_staging_scrolled[cascade] = false;
			indirect_staging_scroll_shift[cascade] = Vector3i();
			indirect_staging_adaptive[cascade] = false;
			indirect_staging_boundary_waiting[cascade] = false;
			indirect_staging_boundary_ready[cascade] = false;
			indirect_staging_expansion_count[cascade] = 0;
			indirect_staging_next_step[cascade] = 0;
			indirect_blend_active[cascade] = false;
			indirect_blend_has_history[cascade] = false;
			indirect_blend_step[cascade] = 0;
			indirect_blend_frame_count[cascade] = 0;
			indirect_active_converged[cascade] = false;
			indirect_active_definition[cascade] = IndirectCascadeDefinition();
			indirect_staging_definition[cascade] = IndirectCascadeDefinition();
			indirect_blend_definition[cascade] = IndirectCascadeDefinition();
			indirect_staging_detected_frame[cascade] = 0;
			indirect_blend_detected_frame[cascade] = 0;
		}
		indirect_staging_batch_classified = false;
		indirect_staging_batch_low_latency = false;
		indirect_staging_dirty_cell_count = 0;
		indirect_staging_cell_pass_workload = 0;
		indirect_staging_dispatch_count = 0;
		indirect_staging_batch_detected_frame = 0;
		indirect_grid_resolution = resolution;
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_near"), indirect_grid_texture[0]);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_far"), indirect_grid_texture[1]);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_distant"), indirect_grid_texture[2]);
		const uint64_t bytes_per_directional_texture = uint64_t(resolution) * uint64_t(resolution) * uint64_t(resolution) * INDIRECT_DIRECTION_COUNT * 8u;
		const uint64_t bytes_per_cascade = bytes_per_directional_texture * 5u;
		print_verbose(vformat("Voxel GI directional allocation: %d^3 cells, six RGBA16F lobes packed along X, %.2f MiB per cascade, %.2f MiB total.", resolution, double(bytes_per_cascade) / (1024.0 * 1024.0), double(bytes_per_cascade * INDIRECT_CASCADE_COUNT) / (1024.0 * 1024.0)));
	}

	const RID inject_shader_rid = indirect_inject_shader.version_get_shader(indirect_inject_shader_version, 0);
	const RID propagate_shader_rid = indirect_propagate_shader.version_get_shader(indirect_propagate_shader_version, 0);
	const RID blend_shader_rid = indirect_blend_shader.version_get_shader(indirect_blend_shader_version, 0);
	if (!indirect_color_grid_uniform_buffer.is_valid()) {
		indirect_color_grid_uniform_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(IndirectColorGridUniformData));
	}
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		if (!indirect_parent_grid_uniform_buffer[cascade].is_valid()) {
			indirect_parent_grid_uniform_buffer[cascade] = RD::get_singleton()->uniform_buffer_create(sizeof(IndirectParentGridUniformData));
		}
	}
	IndirectColorGridUniformData color_grid_data = {};
	for (uint32_t color_cascade = 0; color_cascade < REFLECTION_CASCADE_COUNT; color_cascade++) {
		color_grid_data.origin_cell_size[color_cascade][0] = ddgi_state.reflection_color_grid_origin[color_cascade].x;
		color_grid_data.origin_cell_size[color_cascade][1] = ddgi_state.reflection_color_grid_origin[color_cascade].y;
		color_grid_data.origin_cell_size[color_cascade][2] = ddgi_state.reflection_color_grid_origin[color_cascade].z;
		color_grid_data.origin_cell_size[color_cascade][3] = ddgi_state.reflection_color_grid_cell_size[color_cascade];
	}
	color_grid_data.resolution[0] = ddgi_state.reflection_color_grid_resolution;
	RD::get_singleton()->buffer_update(indirect_color_grid_uniform_buffer, 0, sizeof(IndirectColorGridUniformData), &color_grid_data);
	auto copy_directional_region = [resolution](RID p_source, RID p_destination, const Vector3i &p_origin, const Vector3i &p_size) {
		Error result = OK;
		for (uint32_t direction = 0; direction < INDIRECT_DIRECTION_COUNT; direction++) {
			const Vector3 source_origin(p_origin.x + int(direction * resolution), p_origin.y, p_origin.z);
			const Error copy_error = RD::get_singleton()->texture_copy(p_source, p_destination, source_origin, source_origin, Vector3(p_size), 0, 0, 0, 0);
			if (copy_error != OK) {
				result = copy_error;
			}
		}
		return result;
	};
	auto copy_directional_region_shifted = [resolution](RID p_source, RID p_destination, const Vector3i &p_source_origin, const Vector3i &p_destination_origin, const Vector3i &p_size) {
		Error result = OK;
		for (uint32_t direction = 0; direction < INDIRECT_DIRECTION_COUNT; direction++) {
			const Vector3 source_origin(p_source_origin.x + int(direction * resolution), p_source_origin.y, p_source_origin.z);
			const Vector3 destination_origin(p_destination_origin.x + int(direction * resolution), p_destination_origin.y, p_destination_origin.z);
			const Error copy_error = RD::get_singleton()->texture_copy(p_source, p_destination, source_origin, destination_origin, Vector3(p_size), 0, 0, 0, 0);
			if (copy_error != OK) {
				result = copy_error;
			}
		}
		return result;
	};
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
	auto definition_matches_except_origin = [](const IndirectCascadeDefinition &p_a, const IndirectCascadeDefinition &p_b) {
		return Math::is_equal_approx(p_a.cell_size, p_b.cell_size) && p_a.world_revision == p_b.world_revision &&
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
		indirect_staging_batch_classified = false;
		indirect_staging_batch_low_latency = false;
		indirect_staging_dirty_cell_count = 0;
		indirect_staging_cell_pass_workload = 0;
		indirect_staging_dispatch_count = 0;
		indirect_staging_batch_detected_frame = 0;
	}

	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		IndirectCascadeDefinition desired;
		desired.origin = grid_origins[cascade];
		desired.cell_size = cell_sizes[cascade];
		desired.world_revision = world.revision;
		desired.light_direction = p_light_direction;
		desired.light_color = p_light_color;
		desired.light_energy = p_light_energy;
		// One propagation pass advances one cell in the current cascade. Halving
		// the pass count for each coarser cascade silently capped Ultra's far field
		// to 3.2 m and its distant field to 6.4 m, so ordinary gameplay corridors
		// could not receive light from their opening. Keep the artist-selected pass
		// count for every cascade; coarser cells then provide the intended longer
		// physical transport range without increasing steady-state frame cost.
		desired.propagation_steps = base_propagation_steps;
		desired.propagation_decay = propagation_decay;
		desired.shadow_bias = shadow_bias;
		desired.dirty_updates_enabled = dirty_updates_enabled;

		if (indirect_blend_active[cascade]) {
			// Finish publishing the immutable completed target. Newer camera/world
			// definitions are picked up afterward instead of repeatedly canceling
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
			if (!volume_storage->create_world_occupancy_snapshot(indirect_staging_world)) {
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
		bool camera_scroll = false;
		Vector3i camera_scroll_shift;
		bool adaptive_update = false;
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
					adaptive_update = build_world.last_face_transmittance_decreased || build_world.last_face_transmittance_increased;
				}
			}
		}
		// A snapped camera recenter does not invalidate the world-space values in
		// the overlapping portion of a clipmap. Reuse that overlap and solve only
		// the newly exposed slab plus a propagation halo. Rebuilding all 64^3 cells
		// made the active field trail the camera by dozens of frames and caused the
		// same wall to change brightness depending on direction of travel.
		const bool can_scroll_from_active = !can_use_dirty_update && indirect_cascade_initialized[cascade] &&
				indirect_active_converged[cascade] && !indirect_active_definition[cascade].origin.is_equal_approx(desired.origin) &&
				definition_matches_except_origin(indirect_active_definition[cascade], desired);
		if (can_scroll_from_active) {
			const Vector3 shift_f = (desired.origin - indirect_active_definition[cascade].origin) / desired.cell_size;
			Vector3i shift;
			int shifted_axes = 0;
			bool integral_shift = true;
			for (int axis = 0; axis < 3; axis++) {
				shift[axis] = int(Math::round(shift_f[axis]));
				integral_shift = integral_shift && Math::is_equal_approx(shift_f[axis], float(shift[axis]));
				shifted_axes += shift[axis] != 0 ? 1 : 0;
			}
			// A single slab is rectangular and can be updated by the existing bounded
			// dispatch. Multi-axis teleports fall back to a coherent full rebuild;
			// representing their L-shaped exposed union as one box would be full-size
			// anyway.
			if (integral_shift && shifted_axes == 1 && Math::abs(shift.x) < int(resolution) && Math::abs(shift.y) < int(resolution) && Math::abs(shift.z) < int(resolution)) {
				const int padding_cells = desired.propagation_steps + 1;
				for (int axis = 0; axis < 3; axis++) {
					if (shift[axis] > 0) {
						dispatch_origin[axis] = MAX(0, int(resolution) - shift[axis] - padding_cells);
						dispatch_size[axis] = int(resolution) - dispatch_origin[axis];
					} else if (shift[axis] < 0) {
						dispatch_size[axis] = MIN(int(resolution), -shift[axis] + padding_cells);
					}
				}
				partial_update = true;
				camera_scroll = true;
				camera_scroll_shift = shift;
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
		indirect_staging_scrolled[cascade] = camera_scroll;
		indirect_staging_scroll_shift[cascade] = camera_scroll_shift;
		indirect_staging_adaptive[cascade] = adaptive_update;
		indirect_staging_boundary_waiting[cascade] = false;
		indirect_staging_boundary_ready[cascade] = false;
		indirect_staging_boundary_delta[cascade] = 0.0f;
		indirect_staging_expansion_count[cascade] = 0;
		indirect_staging_boundary_revision[cascade] = desired.world_revision;
		indirect_staging_detected_frame[cascade] = indirect_render_frame_index;
	}

	bool has_staging_work = false;
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		has_staging_work = has_staging_work || indirect_staging_active[cascade];
	}
	if (has_staging_work && !indirect_staging_batch_classified) {
		bool all_pending_work_is_coherent_fast = true;
		uint64_t dirty_cell_count = 0;
		uint64_t cell_pass_workload = 0;
		for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
			if (indirect_blend_active[cascade]) {
				all_pending_work_is_coherent_fast = false;
			}
			if (!indirect_staging_active[cascade]) {
				continue;
			}
			const bool camera_only_shift = indirect_cascade_initialized[cascade] &&
					definition_matches_except_origin(indirect_active_definition[cascade], indirect_staging_definition[cascade]);
			all_pending_work_is_coherent_fast = all_pending_work_is_coherent_fast && (indirect_staging_partial[cascade] || camera_only_shift);
			const Vector3i &dirty_size = indirect_staging_dispatch_size[cascade];
			const uint64_t cascade_cell_count = uint64_t(dirty_size.x) * uint64_t(dirty_size.y) * uint64_t(dirty_size.z);
			const uint32_t remaining_injection_passes = indirect_staging_injected[cascade] ? 0u : 1u;
			const uint32_t remaining_propagation_passes = uint32_t(indirect_staging_definition[cascade].propagation_steps) - indirect_staging_next_step[cascade];
			dirty_cell_count += cascade_cell_count;
			cell_pass_workload += cascade_cell_count * uint64_t(remaining_injection_passes + remaining_propagation_passes);
		}
		indirect_staging_batch_classified = true;
		// A camera-only clipmap shift must not trickle propagation across frames:
		// that made lighting at an identical world/camera position depend on the
		// direction of travel. Complete the coherent target and publish it once.
		indirect_staging_batch_low_latency = low_latency_dirty_updates_enabled && all_pending_work_is_coherent_fast && cell_pass_workload <= dirty_cell_pass_budget;
		indirect_staging_dirty_cell_count = dirty_cell_count;
		indirect_staging_cell_pass_workload = cell_pass_workload;
		indirect_staging_dispatch_count = 0;
		indirect_staging_batch_detected_frame = indirect_render_frame_index;
	}

	auto publish_completed_cascade = [&](uint32_t p_cascade, const IndirectCascadeDefinition &p_build) {
		const String publish_label = vformat("Indirect Cascade Publish (%s)", cascade_names[p_cascade]);
		RENDER_TIMESTAMP(publish_label);
		RD::get_singleton()->draw_command_begin_label(publish_label.utf8().span());
		const bool scrolled = indirect_staging_scrolled[p_cascade];
		const Vector3i publish_origin = scrolled ? Vector3i() : indirect_staging_dispatch_origin[p_cascade];
		const Vector3i publish_size = scrolled ? Vector3i(resolution, resolution, resolution) : indirect_staging_dispatch_size[p_cascade];
		if (temporal_blend_enabled && temporal_blend_frames > 1u && !indirect_staging_batch_low_latency && !scrolled) {
			// The staging result is complete and immutable. Blend only its updated
			// region; all other active texels remain untouched.
			indirect_blend_definition[p_cascade] = p_build;
			indirect_blend_history_origin[p_cascade] = indirect_grid_origin[p_cascade];
			indirect_blend_history_cell_size[p_cascade] = indirect_grid_cell_size[p_cascade];
			indirect_blend_dispatch_origin[p_cascade] = publish_origin;
			indirect_blend_dispatch_size[p_cascade] = publish_size;
			indirect_blend_step[p_cascade] = 0;
			indirect_blend_frame_count[p_cascade] = temporal_blend_frames;
			indirect_blend_has_history[p_cascade] = indirect_cascade_initialized[p_cascade];
			indirect_blend_detected_frame[p_cascade] = indirect_staging_detected_frame[p_cascade];
			indirect_blend_active[p_cascade] = true;
			indirect_staging_active[p_cascade] = false;
			indirect_staging_injected[p_cascade] = false;
			indirect_staging_scrolled[p_cascade] = false;
			indirect_staging_scroll_shift[p_cascade] = Vector3i();
			indirect_staging_adaptive[p_cascade] = false;
			indirect_staging_boundary_waiting[p_cascade] = false;
			indirect_staging_boundary_ready[p_cascade] = false;
			RD::get_singleton()->draw_command_end_label();
			return;
		}

		// Keep both the public wrapper RID and its underlying RD texture fixed.
		// Queue-ordered copying never exposes a partially written grid.
		const Error publish_error = copy_directional_region(indirect_staging_grid_rd[p_cascade][0], indirect_grid_rd[p_cascade][0], publish_origin, publish_size);
		RD::get_singleton()->draw_command_end_label();
		if (publish_error == OK) {
			indirect_active_definition[p_cascade] = p_build;
			indirect_grid_origin[p_cascade] = p_build.origin;
			indirect_grid_cell_size[p_cascade] = p_build.cell_size;
			indirect_cascade_initialized[p_cascade] = true;
			indirect_active_converged[p_cascade] = true;
			indirect_staging_active[p_cascade] = false;
			indirect_staging_injected[p_cascade] = false;
			indirect_staging_scrolled[p_cascade] = false;
			indirect_staging_scroll_shift[p_cascade] = Vector3i();
			indirect_staging_adaptive[p_cascade] = false;
			indirect_staging_boundary_waiting[p_cascade] = false;
			indirect_staging_boundary_ready[p_cascade] = false;
			if (!indirect_staging_batch_low_latency) {
				indirect_temporal_update_count++;
				indirect_last_publication_frames = uint32_t(indirect_render_frame_index - indirect_staging_detected_frame[p_cascade] + 1u);
				print_verbose(vformat("Voxel Forward indirect temporal cascade publication #%d (%s): %d frame(s) from detection to publication.", indirect_temporal_update_count, cascade_names[p_cascade], indirect_last_publication_frames));
			}
		} else {
			// Re-run the deterministic final pass before retrying publication.
			indirect_staging_next_step[p_cascade] = uint32_t(MAX(0, p_build.propagation_steps - 1));
		}
	};

	uint32_t remaining_staging_dispatches = 0;
	if (indirect_staging_batch_low_latency) {
		for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
			if (!indirect_staging_active[cascade]) {
				continue;
			}
			remaining_staging_dispatches += indirect_staging_boundary_ready[cascade] ? 1u : 0u;
			remaining_staging_dispatches += indirect_staging_injected[cascade] ? 0u : 1u;
			remaining_staging_dispatches += uint32_t(indirect_staging_definition[cascade].propagation_steps) - indirect_staging_next_step[cascade];
		}
	}
	uint32_t staging_dispatch_limit = indirect_staging_batch_low_latency ? remaining_staging_dispatches : dispatch_budget;
	if (!indirect_staging_batch_low_latency) {
		// Keep one scrolled cascade atomic. Its exposed slab is bounded by the dirty
		// workload budget, while splitting its Jacobi passes over many frames would
		// keep the old clipmap origin visible long after the camera crossed the snap.
		for (uint32_t probe = 0; probe < INDIRECT_CASCADE_COUNT; probe++) {
			const uint32_t cascade = INDIRECT_CASCADE_COUNT - 1u - probe;
			if (!indirect_staging_active[cascade] || !indirect_staging_scrolled[cascade]) {
				continue;
			}
			const Vector3i &region_size = indirect_staging_dispatch_size[cascade];
			const uint32_t remaining = (indirect_staging_injected[cascade] ? 0u : 1u) +
					uint32_t(indirect_staging_definition[cascade].propagation_steps) - indirect_staging_next_step[cascade];
			const uint64_t workload = uint64_t(region_size.x) * uint64_t(region_size.y) * uint64_t(region_size.z) * uint64_t(remaining);
			if (workload <= dirty_cell_pass_budget) {
				staging_dispatch_limit = MAX(staging_dispatch_limit, remaining);
			}
			break;
		}
	}
	uint32_t staging_dispatched = 0;
	if (has_staging_work) {
		if (indirect_staging_batch_low_latency) {
			RENDER_TIMESTAMP("Indirect Dirty Low-Latency Update");
			RD::get_singleton()->draw_command_begin_label("Indirect Dirty Low-Latency Update");
		} else {
			RENDER_TIMESTAMP("Voxel Indirect Light");
			RD::get_singleton()->draw_command_begin_label("Voxel Indirect Light");
		}
		// Injection and propagation own this budget. Temporal blending is a much
		// cheaper copy-like pass and has a separate bounded budget below, so a
		// completed cascade can no longer stall all remaining cascade generation.
		while (staging_dispatched < staging_dispatch_limit) {
			uint32_t cascade = INDIRECT_CASCADE_COUNT;
			for (uint32_t probe = 0; probe < INDIRECT_CASCADE_COUNT; probe++) {
				// A child consumes the completed parent result during injection. Solve
				// Distant -> Far -> Near and never inject a child from stale parent data.
				const uint32_t candidate = INDIRECT_CASCADE_COUNT - 1u - probe;
				bool parent_ready = true;
				if (candidate + 1u < INDIRECT_CASCADE_COUNT) {
					const uint32_t parent = candidate + 1u;
					const uint64_t child_revision = indirect_staging_definition[candidate].world_revision;
					parent_ready = !indirect_staging_active[parent] &&
							((indirect_blend_active[parent] && indirect_blend_definition[parent].world_revision == child_revision) ||
									(indirect_cascade_initialized[parent] && indirect_active_definition[parent].world_revision == child_revision));
				}
				if (indirect_staging_active[candidate] && !indirect_staging_boundary_waiting[candidate] && parent_ready) {
					cascade = candidate;
					break;
				}
			}
			if (cascade == INDIRECT_CASCADE_COUNT) {
				if (has_staging_work && indirect_render_frame_index % 60u == 0u) {
					print_verbose(vformat("Voxel GI scheduler waiting: active=%d/%d/%d injected=%d/%d/%d steps=%d/%d/%d blend=%d/%d/%d initialized=%d/%d/%d revisions=%d/%d/%d.",
							indirect_staging_active[0], indirect_staging_active[1], indirect_staging_active[2],
							indirect_staging_injected[0], indirect_staging_injected[1], indirect_staging_injected[2],
							indirect_staging_next_step[0], indirect_staging_next_step[1], indirect_staging_next_step[2],
							indirect_blend_active[0], indirect_blend_active[1], indirect_blend_active[2],
							indirect_cascade_initialized[0], indirect_cascade_initialized[1], indirect_cascade_initialized[2],
							indirect_staging_definition[0].world_revision, indirect_active_definition[1].world_revision, indirect_active_definition[2].world_revision));
				}
				break;
			}
			if (!indirect_staging_batch_low_latency) {
				indirect_schedule_cursor = (cascade + 1u) % INDIRECT_CASCADE_COUNT;
			}
			const IndirectCascadeDefinition &build = indirect_staging_definition[cascade];
			const VoxelForwardVolumeStorage::WorldOccupancy &build_world = indirect_staging_world_owned ? indirect_staging_world : world;
			Vector3i dispatch_origin = indirect_staging_dispatch_origin[cascade];
			Vector3i dispatch_size = indirect_staging_dispatch_size[cascade];

			if (indirect_staging_boundary_ready[cascade]) {
				const float boundary_delta = indirect_staging_boundary_delta[cascade];
				indirect_last_boundary_delta = boundary_delta;
				indirect_staging_boundary_ready[cascade] = false;
				const Vector3i old_end = dispatch_origin + dispatch_size;
				Vector3i expanded_origin = dispatch_origin;
				Vector3i expanded_end = old_end;
				if (boundary_delta > invalidation_epsilon && indirect_staging_adaptive[cascade] && indirect_staging_partial[cascade]) {
					const int expansion = MAX(1, build.propagation_steps);
					for (int axis = 0; axis < 3; axis++) {
						expanded_origin[axis] = MAX(0, expanded_origin[axis] - expansion);
						expanded_end[axis] = MIN(int(resolution), expanded_end[axis] + expansion);
					}
				}
				if (expanded_origin != dispatch_origin || expanded_end != old_end) {
					const uint64_t old_cells = uint64_t(dispatch_size.x) * uint64_t(dispatch_size.y) * uint64_t(dispatch_size.z);
					dispatch_origin = expanded_origin;
					dispatch_size = expanded_end - expanded_origin;
					const uint64_t expanded_cells = uint64_t(dispatch_size.x) * uint64_t(dispatch_size.y) * uint64_t(dispatch_size.z);
					indirect_staging_dispatch_origin[cascade] = dispatch_origin;
					indirect_staging_dispatch_size[cascade] = dispatch_size;
					indirect_staging_next_step[cascade] = 0;
					indirect_staging_injected[cascade] = false;
					indirect_staging_expansion_count[cascade]++;
					indirect_staging_dirty_cell_count += expanded_cells - old_cells;
					indirect_staging_cell_pass_workload += expanded_cells * uint64_t(1 + build.propagation_steps);
					if (indirect_staging_batch_low_latency && indirect_staging_cell_pass_workload > dirty_cell_pass_budget) {
						indirect_staging_batch_low_latency = false;
						staging_dispatch_limit = staging_dispatched + dispatch_budget;
					}
				} else {
					indirect_last_invalidated_cell_count = uint64_t(dispatch_size.x) * uint64_t(dispatch_size.y) * uint64_t(dispatch_size.z);
					indirect_last_convergence_expansions = indirect_staging_expansion_count[cascade];
					indirect_staging_adaptive[cascade] = false;
					publish_completed_cascade(cascade, build);
					continue;
				}
			}

			if (!indirect_staging_injected[cascade]) {
				if (indirect_staging_partial[cascade]) {
					const Vector3i full_size(resolution, resolution, resolution);
					Error copy_0 = OK;
					Error copy_1 = OK;
					if (indirect_staging_scrolled[cascade]) {
						const Vector3i shift = indirect_staging_scroll_shift[cascade];
						const Vector3i source_origin(MAX(shift.x, 0), MAX(shift.y, 0), MAX(shift.z, 0));
						const Vector3i destination_origin(MAX(-shift.x, 0), MAX(-shift.y, 0), MAX(-shift.z, 0));
						const Vector3i overlap_size(int(resolution) - Math::abs(shift.x), int(resolution) - Math::abs(shift.y), int(resolution) - Math::abs(shift.z));
						copy_0 = copy_directional_region_shifted(indirect_grid_rd[cascade][0], indirect_staging_grid_rd[cascade][0], source_origin, destination_origin, overlap_size);
						copy_1 = copy_directional_region_shifted(indirect_grid_rd[cascade][0], indirect_staging_grid_rd[cascade][1], source_origin, destination_origin, overlap_size);
					} else {
						copy_0 = copy_directional_region(indirect_grid_rd[cascade][0], indirect_staging_grid_rd[cascade][0], Vector3i(), full_size);
						copy_1 = copy_directional_region(indirect_grid_rd[cascade][0], indirect_staging_grid_rd[cascade][1], Vector3i(), full_size);
					}
					if (copy_0 != OK || copy_1 != OK) {
						if (indirect_staging_batch_low_latency) {
							// A failed history seed promotes this cascade to a full-grid
							// rebuild. Keep that fallback on the ordinary bounded path.
							const Vector3i &partial_size = indirect_staging_dispatch_size[cascade];
							const uint64_t partial_cells = uint64_t(partial_size.x) * uint64_t(partial_size.y) * uint64_t(partial_size.z);
							const uint64_t full_cells = uint64_t(resolution) * uint64_t(resolution) * uint64_t(resolution);
							const uint32_t remaining_passes = 1u + uint32_t(indirect_staging_definition[cascade].propagation_steps) - indirect_staging_next_step[cascade];
							indirect_staging_dirty_cell_count += full_cells - partial_cells;
							indirect_staging_cell_pass_workload += (full_cells - partial_cells) * uint64_t(remaining_passes);
							indirect_staging_batch_low_latency = false;
							staging_dispatch_limit = staging_dispatched + dispatch_budget;
						}
						indirect_staging_partial[cascade] = false;
						indirect_staging_scrolled[cascade] = false;
						indirect_staging_scroll_shift[cascade] = Vector3i();
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
				RD::Uniform u_injection(RD::UNIFORM_TYPE_IMAGE, 4, Vector<RID>({ indirect_injection_grid_rd[cascade] }));
				RD::Uniform u_color_near(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ p_sampler, ddgi_state.reflection_color_grid_rd[0] }));
				RD::Uniform u_color_far(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ p_sampler, ddgi_state.reflection_color_grid_rd[1] }));
				RD::Uniform u_color_distant(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ p_sampler, ddgi_state.reflection_color_grid_rd[2] }));
				RD::Uniform u_material_near(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ p_sampler, ddgi_state.reflection_material_grid_rd[0] }));
				RD::Uniform u_material_far(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ p_sampler, ddgi_state.reflection_material_grid_rd[1] }));
				RD::Uniform u_material_distant(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, Vector<RID>({ p_sampler, ddgi_state.reflection_material_grid_rd[2] }));
				RD::Uniform u_color_grid_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 11, Vector<RID>({ indirect_color_grid_uniform_buffer }));
				IndirectParentGridUniformData parent_data = {};
				RID parent_grid = indirect_grid_rd[INDIRECT_CASCADE_COUNT - 1u][0];
				if (cascade + 1u < INDIRECT_CASCADE_COUNT) {
					const uint32_t parent = cascade + 1u;
					const bool use_staging_parent = indirect_blend_active[parent] && indirect_blend_definition[parent].world_revision == build.world_revision;
					const IndirectCascadeDefinition &parent_definition = use_staging_parent ? indirect_blend_definition[parent] : indirect_active_definition[parent];
					parent_grid = use_staging_parent ? indirect_staging_grid_rd[parent][0] : indirect_grid_rd[parent][0];
					parent_data.origin_cell_size[0] = parent_definition.origin.x;
					parent_data.origin_cell_size[1] = parent_definition.origin.y;
					parent_data.origin_cell_size[2] = parent_definition.origin.z;
					parent_data.origin_cell_size[3] = parent_definition.cell_size;
					parent_data.state[0] = 1;
					parent_data.state[1] = resolution;
				}
				RD::get_singleton()->buffer_update(indirect_parent_grid_uniform_buffer[cascade], 0, sizeof(IndirectParentGridUniformData), &parent_data);
				RD::Uniform u_parent_grid(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 12, Vector<RID>({ p_sampler, parent_grid }));
				RD::Uniform u_parent_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 13, Vector<RID>({ indirect_parent_grid_uniform_buffer[cascade] }));
				const RID inject_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(inject_shader_rid, 0, u_output, u_atlas, u_directory, u_bricks, u_injection, u_color_near, u_color_far, u_color_distant, u_material_near, u_material_far, u_material_distant, u_color_grid_params, u_parent_grid, u_parent_params);

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
				inject_push.atlas_center_resolution[0] = ddgi_state.shadow_atlas_center.x;
				inject_push.atlas_center_resolution[1] = ddgi_state.shadow_atlas_center.y;
				inject_push.atlas_center_resolution[2] = ddgi_state.shadow_atlas_center.z;
				inject_push.atlas_center_resolution[3] = ddgi_state.shadow_atlas_resolution;
				inject_push.tangent_near_extent[0] = ddgi_state.shadow_atlas_tangent.x;
				inject_push.tangent_near_extent[1] = ddgi_state.shadow_atlas_tangent.y;
				inject_push.tangent_near_extent[2] = ddgi_state.shadow_atlas_tangent.z;
				inject_push.tangent_near_extent[3] = ddgi_state.shadow_atlas_near_extent;
				inject_push.bitangent_far_extent[0] = ddgi_state.shadow_atlas_bitangent.x;
				inject_push.bitangent_far_extent[1] = ddgi_state.shadow_atlas_bitangent.y;
				inject_push.bitangent_far_extent[2] = ddgi_state.shadow_atlas_bitangent.z;
				inject_push.bitangent_far_extent[3] = ddgi_state.shadow_atlas_far_extent;
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
				indirect_staging_dispatch_count++;
				continue;
			}

			const uint32_t step = indirect_staging_next_step[cascade];
			const uint32_t source = step & 1u;
			const uint32_t destination = source ^ 1u;
			RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ p_sampler, indirect_staging_grid_rd[cascade][source] }));
			RD::Uniform u_destination(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ indirect_staging_grid_rd[cascade][destination] }));
			RD::Uniform u_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ build_world.directory_buffer }));
			RD::Uniform u_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ build_world.brick_buffer }));
			RD::Uniform u_injection(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ p_sampler, indirect_injection_grid_rd[cascade] }));
			const RID propagate_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(propagate_shader_rid, 0, u_source, u_destination, u_directory, u_bricks, u_injection);

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
			indirect_staging_dispatch_count++;

			if (indirect_staging_next_step[cascade] == uint32_t(build.propagation_steps)) {
				// Every configured cascade pass count is even. The completed result is
				// therefore staging buffer 0 after 0->1, 1->0 ping-pong propagation.
				if (indirect_staging_adaptive[cascade] && indirect_staging_partial[cascade] &&
						indirect_boundary_delta_pipeline.is_valid() && indirect_boundary_delta_buffer[cascade].is_valid()) {
					RD::get_singleton()->buffer_clear(indirect_boundary_delta_buffer[cascade], 0, sizeof(uint32_t));
					RD::Uniform u_active(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ p_sampler, indirect_grid_rd[cascade][0] }));
					RD::Uniform u_staging(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ p_sampler, indirect_staging_grid_rd[cascade][0] }));
					RD::Uniform u_result(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ indirect_boundary_delta_buffer[cascade] }));
					const RID boundary_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(indirect_boundary_delta_shader.version_get_shader(indirect_boundary_delta_shader_version, 0), 0, u_active, u_staging, u_result);
					IndirectBoundaryDeltaPushConstant boundary_push = {};
					boundary_push.dispatch_origin_resolution[0] = dispatch_origin.x;
					boundary_push.dispatch_origin_resolution[1] = dispatch_origin.y;
					boundary_push.dispatch_origin_resolution[2] = dispatch_origin.z;
					boundary_push.dispatch_origin_resolution[3] = resolution;
					boundary_push.dispatch_size[0] = dispatch_size.x;
					boundary_push.dispatch_size[1] = dispatch_size.y;
					boundary_push.dispatch_size[2] = dispatch_size.z;
					RENDER_TIMESTAMP("Indirect Boundary Convergence");
					RD::get_singleton()->draw_command_begin_label("Indirect Boundary Convergence");
					RD::ComputeListID boundary_list = RD::get_singleton()->compute_list_begin();
					RD::get_singleton()->compute_list_bind_compute_pipeline(boundary_list, indirect_boundary_delta_pipeline);
					RD::get_singleton()->compute_list_bind_uniform_set(boundary_list, boundary_uniform_set, 0);
					RD::get_singleton()->compute_list_set_push_constant(boundary_list, &boundary_push, sizeof(IndirectBoundaryDeltaPushConstant));
					RD::get_singleton()->compute_list_dispatch_threads(boundary_list, dispatch_size.x, dispatch_size.y, dispatch_size.z);
					RD::get_singleton()->compute_list_end();
					RD::get_singleton()->draw_command_end_label();
					indirect_staging_boundary_waiting[cascade] = true;
					indirect_staging_boundary_ready[cascade] = false;
					indirect_staging_boundary_revision[cascade] = build.world_revision;
					const Error readback_error = RD::get_singleton()->buffer_get_data_async(indirect_boundary_delta_buffer[cascade],
							callable_mp(indirect_boundary_readback_receiver, &IndirectBoundaryReadbackReceiver::completed).bind(cascade, build.world_revision), 0, sizeof(uint32_t));
					if (readback_error != OK) {
						indirect_staging_boundary_waiting[cascade] = false;
						indirect_staging_boundary_ready[cascade] = true;
						indirect_staging_boundary_delta[cascade] = 1e20f;
					}
				} else {
					indirect_last_invalidated_cell_count = uint64_t(dispatch_size.x) * uint64_t(dispatch_size.y) * uint64_t(dispatch_size.z);
					indirect_last_convergence_expansions = indirect_staging_expansion_count[cascade];
					publish_completed_cascade(cascade, build);
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
		if (indirect_staging_batch_classified) {
			indirect_last_publication_frames = uint32_t(indirect_render_frame_index - indirect_staging_batch_detected_frame + 1u);
			if (indirect_staging_batch_low_latency) {
				indirect_low_latency_update_count++;
				print_verbose(vformat("Voxel Forward indirect low-latency update #%d: %d dirty cells, %d cell-passes, %d dispatches, %d frame(s) to publication.", indirect_low_latency_update_count, indirect_staging_dirty_cell_count, indirect_staging_cell_pass_workload, indirect_staging_dispatch_count, indirect_last_publication_frames));
			}
			indirect_staging_batch_classified = false;
			indirect_staging_batch_low_latency = false;
			indirect_staging_dirty_cell_count = 0;
			indirect_staging_cell_pass_workload = 0;
			indirect_staging_dispatch_count = 0;
			indirect_staging_batch_detected_frame = 0;
		}
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

		const Error blend_copy_error = copy_directional_region(indirect_grid_rd[cascade][1], indirect_grid_rd[cascade][0], dispatch_origin, dispatch_size);
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
			indirect_temporal_update_count++;
			indirect_last_publication_frames = uint32_t(indirect_render_frame_index - indirect_blend_detected_frame[cascade] + 1u);
			print_verbose(vformat("Voxel Forward indirect temporal cascade publication #%d (%s): %d frame(s) from detection to publication.", indirect_temporal_update_count, cascade_names[cascade], indirect_last_publication_frames));
		}
	}

	indirect_world_revision = world.revision;
	indirect_light_direction = p_light_direction;
	indirect_light_color = p_light_color;
	indirect_light_energy = p_light_energy;
	indirect_dirty_updates_enabled = dirty_updates_enabled;

	// Spatial debug metadata is published only as tiny uniforms. The normal
	// render path never binds or samples staging textures.
	static const StringName dirty_min_names[INDIRECT_CASCADE_COUNT] = {
		SNAME("voxel_forward_indirect_dirty_min0"), SNAME("voxel_forward_indirect_dirty_min1"), SNAME("voxel_forward_indirect_dirty_min2")
	};
	static const StringName dirty_max_names[INDIRECT_CASCADE_COUNT] = {
		SNAME("voxel_forward_indirect_dirty_max0"), SNAME("voxel_forward_indirect_dirty_max1"), SNAME("voxel_forward_indirect_dirty_max2")
	};
	int staging_mask = 0;
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		Vector3 dirty_min(1e20, 1e20, 1e20);
		Vector3 dirty_max(-1e20, -1e20, -1e20);
		const bool staging_visible = indirect_staging_active[cascade] || indirect_blend_active[cascade];
		if (staging_visible) {
			staging_mask |= 1 << cascade;
			const IndirectCascadeDefinition &definition = indirect_blend_active[cascade] ? indirect_blend_definition[cascade] : indirect_staging_definition[cascade];
			const Vector3i region_origin = indirect_blend_active[cascade] ? indirect_blend_dispatch_origin[cascade] : indirect_staging_dispatch_origin[cascade];
			const Vector3i region_size = indirect_blend_active[cascade] ? indirect_blend_dispatch_size[cascade] : indirect_staging_dispatch_size[cascade];
			dirty_min = definition.origin + Vector3(region_origin) * definition.cell_size;
			dirty_max = definition.origin + Vector3(region_origin + region_size) * definition.cell_size;
		}
		material_storage->global_shader_parameter_set_override(dirty_min_names[cascade], dirty_min);
		material_storage->global_shader_parameter_set_override(dirty_max_names[cascade], dirty_max);
	}
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_staging_mask"), staging_mask);
	Vector3 active_revisions(-1, -1, -1);
	Vector3 staging_revisions(-1, -1, -1);
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		if (indirect_cascade_initialized[cascade] && indirect_active_definition[cascade].world_revision != UINT64_MAX) {
			active_revisions[cascade] = double(indirect_active_definition[cascade].world_revision);
		}
		if ((indirect_staging_active[cascade] || indirect_blend_active[cascade]) && indirect_staging_definition[cascade].world_revision != UINT64_MAX) {
			staging_revisions[cascade] = double(indirect_staging_definition[cascade].world_revision);
		}
	}
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_active_revisions"), active_revisions);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_staging_revisions"), staging_revisions);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_update_metrics"), Vector4(double(indirect_last_invalidated_cell_count), double(indirect_last_convergence_expansions), double(indirect_last_publication_frames), indirect_last_boundary_delta));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_topology_edit"), Vector4(double(world.last_added_voxel_count), double(world.last_removed_voxel_count), world.last_face_transmittance_decreased ? 1.0 : 0.0, world.last_face_transmittance_increased ? 1.0 : 0.0));

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
	const bool indirect_needs_color = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/enabled")) && int(GLOBAL_GET("rendering/voxel_forward/indirect_light/backend")) != 0;
	if ((!enabled && !indirect_needs_color) || p_render_data == nullptr || p_render_data->scene_data == nullptr || p_render_data->scene_data->view_count != 1 || !reflection_color_inject_pipeline.is_valid()) {
		disable_reflections();
		return;
	}
	const VoxelForwardVolumeStorage::WorldOccupancy &world = volume_storage->get_world_occupancy();
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

	if (ddgi_state.reflection_color_grid_resolution != resolution) {
		RD::TextureFormat format;
		format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		format.width = resolution;
		format.height = resolution;
		format.depth = resolution;
		format.texture_type = RD::TEXTURE_TYPE_3D;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		for (uint32_t cascade = 0; cascade < REFLECTION_CASCADE_COUNT; cascade++) {
			if (ddgi_state.reflection_color_grid_rd[cascade].is_valid()) {
				RD::get_singleton()->free_rid(ddgi_state.reflection_color_grid_rd[cascade]);
			}
			ddgi_state.reflection_color_grid_rd[cascade] = RD::get_singleton()->texture_create(format, RD::TextureView());
			if (ddgi_state.reflection_material_grid_rd[cascade].is_valid()) {
				RD::get_singleton()->free_rid(ddgi_state.reflection_material_grid_rd[cascade]);
			}
			ddgi_state.reflection_material_grid_rd[cascade] = RD::get_singleton()->texture_create(format, RD::TextureView());
			RD::get_singleton()->set_resource_name(ddgi_state.reflection_color_grid_rd[cascade], vformat("Voxel Reflection Color Cascade %d", cascade));
			RD::get_singleton()->set_resource_name(ddgi_state.reflection_material_grid_rd[cascade], vformat("Voxel Material Properties Cascade %d", cascade));
			ddgi_state.reflection_color_grid_initialized[cascade] = false;
		}
		ddgi_state.reflection_color_grid_resolution = resolution;
	}

	const uint64_t shading_revision = volume_storage->get_shading_texture_revision();
	bool cascade_invalid[REFLECTION_CASCADE_COUNT] = {};
	bool any_invalid = ddgi_state.reflection_color_world_revision != world.revision || ddgi_state.reflection_color_shading_revision != shading_revision;
	for (uint32_t cascade = 0; cascade < REFLECTION_CASCADE_COUNT; cascade++) {
		cascade_invalid[cascade] = ddgi_state.reflection_color_world_revision != world.revision || ddgi_state.reflection_color_shading_revision != shading_revision || !ddgi_state.reflection_color_grid_initialized[cascade] ||
				!ddgi_state.reflection_color_grid_origin[cascade].is_equal_approx(grid_origins[cascade]) ||
				!Math::is_equal_approx(ddgi_state.reflection_color_grid_cell_size[cascade], cell_sizes[cascade]);
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
			RD::get_singleton()->texture_clear(ddgi_state.reflection_color_grid_rd[cascade], Color(0, 0, 0, 0), 0, 1, 0, 1);
			RD::get_singleton()->texture_clear(ddgi_state.reflection_material_grid_rd[cascade], Color(0, 0, 0, 0), 0, 1, 0, 1);
			const AABB grid_bounds(grid_origins[cascade], Vector3(1, 1, 1) * (cell_sizes[cascade] * float(resolution)));
			for (const KeyValue<RID, VoxelForwardVolumeStorage::Volume> &entry : volume_storage->get_volumes()) {
				const VoxelForwardVolumeStorage::Volume &volume = entry.value;
				RID voxel_texture = texture_storage->texture_get_rd_texture(volume.voxel_texture);
				RID brick_texture = texture_storage->texture_get_rd_texture(volume.brick_texture);
				RID palette_texture = texture_storage->texture_get_rd_texture(volume.palette_texture, true);
				RID material_texture = texture_storage->texture_get_rd_texture(volume.material_texture, true);
				RID transparency_texture = texture_storage->texture_get_rd_texture(volume.transparency_texture, true);
				RID metallic_texture = texture_storage->texture_get_rd_texture(volume.metallic_texture, true);
				RID specularity_texture = texture_storage->texture_get_rd_texture(volume.specularity_texture, true);
				RID emission_texture = texture_storage->texture_get_rd_texture(volume.emission_texture, true);
				if (!voxel_texture.is_valid() || !brick_texture.is_valid() || !palette_texture.is_valid() || !material_texture.is_valid() || !transparency_texture.is_valid() || !metallic_texture.is_valid() || !specularity_texture.is_valid() || !emission_texture.is_valid()) {
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

				RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ ddgi_state.reflection_color_grid_rd[cascade] }));
				RD::Uniform u_voxels(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, voxel_texture }));
				RD::Uniform u_bricks(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, brick_texture }));
				RD::Uniform u_palette(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ sampler, palette_texture }));
				RD::Uniform u_material(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ sampler, material_texture }));
				RD::Uniform u_material_output(RD::UNIFORM_TYPE_IMAGE, 5, Vector<RID>({ ddgi_state.reflection_material_grid_rd[cascade] }));
				RD::Uniform u_transparency(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ sampler, transparency_texture }));
				RD::Uniform u_metallic(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ sampler, metallic_texture }));
				RD::Uniform u_specularity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ sampler, specularity_texture }));
				RD::Uniform u_emission(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ sampler, emission_texture }));
				const RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(inject_shader_rid, 0, u_output, u_voxels, u_bricks, u_palette, u_material, u_material_output, u_transparency, u_metallic, u_specularity, u_emission);

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
				push.dispatch_origin[3] = (volume.has_transparency_texture ? 1 : 0) |
						(volume.has_metallic_texture ? 2 : 0) |
						(volume.has_specularity_texture ? 4 : 0) |
						(volume.has_emission_texture ? 8 : 0);
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
			ddgi_state.reflection_color_grid_origin[cascade] = grid_origins[cascade];
			ddgi_state.reflection_color_grid_cell_size[cascade] = cell_sizes[cascade];
			ddgi_state.reflection_color_grid_initialized[cascade] = true;
		}
		RD::get_singleton()->draw_command_end_label();
		ddgi_state.reflection_color_world_revision = world.revision;
		ddgi_state.reflection_color_shading_revision = shading_revision;
		print_verbose(vformat("Voxel Forward reflection color clipmaps updated: %d^3 x 3, %.2f / %.2f / %.2f m cells, revision %d.", resolution, near_cell_size, far_cell_size, distant_cell_size, world.revision));
	}
	if (!enabled || p_render_data->render_buffers.is_null() || !reflection_resolve_pipeline.is_valid() || !ddgi_state.reflection_uniform_buffer.is_valid()) {
		disable_reflections();
		return;
	}

	Ref<RenderSceneBuffersRD> render_buffers = p_render_data->render_buffers;
	const Size2i internal_size = render_buffers->get_internal_size();
	const float resolution_scale = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/reflections/resolution_scale")), 0.25f, 1.0f);
	const Size2i screen_size(MAX(1, int(Math::ceil(internal_size.x * resolution_scale))), MAX(1, int(Math::ceil(internal_size.y * resolution_scale))));
	const StringName scope = SNAME("voxel_forward_reflection");
	const StringName texture_name = SNAME("reflection_radiance");
	if (render_buffers->has_texture(scope, texture_name) && ddgi_state.reflection_screen_size != screen_size) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_reflection"), Variant());
		if (ddgi_state.reflection_texture.is_valid()) {
			texture_storage->texture_free(ddgi_state.reflection_texture);
			ddgi_state.reflection_texture = RID();
		}
		render_buffers->clear_context(scope);
		ddgi_state.reflection_source_rd = RID();
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
	if (ddgi_state.reflection_source_rd != output) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_reflection"), Variant());
		if (ddgi_state.reflection_texture.is_valid()) {
			texture_storage->texture_free(ddgi_state.reflection_texture);
		}
		ddgi_state.reflection_texture = texture_storage->texture_allocate();
		texture_storage->texture_rd_initialize(ddgi_state.reflection_texture, output);
		ddgi_state.reflection_source_rd = output;
		ddgi_state.reflection_resolve_cache_valid = false;
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_reflection"), ddgi_state.reflection_texture);
	}
	if (ddgi_context_changed) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_reflection"), ddgi_state.reflection_texture);
	}
	ddgi_state.reflection_screen_size = screen_size;

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
		reflection_data.color_grid_origin_cell_size[cascade][0] = ddgi_state.reflection_color_grid_origin[cascade].x;
		reflection_data.color_grid_origin_cell_size[cascade][1] = ddgi_state.reflection_color_grid_origin[cascade].y;
		reflection_data.color_grid_origin_cell_size[cascade][2] = ddgi_state.reflection_color_grid_origin[cascade].z;
		reflection_data.color_grid_origin_cell_size[cascade][3] = ddgi_state.reflection_color_grid_cell_size[cascade];
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
	bool indirect_update_in_progress = false;
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		indirect_update_in_progress = indirect_update_in_progress || indirect_staging_active[cascade] || indirect_blend_active[cascade];
	}
	const bool can_reuse_resolve = ddgi_state.reflection_resolve_cache_valid && !indirect_update_in_progress && ddgi_state.reflection_last_output == output &&
			ddgi_state.reflection_last_world_revision == world.revision && ddgi_state.reflection_last_color_world_revision == ddgi_state.reflection_color_world_revision &&
			ddgi_state.reflection_last_indirect_low_latency_update_count == indirect_low_latency_update_count &&
			ddgi_state.reflection_last_indirect_update_count == indirect_temporal_update_count &&
			memcmp(&ddgi_state.reflection_last_uniform_data, &reflection_data, sizeof(ReflectionUniformData)) == 0;
	if (can_reuse_resolve) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_reflection_intensity"), MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/reflections/intensity"))));
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_reflection_ready"), true);
		return;
	}
	RD::get_singleton()->buffer_update(ddgi_state.reflection_uniform_buffer, 0, sizeof(ReflectionUniformData), &reflection_data);

	RID indirect_textures[INDIRECT_CASCADE_COUNT];
	for (uint32_t cascade = 0; cascade < INDIRECT_CASCADE_COUNT; cascade++) {
		indirect_textures[cascade] = indirect_grid_rd[cascade][0].is_valid() ? indirect_grid_rd[cascade][0] : ddgi_state.reflection_color_grid_rd[cascade];
	}
	const RID resolve_shader_rid = reflection_resolve_shader.version_get_shader(reflection_resolve_shader_version, 0);
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, depth }));
	RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ output }));
	RD::Uniform u_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ world.directory_buffer }));
	RD::Uniform u_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ world.brick_buffer }));
	RD::Uniform u_color_near(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ sampler, ddgi_state.reflection_color_grid_rd[0] }));
	RD::Uniform u_color_far(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ sampler, ddgi_state.reflection_color_grid_rd[1] }));
	RD::Uniform u_color_distant(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ sampler, ddgi_state.reflection_color_grid_rd[2] }));
	RD::Uniform u_indirect_near(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ sampler, indirect_textures[0] }));
	RD::Uniform u_indirect_far(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ sampler, indirect_textures[1] }));
	RD::Uniform u_indirect_distant(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ sampler, indirect_textures[2] }));
	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 10, Vector<RID>({ ddgi_state.reflection_uniform_buffer }));
	const RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(resolve_shader_rid, 0, u_depth, u_output, u_directory, u_bricks, u_color_near, u_color_far, u_color_distant, u_indirect_near, u_indirect_far, u_indirect_distant, u_params);

	RENDER_TIMESTAMP("Voxel Face Reflections");
	RD::get_singleton()->draw_command_begin_label("Voxel Face Reflections");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, reflection_resolve_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, screen_size.x, screen_size.y, 1);
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();
	ddgi_state.reflection_last_uniform_data = reflection_data;
	ddgi_state.reflection_last_output = output;
	ddgi_state.reflection_last_world_revision = world.revision;
	ddgi_state.reflection_last_color_world_revision = ddgi_state.reflection_color_world_revision;
	ddgi_state.reflection_last_indirect_low_latency_update_count = indirect_low_latency_update_count;
	ddgi_state.reflection_last_indirect_update_count = indirect_temporal_update_count;
	ddgi_state.reflection_resolve_cache_valid = true;
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_reflection_intensity"), MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/reflections/intensity"))));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_reflection_ready"), true);
}

void RenderVoxelForward::_render_local_shadows(const RenderDataRD *p_render_data) {
	local_shadow_masks_ready = false;
	if (p_render_data == nullptr || p_render_data->render_buffers.is_null() || p_render_data->scene_data == nullptr ||
			p_render_data->scene_data->view_count != 1 || !local_shadow_resolve_pipeline.is_valid() || !local_shadow_uniform_buffer.is_valid() ||
			!bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/enabled"))) {
		return;
	}

	const VoxelForwardVolumeStorage::WorldOccupancy &world = volume_storage->get_world_occupancy();
	Ref<RenderSceneBuffersRD> render_buffers = p_render_data->render_buffers;
	Ref<RenderBufferDataForwardClustered> forward_buffers;
	if (render_buffers->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		forward_buffers = render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	}
	const uint32_t configured_layers = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/max_local_lights")), 0, int(MAX_LOCAL_SHADOW_LIGHTS)));
	if (configured_layers == 0 || !world.directory_buffer.is_valid() || !world.brick_buffer.is_valid() ||
			forward_buffers.is_null() || !forward_buffers->has_voxel_hit() || !forward_buffers->has_voxel_hit_position()) {
		LocalShadowUniformData disabled = {};
		RD::get_singleton()->buffer_update(local_shadow_uniform_buffer, 0, sizeof(LocalShadowUniformData), &disabled);
		return;
	}

	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	LocalShadowUniformData local_data = {};
	const Projection view_projection = p_render_data->scene_data->get_view_projection(0) * Projection(p_render_data->scene_data->cam_transform.affine_inverse());
	RendererRD::MaterialStorage::store_camera(view_projection.inverse(), local_data.inv_view_projection);
	const Size2i internal_size = render_buffers->get_internal_size();
	// Local-light masks are part of the shadow-mask feature and must honor its
	// configured resolution. The directional resolve already uses this scale;
	// dispatching every local-light layer at full resolution made the local
	// pass dominate GPU time even when the user selected a half-resolution mask.
	const float resolution_scale = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/resolution_scale")), 0.25f, 1.0f);
	const Size2i screen_size(MAX(1, int(Math::ceil(internal_size.x * resolution_scale))), MAX(1, int(Math::ceil(internal_size.y * resolution_scale))));
	local_data.state[0] = screen_size.x;
	local_data.state[1] = screen_size.y;
	local_data.trace_settings[0] = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/max_distance")));
	const int local_soft_shadow_samples = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/local_soft_shadow_samples")), 1, 8);
	// A single sample is the stable hard-shadow path. Previously the Medium
	// preset selected "soft, one sample" and accidentally entered the four-tap
	// bilinear branch, producing stippled bands on grazing voxel stairs.
	local_data.trace_settings[1] = int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_mode")) == 1 && local_soft_shadow_samples > 1 ? 1.0f : 0.0f;
	local_data.trace_settings[2] = local_data.trace_settings[1] > 0.5f ? float(local_soft_shadow_samples) : 1.0f;
	local_data.trace_settings[3] = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_radius_voxels")));

	uint32_t entry_count = 0;
	uint32_t omni_entry_count = 0;
	uint32_t spot_entry_count = 0;
	auto append_light = [&](uint32_t p_type, uint32_t p_light_index, const RendererRD::LightStorage::VoxelLocalShadowLightData &p_light) {
		if (entry_count >= configured_layers || p_light.shadow_opacity <= 0.001f) {
			return;
		}
		LocalShadowEntryData &entry = local_data.entries[entry_count];
		const Transform3D &camera_transform = p_render_data->scene_data->cam_transform;
		const Vector3 world_position = camera_transform.xform(p_light.position);
		const Vector3 world_direction = p_type == 1u ? camera_transform.basis.xform(p_light.direction).normalized() : Vector3();
		entry.position_inv_radius[0] = world_position.x;
		entry.position_inv_radius[1] = world_position.y;
		entry.position_inv_radius[2] = world_position.z;
		entry.position_inv_radius[3] = p_light.inv_radius;
		entry.direction_cone[0] = world_direction.x;
		entry.direction_cone[1] = world_direction.y;
		entry.direction_cone[2] = world_direction.z;
		entry.direction_cone[3] = p_light.cone_angle;
		entry.indices[0] = p_type;
		entry.indices[1] = p_light_index;
		entry.indices[2] = entry_count;
		entry.indices[3] = 1;
		entry_count++;
	};

	for (uint32_t light_index = 0; light_index < light_storage->get_omni_light_count() && entry_count < configured_layers; light_index++) {
		RendererRD::LightStorage::VoxelLocalShadowLightData light;
		if (light_storage->get_omni_voxel_shadow_light_data(light_index, light)) {
			const uint32_t previous_count = entry_count;
			append_light(0u, light_index, light);
			omni_entry_count += entry_count != previous_count ? 1 : 0;
		}
	}
	for (uint32_t light_index = 0; light_index < light_storage->get_spot_light_count() && entry_count < configured_layers; light_index++) {
		RendererRD::LightStorage::VoxelLocalShadowLightData light;
		if (light_storage->get_spot_voxel_shadow_light_data(light_index, light)) {
			const uint32_t previous_count = entry_count;
			append_light(1u, light_index, light);
			spot_entry_count += entry_count != previous_count ? 1 : 0;
		}
	}
	if (omni_entry_count != local_shadow_last_omni_count || spot_entry_count != local_shadow_last_spot_count) {
		print_verbose(vformat("Voxel local shadow resolve selected %d omni and %d spot lights (%d layer limit).", omni_entry_count, spot_entry_count, configured_layers));
		local_shadow_last_omni_count = omni_entry_count;
		local_shadow_last_spot_count = spot_entry_count;
	}
	local_data.state[2] = entry_count;
	local_data.state[3] = entry_count > 0 ? 1 : 0;
	RD::get_singleton()->buffer_update(local_shadow_uniform_buffer, 0, sizeof(LocalShadowUniformData), &local_data);
	if (entry_count == 0) {
		return;
	}

	const StringName scope = SNAME("voxel_forward_local_shadows");
	const StringName texture_name = SNAME("local_shadow_masks");
	const RID previous_local_shadow_mask = local_shadow_mask_rd;
	if (render_buffers->has_texture(scope, texture_name) && (local_shadow_screen_size != screen_size || local_shadow_layer_count != configured_layers)) {
		render_buffers->clear_context(scope);
		local_shadow_mask_rd = RID();
	}
	if (!render_buffers->has_texture(scope, texture_name)) {
		render_buffers->create_texture(scope, texture_name, RD::DATA_FORMAT_R8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, screen_size, configured_layers);
		print_verbose(vformat("Voxel local shadow mask allocation: %dx%dx%d R8 (%d bytes).", screen_size.x, screen_size.y, configured_layers, uint64_t(screen_size.x) * uint64_t(screen_size.y) * configured_layers));
	}
	local_shadow_mask_rd = render_buffers->get_texture(scope, texture_name);
	if (local_shadow_mask_rd != previous_local_shadow_mask) {
		// Scene uniform sets retain the RD texture RID. Rebuild them whenever a
		// resolution or layer-count change replaces the local-shadow array.
		base_uniforms_changed();
	}
	local_shadow_screen_size = screen_size;
	local_shadow_layer_count = configured_layers;
	const RID depth = render_buffers->get_depth_texture();
	const RID hit_payload = forward_buffers->get_voxel_hit();
	const RID hit_position = forward_buffers->get_voxel_hit_position();
	if (!local_shadow_mask_rd.is_valid() || !RD::get_singleton()->texture_is_valid(local_shadow_mask_rd) ||
			!depth.is_valid() || !RD::get_singleton()->texture_is_valid(depth) ||
			!hit_payload.is_valid() || !RD::get_singleton()->texture_is_valid(hit_payload) ||
			!hit_position.is_valid() || !RD::get_singleton()->texture_is_valid(hit_position)) {
		local_shadow_mask_rd = RID();
		base_uniforms_changed();
		return;
	}

	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	const RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID shader_rid = local_shadow_resolve_shader.version_get_shader(local_shadow_resolve_shader_version, 0);
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, depth }));
	RD::Uniform u_hit(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, hit_payload }));
	RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ local_shadow_mask_rd }));
	RD::Uniform u_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ world.directory_buffer }));
	RD::Uniform u_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ world.brick_buffer }));
	RD::Uniform u_occupancy(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, Vector<RID>({ occupancy_uniform_buffer }));
	RD::Uniform u_local(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 6, Vector<RID>({ local_shadow_uniform_buffer }));
	RD::Uniform u_hit_position(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ sampler, hit_position }));
	RD::Uniform u_dynamic_volumes(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, Vector<RID>({ ddgi_state.ddgi_dynamic_volume_buffer }));
	RD::Uniform u_dynamic_bvh(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, Vector<RID>({ ddgi_state.ddgi_dynamic_bvh_buffer }));
	RD::Uniform u_dynamic_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, Vector<RID>({ ddgi_state.ddgi_dynamic_directory_buffer }));
	RD::Uniform u_dynamic_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25, Vector<RID>({ ddgi_state.ddgi_dynamic_brick_buffer }));
	const RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(shader_rid, 0, u_depth, u_hit, u_output, u_directory, u_bricks, u_occupancy, u_local, u_hit_position, u_dynamic_volumes, u_dynamic_bvh, u_dynamic_directory, u_dynamic_bricks);

	RENDER_TIMESTAMP("Voxel Local Shadow Resolve");
	RD::get_singleton()->draw_command_begin_label("Voxel Local Shadow Resolve");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, local_shadow_resolve_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, screen_size.x, screen_size.y, entry_count);
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();
	local_shadow_masks_ready = true;
}

void RenderVoxelForward::_add_voxel_occupancy_uniforms(Vector<RD::Uniform> &r_uniforms) {
	const VoxelForwardVolumeStorage::WorldOccupancy &world = volume_storage->get_world_occupancy();
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
		&volume_storage->get_batch_voxel_textures(),
		&volume_storage->get_batch_brick_textures(),
		&volume_storage->get_batch_neighbor_textures(),
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

void RenderVoxelForward::_add_voxel_local_shadow_uniforms(LocalVector<RD::Uniform> &r_uniforms, bool p_multiview) {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	const bool local_mask_valid = local_shadow_mask_rd.is_valid() && RD::get_singleton()->texture_is_valid(local_shadow_mask_rd);
	const bool use_masks = !p_multiview && local_shadow_masks_ready && local_mask_valid && local_shadow_uniform_buffer.is_valid();
	const RID fallback_mask = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE);

	RD::Uniform masks;
	masks.binding = 40;
	masks.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
	masks.append_id(material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
	masks.append_id(use_masks ? local_shadow_mask_rd : fallback_mask);
	r_uniforms.push_back(masks);

	RD::Uniform params;
	params.binding = 41;
	params.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	// The scene shader always declares the complete local-shadow block. Bind an
	// equally sized, disabled block in passes that cannot consume the mask so the
	// descriptor remains valid and state.w == 0 selects the traversal fallback.
	params.append_id(use_masks ? local_shadow_uniform_buffer : local_shadow_disabled_uniform_buffer);
	r_uniforms.push_back(params);
}

void RenderVoxelForward::_fill_voxel_instance_data(RID p_base, VoxelInstanceData &r_instance_data) const {
	const VoxelForwardVolumeStorage::Volume *volume = volume_storage->get_volume(p_base);
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
	r_instance_data.voxel_size_pad[1] = volume->outline_width;
	static_assert(sizeof(volume->outline_color_rgba8) == sizeof(r_instance_data.voxel_size_pad[2]));
	memcpy(&r_instance_data.voxel_size_pad[2], &volume->outline_color_rgba8, sizeof(volume->outline_color_rgba8));
}

bool RenderVoxelForward::_render_scene_custom_uses_resolved_depth() const {
	return bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/enabled")) || bool(GLOBAL_GET("rendering/voxel_forward/reflections/enabled")) || bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/enabled"));
}

void RenderVoxelForward::_render_scene_custom_pre_opaque(RenderDataRD *p_render_data, bool p_depth_prepass) {
	if (!p_depth_prepass) {
		local_shadow_masks_ready = false;
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_ready"), false);
		RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_reflection_ready"), false);
		if (ddgi_state.shadow_mask_texture.is_valid()) {
			RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), Variant());
			RendererRD::TextureStorage::get_singleton()->texture_free(ddgi_state.shadow_mask_texture);
			ddgi_state.shadow_mask_texture = RID();
			ddgi_state.shadow_mask_source_rd = RID();
		}
		_reset_shadow_atlas_state();
	} else {
		// ReSTIR and DDGI shade ray-hit surfels from the material color clipmaps.
		// ReSTIR dispatch is independent from the optional shadow-atlas lifecycle.
		_render_voxel_reflections(p_render_data);
		_render_restir_gi_for_scene(p_render_data);
		_render_shadow_atlas(p_render_data);
		_render_ddgi_gi_for_scene(p_render_data);
		_render_local_shadows(p_render_data);
	}
}

void RenderVoxelForward::_render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) {
	if (p_render_data != nullptr && p_render_data->render_buffers.is_valid()) {
		p_render_data->render_buffers->clear_voxel_sunlight_atlas();
	}
	const RID scenario = p_render_data->scene_data->scenario;
	if (lighting_scenario != scenario) {
		if (lighting_scenario.is_valid()) {
			SWAP(ddgi_state, ddgi_worlds[lighting_scenario]);
		}
		if (ddgi_worlds.has(scenario)) {
			SWAP(ddgi_state, ddgi_worlds[scenario]);
		}
		lighting_scenario = scenario;
		volume_storage = volume_registry.get_scenario_storage(scenario);
		ddgi_context_changed = true;
		if (!ddgi_state.reflection_uniform_buffer.is_valid()) {
			ddgi_state.reflection_uniform_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(ReflectionUniformData));
		}
		for (uint32_t i = 0; i < DDGI_FRAME_RESOURCE_COUNT; i++) {
			if (!ddgi_state.ddgi_resolve_uniform_buffer[i].is_valid()) {
				ddgi_state.ddgi_resolve_uniform_buffer[i] = RD::get_singleton()->uniform_buffer_create(sizeof(DdgiResolveUniformData));
			}
			if (!ddgi_state.ddgi_temporal_uniform_buffer[i].is_valid()) {
				ddgi_state.ddgi_temporal_uniform_buffer[i] = RD::get_singleton()->uniform_buffer_create(sizeof(DdgiTemporalUniformData));
			}
		}
		// Legacy GI histories are not used by the DDGI bake path.
		_free_indirect_light();
		_free_restir_gi();
	}

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
	// Runtime settings and diagnostics must remain live even while the shadow
	// atlas or a replacement GI generation is still converging. Previously these
	// globals were updated only from _render_indirect_light(), so an incomplete
	// initial atlas made backend/debug changes appear to do nothing.
	const bool indirect_enabled = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/enabled"));
	const int indirect_backend = indirect_enabled ? CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/backend")), 1, 2) : 0;
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_backend"), indirect_backend);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_debug_mode"), indirect_backend == 1 ? int(GLOBAL_GET("rendering/voxel_forward/indirect_light/restir/debug_mode")) : 0);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_ddgi_debug_mode"), indirect_backend == 2 ? int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/debug_mode")) : 0);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_intensity"), MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/indirect_light/intensity"))));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_corner_ao_strength"), CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/voxel_gi/corner_occlusion_strength")), 0.0f, 1.0f));
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_corner_ao_tint"), Color(GLOBAL_GET("rendering/voxel_forward/indirect_light/voxel_gi/corner_occlusion_tint")));
	const bool shadow_mask_enabled = bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/enabled"));
	const bool reflection_enabled = bool(GLOBAL_GET("rendering/voxel_forward/reflections/enabled"));
	const bool ddgi_enabled = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/enabled")) && int(GLOBAL_GET("rendering/voxel_forward/indirect_light/backend")) == 2;
	const bool restir_enabled = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/enabled")) && int(GLOBAL_GET("rendering/voxel_forward/indirect_light/backend")) == 1;
	if (!restir_enabled) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_restir_ready"), false);
		restir_history_valid = false;
	}
	if (ddgi_enabled && p_render_data != nullptr && p_render_data->scene_data != nullptr) {
		const uint32_t ddgi_resolution = uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/spatial_cascade_resolution")), 8, 32));
		volume_storage->prioritize_queued_ddgi_pages_for_world_load(p_render_data->scene_data->cam_transform.origin, ddgi_resolution);
	}
	RENDER_TIMESTAMP("Update Voxel World Occupancy");
	volume_storage->update_world_occupancy(shadow_mask_enabled || reflection_enabled || ddgi_enabled || restir_enabled);
	RENDER_TIMESTAMP("Voxel World Occupancy Updated");
	const VoxelForwardVolumeStorage::WorldOccupancy &current_world = volume_storage->get_world_occupancy();
	_update_dynamic_voxel_lighting_scene(current_world);
	if (current_world.directory_buffer != bound_occupancy_directory || current_world.brick_buffer != bound_occupancy_bricks) {
		bound_occupancy_directory = current_world.directory_buffer;
		bound_occupancy_bricks = current_world.brick_buffer;
		base_uniforms_changed();
	}
	if (bound_batch_texture_revision != volume_storage->get_batch_texture_revision()) {
		bound_batch_texture_revision = volume_storage->get_batch_texture_revision();
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
	const int voxel_shadow_filter_samples = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/local_soft_shadow_samples")), 1, 8);
	occupancy_data.limits[1] = int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_mode")) == 1 && voxel_shadow_filter_samples > 1 ? 1.0f : 0.0f;
	// Four- and eight-sample modes use centered rings. One sample retains the
	// hard-shadow fast path while keeping a single quality control.
	occupancy_data.limits[2] = occupancy_data.limits[1] > 0.5f ? voxel_shadow_filter_samples : 1.0f;
	occupancy_data.limits[3] = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_radius_voxels")));
	RD::get_singleton()->buffer_update(occupancy_uniform_buffer, 0, sizeof(OccupancyUniformData), &occupancy_data);
	visible_volumes.clear();
	const bool custom_visibility_enabled = bool(GLOBAL_GET("rendering/voxel_forward/experimental_custom_visibility"));
	const bool inspect_visible_volumes = custom_visibility_enabled || is_print_verbose_enabled();
	uint32_t visible_volume_count = 0;
	if (inspect_visible_volumes && p_render_data != nullptr && p_render_data->instances != nullptr) {
		for (uint32_t i = 0; i < p_render_data->instances->size(); i++) {
			RenderGeometryInstance *instance = (*p_render_data->instances)[i];
			const VoxelForwardVolumeStorage::Volume *volume = volume_storage->get_volume(instance->get_base());
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
	const uint32_t registered_volume_count = volume_storage->get_volume_count();
	const uint64_t occupancy_gpu_bytes = volume_storage->get_occupancy_gpu_bytes();
	const VoxelForwardVolumeStorage::WorldOccupancy &world_occupancy = volume_storage->get_world_occupancy();
	if (registered_volume_count != last_registered_volume_count || visible_volume_count != last_visible_volume_count || occupancy_gpu_bytes != last_occupancy_gpu_bytes || world_occupancy.revision != last_world_occupancy_revision) {
		print_verbose(vformat("Voxel Forward: %d registered voxel volumes, %d visible, %.2f MiB occupancy; world table %d bricks (%d mixed, %d tombstones), max probe %d, %d incompatible; last update %d dirty bricks / %.2f KiB, %d incremental / %d full.", registered_volume_count, visible_volume_count, double(occupancy_gpu_bytes) / (1024.0 * 1024.0), world_occupancy.occupied_brick_count, world_occupancy.mixed_brick_count, world_occupancy.tombstone_count, world_occupancy.max_probe_count, world_occupancy.incompatible_volume_count, world_occupancy.last_dirty_brick_count, double(world_occupancy.last_uploaded_bytes) / 1024.0, world_occupancy.incremental_update_count, world_occupancy.full_rebuild_count));
		last_registered_volume_count = registered_volume_count;
		last_visible_volume_count = visible_volume_count;
		last_occupancy_gpu_bytes = occupancy_gpu_bytes;
		last_world_occupancy_revision = world_occupancy.revision;
	}

	RenderForwardClustered::_render_scene(p_render_data, p_default_bg_color);
	ddgi_context_changed = false;
}

void RenderVoxelForward::_render_shadow_atlas(const RenderDataRD *p_render_data) {
	// Snapshots retired after last frame's publish can be released now. Rendering
	// Device defers the underlying destruction until submitted GPU work is done.
	_release_shadow_atlas_snapshot(ddgi_state.shadow_atlas_retired_snapshot);
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	auto disable_voxel_lighting = [material_storage]() {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_ready"), false);
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_indirect_ready"), false);
	};
	if (!bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/enabled")) || p_render_data == nullptr || p_render_data->render_buffers.is_null() || p_render_data->scene_data == nullptr || p_render_data->scene_data->view_count != 1 || p_render_data->lights == nullptr || !shadow_atlas_pipeline.is_valid() || !shadow_resolve_legacy_pipeline.is_valid() || !shadow_resolve_payload_pipeline.is_valid()) {
		disable_voxel_lighting();
		if (ddgi_state.shadow_mask_texture.is_valid()) {
			RendererRD::MaterialStorage::get_singleton()->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), Variant());
			RendererRD::TextureStorage::get_singleton()->texture_free(ddgi_state.shadow_mask_texture);
			ddgi_state.shadow_mask_texture = RID();
			ddgi_state.shadow_mask_source_rd = RID();
		}
		_reset_shadow_atlas_state();
		return;
	}
	const VoxelForwardVolumeStorage::WorldOccupancy &world = volume_storage->get_world_occupancy();
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
	ddgi_state.ddgi_directional_light_base = RID();
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
		ddgi_state.ddgi_directional_light_base = light;
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
	const StringName mask_scope = SNAME("voxel_forward_shadow_mask");
	const StringName atlas_texture_names[2] = { SNAME("occupancy_shadow_atlas_a"), SNAME("occupancy_shadow_atlas_b") };
	const StringName mask_texture_name = SNAME("occupancy_shadow_mask");
	if (ddgi_state.shadow_atlas_allocated_resolution != 0 && ddgi_state.shadow_atlas_allocated_resolution != atlas_resolution) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), Variant());
		if (ddgi_state.shadow_mask_texture.is_valid()) {
			RendererRD::TextureStorage::get_singleton()->texture_free(ddgi_state.shadow_mask_texture);
			ddgi_state.shadow_mask_texture = RID();
		}
		render_buffers->clear_context(scope);
		if (ddgi_state.shadow_atlas_scratch_rd.is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.shadow_atlas_scratch_rd);
			ddgi_state.shadow_atlas_scratch_rd = RID();
		}
		ddgi_state.shadow_mask_source_rd = RID();
		ddgi_state.shadow_atlas_source_rd = RID();
		ddgi_state.shadow_atlas_scratch_source_rd = RID();
		ddgi_state.shadow_atlas_scratch_resolution = 0;
		_release_shadow_atlas_snapshot(ddgi_state.shadow_atlas_staging);
		ddgi_state.shadow_atlas_temporal_rebuild_in_progress = false;
		ddgi_state.shadow_atlas_rebuild_next_texel = 0;
		ddgi_state.shadow_atlas_rebuild_total_texels = 0;
		ddgi_state.shadow_atlas_active_slot = 0;
		ddgi_state.shadow_atlas_initialized = false;
	}
	for (uint32_t slot = 0; slot < 2; slot++) {
		if (!render_buffers->has_texture(scope, atlas_texture_names[slot])) {
			render_buffers->create_texture(scope, atlas_texture_names[slot], RD::DATA_FORMAT_R32_SFLOAT, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT, RD::TEXTURE_SAMPLES_1, Size2i(atlas_resolution * 2, atlas_resolution));
		}
	}
	ddgi_state.shadow_atlas_allocated_resolution = atlas_resolution;
	if (render_buffers->has_texture(mask_scope, mask_texture_name) && ddgi_state.shadow_mask_screen_size != screen_size) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), Variant());
		if (ddgi_state.shadow_mask_texture.is_valid()) {
			RendererRD::TextureStorage::get_singleton()->texture_free(ddgi_state.shadow_mask_texture);
			ddgi_state.shadow_mask_texture = RID();
		}
		ddgi_state.shadow_mask_source_rd = RID();
		render_buffers->clear_context(mask_scope);
	}
	if (!render_buffers->has_texture(mask_scope, mask_texture_name)) {
		render_buffers->create_texture(mask_scope, mask_texture_name, RD::DATA_FORMAT_R8_UNORM, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, screen_size);
	}
	ddgi_state.shadow_mask_screen_size = screen_size;
	RID atlas_slots[2] = {
		render_buffers->get_texture(scope, atlas_texture_names[0]),
		render_buffers->get_texture(scope, atlas_texture_names[1])
	};
	RID atlas_output = atlas_slots[ddgi_state.shadow_atlas_active_slot];
	RID atlas_staging_output = atlas_slots[1u - ddgi_state.shadow_atlas_active_slot];
	const RID mask_output = render_buffers->get_texture(mask_scope, mask_texture_name);
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
	if (ddgi_state.shadow_mask_source_rd != mask_output) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), Variant());
		if (ddgi_state.shadow_mask_texture.is_valid()) {
			texture_storage->texture_free(ddgi_state.shadow_mask_texture);
		}
		ddgi_state.shadow_mask_texture = texture_storage->texture_allocate();
		texture_storage->texture_rd_initialize(ddgi_state.shadow_mask_texture, mask_output);
		ddgi_state.shadow_mask_source_rd = mask_output;
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), ddgi_state.shadow_mask_texture);
	}

	if (ddgi_context_changed) {
		material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_mask"), ddgi_state.shadow_mask_texture);
	}
	Vector3 reference_axis = Math::abs(light_direction.y) < 0.95f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
	const Vector3 tangent = reference_axis.cross(light_direction).normalized();
	const Vector3 bitangent = light_direction.cross(tangent).normalized();
	const Vector3 camera_position = p_render_data->scene_data->cam_transform.origin;
	const Vector3 camera_from_center = camera_position - ddgi_state.shadow_atlas_center;
	const float recenter_distance = near_extent * CLAMP(float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/atlas_recenter_ratio")), 0.05f, 0.75f);
	const bool tangent_outside_center = !ddgi_state.shadow_atlas_initialized || Math::abs(camera_from_center.dot(tangent)) > recenter_distance;
	const bool bitangent_outside_center = !ddgi_state.shadow_atlas_initialized || Math::abs(camera_from_center.dot(bitangent)) > recenter_distance;
	const bool depth_outside_center = !ddgi_state.shadow_atlas_initialized || Math::abs(camera_from_center.dot(light_direction)) > recenter_distance;
	const bool camera_outside_center = tangent_outside_center || bitangent_outside_center || depth_outside_center;
	const bool incremental_enabled = bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/incremental_atlas_updates"));
	const bool dirty_region_updates_enabled = bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/dirty_region_updates_enabled"));
	const bool incremental_mode_changed = ddgi_state.shadow_atlas_initialized && ddgi_state.shadow_atlas_incremental_enabled != incremental_enabled;
	const bool world_invalid = ddgi_state.shadow_atlas_world_revision != world.revision;
	// Occupancy buffer replacement always advances world.revision. Comparing live
	// buffer RIDs here would incorrectly invalidate an atlas built from a private
	// immutable snapshot of the same revision.
	const bool source_invalid = ddgi_state.shadow_atlas_source_rd != atlas_output;
	const bool light_invalid = !ddgi_state.shadow_atlas_light_direction.is_equal_approx(light_direction) ||
			!ddgi_state.shadow_atlas_tangent.is_equal_approx(tangent) || !ddgi_state.shadow_atlas_bitangent.is_equal_approx(bitangent);
	const bool layout_invalid = ddgi_state.shadow_atlas_resolution != atlas_resolution ||
			!Math::is_equal_approx(ddgi_state.shadow_atlas_near_extent, near_extent) ||
			!Math::is_equal_approx(ddgi_state.shadow_atlas_far_extent, far_extent);
	const bool atlas_invalid = !ddgi_state.shadow_atlas_initialized || source_invalid || world_invalid || light_invalid || layout_invalid || camera_outside_center || incremental_mode_changed;
	const int32_t max_shadow_steps = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/max_steps")), 1, 4096);
	const bool temporal_rebuild_enabled = bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/temporal_rebuild_enabled"));
	const bool force_full_rebuild = bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/force_full_rebuild"));
	const bool force_full_rebuild_requested = force_full_rebuild && !ddgi_state.shadow_atlas_force_full_rebuild_latched;
	ddgi_state.shadow_atlas_force_full_rebuild_latched = force_full_rebuild;
	if (!temporal_rebuild_enabled && ddgi_state.shadow_atlas_temporal_rebuild_in_progress) {
		_release_shadow_atlas_snapshot(ddgi_state.shadow_atlas_staging);
		ddgi_state.shadow_atlas_temporal_rebuild_in_progress = false;
		ddgi_state.shadow_atlas_rebuild_next_texel = 0;
		ddgi_state.shadow_atlas_rebuild_total_texels = 0;
	}

	// Snap the target independently from the staging state. Camera changes that
	// arrive during a rebuild are coalesced and picked up after the current
	// complete atlas is published instead of continually restarting partial work.
	const float planar_snap_size = MAX(world.voxel_size * 0.25f, far_extent * 2.0f / float(atlas_resolution));
	const float depth_snap_size = MAX(world.voxel_size * 0.25f, near_extent * 2.0f / float(atlas_resolution));
	const float requested_tangent = tangent_outside_center ? Math::floor(camera_position.dot(tangent) / planar_snap_size + 0.5f) * planar_snap_size : ddgi_state.shadow_atlas_center.dot(tangent);
	const float requested_bitangent = bitangent_outside_center ? Math::floor(camera_position.dot(bitangent) / planar_snap_size + 0.5f) * planar_snap_size : ddgi_state.shadow_atlas_center.dot(bitangent);
	const float requested_depth = depth_outside_center ? Math::floor(camera_position.dot(light_direction) / depth_snap_size + 0.5f) * depth_snap_size : ddgi_state.shadow_atlas_center.dot(light_direction);
	const Vector3 requested_center = tangent * requested_tangent + bitangent * requested_bitangent + light_direction * requested_depth;
	const bool direct_reliable_world_delta = !world_invalid ||
			(ddgi_state.shadow_atlas_world_revision != UINT64_MAX && ddgi_state.shadow_atlas_world_revision + 1 == world.revision &&
					world.last_incremental_revision == world.revision && !world.last_dirty_bricks.is_empty());
	const bool coalesced_reliable_world_delta = world_invalid && ddgi_state.shadow_atlas_pending_world_revision == world.revision &&
			ddgi_state.shadow_atlas_pending_dirty_reliable && !ddgi_state.shadow_atlas_pending_dirty_bricks.is_empty();
	const bool reliable_world_delta = direct_reliable_world_delta || coalesced_reliable_world_delta;
	const bool can_attempt_dirty_region_update = dirty_region_updates_enabled && world_invalid && reliable_world_delta && !camera_outside_center;
	const bool can_attempt_camera_scroll = incremental_enabled && camera_outside_center && shadow_atlas_scroll_pipeline.is_valid();
	const bool can_attempt_ordinary_incremental = ddgi_state.shadow_atlas_initialized && !source_invalid && !light_invalid && !layout_invalid && !incremental_mode_changed &&
			(can_attempt_dirty_region_update || can_attempt_camera_scroll);
	const bool temporal_path_required = ddgi_state.shadow_atlas_temporal_rebuild_in_progress || ddgi_state.shadow_atlas_pending_full_rebuild || (atlas_invalid && !can_attempt_ordinary_incremental);

	if (temporal_rebuild_enabled && temporal_path_required) {
		// Render-buffer contexts may replace their textures without changing atlas
		// dimensions (for example, switching from the editor viewport to Play).
		// Unlike a world revision, a dead staging image cannot be allowed to finish.
		// Abort only this invalid-resource build and immediately take a new coherent
		// snapshot targeting the replacement texture.
		if (ddgi_state.shadow_atlas_temporal_rebuild_in_progress &&
				(ddgi_state.shadow_atlas_staging.texture != atlas_staging_output || !ddgi_state.shadow_atlas_staging.texture.is_valid() || !rd->texture_is_valid(ddgi_state.shadow_atlas_staging.texture))) {
			const uint64_t abandoned_texels = ddgi_state.shadow_atlas_rebuild_next_texel;
			_release_shadow_atlas_snapshot(ddgi_state.shadow_atlas_staging);
			ddgi_state.shadow_atlas_temporal_rebuild_in_progress = false;
			ddgi_state.shadow_atlas_rebuild_next_texel = 0;
			ddgi_state.shadow_atlas_rebuild_total_texels = 0;
			ddgi_state.shadow_atlas_pending_full_rebuild = true;
			ddgi_state.shadow_atlas_temporal_restart_count++;
			print_verbose(vformat("Voxel Forward shadow atlas temporal rebuild aborted after %d texels: staging render-buffer texture was replaced; restart #%d will use the current texture.", abandoned_texels, ddgi_state.shadow_atlas_temporal_restart_count));
		}
		if (ddgi_state.shadow_atlas_temporal_rebuild_in_progress) {
			if (ddgi_state.shadow_atlas_staging.world_revision != world.revision && ddgi_state.shadow_atlas_pending_world_revision != world.revision) {
				const uint64_t expected_revision = ddgi_state.shadow_atlas_pending_world_revision == UINT64_MAX ? ddgi_state.shadow_atlas_staging.world_revision + 1 : ddgi_state.shadow_atlas_pending_world_revision + 1;
				if (world.revision != expected_revision || world.last_incremental_revision != world.revision || world.last_dirty_bricks.is_empty()) {
					ddgi_state.shadow_atlas_pending_dirty_reliable = false;
				}
				for (const Vector3i &dirty_brick : world.last_dirty_bricks) {
					ddgi_state.shadow_atlas_pending_dirty_bricks.insert(dirty_brick);
				}
				ddgi_state.shadow_atlas_pending_world_revision = world.revision;
				ddgi_state.shadow_atlas_pending_change_count++;
			}
			const bool staging_definition_changed =
					ddgi_state.shadow_atlas_staging.texture != atlas_staging_output ||
					ddgi_state.shadow_atlas_staging.world_revision != world.revision ||
					ddgi_state.shadow_atlas_staging.world_origin != world.origin ||
					!Math::is_equal_approx(ddgi_state.shadow_atlas_staging.voxel_size, world.voxel_size) ||
					!ddgi_state.shadow_atlas_staging.center.is_equal_approx(requested_center) ||
					!ddgi_state.shadow_atlas_staging.light_direction.is_equal_approx(light_direction) ||
					!ddgi_state.shadow_atlas_staging.tangent.is_equal_approx(tangent) ||
					!ddgi_state.shadow_atlas_staging.bitangent.is_equal_approx(bitangent) ||
					ddgi_state.shadow_atlas_staging.resolution != atlas_resolution ||
					ddgi_state.shadow_atlas_staging.directory_mask != world.directory_mask ||
					ddgi_state.shadow_atlas_staging.max_steps != max_shadow_steps ||
					!Math::is_equal_approx(ddgi_state.shadow_atlas_staging.near_extent, near_extent) ||
					!Math::is_equal_approx(ddgi_state.shadow_atlas_staging.far_extent, far_extent);
			if (staging_definition_changed || force_full_rebuild_requested) {
				if (ddgi_state.shadow_atlas_pending_change_count == 0) {
					ddgi_state.shadow_atlas_pending_change_count++;
				}
				const bool incompatible_pending_change = force_full_rebuild_requested ||
						ddgi_state.shadow_atlas_staging.texture != atlas_staging_output ||
						ddgi_state.shadow_atlas_staging.world_origin != world.origin ||
						!Math::is_equal_approx(ddgi_state.shadow_atlas_staging.voxel_size, world.voxel_size) ||
						!ddgi_state.shadow_atlas_staging.light_direction.is_equal_approx(light_direction) ||
						!ddgi_state.shadow_atlas_staging.tangent.is_equal_approx(tangent) ||
						!ddgi_state.shadow_atlas_staging.bitangent.is_equal_approx(bitangent) ||
						ddgi_state.shadow_atlas_staging.resolution != atlas_resolution ||
						ddgi_state.shadow_atlas_staging.directory_mask != world.directory_mask ||
						ddgi_state.shadow_atlas_staging.max_steps != max_shadow_steps ||
						!Math::is_equal_approx(ddgi_state.shadow_atlas_staging.near_extent, near_extent) ||
						!Math::is_equal_approx(ddgi_state.shadow_atlas_staging.far_extent, far_extent);
				ddgi_state.shadow_atlas_pending_full_rebuild = ddgi_state.shadow_atlas_pending_full_rebuild || incompatible_pending_change;
			}
		}

		// Never restart an active build. It reads private occupancy buffers and is
		// therefore a coherent, immutable definition even if the live world moves.
		const bool begin_rebuild = !ddgi_state.shadow_atlas_temporal_rebuild_in_progress && (atlas_invalid || force_full_rebuild_requested || ddgi_state.shadow_atlas_pending_full_rebuild);
		if (begin_rebuild) {
			String rebuild_reason;
			if (force_full_rebuild_requested) {
				rebuild_reason = "explicit full-rebuild request";
			} else if (!ddgi_state.shadow_atlas_initialized) {
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
			if (!volume_storage->create_world_occupancy_snapshot(snapshot)) {
				print_verbose(vformat("Voxel Forward shadow atlas temporal rebuild deferred: occupancy snapshot allocation failed at revision %d; previous complete atlas retained.", world.revision));
			} else {
				_release_shadow_atlas_snapshot(ddgi_state.shadow_atlas_staging);
				ddgi_state.shadow_atlas_staging.texture = atlas_staging_output;
				ddgi_state.shadow_atlas_staging.directory = snapshot.directory_buffer;
				ddgi_state.shadow_atlas_staging.bricks = snapshot.brick_buffer;
				ddgi_state.shadow_atlas_staging.owns_occupancy_snapshot = true;
				ddgi_state.shadow_atlas_staging.world_revision = snapshot.revision;
				ddgi_state.shadow_atlas_staging.world_origin = snapshot.origin;
				ddgi_state.shadow_atlas_staging.center = requested_center;
				ddgi_state.shadow_atlas_staging.light_direction = light_direction;
				ddgi_state.shadow_atlas_staging.tangent = tangent;
				ddgi_state.shadow_atlas_staging.bitangent = bitangent;
				ddgi_state.shadow_atlas_staging.resolution = atlas_resolution;
				ddgi_state.shadow_atlas_staging.directory_mask = snapshot.directory_mask;
				ddgi_state.shadow_atlas_staging.max_steps = max_shadow_steps;
				ddgi_state.shadow_atlas_staging.voxel_size = snapshot.voxel_size;
				ddgi_state.shadow_atlas_staging.near_extent = near_extent;
				ddgi_state.shadow_atlas_staging.far_extent = far_extent;
				ddgi_state.shadow_atlas_rebuild_next_texel = 0;
				ddgi_state.shadow_atlas_rebuild_total_texels = uint64_t(atlas_resolution) * uint64_t(atlas_resolution) * 2u;
				ddgi_state.shadow_atlas_temporal_rebuild_in_progress = true;
				ddgi_state.shadow_atlas_temporal_rebuild_count++;
				ddgi_state.shadow_atlas_pending_world_revision = UINT64_MAX;
				ddgi_state.shadow_atlas_pending_change_count = 0;
				ddgi_state.shadow_atlas_pending_full_rebuild = false;
				ddgi_state.shadow_atlas_pending_dirty_reliable = true;
				ddgi_state.shadow_atlas_pending_dirty_bricks.clear();
				print_verbose(vformat("Voxel Forward shadow atlas temporal rebuild #%d started: %s, %d texels, center %s, immutable revision %d.", ddgi_state.shadow_atlas_temporal_rebuild_count, rebuild_reason, ddgi_state.shadow_atlas_rebuild_total_texels, requested_center, snapshot.revision));
			}
		}

		if (ddgi_state.shadow_atlas_temporal_rebuild_in_progress) {
			const uint64_t configured_budget = uint64_t(MAX(64, int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/rebuild_texel_budget"))));
			// The project setting is the explicit work ceiling. The previous hidden
			// 1,024-texel cap made every larger configured value ineffective and made
			// the default 512 atlas require 512 frames to publish. Align down to whole
			// workgroups so each frame remains deterministic and bounded.
			static constexpr uint64_t WORKGROUP_TEXELS = 8u * 8u;
			const uint64_t frame_budget = MAX(WORKGROUP_TEXELS, configured_budget / WORKGROUP_TEXELS * WORKGROUP_TEXELS);
			uint64_t frame_remaining = MIN(frame_budget, ddgi_state.shadow_atlas_rebuild_total_texels - ddgi_state.shadow_atlas_rebuild_next_texel);
			const uint32_t atlas_width = ddgi_state.shadow_atlas_staging.resolution * 2u;
			const uint32_t workgroups_per_row = atlas_width / 8u;
			const RID atlas_shader_rid = shadow_atlas_shader.version_get_shader(shadow_atlas_shader_version, 0);
			RD::Uniform u_staging_output(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ ddgi_state.shadow_atlas_staging.texture }));
			RD::Uniform u_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ ddgi_state.shadow_atlas_staging.directory }));
			RD::Uniform u_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ ddgi_state.shadow_atlas_staging.bricks }));
			const RID staging_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(atlas_shader_rid, 0, u_staging_output, u_directory, u_bricks);

			ShadowAtlasPushConstant atlas_push = {};
			atlas_push.world_origin_voxel_size[0] = ddgi_state.shadow_atlas_staging.world_origin.x;
			atlas_push.world_origin_voxel_size[1] = ddgi_state.shadow_atlas_staging.world_origin.y;
			atlas_push.world_origin_voxel_size[2] = ddgi_state.shadow_atlas_staging.world_origin.z;
			atlas_push.world_origin_voxel_size[3] = ddgi_state.shadow_atlas_staging.voxel_size;
			atlas_push.atlas_center_depth[0] = ddgi_state.shadow_atlas_staging.center.x;
			atlas_push.atlas_center_depth[1] = ddgi_state.shadow_atlas_staging.center.y;
			atlas_push.atlas_center_depth[2] = ddgi_state.shadow_atlas_staging.center.z;
			atlas_push.atlas_center_depth[3] = ddgi_state.shadow_atlas_staging.far_extent;
			atlas_push.tangent_near_extent[0] = ddgi_state.shadow_atlas_staging.tangent.x;
			atlas_push.tangent_near_extent[1] = ddgi_state.shadow_atlas_staging.tangent.y;
			atlas_push.tangent_near_extent[2] = ddgi_state.shadow_atlas_staging.tangent.z;
			atlas_push.tangent_near_extent[3] = ddgi_state.shadow_atlas_staging.near_extent;
			atlas_push.bitangent_far_extent[0] = ddgi_state.shadow_atlas_staging.bitangent.x;
			atlas_push.bitangent_far_extent[1] = ddgi_state.shadow_atlas_staging.bitangent.y;
			atlas_push.bitangent_far_extent[2] = ddgi_state.shadow_atlas_staging.bitangent.z;
			atlas_push.bitangent_far_extent[3] = ddgi_state.shadow_atlas_staging.far_extent;
			atlas_push.atlas_directory_steps[0] = ddgi_state.shadow_atlas_staging.resolution;
			atlas_push.atlas_directory_steps[1] = 2;
			atlas_push.atlas_directory_steps[2] = ddgi_state.shadow_atlas_staging.directory_mask;
			atlas_push.atlas_directory_steps[3] = ddgi_state.shadow_atlas_staging.max_steps;

			RENDER_TIMESTAMP("Shadow Atlas Temporal Rebuild");
			RD::get_singleton()->draw_command_begin_label("Shadow Atlas Temporal Rebuild");
			RD::ComputeListID atlas_list = RD::get_singleton()->compute_list_begin();
			RD::get_singleton()->compute_list_bind_compute_pipeline(atlas_list, shadow_atlas_pipeline);
			RD::get_singleton()->compute_list_bind_uniform_set(atlas_list, staging_uniform_set, 0);
			while (frame_remaining > 0) {
				const uint64_t workgroup_index = ddgi_state.shadow_atlas_rebuild_next_texel / WORKGROUP_TEXELS;
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
				ddgi_state.shadow_atlas_rebuild_next_texel += dispatched_texels;
				frame_remaining -= dispatched_texels;
			}
			RD::get_singleton()->compute_list_end();
			RD::get_singleton()->draw_command_end_label();

			if (ddgi_state.shadow_atlas_rebuild_next_texel == ddgi_state.shadow_atlas_rebuild_total_texels) {
				const uint64_t completed_texels = ddgi_state.shadow_atlas_rebuild_total_texels;
				RENDER_TIMESTAMP("Shadow Atlas Publish");
				RD::get_singleton()->draw_command_begin_label("Shadow Atlas Publish");
				ddgi_state.shadow_atlas_active_slot = 1u - ddgi_state.shadow_atlas_active_slot;
				atlas_output = ddgi_state.shadow_atlas_staging.texture;
				atlas_staging_output = atlas_slots[1u - ddgi_state.shadow_atlas_active_slot];
				ddgi_state.shadow_atlas_source_rd = atlas_output;
				// The published texture records the snapshot revision. Live buffer RIDs
				// are bookkeeping only; newer revisions remain invalid and trigger one
				// coherent follow-up update on the next frame.
				ddgi_state.shadow_atlas_directory_rd = world.directory_buffer;
				ddgi_state.shadow_atlas_bricks_rd = world.brick_buffer;
				ddgi_state.shadow_atlas_world_revision = ddgi_state.shadow_atlas_staging.world_revision;
				ddgi_state.shadow_atlas_center = ddgi_state.shadow_atlas_staging.center;
				ddgi_state.shadow_atlas_light_direction = ddgi_state.shadow_atlas_staging.light_direction;
				ddgi_state.shadow_atlas_tangent = ddgi_state.shadow_atlas_staging.tangent;
				ddgi_state.shadow_atlas_bitangent = ddgi_state.shadow_atlas_staging.bitangent;
				ddgi_state.shadow_atlas_resolution = ddgi_state.shadow_atlas_staging.resolution;
				ddgi_state.shadow_atlas_voxel_size = ddgi_state.shadow_atlas_staging.voxel_size;
				ddgi_state.shadow_atlas_near_extent = ddgi_state.shadow_atlas_staging.near_extent;
				ddgi_state.shadow_atlas_far_extent = ddgi_state.shadow_atlas_staging.far_extent;
				ddgi_state.shadow_atlas_incremental_enabled = incremental_enabled;
				ddgi_state.shadow_atlas_initialized = true;
				ddgi_state.shadow_atlas_temporal_rebuild_in_progress = false;
				ddgi_state.shadow_atlas_rebuild_next_texel = 0;
				ddgi_state.shadow_atlas_rebuild_total_texels = 0;
				ddgi_state.shadow_atlas_retired_snapshot = ddgi_state.shadow_atlas_staging;
				ddgi_state.shadow_atlas_staging = ShadowAtlasBuildState();
				ddgi_state.shadow_atlas_temporal_publish_count++;
				RD::get_singleton()->draw_command_end_label();
				print_verbose(vformat("Voxel Forward shadow atlas temporal publish #%d completed from rebuild #%d; active slot %d, center %s, exact revision %d, %d texels processed, latest revision %d, %d pending changes / %d pending dirty regions, %d restarts.", ddgi_state.shadow_atlas_temporal_publish_count, ddgi_state.shadow_atlas_temporal_rebuild_count, ddgi_state.shadow_atlas_active_slot, ddgi_state.shadow_atlas_center, ddgi_state.shadow_atlas_world_revision, completed_texels, world.revision, ddgi_state.shadow_atlas_pending_change_count, ddgi_state.shadow_atlas_pending_dirty_bricks.size(), ddgi_state.shadow_atlas_temporal_restart_count));
			}
		}
	} else if (atlas_invalid || force_full_rebuild_requested) {
		// Snapping planar movement to the far-cascade texel grid makes the default
		// near/far ratio an exact integer shift in both cascades. Axes that did not
		// cross their hysteresis threshold retain their old coordinate, especially
		// the depth axis whose movement changes the traced ray interval.
		const Vector3 center_delta = requested_center - ddgi_state.shadow_atlas_center;
		bool use_incremental = !force_full_rebuild_requested && ddgi_state.shadow_atlas_initialized && !source_invalid && !light_invalid && !layout_invalid && !incremental_mode_changed &&
				(can_attempt_dirty_region_update || can_attempt_camera_scroll);
		String fallback_reason;
		if (force_full_rebuild_requested) {
			fallback_reason = "explicit full-rebuild request";
		} else if (!incremental_enabled && !can_attempt_dirty_region_update) {
			fallback_reason = "camera scrolling disabled and no reliable dirty world region is available";
		} else if (!ddgi_state.shadow_atlas_initialized) {
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
				const HashSet<Vector3i> *coalesced_dirty_bricks = coalesced_reliable_world_delta ? &ddgi_state.shadow_atlas_pending_dirty_bricks : nullptr;
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
			if (!ddgi_state.shadow_atlas_scratch_rd.is_valid() || ddgi_state.shadow_atlas_scratch_resolution != atlas_resolution || ddgi_state.shadow_atlas_scratch_source_rd != atlas_output) {
				if (ddgi_state.shadow_atlas_scratch_rd.is_valid()) {
					RD::get_singleton()->free_rid(ddgi_state.shadow_atlas_scratch_rd);
				}
				RD::TextureFormat scratch_format;
				scratch_format.format = RD::DATA_FORMAT_R32_SFLOAT;
				scratch_format.width = atlas_resolution * 2;
				scratch_format.height = atlas_resolution;
				scratch_format.texture_type = RD::TEXTURE_TYPE_2D;
				scratch_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
				ddgi_state.shadow_atlas_scratch_rd = RD::get_singleton()->texture_create(scratch_format, RD::TextureView());
				ddgi_state.shadow_atlas_scratch_resolution = ddgi_state.shadow_atlas_scratch_rd.is_valid() ? atlas_resolution : 0;
				ddgi_state.shadow_atlas_scratch_source_rd = ddgi_state.shadow_atlas_scratch_rd.is_valid() ? atlas_output : RID();
				if (ddgi_state.shadow_atlas_scratch_rd.is_valid()) {
					RD::get_singleton()->set_resource_name(ddgi_state.shadow_atlas_scratch_rd, "Voxel Shadow Atlas Scroll Scratch");
				}
			}
			if (!ddgi_state.shadow_atlas_scratch_rd.is_valid()) {
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
				scratch_initialized = RD::get_singleton()->texture_copy(atlas_output, ddgi_state.shadow_atlas_scratch_rd, Vector3(), Vector3(), Vector3(atlas_resolution * 2, atlas_resolution, 1), 0, 0, 0, 0) == OK;
				if (!scratch_initialized) {
					fallback_reason = "copying the live atlas to scratch failed";
				}
			}
			if (scratch_initialized) {
				RD::ComputeListID atlas_list = RD::get_singleton()->compute_list_begin();
				if (scroll_required) {
					const RID scroll_shader_rid = shadow_atlas_scroll_shader.version_get_shader(shadow_atlas_scroll_shader_version, 0);
					RD::Uniform u_scroll_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, atlas_output }));
					RD::Uniform u_scroll_destination(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ ddgi_state.shadow_atlas_scratch_rd }));
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
					RD::Uniform u_scratch_output(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ ddgi_state.shadow_atlas_scratch_rd }));
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
				update_complete = RD::get_singleton()->texture_copy(ddgi_state.shadow_atlas_scratch_rd, atlas_output, Vector3(), Vector3(), Vector3(atlas_resolution * 2, atlas_resolution, 1), 0, 0, 0, 0) == OK;
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
			ddgi_state.shadow_atlas_pending_full_rebuild = true;
			ddgi_state.shadow_atlas_pending_change_count++;
			ddgi_state.shadow_atlas_pending_world_revision = world.revision;
			print_verbose(vformat("Voxel Forward shadow atlas incremental update deferred to temporal rebuild: %s, %d dirty regions, latest revision %d.", fallback_reason, ddgi_state.shadow_atlas_pending_dirty_bricks.size(), world.revision));
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
			ddgi_state.shadow_atlas_full_rebuild_count++;
			print_verbose(vformat("Voxel Forward shadow atlas full rebuild #%d: %s, %d texels, center %s, revision %d.", ddgi_state.shadow_atlas_full_rebuild_count, fallback_reason, retraced_texels, requested_center, world.revision));
		} else {
			ddgi_state.shadow_atlas_incremental_update_count++;
			if (scroll_required) {
				ddgi_state.shadow_atlas_incremental_scroll_count++;
			}
			print_verbose(vformat("Voxel Forward shadow atlas incremental update #%d (scroll #%d): near shift %s, far shift %s, %d dirty rectangles / %d merged retrace rectangles / %d retraced texels, center %s, revision %d.", ddgi_state.shadow_atlas_incremental_update_count, ddgi_state.shadow_atlas_incremental_scroll_count, cascade_shifts[0], cascade_shifts[1], dirty_rectangles_processed, update_rects.size(), retraced_texels, requested_center, world.revision));
		}
		RD::get_singleton()->draw_command_end_label();

		if (update_complete) {
			ddgi_state.shadow_atlas_center = requested_center;
			ddgi_state.shadow_atlas_light_direction = light_direction;
			ddgi_state.shadow_atlas_tangent = tangent;
			ddgi_state.shadow_atlas_bitangent = bitangent;
			ddgi_state.shadow_atlas_world_revision = world.revision;
			ddgi_state.shadow_atlas_resolution = atlas_resolution;
			ddgi_state.shadow_atlas_voxel_size = world.voxel_size;
			ddgi_state.shadow_atlas_near_extent = near_extent;
			ddgi_state.shadow_atlas_far_extent = far_extent;
			ddgi_state.shadow_atlas_source_rd = atlas_output;
			ddgi_state.shadow_atlas_directory_rd = world.directory_buffer;
			ddgi_state.shadow_atlas_bricks_rd = world.brick_buffer;
			ddgi_state.shadow_atlas_incremental_enabled = incremental_enabled;
			ddgi_state.shadow_atlas_pending_world_revision = UINT64_MAX;
			ddgi_state.shadow_atlas_pending_change_count = 0;
			ddgi_state.shadow_atlas_pending_full_rebuild = false;
			ddgi_state.shadow_atlas_pending_dirty_reliable = true;
			ddgi_state.shadow_atlas_pending_dirty_bricks.clear();
		}
		ddgi_state.shadow_atlas_initialized = true;
	}

	// A newly allocated or resized pair has no complete texture to expose until
	// its first temporal rebuild publishes. Stale but complete atlases remain
	// available while ordinary camera/world invalidations are processed.
	if (!ddgi_state.shadow_atlas_initialized || !ddgi_state.shadow_atlas_source_rd.is_valid() || ddgi_state.shadow_atlas_source_rd != atlas_slots[ddgi_state.shadow_atlas_active_slot]) {
		disable_voxel_lighting();
		return;
	}
	atlas_output = ddgi_state.shadow_atlas_source_rd;
	// Atmosphere must not illuminate newly enclosed air from an older snapshot.
	// Keep surface/DDGI publication policy unchanged; only expose a current atlas.
	if (ddgi_state.shadow_atlas_world_revision == world.revision &&
			ddgi_state.shadow_atlas_light_direction.is_equal_approx(light_direction)) {
		Vector<float> sunlight;
		sunlight.resize(20);
		float *data = sunlight.ptrw();
		const Vector3 axes[5] = { ddgi_state.shadow_atlas_center, ddgi_state.shadow_atlas_light_direction,
			ddgi_state.shadow_atlas_tangent, ddgi_state.shadow_atlas_bitangent, world.origin };
		const float values[5] = { ddgi_state.shadow_atlas_voxel_size, ddgi_state.shadow_atlas_near_extent,
			ddgi_state.shadow_atlas_far_extent, float(ddgi_state.shadow_atlas_resolution), 1.0f };
		for (int i = 0; i < 5; i++) {
			data[i * 4 + 0] = axes[i].x;
			data[i * 4 + 1] = axes[i].y;
			data[i * 4 + 2] = axes[i].z;
			data[i * 4 + 3] = values[i];
		}
		render_buffers->set_voxel_sunlight_atlas(atlas_output, sunlight);
	}
	Ref<RenderBufferDataForwardClustered> forward_buffers;
	if (render_buffers->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		forward_buffers = render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	}
	// Receiver ownership comes from the architectural hit payload even when an
	// off-grid volume exists; dynamic blockers are evaluated by the shared local
	// voxel BVH below rather than making the payload path globally unavailable.
	const bool use_hit_payload = bool(GLOBAL_GET("rendering/voxel_forward/architectural_hit_buffer/enabled")) && forward_buffers.is_valid() &&
			forward_buffers->has_voxel_hit() && forward_buffers->has_voxel_hit_position();
	if (!use_hit_payload) {
		ERR_PRINT_ONCE("Voxel Forward directional shadow resolve requires the canonical voxel hit and face-position buffers.");
		disable_voxel_lighting();
		return;
	}
	const uint32_t resolve_mode = 1u;
	const RID resolve_shader_rid = shadow_resolve_shader.version_get_shader(shadow_resolve_shader_version, resolve_mode);
	const RID resolve_pipeline = shadow_resolve_payload_pipeline;
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, depth }));
	RD::Uniform u_atlas(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, atlas_output }));
	RD::Uniform u_mask(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ mask_output }));
	RD::Uniform u_occupancy(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, Vector<RID>({ occupancy_uniform_buffer }));
	RD::Uniform u_dynamic_volumes(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, Vector<RID>({ ddgi_state.ddgi_dynamic_volume_buffer }));
	RD::Uniform u_dynamic_bvh(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, Vector<RID>({ ddgi_state.ddgi_dynamic_bvh_buffer }));
	RD::Uniform u_dynamic_directory(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, Vector<RID>({ ddgi_state.ddgi_dynamic_directory_buffer }));
	RD::Uniform u_dynamic_bricks(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25, Vector<RID>({ ddgi_state.ddgi_dynamic_brick_buffer }));
	const RID hit_payload = forward_buffers->get_voxel_hit();
	const RID hit_position = forward_buffers->get_voxel_hit_position();
	if (!RD::get_singleton()->texture_is_valid(hit_payload) || !RD::get_singleton()->texture_is_valid(hit_position)) {
		ERR_PRINT_ONCE("Voxel Forward directional shadow resolve received an invalid canonical receiver texture.");
		disable_voxel_lighting();
		return;
	}
	RD::Uniform u_hit_payload(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ sampler, hit_payload }));
	RD::Uniform u_hit_position(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ sampler, hit_position }));
	RID resolve_uniform_set = UniformSetCacheRD::get_singleton()->get_cache(resolve_shader_rid, 0, u_depth, u_atlas, u_mask, u_occupancy, u_hit_payload, u_hit_position, u_dynamic_volumes, u_dynamic_bvh, u_dynamic_directory, u_dynamic_bricks);

	ShadowResolvePushConstant resolve_push = {};
	const Projection view_projection = p_render_data->scene_data->get_view_projection(0) * Projection(p_render_data->scene_data->cam_transform.affine_inverse());
	RendererRD::MaterialStorage::store_camera(view_projection.inverse(), resolve_push.inv_view_projection);
	resolve_push.atlas_center_voxel_size[0] = ddgi_state.shadow_atlas_center.x;
	resolve_push.atlas_center_voxel_size[1] = ddgi_state.shadow_atlas_center.y;
	resolve_push.atlas_center_voxel_size[2] = ddgi_state.shadow_atlas_center.z;
	resolve_push.atlas_center_voxel_size[3] = ddgi_state.shadow_atlas_voxel_size;
	resolve_push.light_direction_near_extent[0] = ddgi_state.shadow_atlas_light_direction.x;
	resolve_push.light_direction_near_extent[1] = ddgi_state.shadow_atlas_light_direction.y;
	resolve_push.light_direction_near_extent[2] = ddgi_state.shadow_atlas_light_direction.z;
	resolve_push.light_direction_near_extent[3] = ddgi_state.shadow_atlas_near_extent;
	resolve_push.far_extent_bias[0] = ddgi_state.shadow_atlas_far_extent;
	resolve_push.atlas_filter[0] = ddgi_state.shadow_atlas_resolution;
	const bool soft_shadow_enabled = int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_mode")) == 1;
	const uint32_t sample_count = soft_shadow_enabled ? uint32_t(CLAMP(int(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_samples")), 1, 8)) : 1u;
	const uint32_t radius_q = uint32_t(CLAMP(Math::round(MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/soft_shadow_radius_voxels"))) * 16.0f), 0.0f, 4095.0f));
	resolve_push.atlas_filter[1] = int32_t(sample_count | (soft_shadow_enabled ? 0x80u : 0u) | (radius_q << 8u));
	resolve_push.far_extent_bias[1] = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/atlas_bias_voxels")));
	resolve_push.far_extent_bias[2] = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/atlas_slope_bias")));
	resolve_push.far_extent_bias[3] = MAX(resolve_push.far_extent_bias[1], float(GLOBAL_GET("rendering/voxel_forward/shadow_mask/atlas_bias_clamp_voxels")));

	RENDER_TIMESTAMP("Shadow Mask Resolve");
	RD::get_singleton()->draw_command_begin_label("Shadow Mask Resolve");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, resolve_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, resolve_uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &resolve_push, sizeof(ShadowResolvePushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, screen_size.x, screen_size.y, 1);
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_light_direction"), ddgi_state.shadow_atlas_light_direction);
	material_storage->global_shader_parameter_set_override(SNAME("voxel_forward_shadow_ready"), true);
}

void RenderVoxelForward::_render_buffers_debug_draw(const RenderDataRD *p_render_data) {
	RenderForwardClustered::_render_buffers_debug_draw(p_render_data);
	if (!bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/debug_view")) || p_render_data == nullptr || p_render_data->render_buffers.is_null()) {
		return;
	}
	Ref<RenderSceneBuffersRD> render_buffers = p_render_data->render_buffers;
	const StringName scope = SNAME("voxel_forward_shadow_mask");
	const StringName texture_name = SNAME("occupancy_shadow_mask");
	if (!render_buffers->has_texture(scope, texture_name)) {
		return;
	}
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	const RID render_target = render_buffers->get_render_target();
	const Size2i target_size = texture_storage->render_target_get_size(render_target);
	copy_effects->copy_to_fb_rect(render_buffers->get_texture(scope, texture_name), texture_storage->render_target_get_rd_framebuffer(render_target), Rect2i(Point2i(), target_size), false, true, false, false, RID(), false, true);
}

void RenderVoxelForward::_render_voxel_outline(RenderDataRD *p_render_data, RID p_framebuffer, uint32_t p_color_attachment_count) {
	if (!bool(GLOBAL_GET("rendering/voxel_forward/outline/enabled")) ||
			!bool(GLOBAL_GET("rendering/voxel_forward/architectural_hit_buffer/enabled")) ||
			p_render_data == nullptr || p_render_data->render_buffers.is_null() ||
			p_render_data->scene_data->view_count != 1 || p_color_attachment_count < 1 || p_color_attachment_count > 3) {
		return;
	}
	if (!volume_storage->has_enabled_outlines()) {
		return;
	}

	Ref<RenderSceneBuffersRD> render_buffers = p_render_data->render_buffers;
	Ref<RenderBufferDataForwardClustered> forward_buffers = render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	if (forward_buffers.is_null() || !forward_buffers->has_voxel_hit() ||
			!render_buffers->has_texture(RB_SCOPE_BUFFERS, RB_TEX_BACK_DEPTH)) {
		return;
	}
	RID instance_buffer = _get_opaque_instance_buffer();
	const uint32_t instance_count = _get_opaque_instance_count();
	if (!instance_buffer.is_valid() || instance_count == 0) {
		return;
	}

	_ensure_outline_resources();
	const uint32_t sample_index = uint32_t(render_buffers->get_texture_samples());
	if (sample_index >= RD::TEXTURE_SAMPLES_MAX) {
		return;
	}
	RID pipeline = outline_pipelines[sample_index][p_color_attachment_count - 1].get_render_pipeline(RD::INVALID_ID, RD::get_singleton()->framebuffer_get_format(p_framebuffer));
	if (!pipeline.is_valid()) {
		return;
	}

	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader = outline_shader.version_get_shader(outline_shader_version, 0);
	RD::Uniform u_hit(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, forward_buffers->get_voxel_hit() }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, render_buffers->get_texture(RB_SCOPE_BUFFERS, RB_TEX_BACK_DEPTH) }));
	RD::Uniform u_instances(RD::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC, 2, Vector<RID>({ instance_buffer }));
	RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(shader, 0, u_hit, u_depth, u_instances);

	OutlinePushConstant push_constant = {};
	const Size2i screen_size = render_buffers->get_internal_size();
	push_constant.screen_instances[0] = screen_size.x;
	push_constant.screen_instances[1] = screen_size.y;
	push_constant.screen_instances[2] = int32_t(instance_count);
	push_constant.instance_layout[0] = int32_t(_get_instance_data_stride_words());
	push_constant.instance_layout[1] = int32_t(_get_voxel_style_word_offset());
	push_constant.thresholds[0] = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/outline/depth_threshold")));
	push_constant.thresholds[1] = MAX(0.0f, float(GLOBAL_GET("rendering/voxel_forward/outline/planar_depth_tolerance")));

	RENDER_TIMESTAMP("Voxel Outline Post Process");
	RD::get_singleton()->draw_command_begin_label("Voxel Outline Post Process");
	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 1.0f, 0u, p_render_data->render_region);
	RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, pipeline);
	RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set, 0);
	RD::get_singleton()->draw_list_set_push_constant(draw_list, &push_constant, sizeof(OutlinePushConstant));
	RD::get_singleton()->draw_list_draw(draw_list, false, 1, 3);
	RD::get_singleton()->draw_list_end();
	RD::get_singleton()->draw_command_end_label();
}

void RenderVoxelForward::_render_ddgi_debug(RenderDataRD *p_render_data, RID p_framebuffer, uint32_t p_color_attachment_count) {
	const int debug_mode = int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/debug_mode"));
	if (debug_mode <= 0 || debug_mode > 10 || p_render_data == nullptr || p_render_data->render_buffers.is_null() ||
			p_render_data->scene_data == nullptr || p_render_data->scene_data->view_count != 1 ||
			p_color_attachment_count < 1 || p_color_attachment_count > 3 || ddgi_state.ddgi_probe_resolution == 0) {
		return;
	}
	_ensure_ddgi_debug_resources();
	const uint32_t sample_index = uint32_t(p_render_data->render_buffers->get_texture_samples());
	if (sample_index >= RD::TEXTURE_SAMPLES_MAX) {
		return;
	}
	const bool depth_test = bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/debug_depth_test"));
	PipelineCacheRD &pipeline_cache = depth_test ? ddgi_debug_depth_pipelines[sample_index][p_color_attachment_count - 1] : ddgi_debug_overlay_pipelines[sample_index][p_color_attachment_count - 1];
	RID pipeline = pipeline_cache.get_render_pipeline(RD::INVALID_ID, RD::get_singleton()->framebuffer_get_format(p_framebuffer));
	if (!pipeline.is_valid()) {
		return;
	}
	const int selected_lod = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/debug_lod")), -1, int(DDGI_LOD_COUNT - 1));
	const float marker_size = CLAMP(float(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/debug_marker_size")), 1.0f, 32.0f);
	const Size2i screen_size = p_render_data->render_buffers->get_internal_size();
	Projection view_projection = p_render_data->scene_data->get_view_projection(0) * Projection(p_render_data->scene_data->cam_transform.affine_inverse());
	RID shader = ddgi_debug_shader.version_get_shader(ddgi_debug_shader_version, 0);
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const VoxelForwardVolumeStorage::WorldOccupancy &world = volume_storage->get_world_occupancy();
	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 1.0f, 0u, p_render_data->render_region);
	RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, pipeline);
	for (uint32_t lod = 0; lod < DDGI_LOD_COUNT; lod++) {
		if ((selected_lod >= 0 && int(lod) != selected_lod) || !ddgi_state.ddgi_cascades[lod].probe_records.is_valid()) {
			continue;
		}
		RD::Uniform records(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ ddgi_state.ddgi_cascades[lod].probe_records }));
		RD::Uniform irradiance(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, ddgi_state.ddgi_cascades[lod].irradiance_rd }));
		RD::Uniform depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, ddgi_state.ddgi_cascades[lod].visibility_rd }));
		RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(shader, 0, records, irradiance, depth);
		DdgiDebugPushConstant push = {};
		RendererRD::MaterialStorage::store_camera(view_projection, push.view_projection);
		push.grid_origin_marker_size[0] = ddgi_state.ddgi_cascades[lod].origin.x;
		push.grid_origin_marker_size[1] = ddgi_state.ddgi_cascades[lod].origin.y;
		push.grid_origin_marker_size[2] = ddgi_state.ddgi_cascades[lod].origin.z;
		push.grid_origin_marker_size[3] = marker_size;
		push.cell_size[0] = ddgi_state.ddgi_cascades[lod].cell_size.x;
		push.cell_size[1] = ddgi_state.ddgi_cascades[lod].cell_size.y;
		push.cell_size[2] = ddgi_state.ddgi_cascades[lod].cell_size.z;
		push.cell_size[3] = ddgi_state.ddgi_cascades[lod].cell_size.length() * 4.0f;
		push.world_origin[0] = world.origin.x;
		push.world_origin[1] = world.origin.y;
		push.world_origin[2] = world.origin.z;
		push.grid_lod_debug[0] = ddgi_state.ddgi_probe_resolution;
		push.grid_lod_debug[1] = lod;
		push.grid_lod_debug[2] = debug_mode;
		push.screen_frame[0] = screen_size.x;
		push.screen_frame[1] = screen_size.y;
		push.screen_frame[2] = uint32_t(ddgi_state.ddgi_frame_index);
		RD::get_singleton()->draw_list_bind_uniform_set(draw_list, uniform_set, 0);
		RD::get_singleton()->draw_list_set_push_constant(draw_list, &push, sizeof(push));
		RD::get_singleton()->draw_list_draw(draw_list, false, ddgi_state.ddgi_probe_resolution * ddgi_state.ddgi_probe_resolution * ddgi_state.ddgi_probe_resolution, 6);
	}
	RD::get_singleton()->draw_list_end();
}

void RenderVoxelForward::_render_scene_custom_opaque(RenderDataRD *p_render_data, RID p_framebuffer, uint32_t p_color_pass_flags, uint32_t p_color_attachment_count, bool p_depth_prepass) {
	if (!bool(GLOBAL_GET("rendering/voxel_forward/experimental_custom_visibility")) || visible_volumes.is_empty() || p_render_data->scene_data->cam_orthogonal || p_render_data->scene_data->view_count != 1 || p_color_attachment_count < 1 || p_color_attachment_count > 3) {
		_render_voxel_outline(p_render_data, p_framebuffer, p_color_attachment_count);
		_render_ddgi_debug(p_render_data, p_framebuffer, p_color_attachment_count);
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
	_render_voxel_outline(p_render_data, p_framebuffer, p_color_attachment_count);
	_render_ddgi_debug(p_render_data, p_framebuffer, p_color_attachment_count);
}

} // namespace RendererSceneRenderImplementation
