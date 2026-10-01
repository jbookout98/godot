/**************************************************************************/
/*  render_voxel_forward.h                                                */
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

#pragma once

#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_ddgi_activate.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_ddgi_debug.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_ddgi_integrate.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_ddgi_record_scroll.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_ddgi_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_ddgi_seed.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_ddgi_temporal.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_ddgi_trace.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_indirect_blend.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_indirect_boundary_delta.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_indirect_inject.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_indirect_propagate.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_local_shadow_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_outline.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_reflection_color_inject.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_reflection_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_restir_denoise.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_restir_spatial.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_restir_temporal.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_shadow_atlas.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_shadow_atlas_scroll.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_shadow_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/voxel_forward/voxel_visibility.glsl.gen.h"
#include "servers/rendering/renderer_rd/voxel_forward/voxel_forward_volume_storage.h"
#include "servers/rendering/renderer_rd/voxel_forward/voxel_lighting_data.h"

namespace RendererSceneRenderImplementation {

class IndirectBoundaryReadbackReceiver;

// Voxel Forward intentionally starts as a behavior-preserving Forward+
// renderer. Voxel-owned visibility and lighting passes will be introduced
// behind this type one milestone at a time, keeping stock Forward+ available
// as a known-good fallback throughout development.
class RenderVoxelForward : public RenderForwardClustered {
	static RenderVoxelForward *bake_renderer;
	friend class IndirectBoundaryReadbackReceiver;

private:
	struct VisibleVolume {
		RID voxel_texture;
		RID brick_texture;
		RID palette_texture;
		Transform3D transform;
		Vector3i dimensions;
		float voxel_size = 0.1f;
	};

