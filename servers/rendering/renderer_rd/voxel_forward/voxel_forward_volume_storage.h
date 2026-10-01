/**************************************************************************/
/*  voxel_forward_volume_storage.h                                        */
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

#include "core/math/color.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3i.h"
#include "core/object/worker_thread_pool.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

namespace RendererSceneRenderImplementation {

// Render-thread-owned description of the GPU resources backing a voxel
// volume. Scene nodes enqueue changes through RenderingServer so registration,
// texture creation, and texture destruction retain command ordering.
class VoxelForwardVolumeStorage {
public:
	// Fixed descriptor capacity, deliberately above the default 515-volume
	// residency budget. Index zero is a normal usable slot.
	static constexpr uint32_t MAX_BATCH_TEXTURES = 576;
	static constexpr uint32_t INVALID_BATCH_TEXTURE_INDEX = UINT32_MAX;
	static constexpr uint32_t DDGI_PLACEMENT_LOD_COUNT = 4;
	static constexpr int DDGI_PLACEMENT_PAGE_SIZE = 4;
	static constexpr int DDGI_PLACEMENT_PAGE_PROBE_COUNT = DDGI_PLACEMENT_PAGE_SIZE * DDGI_PLACEMENT_PAGE_SIZE * DDGI_PLACEMENT_PAGE_SIZE;
	enum DdgiProbePlacementFlags : uint32_t {
		DDGI_PROBE_CELL_OCCUPIED = 1u << 0,
		DDGI_PROBE_CELL_SOLID = 1u << 1,
		DDGI_PROBE_CELL_NEAR_SURFACE = 1u << 2,
	};

	enum DdgiProbeInitialState : uint32_t {
		DDGI_PROBE_OFF = 0,
		DDGI_PROBE_SLEEPING = 1,
		DDGI_PROBE_NEWLY_AWAKE = 2,
		DDGI_PROBE_NEWLY_VIGILANT = 3,
		DDGI_PROBE_AWAKE = 4,
		DDGI_PROBE_VIGILANT = 5,
	};

	struct DdgiProbePlacement {
		Vector3 offset;
		Vector3 position;
		Vector3i logical_cell;
		uint64_t stable_id = 0;
		uint64_t revision = 0;
		uint32_t flags = 0;
		uint32_t initial_state = DDGI_PROBE_OFF;
		bool valid = false;
	};

	struct DdgiPlacementPageSeed {
		uint64_t relevant_mask = 0;
		uint64_t occupied_mask = 0;
	};

	struct DdgiPlacementPage {
		Vector3i coordinate;
		uint64_t revision = 0;
		uint64_t relevant_mask = 0;
		DdgiProbePlacement records[DDGI_PLACEMENT_PAGE_PROBE_COUNT];
	};

	struct Volume {
		RID base;
		RID voxel_texture;
		RID brick_texture;
		RID neighbor_texture;
		RID palette_texture;
		// Legacy packed roughness/metallic/emission palette. Separate channel
		// textures below are authoritative whenever their corresponding flag is set.
		RID material_texture;
		RID metallic_texture;
		RID specularity_texture;
		RID emission_texture;
		RID transparency_texture;
		bool casts_shadow = true;
		bool has_metallic_texture = false;
		bool has_specularity_texture = false;
		bool has_emission_texture = false;
		bool has_transparency_texture = false;
		PackedByteArray occupancy_directory_cpu;
		PackedByteArray occupancy_bricks_cpu;
		Transform3D transform;
		Vector3i dimensions;
		Vector3i brick_dimensions;
		Vector3i atlas_brick_dimensions;
		uint32_t occupied_brick_count = 0;
		uint32_t batch_texture_index = INVALID_BATCH_TEXTURE_INDEX;
		uint32_t neighbor_mask = 0;
		uint32_t neighbor_diagonal_mask = 0;
		uint32_t outline_color_rgba8 = 0;
		float voxel_size = 0.1f;
		float outline_width = 0.0f;
		uint64_t revision = 0;
		uint64_t lighting_revision = 0;
	};

