/**************************************************************************/
/*  voxel_forward_volume_storage.h                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "core/math/vector3i.h"
#include "core/math/transform_3d.h"
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
	struct Volume {
		RID base;
		RID voxel_texture;
		RID brick_texture;
		RID palette_texture;
		RID material_texture;
		PackedByteArray occupancy_directory_cpu;
		PackedByteArray occupancy_bricks_cpu;
		Transform3D transform;
		Vector3i dimensions;
		Vector3i brick_dimensions;
		Vector3i atlas_brick_dimensions;
		float voxel_size = 0.1f;
		uint64_t revision = 0;
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
		Vector<Vector3i> last_dirty_bricks;
		uint64_t last_incremental_revision = 0;
		uint64_t incremental_update_count = 0;
		uint64_t full_rebuild_count = 0;
		uint64_t revision = 0;
	};

private:
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
	};

	static VoxelForwardVolumeStorage *singleton;
	HashMap<RID, Volume> volumes;
	WorldOccupancy world_occupancy;
	uint64_t occupancy_gpu_bytes = 0;
	uint64_t world_occupancy_gpu_bytes = 0;
	uint32_t world_occupancy_directory_capacity_bytes = 0;
	uint32_t world_occupancy_brick_capacity_bytes = 0;
	PackedByteArray world_occupancy_directory_cpu;
	PackedByteArray world_occupancy_bricks_cpu;
	HashSet<Vector3i> pending_incremental_world_bricks;
	Vector<uint32_t> world_occupancy_free_mixed_slots;
	bool world_occupancy_dirty = true;
	uint32_t world_occupancy_quiet_frames = 0;
	WorkerThreadPool::TaskID world_occupancy_build_task = WorkerThreadPool::INVALID_TASK_ID;
	WorldOccupancyBuild *world_occupancy_build = nullptr;

	void _free_world_gpu_resources();
	void _request_full_world_rebuild();
	bool _queue_incremental_volume_change(const Volume &p_previous, const Volume &p_current, const Vector3i &p_dirty_position, const Vector3i &p_dirty_size);
	bool _apply_incremental_world_updates();
	static void _build_world_occupancy(void *p_userdata);
	void _finish_world_occupancy_build();

public:
	static VoxelForwardVolumeStorage *get_singleton() { return singleton; }

	static void volume_set_on_render_thread(RID p_base, RID p_voxel_texture, RID p_brick_texture, RID p_palette_texture, RID p_material_texture, PackedByteArray p_occupancy_directory, PackedByteArray p_occupancy_bricks, Vector3i p_dimensions, Vector3i p_brick_dimensions, Vector3i p_atlas_brick_dimensions, Transform3D p_transform, float p_voxel_size, Vector3i p_dirty_position, Vector3i p_dirty_size, int64_t p_revision);
	static void volume_transform_set_on_render_thread(RID p_base, Transform3D p_transform);
	static void volume_remove_on_render_thread(RID p_base);
	void update_world_occupancy(bool p_enabled);

	const Volume *get_volume(RID p_base) const;
	const HashMap<RID, Volume> &get_volumes() const { return volumes; }
	const WorldOccupancy &get_world_occupancy() const { return world_occupancy; }
	uint32_t get_volume_count() const { return volumes.size(); }
	uint64_t get_occupancy_gpu_bytes() const { return occupancy_gpu_bytes; }

	VoxelForwardVolumeStorage();
	~VoxelForwardVolumeStorage();
};

} // namespace RendererSceneRenderImplementation