	struct DdgiGpuProbeRecord;
	struct BakedWorld {
		Ref<VoxelLightingData> data;
		ObjectID owner;
		HashMap<Vector4i, int> offsets;
		HashMap<RID, uint64_t> authored_volume_revisions;
		HashMap<RID, uint64_t> authored_local_light_versions;
		HashMap<RID, Transform3D> authored_local_light_transforms;
		uint64_t content_revision = UINT64_MAX;
		uint64_t shading_revision = UINT64_MAX;
		uint64_t lighting_revision = UINT64_MAX;
		uint64_t local_light_hash = UINT64_MAX;
		uint64_t restored_probes = 0;
		uint32_t initial_lods = 0;
		bool invalidated = false;
		bool source_verified = false;
		bool refresh_seeds = false;
		bool verification_pending = false;
		uint64_t ticket = 0;
		Callable verify_source;
	};
	HashMap<RID, BakedWorld> baked_worlds;
	uint64_t next_bake_ticket = 0;
	VoxelDdgiSeedShaderRD ddgi_seed_shader;
	RID ddgi_seed_version;
	RID ddgi_seed_pipeline;
	void _invalidate_changed_bake();
	void _seed_baked_records(uint32_t p_lod, const Vector<uint32_t> &p_slots, const Vector<DdgiGpuProbeRecord> &p_records);
	VoxelForwardVolumeStorage volume_registry;
	VoxelForwardVolumeStorage *volume_storage = &volume_registry;
	RID lighting_scenario;
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
	VoxelLocalShadowResolveShaderRD local_shadow_resolve_shader;
	RID local_shadow_resolve_shader_version;
	RID local_shadow_resolve_pipeline;
	RID local_shadow_uniform_buffer;
	RID local_shadow_disabled_uniform_buffer;
	RID local_shadow_mask_rd;
	Size2i local_shadow_screen_size;
	uint32_t local_shadow_layer_count = 0;
	bool local_shadow_masks_ready = false;
	uint32_t local_shadow_last_omni_count = UINT32_MAX;
	uint32_t local_shadow_last_spot_count = UINT32_MAX;
	VoxelIndirectInjectShaderRD indirect_inject_shader;
	RID indirect_inject_shader_version;
	RID indirect_inject_pipeline;
	VoxelIndirectPropagateShaderRD indirect_propagate_shader;
	RID indirect_propagate_shader_version;
	RID indirect_propagate_pipeline;
	VoxelIndirectBlendShaderRD indirect_blend_shader;
	RID indirect_blend_shader_version;
	RID indirect_blend_pipeline;
	VoxelIndirectBoundaryDeltaShaderRD indirect_boundary_delta_shader;
	RID indirect_boundary_delta_shader_version;
	RID indirect_boundary_delta_pipeline;
	IndirectBoundaryReadbackReceiver *indirect_boundary_readback_receiver = nullptr;
	VoxelDdgiTraceShaderRD ddgi_trace_shader;
	RID ddgi_trace_shader_version;
	RID ddgi_trace_pipeline;
	VoxelDdgiActivateShaderRD ddgi_activate_shader;
	RID ddgi_activate_shader_version;
	RID ddgi_activate_pipeline;
	VoxelDdgiRecordScrollShaderRD ddgi_record_scroll_shader;
	RID ddgi_record_scroll_shader_version;
	RID ddgi_record_scroll_pipeline;
	VoxelDdgiDebugShaderRD ddgi_debug_shader;
	RID ddgi_debug_shader_version;
	PipelineCacheRD ddgi_debug_overlay_pipelines[RD::TEXTURE_SAMPLES_MAX][3];
	PipelineCacheRD ddgi_debug_depth_pipelines[RD::TEXTURE_SAMPLES_MAX][3];
	bool ddgi_debug_resources_initialized = false;
	VoxelDdgiIntegrateShaderRD ddgi_integrate_shader;
	RID ddgi_integrate_shader_version;
	RID ddgi_integrate_pipeline;
	VoxelDdgiResolveShaderRD ddgi_resolve_shader;
	RID ddgi_resolve_shader_version;
	RID ddgi_resolve_pipeline;
	static constexpr uint32_t DDGI_FRAME_RESOURCE_COUNT = 3;
	VoxelDdgiTemporalShaderRD ddgi_temporal_shader;
	RID ddgi_temporal_shader_version;
	RID ddgi_temporal_pipeline;
	VoxelRestirTemporalShaderRD restir_temporal_shader;
	RID restir_temporal_shader_version;
	RID restir_temporal_pipeline;
	VoxelRestirSpatialShaderRD restir_spatial_shader;
	RID restir_spatial_shader_version;
	RID restir_spatial_pipeline;
	VoxelRestirDenoiseShaderRD restir_denoise_shader;
	RID restir_denoise_shader_version;
	RID restir_denoise_pipeline;
	VoxelReflectionColorInjectShaderRD reflection_color_inject_shader;
	RID reflection_color_inject_shader_version;
	RID reflection_color_inject_pipeline;
	VoxelReflectionResolveShaderRD reflection_resolve_shader;
	RID reflection_resolve_shader_version;
	RID reflection_resolve_pipeline;

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