	struct WorldOccupancy {
		RID directory_buffer;
		RID brick_buffer;
		Vector3 origin;
		float voxel_size = 0.0f;
		uint32_t directory_mask = 0;
		uint32_t occupied_brick_count = 0;
		uint32_t mixed_brick_count = 0;
		uint32_t max_probe_count = 0;
		uint32_t tombstone_count = 0;
		uint32_t incompatible_volume_count = 0;
		uint32_t last_dirty_brick_count = 0;
		uint32_t last_uploaded_bytes = 0;
		uint32_t last_candidate_volume_checks = 0;
		uint32_t last_intersecting_volume_count = 0;
		uint32_t last_buffer_update_call_count = 0;
		uint32_t last_added_voxel_count = 0;
		uint32_t last_removed_voxel_count = 0;
		bool last_face_transmittance_decreased = false;
		bool last_face_transmittance_increased = false;
		uint64_t spatial_index_payload_bytes = 0;
		uint64_t last_incremental_update_usec = 0;
		uint64_t last_full_upload_usec = 0;
		Vector<Vector3i> last_dirty_bricks;
		uint64_t last_incremental_revision = 0;
		uint64_t incremental_update_count = 0;
		uint64_t full_rebuild_count = 0;
		uint64_t revision = 0;
		uint64_t ddgi_probe_cache_revision = 0;
		uint64_t ddgi_last_probe_bake_usec = 0;
		uint64_t ddgi_full_probe_bake_count = 0;
		uint64_t ddgi_incremental_probe_bake_count = 0;
		uint32_t ddgi_last_dirty_probe_cell_count = 0;
		uint32_t ddgi_cached_probe_count[DDGI_PLACEMENT_LOD_COUNT] = {};
		uint32_t ddgi_cached_page_count[DDGI_PLACEMENT_LOD_COUNT] = {};
		uint32_t ddgi_pending_page_count = 0;
		uint32_t ddgi_last_pages_built = 0;
		uint64_t ddgi_page_publication_serial = 0;
		// Published with occupancy and probe placement so every DDGI stage uses
		// the same cascade layout while a runtime spacing change rebuilds.
		uint32_t ddgi_cell_spacing_voxels[DDGI_PLACEMENT_LOD_COUNT] = { 8, 16, 32, 64 };
		Vector<Vector3i> ddgi_last_published_pages[DDGI_PLACEMENT_LOD_COUNT];
		Vector<uint64_t> ddgi_last_published_invalidation_masks[DDGI_PLACEMENT_LOD_COUNT];
		String last_full_rebuild_reason = "initial build";
	};

private:
	static constexpr int WORLD_VOLUME_SPATIAL_CELL_BRICKS = 16;

	struct WorldVolumeSpatialEntry {
		Vector3i cell_begin;
		Vector3i cell_end;
		bool indexed = false;
	};

	struct WorldOccupancyBuild {
		Vector<Volume> volumes;
		PackedByteArray directory_bytes;
		PackedByteArray brick_bytes;
		Vector3 origin;
		float voxel_size = 0.0f;
		uint32_t directory_mask = 0;
		uint32_t occupied_brick_count = 0;
		uint32_t mixed_brick_count = 0;
		uint32_t max_probe_count = 0;
		uint32_t incompatible_volume_count = 0;
		uint32_t minimum_directory_capacity = 0;
		uint32_t brick_aligned_volume_count = 0;
		uint64_t build_usec = 0;
		uint64_t requested_revision = 0;
		uint32_t ddgi_cell_spacing_voxels[DDGI_PLACEMENT_LOD_COUNT] = { 8, 16, 32, 64 };
		HashSet<Vector3i> occupied_bricks;
		HashMap<Vector3i, DdgiPlacementPageSeed> ddgi_page_seeds[DDGI_PLACEMENT_LOD_COUNT];
	};

	struct DdgiPlacementJob {
		uint32_t lod = 0;
		Vector3i page_coordinate;
		DdgiPlacementPageSeed seed;
		uint64_t invalidation_mask = UINT64_MAX;
	};

