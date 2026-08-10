#include "voxel_volume_streaming_manager.h"

#include "core/config/project_settings.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "voxel_volume_3d.h"

VoxelVolumeStreamingManager *VoxelVolumeStreamingManager::singleton = nullptr;

struct VoxelStreamingCandidate {
	VoxelVolume3D *volume = nullptr;
	real_t distance_squared = 0.0;
	bool operator<(const VoxelStreamingCandidate &p_other) const { return distance_squared < p_other.distance_squared; }
};

struct VoxelNeighborSet {
	VoxelVolume3D *volume = nullptr;
	VoxelVolume3D *faces[VoxelVolume3D::NEIGHBOR_FACE_COUNT] = {};
};

static int _get_neighbor_face(const VoxelVolume3D *p_volume, const VoxelVolume3D *p_other) {
	const Ref<VoxelShapeData> data = p_volume->get_voxel_data();
	const Ref<VoxelShapeData> other_data = p_other->get_voxel_data();
	if (data.is_null() || other_data.is_null() || !p_volume->is_streaming_resident() || !p_other->is_streaming_resident()) {
		return -1;
	}
	if (!Math::is_equal_approx(data->get_voxel_size(), other_data->get_voxel_size())) {
		return -1;
	}
	const Transform3D relative = p_volume->get_global_transform().affine_inverse() * p_other->get_global_transform();
	if (!relative.basis.is_equal_approx(Basis())) {
		return -1;
	}
	const Vector3i dimensions = data->get_dimensions();
	const Vector3i other_dimensions = other_data->get_dimensions();
	const Vector3 size = Vector3(dimensions) * data->get_voxel_size();
	const Vector3 other_size = Vector3(other_dimensions) * other_data->get_voxel_size();
	const real_t tolerance = MAX(real_t(0.00001), data->get_voxel_size() * real_t(0.001));
	const Vector3 &origin = relative.origin;
	auto near_value = [tolerance](real_t p_a, real_t p_b) { return Math::abs(p_a - p_b) <= tolerance; };

	if (near_value(origin.x, -other_size.x) && near_value(origin.y, 0.0) && near_value(origin.z, 0.0) &&
			dimensions.y == other_dimensions.y && dimensions.z == other_dimensions.z) {
		return VoxelVolume3D::NEIGHBOR_NEGATIVE_X;
	}
	if (near_value(origin.x, size.x) && near_value(origin.y, 0.0) && near_value(origin.z, 0.0) &&
			dimensions.y == other_dimensions.y && dimensions.z == other_dimensions.z) {
		return VoxelVolume3D::NEIGHBOR_POSITIVE_X;
	}
	if (near_value(origin.x, 0.0) && near_value(origin.y, -other_size.y) && near_value(origin.z, 0.0) &&
			dimensions.x == other_dimensions.x && dimensions.z == other_dimensions.z) {
		return VoxelVolume3D::NEIGHBOR_NEGATIVE_Y;
	}
	if (near_value(origin.x, 0.0) && near_value(origin.y, size.y) && near_value(origin.z, 0.0) &&
			dimensions.x == other_dimensions.x && dimensions.z == other_dimensions.z) {
		return VoxelVolume3D::NEIGHBOR_POSITIVE_Y;
	}
	if (near_value(origin.x, 0.0) && near_value(origin.y, 0.0) && near_value(origin.z, -other_size.z) &&
			dimensions.x == other_dimensions.x && dimensions.y == other_dimensions.y) {
		return VoxelVolume3D::NEIGHBOR_NEGATIVE_Z;
	}
	if (near_value(origin.x, 0.0) && near_value(origin.y, 0.0) && near_value(origin.z, size.z) &&
			dimensions.x == other_dimensions.x && dimensions.y == other_dimensions.y) {
		return VoxelVolume3D::NEIGHBOR_POSITIVE_Z;
	}
	return -1;
}

VoxelVolumeStreamingManager::VoxelVolumeStreamingManager() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = this;
}

VoxelVolumeStreamingManager::~VoxelVolumeStreamingManager() {
	// SceneTree removes Object callables during teardown. At this initialization
	// level its signals may already be gone, so querying them here is invalid.
	connected_tree = nullptr;
	if (singleton == this) {
		singleton = nullptr;
	}
}