	static constexpr uint32_t INDIRECT_CASCADE_COUNT = 3;
	// Six world-axis incident-radiance lobes are packed side-by-side along X in
	// every 3D texture. This keeps one sampler per cascade while retaining
	// directional transport and filtering.
	static constexpr uint32_t INDIRECT_DIRECTION_COUNT = 6;
	RID indirect_boundary_delta_buffer[INDIRECT_CASCADE_COUNT];
	RID indirect_grid_rd[INDIRECT_CASCADE_COUNT][2];
	RID indirect_staging_grid_rd[INDIRECT_CASCADE_COUNT][2];
	RID indirect_injection_grid_rd[INDIRECT_CASCADE_COUNT];
	RID indirect_color_grid_uniform_buffer;
	RID indirect_parent_grid_uniform_buffer[INDIRECT_CASCADE_COUNT];
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
	// Camera-only recenters reuse the world-space overlap from the active
	// clipmap. The shift is expressed as old texture cells sampled for new cell 0.
	bool indirect_staging_scrolled[INDIRECT_CASCADE_COUNT] = {};
	Vector3i indirect_staging_scroll_shift[INDIRECT_CASCADE_COUNT];
	bool indirect_staging_adaptive[INDIRECT_CASCADE_COUNT] = {};
	bool indirect_staging_boundary_waiting[INDIRECT_CASCADE_COUNT] = {};
	bool indirect_staging_boundary_ready[INDIRECT_CASCADE_COUNT] = {};
	float indirect_staging_boundary_delta[INDIRECT_CASCADE_COUNT] = {};
	uint32_t indirect_staging_expansion_count[INDIRECT_CASCADE_COUNT] = {};
	uint64_t indirect_staging_boundary_revision[INDIRECT_CASCADE_COUNT] = {};
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
	uint64_t indirect_last_invalidated_cell_count = 0;
	uint32_t indirect_last_convergence_expansions = 0;
	float indirect_last_boundary_delta = 0.0f;
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
	// Irradiance and visibility have different angular-frequency requirements.
	// Keep a one-texel guard border around every octahedral tile so filtered
	// samples can never read a neighboring probe.
	static constexpr uint32_t DDGI_IRRADIANCE_INTERIOR_SIZE = 8;
	static constexpr uint32_t DDGI_IRRADIANCE_TILE_SIZE = DDGI_IRRADIANCE_INTERIOR_SIZE + 2;
	static constexpr uint32_t DDGI_VISIBILITY_INTERIOR_SIZE = 16;
	static constexpr uint32_t DDGI_VISIBILITY_TILE_SIZE = DDGI_VISIBILITY_INTERIOR_SIZE + 2;
	static constexpr uint32_t DDGI_LOD_COUNT = 4;
	static constexpr uint32_t DDGI_PROBE_RECORD_SIZE = 48;
	struct DdgiCascade {
		RID irradiance_rd;
		RID visibility_rd;
		RID metadata_rd;
		RID irradiance_texture;
		RID visibility_texture;
		RID metadata_texture;
		RID probe_records;
		// CPU placement is uploaded here first. The merge compute pass owns the
		// authoritative records so local edits can preserve compatible history.
		RID probe_upload_records;
		Vector3 origin;
		Vector3i logical_origin;
		Vector3i phase_offset;
		Vector3 cell_size;
		bool initialized = false;
		uint64_t occupancy_revision = 0;
		uint64_t lighting_revision = 0;
		uint32_t active_update_cursor = 0;
		uint32_t urgent_update_cursor = 0;
		uint32_t edit_boost_updates_remaining = 0;
		RID probe_index_buffer;
		RID probe_counter_buffer;
		RID probe_dispatch_buffer;
		RID dynamic_bin_buffer;
		RID classify_uniform_set;
		RID integrate_uniform_set;
		RID trace_uniform_set[DDGI_FRAME_RESOURCE_COUNT];
		RID trace_shadow[DDGI_FRAME_RESOURCE_COUNT];
		RID trace_environment[DDGI_FRAME_RESOURCE_COUNT];
		RID trace_sampler[DDGI_FRAME_RESOURCE_COUNT];
		RID trace_sky_sampler[DDGI_FRAME_RESOURCE_COUNT];
		RID trace_color_grid[DDGI_FRAME_RESOURCE_COUNT];
		uint32_t dynamic_bin_capacity = 0;
		uint32_t probe_index_capacity = 0;
		uint64_t dynamic_bin_hash = 0;
		uint64_t dynamic_source_hash = 0;
		// External residency, geometry, or lighting changes provide a conservative
		// upper bound for probes entering a transitional state. Once that bounded
		// work has received its required updates, rebuild the category prefixes
		// exactly once instead of compacting the full grid after every integration.
		bool activation_dirty = true;
	};

	struct DdgiLocalLightGpuData {
		float position_range[4];
		float direction_type[4];
		float color_energy[4];
		float attenuation_source_size[4];
	};