	struct DdgiPlacementBatch {
		PackedByteArray directory_bytes;
		PackedByteArray brick_bytes;
		uint32_t directory_mask = 0;
		uint32_t max_probe_count = 0;
		Vector3 world_origin;
		float voxel_size = 0.0f;
		uint64_t occupancy_revision = 0;
		uint32_t ddgi_cell_spacing_voxels[DDGI_PLACEMENT_LOD_COUNT] = { 8, 16, 32, 64 };
		uint64_t started_usec = 0;
		Vector<DdgiPlacementJob> jobs;
		Vector<DdgiPlacementPage> results;
		uint32_t publish_cursor = 0;
	};

	static VoxelForwardVolumeStorage *singleton;
	HashMap<RID, VoxelForwardVolumeStorage *> scenario_stores;
	HashMap<RID, RID> base_scenarios;
	bool registry = false;
	HashMap<RID, Volume> volumes;
	Vector<RID> batch_voxel_textures;
	Vector<RID> batch_brick_textures;
	Vector<RID> batch_neighbor_textures;
	Vector<uint32_t> free_batch_texture_indices;
	uint64_t batch_texture_revision = 1;
	uint64_t shading_texture_revision = 1;
	uint64_t lighting_content_revision = 1;
	uint32_t enabled_outline_volume_count = 0;
	WorldOccupancy world_occupancy;
	uint64_t occupancy_gpu_bytes = 0;
	uint64_t world_occupancy_gpu_bytes = 0;
	uint32_t world_occupancy_directory_capacity_bytes = 0;
	uint32_t world_occupancy_brick_capacity_bytes = 0;
	PackedByteArray world_occupancy_directory_cpu;
	PackedByteArray world_occupancy_bricks_cpu;
	HashSet<Vector3i> world_occupied_bricks_cpu;
	HashMap<Vector3i, DdgiPlacementPage> ddgi_placement_pages[DDGI_PLACEMENT_LOD_COUNT];
	HashMap<Vector3i, DdgiPlacementPageSeed> ddgi_page_seeds[DDGI_PLACEMENT_LOD_COUNT];
	HashSet<Vector3i> pending_ddgi_page_set[DDGI_PLACEMENT_LOD_COUNT];
	Vector<Vector3i> pending_ddgi_pages[DDGI_PLACEMENT_LOD_COUNT];
	HashMap<Vector3i, uint64_t> pending_ddgi_page_invalidation_masks[DDGI_PLACEMENT_LOD_COUNT];
	Vector3 ddgi_load_priority_world_position;
	uint32_t ddgi_load_priority_resolution = 0;
	bool ddgi_load_priority_valid = false;
	uint64_t ddgi_load_priority_applied_revision = UINT64_MAX;
	WorkerThreadPool::GroupID ddgi_placement_group_task = -1;
	DdgiPlacementBatch *ddgi_placement_batch = nullptr;
	uint32_t ddgi_requested_cell_spacing_voxels[DDGI_PLACEMENT_LOD_COUNT] = { 8, 16, 32, 64 };
	HashSet<Vector3i> pending_incremental_world_bricks;
	Vector<uint32_t> world_occupancy_free_mixed_slots;
	HashMap<Vector3i, Vector<RID>> world_volume_spatial_cells;
	HashMap<RID, WorldVolumeSpatialEntry> world_volume_spatial_entries;
	HashSet<RID> world_volume_unindexed;
	Vector<AABB> pending_lighting_dirty_bounds;
	uint32_t world_volume_spatial_reference_count = 0;
	bool world_volume_spatial_index_valid = false;
	bool world_occupancy_dirty = true;
	bool world_grid_selection_dirty = true;
	Vector3 selected_world_origin;
	float selected_world_voxel_size = 0.0f;
	uint32_t world_occupancy_quiet_frames = 0;
	uint32_t world_occupancy_wait_frames = 0;
	WorkerThreadPool::TaskID world_occupancy_build_task = WorkerThreadPool::INVALID_TASK_ID;
	WorldOccupancyBuild *world_occupancy_build = nullptr;

