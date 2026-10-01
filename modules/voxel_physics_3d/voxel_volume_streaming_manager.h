#pragma once

#include "core/object/object.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"
#include "scene/resources/image_texture.h"

class VoxelVolume3D;
class SceneTree;
struct VoxelNeighborIndexCache;

// Internal service. Users never add this to a scene; VoxelVolume3D registers
// itself while inside a SceneTree.
class VoxelVolumeStreamingManager : public Object {
	SceneTree *connected_tree = nullptr;
	HashSet<ObjectID> volumes;
	HashSet<ObjectID> neighbor_refresh_volumes;
	Vector<ObjectID> neighbor_rebuild_queue;
	int neighbor_rebuild_index = 0;
	bool neighbor_rebuild_rescan = false;
	// Evaluate initial residency on the first process frame. Later evaluations
	// retain the normal 12-frame cadence.
	int frame_counter = 11;
	bool streaming_pending = false;
	bool streaming_evaluated = false;
	int selected_target_count = 0;
	bool neighbors_dirty = true;
	VoxelNeighborIndexCache *neighbor_index_cache = nullptr;
	Ref<ImageTexture3D> voxel_forward_fallback_texture;
	Ref<ImageTexture> voxel_forward_fallback_texture_2d;

	void _process_frame();
	void _connect_tree(SceneTree *p_tree);
	void _update_streaming();
	void _update_neighbors();
	void _update_dirty_neighbors();
	void _register_voxel_forward_globals();

public:
	static VoxelVolumeStreamingManager *singleton;
	static VoxelVolumeStreamingManager *get_singleton() { return singleton; }

	Dictionary get_streaming_status() const;
	void register_volume(VoxelVolume3D *p_volume);
	void unregister_volume(VoxelVolume3D *p_volume);
	void mark_neighbors_dirty() { neighbors_dirty = true; }
	void mark_volume_neighbors_dirty(VoxelVolume3D *p_volume);

	VoxelVolumeStreamingManager();
	~VoxelVolumeStreamingManager();
};