	struct DdgiLocalLightCacheEntry {
		AABB influence_bounds;
		Transform3D transform;
		uint64_t version = 0;
	};

	struct DdgiDynamicVolumeGpuData {
		float world_to_voxel[3][4];
		float normal_to_world[3][4];
		float bounds_min[4];
		float bounds_max[4];
		uint32_t dimensions_directory_offset[4];
		uint32_t brick_dimensions_brick_offset[4];
		uint32_t storage[4];
	};

	struct DdgiResolveUniformData {
		float origins[DDGI_LOD_COUNT][4];
		float cell_sizes[DDGI_LOD_COUNT][4];
		int32_t phase_offsets[DDGI_LOD_COUNT][4];
		int32_t logical_origins[DDGI_LOD_COUNT][4];
		float camera_irradiance_size[4];
		float atlas_sizes[4];
		float tuning[4];
		int32_t screen_resolution_debug[4];
	};

	struct DdgiTemporalUniformData {
		float previous_view_projection[16];
		int32_t screen_history[4];
		float temporal[4];
	};

	struct DdgiDynamicBvhNodeGpuData {
		float bounds_min[4];
		float bounds_max[4];
		uint32_t children[4];
	};

	struct DdgiDynamicContentCacheEntry {
		RID base;
		uint64_t revision = 0;
		uint32_t directory_offset_words = 0;
		uint32_t directory_size_bytes = 0;
		uint32_t brick_offset_words = 0;
		uint32_t brick_size_bytes = 0;
		AABB previous_bounds;
		Transform3D previous_transform;
	};

	static constexpr uint32_t RESTIR_RESERVOIR_SIZE = 96;
	static constexpr uint32_t RESTIR_MAX_DIRTY_BRICKS = 256;
	RID restir_temporal_reservoir[2];
	RID restir_spatial_reservoir[2];
	RID restir_uniform_buffer;
	RID restir_dirty_brick_buffer;
	RID restir_output_texture;
	RID restir_output_rd;
	Size2i restir_screen_size;
	uint32_t restir_frame_index = 0;
	uint32_t restir_history_slot = 0;
	uint64_t restir_world_revision = UINT64_MAX;
	uint64_t restir_shading_revision = UINT64_MAX;
	Vector3 restir_light_direction;
	Color restir_light_color;
	float restir_light_energy = 0.0f;
	bool restir_history_valid = false;
	static constexpr uint32_t REFLECTION_CASCADE_COUNT = 3;
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
		float light_direction_near_extent[4];
		float far_extent_bias[4];
		int32_t atlas_filter[4];
	};
	static_assert(sizeof(ShadowResolvePushConstant) == 128, "Shadow resolve push constants must stay within Vulkan's portable 128-byte limit.");

	static constexpr uint32_t MAX_LOCAL_SHADOW_LIGHTS = 32;
	struct LocalShadowEntryData {
		float position_inv_radius[4];
		float direction_cone[4];
		int32_t indices[4];
	};

	struct LocalShadowUniformData {
		float inv_view_projection[16];
		int32_t state[4];
		float trace_settings[4];
		LocalShadowEntryData entries[MAX_LOCAL_SHADOW_LIGHTS];
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

	struct IndirectColorGridUniformData {
		float origin_cell_size[REFLECTION_CASCADE_COUNT][4];
		int32_t resolution[4];
	};

	struct IndirectParentGridUniformData {
		float origin_cell_size[4];
		int32_t state[4];
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

	struct IndirectBoundaryDeltaPushConstant {
		int32_t dispatch_origin_resolution[4];
		int32_t dispatch_size[4];
	};

	struct DdgiClassifyUniformData {
		float world_origin_voxel_size[4];
		int32_t grid_origin_lod[4];
		int32_t grid_resolution_phase[4];
		int32_t cell_size_voxels[4];
		uint32_t directory_revision[4];
		uint32_t schedule[4];
		uint32_t changes[4];
	};