	void _free_world_gpu_resources();
	void _clear_world_volume_spatial_index();
	void _rebuild_world_volume_spatial_index();
	void _remove_world_volume_spatial_entry(RID p_base);
	void _insert_world_volume_spatial_entry(RID p_base, const Volume &p_volume);
	bool _is_volume_in_world_occupancy(const Volume &p_volume) const;
	void _request_full_world_rebuild(const char *p_reason);
	void _select_world_grid();
	bool _queue_incremental_volume_extent(const Volume &p_volume);
	bool _queue_incremental_volume_change(const Volume &p_previous, const Volume &p_current, const Vector3i &p_dirty_position, const Vector3i &p_dirty_size);
	bool _apply_incremental_world_updates();
	void _queue_ddgi_pages_for_dirty_bricks(const Vector<Vector3i> &p_dirty_bricks);
	void _publish_ddgi_page_seeds(const HashMap<Vector3i, DdgiPlacementPageSeed> (&p_seeds)[DDGI_PLACEMENT_LOD_COUNT], bool p_reset_placement);
	void _process_ddgi_placement_page_queue(uint32_t p_publish_budget = 128);
	void _clear_ddgi_placement_batch();
	static void _build_ddgi_placement_page(void *p_userdata, uint32_t p_index);
	static void _build_world_occupancy(void *p_userdata);
	void _finish_world_occupancy_build();

public:
	static VoxelForwardVolumeStorage *get_singleton() { return singleton; }

	static void volume_set_on_render_thread(RID p_scenario, RID p_base, RID p_voxel_texture, RID p_brick_texture, RID p_neighbor_texture, RID p_palette_texture, RID p_material_texture, RID p_metallic_texture, RID p_specularity_texture, RID p_emission_texture, RID p_transparency_texture, bool p_has_metallic_texture, bool p_has_specularity_texture, bool p_has_emission_texture, bool p_has_transparency_texture, PackedByteArray p_occupancy_directory, PackedByteArray p_occupancy_bricks, Vector3i p_dimensions, Vector3i p_brick_dimensions, Vector3i p_atlas_brick_dimensions, Transform3D p_transform, float p_voxel_size, int p_occupied_brick_count, int p_neighbor_mask, int p_neighbor_diagonal_mask, bool p_outline_enabled, Color p_outline_color, float p_outline_width, Vector3i p_dirty_position, Vector3i p_dirty_size, int64_t p_revision, bool p_casts_shadow);
	static void volume_transform_set_on_render_thread(RID p_base, Transform3D p_transform);
	static void volume_neighbors_set_on_render_thread(RID p_base, RID p_neighbor_texture, int p_neighbor_mask, int p_neighbor_diagonal_mask);
	static void volume_remove_on_render_thread(RID p_base);
	void update_world_occupancy(bool p_enabled);

	const Volume *get_volume(RID p_base) const;
	const HashMap<RID, Volume> &get_volumes() const { return volumes; }
	void get_non_world_occupancy_volumes(Vector<const Volume *> &r_volumes) const;
	void consume_lighting_dirty_bounds(Vector<AABB> &r_bounds);
	const WorldOccupancy &get_world_occupancy() const { return world_occupancy; }
	bool get_ddgi_probe_placement(uint32_t p_lod, const Vector3i &p_logical_cell, DdgiProbePlacement &r_placement) const;
	void prioritize_queued_ddgi_pages_for_world_load(const Vector3 &p_world_position, uint32_t p_spatial_resolution);
	bool create_world_occupancy_snapshot(WorldOccupancy &r_snapshot) const;
	const Vector<RID> &get_batch_voxel_textures() const { return batch_voxel_textures; }
	const Vector<RID> &get_batch_brick_textures() const { return batch_brick_textures; }
	const Vector<RID> &get_batch_neighbor_textures() const { return batch_neighbor_textures; }
	uint64_t get_batch_texture_revision() const { return batch_texture_revision; }
	uint64_t get_shading_texture_revision() const { return shading_texture_revision; }
	uint64_t get_lighting_content_revision() const { return lighting_content_revision; }
	uint32_t get_volume_count() const { return volumes.size(); }
	bool has_enabled_outlines() const { return enabled_outline_volume_count > 0; }
	uint64_t get_occupancy_gpu_bytes() const { return occupancy_gpu_bytes; }

	void free_scenario_storage(RID p_scenario);
	VoxelForwardVolumeStorage *get_scenario_storage(RID p_scenario);
	VoxelForwardVolumeStorage(bool p_registry = true);
	~VoxelForwardVolumeStorage();
};

} // namespace RendererSceneRenderImplementation