void VoxelVolumeStreamingManager::_connect_tree(SceneTree *p_tree) {
	if (connected_tree == p_tree) {
		return;
	}
	if (connected_tree != nullptr && connected_tree->is_connected(SNAME("process_frame"), callable_mp(this, &VoxelVolumeStreamingManager::_process_frame))) {
		connected_tree->disconnect(SNAME("process_frame"), callable_mp(this, &VoxelVolumeStreamingManager::_process_frame));
	}
	connected_tree = p_tree;
	if (connected_tree != nullptr) {
		connected_tree->connect(SNAME("process_frame"), callable_mp(this, &VoxelVolumeStreamingManager::_process_frame));
	}
}

void VoxelVolumeStreamingManager::register_volume(VoxelVolume3D *p_volume) {
	ERR_FAIL_NULL(p_volume);
	volumes.insert(p_volume->get_instance_id());
	neighbors_dirty = true;
	_connect_tree(p_volume->get_tree());
}

void VoxelVolumeStreamingManager::unregister_volume(VoxelVolume3D *p_volume) {
	if (p_volume != nullptr) {
		volumes.erase(p_volume->get_instance_id());
		neighbors_dirty = true;
	}
}

void VoxelVolumeStreamingManager::_process_frame() {
	// Residency does not need per-frame precision. This amortizes ranking costs
	// even with hundreds of 100^3 volumes.
	if (++frame_counter >= 12) {
		frame_counter = 0;
		_update_streaming();
	}
	if (neighbors_dirty) {
		_update_neighbors();
	}
}

void VoxelVolumeStreamingManager::_update_streaming() {
	const int max_resident = MAX(1, int(GLOBAL_GET("rendering/voxel_volume/max_resident_volumes")));
	const real_t global_distance = MAX(real_t(0.0), real_t(GLOBAL_GET("rendering/voxel_volume/streaming_distance")));
	Vector<VoxelStreamingCandidate> candidates;
	Vector<ObjectID> stale;
	for (const ObjectID &id : volumes) {
		VoxelVolume3D *volume = Object::cast_to<VoxelVolume3D>(ObjectDB::get_instance(id));
		if (volume == nullptr) {
			stale.push_back(id);
			continue;
		}
		if (volume->get_streaming_mode() == VoxelVolume3D::STREAMING_ALWAYS_RESIDENT) {
			volume->set_streaming_resident(true);
			continue;
		}
		if (volume->get_streaming_mode() == VoxelVolume3D::STREAMING_MANUAL) {
			continue;
		}
		Viewport *viewport = volume->get_viewport();
		Camera3D *camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
		if (camera == nullptr) {
			volume->set_streaming_resident(true);
			continue;
		}
		const real_t distance_limit = volume->get_streaming_distance() > 0.0 ? volume->get_streaming_distance() : global_distance;
		const Vector3 center = volume->get_global_transform().xform(volume->get_aabb().get_center());
		const real_t distance_squared = center.distance_squared_to(camera->get_global_position());
		if (distance_limit <= 0.0 || distance_squared <= distance_limit * distance_limit) {
			VoxelStreamingCandidate candidate;
			candidate.volume = volume;
			candidate.distance_squared = distance_squared;
			candidates.push_back(candidate);
		} else {
			volume->set_streaming_resident(false);
		}
	}
	for (const ObjectID &id : stale) {
		volumes.erase(id);
	}
	candidates.sort();
	for (int i = 0; i < candidates.size(); i++) {
		candidates[i].volume->set_streaming_resident(i < max_resident);
	}
}

void VoxelVolumeStreamingManager::_update_neighbors() {
	neighbors_dirty = false;
	Vector<VoxelNeighborSet> sets;
	Vector<ObjectID> stale;
	for (const ObjectID &id : volumes) {
		VoxelVolume3D *volume = Object::cast_to<VoxelVolume3D>(ObjectDB::get_instance(id));
		if (volume == nullptr) {
			stale.push_back(id);
			continue;
		}
		VoxelNeighborSet set;
		set.volume = volume;
		sets.push_back(set);
	}
	for (const ObjectID &id : stale) {
		volumes.erase(id);
	}

	for (int i = 0; i < sets.size(); i++) {
		for (int j = 0; j < sets.size(); j++) {
			if (i == j) {
				continue;
			}
			const int face = _get_neighbor_face(sets[i].volume, sets[j].volume);
			if (face >= 0 && sets[i].faces[face] == nullptr) {
				sets.write[i].faces[face] = sets[j].volume;
			}
		}
	}
	for (VoxelNeighborSet &set : sets) {
		set.volume->update_neighbor_faces(set.faces);
	}
}