	struct DdgiGpuProbeRecord {
		int32_t logical_cell_lod[4];
		float physical_position_valid[4];
		uint32_t state_revision_frame_flags[4];
	};

	struct DdgiTraceUniformData {
		float world_origin_voxel_size[4];
		float grid_origin_max_distance[4];
		float cell_size_lod[4];
		float color_origin_cell_size[4];
		float light_direction_energy[4];
		float light_color_bounce[4];
		float sky_color_energy[4];
		float sky_orientation[4];
		float sky_border_mode[4];
		float atlas_center_resolution[4];
		float tangent_near_extent[4];
		float bitangent_far_extent[4];
		int32_t probe_grid[4];
		int32_t logical_origin[4];
		int32_t directory_trace[4];
		int32_t atlas_layout[4];
		int32_t visibility_layout[4];
		int32_t update_state[4];
		int32_t scheduling[4];
		float query_settings[4];
		float camera_position_lod_transition[4];
		float cascade_origin[DDGI_LOD_COUNT][4];
		float cascade_cell_size[DDGI_LOD_COUNT][4];
		int32_t cascade_logical_origin[DDGI_LOD_COUNT][4];
		uint32_t local_lights[4];
	};

	struct RestirUniformData {
		float inv_view_projection[16];
		float previous_view_projection[16];
		float world_origin_voxel_size[4];
		float light_direction_energy[4];
		float light_color_intensity[4];
		float ambient_color_energy[4];
		float color_grid_origin_cell_size[REFLECTION_CASCADE_COUNT][4];
		int32_t screen[4];
		int32_t trace[4];
		uint32_t state[4];
		int32_t reuse[4];
		float limits[4];
		float validation[4];
	};

	struct RestirDirtyBrickData {
		int32_t coordinate[4];
	};

	struct DdgiIntegratePushConstant {
		int32_t probe_grid[4];
		int32_t irradiance_layout[4];
		int32_t visibility_layout[4];
		int32_t update_state[4];
		int32_t scheduling[4];
		float temporal[4];
		float dirty_temporal[4];
	};

	struct DdgiRecordScrollPushConstant {
		int32_t grid_resolution_lod[4];
		int32_t probe_shift[4];
	};

	struct DdgiDebugPushConstant {
		float view_projection[16];
		float grid_origin_marker_size[4];
		float cell_size[4];
		float world_origin[4];
		int32_t grid_lod_debug[4];
		int32_t screen_frame[4];
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
	void _update_dynamic_voxel_lighting_scene(const VoxelForwardVolumeStorage::WorldOccupancy &p_world);
	void _free_dynamic_voxel_lighting_scene();
	void _indirect_boundary_delta_readback(const PackedByteArray &p_data, uint32_t p_cascade, uint64_t p_world_revision);
	void _render_voxel_gi(const RenderDataRD *p_render_data, RID p_shadow_atlas, RID p_sampler, const Vector3 &p_light_direction, const Color &p_light_color, float p_light_energy);

