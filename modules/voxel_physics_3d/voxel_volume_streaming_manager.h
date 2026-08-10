#pragma once

#include "core/object/object.h"
#include "core/templates/hash_set.h"

class VoxelVolume3D;
class SceneTree;

// Internal service. Users never add this to a scene; VoxelVolume3D registers
// itself while inside a SceneTree.
class VoxelVolumeStreamingManager : public Object {
	SceneTree *connected_tree = nullptr;
	HashSet<ObjectID> volumes;
	// Evaluate initial residency on the first process frame. Later evaluations
	// retain the normal 12-frame cadence.
	int frame_counter = 11;
	bool neighbors_dirty = true;

	void _process_frame();
	void _connect_tree(SceneTree *p_tree);
	void _update_streaming();
	void _update_neighbors();

public:
	static VoxelVolumeStreamingManager *singleton;
	static VoxelVolumeStreamingManager *get_singleton() { return singleton; }

	void register_volume(VoxelVolume3D *p_volume);
	void unregister_volume(VoxelVolume3D *p_volume);
	void mark_neighbors_dirty() { neighbors_dirty = true; }

	VoxelVolumeStreamingManager();
	~VoxelVolumeStreamingManager();
};
