#pragma once

#include "core/object/object.h"
#include "core/templates/hash_set.h"
#include "scene/resources/image_texture.h"

class VoxelVolume3D;
class SceneTree;

// Internal service. Users never add this to a scene; VoxelVolume3D registers
// itself while inside a SceneTree.
class VoxelVolumeStreamingManager : public Object {
	SceneTree *connected_tree = nullptr;
	HashSet<ObjectID> volumes;
	HashSet<ObjectID> neighbor_refresh_volumes;
	// Evaluate initial residency on the first process frame. Later evaluations
	// retain the normal 12-frame cadence.
	int frame_counter = 11;
	bool streaming_pending = false;
	bool initial_residency_fill = true;
	int initial_load_batch_size = 0;
	bool neighbors_dirty = true;
	bool occupancy_dirty = true;
	bool occupancy_initialized = false;
	bool occupancy_texture_created = false;
	bool occupancy_coarse_texture_created = false;
	int occupancy_frame_counter = 0;
	Vector3 occupancy_center;
	Vector3 occupancy_origin;
	Vector3 occupancy_coarse_origin;
	int occupancy_resolution = 1;
	real_t occupancy_voxel_size = 0.1;
	Ref<ImageTexture3D> occupancy_texture;
	Ref<ImageTexture3D> occupancy_coarse_texture;

	void _process_frame();
	void _connect_tree(SceneTree *p_tree);
	void _update_streaming();
	void _update_neighbors();
	void _update_dirty_neighbors();
	void _update_occupancy();
	void _register_occupancy_globals();
	void _unregister_occupancy_globals();
	void _upload_occupancy(const PackedByteArray &p_bytes, int p_resolution, bool p_coarse);

public:
	static VoxelVolumeStreamingManager *singleton;
	static VoxelVolumeStreamingManager *get_singleton() { return singleton; }

	void register_volume(VoxelVolume3D *p_volume);
	void unregister_volume(VoxelVolume3D *p_volume);
	void mark_neighbors_dirty() { neighbors_dirty = true; }
	void mark_volume_neighbors_dirty(VoxelVolume3D *p_volume);
	void mark_occupancy_dirty() { occupancy_dirty = true; }

	VoxelVolumeStreamingManager();
	~VoxelVolumeStreamingManager();
};