	struct DdgiWorldState {
		RID shadow_mask_texture;
		RID shadow_mask_source_rd;
		Size2i shadow_mask_screen_size;
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
		RID reflection_color_grid_rd[REFLECTION_CASCADE_COUNT];
		RID reflection_material_grid_rd[REFLECTION_CASCADE_COUNT];
		Vector3 reflection_color_grid_origin[REFLECTION_CASCADE_COUNT];
		float reflection_color_grid_cell_size[REFLECTION_CASCADE_COUNT] = {};
		bool reflection_color_grid_initialized[REFLECTION_CASCADE_COUNT] = {};
		uint32_t reflection_color_grid_resolution = 0;
		uint64_t reflection_color_world_revision = UINT64_MAX;
		uint64_t reflection_color_shading_revision = UINT64_MAX;
		RID reflection_texture;
		RID reflection_source_rd;
		RID reflection_uniform_buffer;
		Size2i reflection_screen_size;
		ReflectionUniformData reflection_last_uniform_data = {};
		RID reflection_last_output;
		uint64_t reflection_last_world_revision = UINT64_MAX;
		uint64_t reflection_last_color_world_revision = UINT64_MAX;
		uint64_t reflection_last_indirect_low_latency_update_count = UINT64_MAX;
		uint64_t reflection_last_indirect_update_count = UINT64_MAX;
		bool reflection_resolve_cache_valid = false;
		RID ddgi_resolve_uniform_buffer[DDGI_FRAME_RESOURCE_COUNT];
		RID ddgi_temporal_uniform_buffer[DDGI_FRAME_RESOURCE_COUNT];
		RID ddgi_resolve_output_texture;
		RID ddgi_resolve_output_rd;
		RID ddgi_published_output_rd;
		RID ddgi_temporal_output_rd[2];
		RID ddgi_temporal_face_rd[2];
		uint32_t ddgi_temporal_slot = 0;
		bool ddgi_temporal_history_valid = false;
		int32_t ddgi_temporal_debug_mode = -1;
		Size2i ddgi_resolve_screen_size;
		DdgiCascade ddgi_cascades[DDGI_LOD_COUNT];
		uint32_t ddgi_probe_resolution = 0;
		uint32_t ddgi_irradiance_atlas_width = 0;
		uint32_t ddgi_irradiance_atlas_height = 0;
		uint32_t ddgi_visibility_atlas_width = 0;
		uint32_t ddgi_visibility_atlas_height = 0;
		uint32_t ddgi_schedule_cursor = 0;
		uint64_t ddgi_world_revision = UINT64_MAX;
		uint64_t ddgi_placement_revision = UINT64_MAX;
		uint64_t ddgi_page_publication_serial = UINT64_MAX;
		bool ddgi_incremental_edit_pending = false;
		uint64_t ddgi_frame_index = 0;
		uint64_t ddgi_lighting_revision = 1;
		Vector3 ddgi_light_direction;
		Color ddgi_light_color;
		float ddgi_light_energy = 0.0f;
		bool ddgi_light_valid = false;
		RID ddgi_directional_light_base;
		RID ddgi_sky;
		Basis ddgi_sky_orientation;
		Color ddgi_environment_color;
		float ddgi_environment_energy = 0.0f;
		int32_t ddgi_environment_mode = -1;
		bool ddgi_environment_valid = false;
		uint64_t ddgi_resident_cache_hits = 0;
		uint64_t ddgi_resident_empty_slots = 0;
		uint64_t ddgi_camera_placement_work_count = 0;
		uint64_t ddgi_camera_upload_bytes = 0;
		uint64_t ddgi_new_plane_upload_count = 0;
		RID ddgi_surfel_buffer;
		RID ddgi_trace_uniform_buffer[DDGI_FRAME_RESOURCE_COUNT];
		Vector<float> ddgi_dynamic_aabb_upload;
		uint32_t ddgi_surfel_capacity = 0;
		RID ddgi_local_light_buffer;
		uint32_t ddgi_local_light_buffer_capacity = 0;
		uint32_t ddgi_local_light_count = 0;
		uint32_t ddgi_local_light_overflow_count = 0;
		uint64_t ddgi_local_light_upload_hash = 0;
		HashMap<RID, DdgiLocalLightCacheEntry> ddgi_local_light_cache;
		RID ddgi_dynamic_volume_buffer;
		RID ddgi_dynamic_bvh_buffer;
		RID ddgi_dynamic_directory_buffer;
		RID ddgi_dynamic_brick_buffer;
		uint32_t ddgi_dynamic_volume_buffer_capacity = 0;
		uint32_t ddgi_dynamic_bvh_buffer_capacity = 0;
		uint32_t ddgi_dynamic_directory_buffer_capacity = 0;
		uint32_t ddgi_dynamic_brick_buffer_capacity = 0;
		uint32_t ddgi_dynamic_volume_count = 0;
		uint32_t ddgi_dynamic_bvh_node_count = 0;
		uint64_t ddgi_dynamic_scene_revision = 0;
		uint64_t ddgi_dynamic_uploaded_bytes = 0;
		Vector<DdgiDynamicContentCacheEntry> ddgi_dynamic_content_cache;
		Vector<AABB> ddgi_dynamic_current_bounds;
		Vector<AABB> ddgi_dynamic_dirty_bounds[DDGI_LOD_COUNT];
	};
	DdgiWorldState ddgi_state;
	HashMap<RID, DdgiWorldState> ddgi_worlds;
	bool ddgi_context_changed = false;
	void _free_world_lighting();
	void _render_restir_gi(const RenderDataRD *p_render_data, RID p_sampler, const Vector3 &p_light_direction, const Color &p_light_color, float p_light_energy);
	void _render_restir_gi_for_scene(const RenderDataRD *p_render_data);
	void _render_ddgi_gi_for_scene(const RenderDataRD *p_render_data);
	void _update_ddgi_local_lights(const RenderDataRD *p_render_data);
	void _render_ddgi_resolve(const RenderDataRD *p_render_data, RID p_sampler);
	void _free_restir_gi();
	void _render_ddgi_indirect_light(const RenderDataRD *p_render_data, RID p_shadow_atlas, RID p_sampler, const Vector3 &p_light_direction, const Color &p_light_color, float p_light_energy);
	void _free_ddgi();
	void _ddgi_upload_resident_probe_records(uint32_t p_lod, uint32_t p_resolution, const Vector3i &p_previous_origin, bool p_had_history, bool p_force_full, bool p_refresh_published_pages, bool p_camera_shift, const VoxelForwardVolumeStorage::WorldOccupancy &p_world);
	void _ensure_ddgi_debug_resources();
	void _render_ddgi_debug(RenderDataRD *p_render_data, RID p_framebuffer, uint32_t p_color_attachment_count);
	void _render_local_shadows(const RenderDataRD *p_render_data);
	void _free_local_shadows();
	void _release_indirect_light_snapshot();
	void _free_indirect_light();
	void _render_voxel_reflections(const RenderDataRD *p_render_data);
	void _free_voxel_reflections();
	void _ensure_visibility_resources();
	void _ensure_outline_resources();
	void _render_voxel_outline(RenderDataRD *p_render_data, RID p_framebuffer, uint32_t p_color_attachment_count);

protected:
	virtual void _add_voxel_occupancy_uniforms(Vector<RD::Uniform> &r_uniforms) override;
	virtual void _add_voxel_local_shadow_uniforms(LocalVector<RD::Uniform> &r_uniforms, bool p_multiview) override;
	virtual void _fill_voxel_instance_data(RID p_base, VoxelInstanceData &r_instance_data) const override;
	virtual bool _render_scene_custom_uses_resolved_depth() const override;
	virtual void _render_scene_custom_pre_opaque(RenderDataRD *p_render_data, bool p_depth_prepass) override;
	virtual void _render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) override;
	virtual void _render_buffers_debug_draw(const RenderDataRD *p_render_data) override;
	virtual void _render_scene_custom_opaque(RenderDataRD *p_render_data, RID p_framebuffer, uint32_t p_color_pass_flags, uint32_t p_color_attachment_count, bool p_depth_prepass) override;

public:
	void free_voxel_world(RID p_scenario) override;
	static void lighting_cache_set(RID p_scenario, ObjectID p_owner, Ref<VoxelLightingData> p_data, Callable p_verify);
	static void lighting_cache_confirm(RID p_scenario, ObjectID p_owner, int64_t p_ticket, bool p_valid, Array p_source_bases);
	static void lighting_cache_capture(RID p_scenario, AABB p_bounds, Callable p_callback);
	static void lighting_cache_status(RID p_scenario, bool p_probe_statistics, Callable p_callback);
	RenderVoxelForward();
	~RenderVoxelForward();
};

} // namespace RendererSceneRenderImplementation
