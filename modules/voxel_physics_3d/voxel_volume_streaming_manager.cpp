/**************************************************************************/
/*  voxel_volume_streaming_manager.cpp                                    */
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

#include "voxel_volume_streaming_manager.h"

#include "voxel_volume_3d.h"

#include "core/config/project_settings.h"
#include "core/math/vector4.h"
#include "core/object/callable_mp.h"
#include "core/templates/hash_map.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_server.h"

VoxelVolumeStreamingManager *VoxelVolumeStreamingManager::singleton = nullptr;

static constexpr const char *VOXEL_FORWARD_SHADOW_MASK_NAME = "voxel_forward_shadow_mask";
static constexpr const char *VOXEL_FORWARD_SHADOW_LIGHT_DIRECTION_NAME = "voxel_forward_shadow_light_direction";
static constexpr const char *VOXEL_FORWARD_SHADOW_READY_NAME = "voxel_forward_shadow_ready";
static constexpr const char *VOXEL_FORWARD_INDIRECT_NEAR_NAME = "voxel_forward_indirect_near";
static constexpr const char *VOXEL_FORWARD_INDIRECT_FAR_NAME = "voxel_forward_indirect_far";
static constexpr const char *VOXEL_FORWARD_INDIRECT_DISTANT_NAME = "voxel_forward_indirect_distant";
static constexpr const char *VOXEL_FORWARD_INDIRECT_NEAR_ORIGIN_NAME = "voxel_forward_indirect_near_origin";
static constexpr const char *VOXEL_FORWARD_INDIRECT_FAR_ORIGIN_NAME = "voxel_forward_indirect_far_origin";
static constexpr const char *VOXEL_FORWARD_INDIRECT_DISTANT_ORIGIN_NAME = "voxel_forward_indirect_distant_origin";
static constexpr const char *VOXEL_FORWARD_INDIRECT_NEAR_CELL_SIZE_NAME = "voxel_forward_indirect_near_cell_size";
static constexpr const char *VOXEL_FORWARD_INDIRECT_FAR_CELL_SIZE_NAME = "voxel_forward_indirect_far_cell_size";
static constexpr const char *VOXEL_FORWARD_INDIRECT_DISTANT_CELL_SIZE_NAME = "voxel_forward_indirect_distant_cell_size";
static constexpr const char *VOXEL_FORWARD_INDIRECT_RESOLUTION_NAME = "voxel_forward_indirect_resolution";
static constexpr const char *VOXEL_FORWARD_INDIRECT_TRANSITION_CELLS_NAME = "voxel_forward_indirect_transition_cells";
static constexpr const char *VOXEL_FORWARD_INDIRECT_INTENSITY_NAME = "voxel_forward_indirect_intensity";
static constexpr const char *VOXEL_FORWARD_INDIRECT_READY_NAME = "voxel_forward_indirect_ready";
static constexpr const char *VOXEL_FORWARD_INDIRECT_BACKEND_NAME = "voxel_forward_indirect_backend";
static constexpr const char *VOXEL_FORWARD_INDIRECT_DEBUG_MODE_NAME = "voxel_forward_indirect_debug_mode";
static constexpr const char *VOXEL_FORWARD_RESTIR_GI_NAME = "voxel_forward_restir_gi";
static constexpr const char *VOXEL_FORWARD_RESTIR_READY_NAME = "voxel_forward_restir_ready";
static constexpr const char *VOXEL_FORWARD_CORNER_AO_STRENGTH_NAME = "voxel_forward_corner_ao_strength";
static constexpr const char *VOXEL_FORWARD_CORNER_AO_TINT_NAME = "voxel_forward_corner_ao_tint";
static constexpr const char *VOXEL_FORWARD_INDIRECT_DIRTY_MIN_NAMES[3] = { "voxel_forward_indirect_dirty_min0", "voxel_forward_indirect_dirty_min1", "voxel_forward_indirect_dirty_min2" };
static constexpr const char *VOXEL_FORWARD_INDIRECT_DIRTY_MAX_NAMES[3] = { "voxel_forward_indirect_dirty_max0", "voxel_forward_indirect_dirty_max1", "voxel_forward_indirect_dirty_max2" };
static constexpr const char *VOXEL_FORWARD_INDIRECT_STAGING_MASK_NAME = "voxel_forward_indirect_staging_mask";
static constexpr const char *VOXEL_FORWARD_INDIRECT_ACTIVE_REVISIONS_NAME = "voxel_forward_indirect_active_revisions";
static constexpr const char *VOXEL_FORWARD_INDIRECT_STAGING_REVISIONS_NAME = "voxel_forward_indirect_staging_revisions";
static constexpr const char *VOXEL_FORWARD_INDIRECT_UPDATE_METRICS_NAME = "voxel_forward_indirect_update_metrics";
static constexpr const char *VOXEL_FORWARD_INDIRECT_TOPOLOGY_EDIT_NAME = "voxel_forward_indirect_topology_edit";
static constexpr const char *VOXEL_FORWARD_DDGI_IRRADIANCE_NAMES[4] = { "voxel_forward_ddgi_irradiance_lod0", "voxel_forward_ddgi_irradiance_lod1", "voxel_forward_ddgi_irradiance_lod2", "voxel_forward_ddgi_irradiance_lod3" };
static constexpr const char *VOXEL_FORWARD_DDGI_DEPTH_NAMES[4] = { "voxel_forward_ddgi_depth_lod0", "voxel_forward_ddgi_depth_lod1", "voxel_forward_ddgi_depth_lod2", "voxel_forward_ddgi_depth_lod3" };
static constexpr const char *VOXEL_FORWARD_DDGI_METADATA_NAMES[4] = { "voxel_forward_ddgi_metadata_lod0", "voxel_forward_ddgi_metadata_lod1", "voxel_forward_ddgi_metadata_lod2", "voxel_forward_ddgi_metadata_lod3" };
static constexpr const char *VOXEL_FORWARD_DDGI_ORIGIN_NAMES[4] = { "voxel_forward_ddgi_origin_lod0", "voxel_forward_ddgi_origin_lod1", "voxel_forward_ddgi_origin_lod2", "voxel_forward_ddgi_origin_lod3" };
static constexpr const char *VOXEL_FORWARD_DDGI_CELL_SIZE_NAMES[4] = { "voxel_forward_ddgi_cell_size_lod0", "voxel_forward_ddgi_cell_size_lod1", "voxel_forward_ddgi_cell_size_lod2", "voxel_forward_ddgi_cell_size_lod3" };
static constexpr const char *VOXEL_FORWARD_DDGI_PHASE_NAMES[4] = { "voxel_forward_ddgi_phase_lod0", "voxel_forward_ddgi_phase_lod1", "voxel_forward_ddgi_phase_lod2", "voxel_forward_ddgi_phase_lod3" };
static constexpr const char *VOXEL_FORWARD_DDGI_LOGICAL_ORIGIN_NAMES[4] = { "voxel_forward_ddgi_logical_origin_lod0", "voxel_forward_ddgi_logical_origin_lod1", "voxel_forward_ddgi_logical_origin_lod2", "voxel_forward_ddgi_logical_origin_lod3" };
static constexpr const char *VOXEL_FORWARD_DDGI_PROBE_RESOLUTION_NAME = "voxel_forward_ddgi_probe_resolution";
static constexpr const char *VOXEL_FORWARD_DDGI_IRRADIANCE_ATLAS_SIZE_NAME = "voxel_forward_ddgi_irradiance_atlas_size";
static constexpr const char *VOXEL_FORWARD_DDGI_VISIBILITY_ATLAS_SIZE_NAME = "voxel_forward_ddgi_visibility_atlas_size";
static constexpr const char *VOXEL_FORWARD_DDGI_SELF_SHADOW_BIAS_NAME = "voxel_forward_ddgi_self_shadow_bias";
static constexpr const char *VOXEL_FORWARD_DDGI_VIEW_BIAS_NAME = "voxel_forward_ddgi_view_bias";
static constexpr const char *VOXEL_FORWARD_DDGI_LOD_TRANSITION_NAME = "voxel_forward_ddgi_lod_transition";
static constexpr const char *VOXEL_FORWARD_DDGI_CAMERA_POSITION_NAME = "voxel_forward_ddgi_camera_position";
static constexpr const char *VOXEL_FORWARD_DDGI_READY_NAME = "voxel_forward_ddgi_ready";
static constexpr const char *VOXEL_FORWARD_DDGI_RESOLVE_NAME = "voxel_forward_ddgi_resolve";
static constexpr const char *VOXEL_FORWARD_DDGI_RESOLVE_READY_NAME = "voxel_forward_ddgi_resolve_ready";
static constexpr const char *VOXEL_FORWARD_DDGI_DEBUG_MODE_NAME = "voxel_forward_ddgi_debug_mode";
static constexpr const char *VOXEL_FORWARD_DDGI_SHADOW_FILL_STRENGTH_NAME = "voxel_forward_ddgi_shadow_fill_strength";
static constexpr const char *VOXEL_FORWARD_DDGI_SHADOW_FILL_TINT_NAME = "voxel_forward_ddgi_shadow_fill_tint";
static constexpr const char *VOXEL_FORWARD_DDGI_SHADOW_FILL_REACH_NAME = "voxel_forward_ddgi_shadow_fill_reach";
static constexpr const char *VOXEL_FORWARD_DDGI_COLOR_SATURATION_NAME = "voxel_forward_ddgi_color_saturation";
static constexpr const char *VOXEL_FORWARD_TOON_ENABLED_NAME = "voxel_forward_toon_enabled";
static constexpr const char *VOXEL_FORWARD_DDGI_TOON_BAND_COUNT_NAME = "voxel_forward_ddgi_toon_band_count";
static constexpr const char *VOXEL_FORWARD_DDGI_TOON_BAND_SOFTNESS_NAME = "voxel_forward_ddgi_toon_band_softness";
static constexpr const char *VOXEL_FORWARD_DDGI_TOON_BAND_RANGE_NAME = "voxel_forward_ddgi_toon_band_range";
static constexpr const char *VOXEL_FORWARD_TOON_SPECULAR_ENABLED_NAME = "voxel_forward_toon_specular_enabled";
static constexpr const char *VOXEL_FORWARD_TOON_SPECULAR_THRESHOLD_NAME = "voxel_forward_toon_specular_threshold";
static constexpr const char *VOXEL_FORWARD_TOON_SPECULAR_SOFTNESS_NAME = "voxel_forward_toon_specular_softness";
static constexpr const char *VOXEL_FORWARD_TOON_SPECULAR_STRENGTH_NAME = "voxel_forward_toon_specular_strength";
static constexpr const char *VOXEL_FORWARD_AMBIENT_COLOR_NAME = "voxel_forward_ambient_color";
static constexpr const char *VOXEL_FORWARD_AMBIENT_ENERGY_NAME = "voxel_forward_ambient_energy";
static constexpr const char *VOXEL_FORWARD_REFLECTION_NAME = "voxel_forward_reflection";
static constexpr const char *VOXEL_FORWARD_REFLECTION_READY_NAME = "voxel_forward_reflection_ready";
static constexpr const char *VOXEL_FORWARD_REFLECTION_INTENSITY_NAME = "voxel_forward_reflection_intensity";

struct VoxelStreamingCandidate {
	VoxelVolume3D *volume = nullptr;
	real_t distance_squared = 0.0;
	real_t ranking_distance_squared = 0.0;
	ObjectID instance_id;
	bool operator<(const VoxelStreamingCandidate &p_other) const {
		if (!Math::is_equal_approx(ranking_distance_squared, p_other.ranking_distance_squared)) {
			return ranking_distance_squared < p_other.ranking_distance_squared;
		}
		if (!Math::is_equal_approx(distance_squared, p_other.distance_squared)) {
			return distance_squared < p_other.distance_squared;
		}
		return instance_id < p_other.instance_id;
	}
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

struct VoxelNeighborFaceCandidate {
	VoxelVolume3D *volume = nullptr;
	int face = -1;
};

using VoxelNeighborFaceIndex = HashMap<Vector3i, Vector<VoxelNeighborFaceCandidate>>;

static Vector3 _get_neighbor_face_center(const VoxelVolume3D *p_volume, int p_face) {
	const Ref<VoxelShapeData> data = p_volume->get_voxel_data();
	if (data.is_null()) {
		return p_volume->get_global_position();
	}
	const Vector3 size = Vector3(data->get_dimensions()) * data->get_voxel_size();
	Vector3 center = size * real_t(0.5);
	switch (p_face) {
		case VoxelVolume3D::NEIGHBOR_NEGATIVE_X:
			center.x = 0.0;
			break;
		case VoxelVolume3D::NEIGHBOR_POSITIVE_X:
			center.x = size.x;
			break;
		case VoxelVolume3D::NEIGHBOR_NEGATIVE_Y:
			center.y = 0.0;
			break;
		case VoxelVolume3D::NEIGHBOR_POSITIVE_Y:
			center.y = size.y;
			break;
		case VoxelVolume3D::NEIGHBOR_NEGATIVE_Z:
			center.z = 0.0;
			break;
		case VoxelVolume3D::NEIGHBOR_POSITIVE_Z:
			center.z = size.z;
			break;
	}
	return p_volume->get_global_transform().xform(center);
}

static Vector3i _get_neighbor_face_cell(const Vector3 &p_position) {
	// The exact cell size is not significant: matching faces have the same world
	// center. Neighboring buckets are queried to preserve the transform tolerance
	// when a center lies directly on a hash-cell boundary.
	static constexpr real_t CELL_SIZE = 1.0;
	return Vector3i(
			int(Math::floor(p_position.x / CELL_SIZE)),
			int(Math::floor(p_position.y / CELL_SIZE)),
			int(Math::floor(p_position.z / CELL_SIZE)));
}

static void _build_neighbor_face_index(const Vector<VoxelVolume3D *> &p_volumes, VoxelNeighborFaceIndex &r_index) {
	for (VoxelVolume3D *volume : p_volumes) {
		if (volume == nullptr || !volume->is_streaming_resident() || volume->get_voxel_data().is_null()) {
			continue;
		}
		for (int face = 0; face < VoxelVolume3D::NEIGHBOR_FACE_COUNT; face++) {
			const Vector3i cell = _get_neighbor_face_cell(_get_neighbor_face_center(volume, face));
			Vector<VoxelNeighborFaceCandidate> *bucket = r_index.getptr(cell);
			if (bucket == nullptr) {
				r_index.insert(cell, Vector<VoxelNeighborFaceCandidate>());
				bucket = r_index.getptr(cell);
			}
			VoxelNeighborFaceCandidate candidate;
			candidate.volume = volume;
			candidate.face = face;
			bucket->push_back(candidate);
		}
	}
}

static void _find_neighbor_faces(VoxelVolume3D *p_target, const VoxelNeighborFaceIndex &p_index, VoxelVolume3D *r_faces[VoxelVolume3D::NEIGHBOR_FACE_COUNT]) {
	for (int face = 0; face < VoxelVolume3D::NEIGHBOR_FACE_COUNT; face++) {
		const Vector3i center_cell = _get_neighbor_face_cell(_get_neighbor_face_center(p_target, face));
		const int opposite_face = face ^ 1;
		for (int z = -1; z <= 1 && r_faces[face] == nullptr; z++) {
			for (int y = -1; y <= 1 && r_faces[face] == nullptr; y++) {
				for (int x = -1; x <= 1 && r_faces[face] == nullptr; x++) {
					const Vector<VoxelNeighborFaceCandidate> *bucket = p_index.getptr(center_cell + Vector3i(x, y, z));
					if (bucket == nullptr) {
						continue;
					}
					for (const VoxelNeighborFaceCandidate &candidate : *bucket) {
						if (candidate.volume == p_target || candidate.face != opposite_face) {
							continue;
						}
						if (_get_neighbor_face(p_target, candidate.volume) == face) {
							r_faces[face] = candidate.volume;
							break;
						}
					}
				}
			}
		}
	}
}

static int _get_neighbor_diagonal(const VoxelVolume3D *p_volume, const VoxelVolume3D *p_other) {
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
	auto near_value = [tolerance](real_t p_a, real_t p_b) { return Math::abs(p_a - p_b) <= tolerance; };
	auto get_axis_offset = [&near_value](real_t p_origin, real_t p_size, real_t p_other_size, int p_dimension, int p_other_dimension) {
		if (near_value(p_origin, -p_other_size)) {
			return -1;
		}
		if (near_value(p_origin, p_size)) {
			return 1;
		}
		if (near_value(p_origin, 0.0) && p_dimension == p_other_dimension) {
			return 0;
		}
		return 2;
	};
	const Vector3i offset(
			get_axis_offset(relative.origin.x, size.x, other_size.x, dimensions.x, other_dimensions.x),
			get_axis_offset(relative.origin.y, size.y, other_size.y, dimensions.y, other_dimensions.y),
			get_axis_offset(relative.origin.z, size.z, other_size.z, dimensions.z, other_dimensions.z));
	if (offset.x == 2 || offset.y == 2 || offset.z == 2) {
		return -1;
	}
	return VoxelVolume3D::get_neighbor_diagonal_index(offset);
}

struct VoxelNeighborDiagonalCandidate {
	VoxelVolume3D *volume = nullptr;
	int diagonal = -1;
};

using VoxelNeighborDiagonalIndex = HashMap<Vector3i, Vector<VoxelNeighborDiagonalCandidate>>;

static Vector3 _get_neighbor_diagonal_center(const VoxelVolume3D *p_volume, int p_diagonal) {
	const Ref<VoxelShapeData> data = p_volume->get_voxel_data();
	if (data.is_null()) {
		return p_volume->get_global_position();
	}
	const Vector3 size = Vector3(data->get_dimensions()) * data->get_voxel_size();
	const Vector3i offset = VoxelVolume3D::get_neighbor_diagonal_offset(p_diagonal);
	Vector3 center = size * real_t(0.5);
	if (offset.x < 0) {
		center.x = 0.0;
	} else if (offset.x > 0) {
		center.x = size.x;
	}
	if (offset.y < 0) {
		center.y = 0.0;
	} else if (offset.y > 0) {
		center.y = size.y;
	}
	if (offset.z < 0) {
		center.z = 0.0;
	} else if (offset.z > 0) {
		center.z = size.z;
	}
	return p_volume->get_global_transform().xform(center);
}

static void _build_neighbor_diagonal_index(const Vector<VoxelVolume3D *> &p_volumes, VoxelNeighborDiagonalIndex &r_index) {
	for (VoxelVolume3D *volume : p_volumes) {
		if (volume == nullptr || !volume->is_streaming_resident() || volume->get_voxel_data().is_null()) {
			continue;
		}
		for (int diagonal = 0; diagonal < VoxelVolume3D::NEIGHBOR_DIAGONAL_COUNT; diagonal++) {
			const Vector3i cell = _get_neighbor_face_cell(_get_neighbor_diagonal_center(volume, diagonal));
			Vector<VoxelNeighborDiagonalCandidate> *bucket = r_index.getptr(cell);
			if (bucket == nullptr) {
				r_index.insert(cell, Vector<VoxelNeighborDiagonalCandidate>());
				bucket = r_index.getptr(cell);
			}
			VoxelNeighborDiagonalCandidate candidate;
			candidate.volume = volume;
			candidate.diagonal = diagonal;
			bucket->push_back(candidate);
		}
	}
}

static void _find_neighbor_diagonals(VoxelVolume3D *p_target, const VoxelNeighborDiagonalIndex &p_index, VoxelVolume3D *r_diagonals[VoxelVolume3D::NEIGHBOR_DIAGONAL_COUNT]) {
	for (int diagonal = 0; diagonal < VoxelVolume3D::NEIGHBOR_DIAGONAL_COUNT; diagonal++) {
		const Vector3i center_cell = _get_neighbor_face_cell(_get_neighbor_diagonal_center(p_target, diagonal));
		const int opposite_diagonal = VoxelVolume3D::get_neighbor_diagonal_index(-VoxelVolume3D::get_neighbor_diagonal_offset(diagonal));
		for (int z = -1; z <= 1 && r_diagonals[diagonal] == nullptr; z++) {
			for (int y = -1; y <= 1 && r_diagonals[diagonal] == nullptr; y++) {
				for (int x = -1; x <= 1 && r_diagonals[diagonal] == nullptr; x++) {
					const Vector<VoxelNeighborDiagonalCandidate> *bucket = p_index.getptr(center_cell + Vector3i(x, y, z));
					if (bucket == nullptr) {
						continue;
					}
					for (const VoxelNeighborDiagonalCandidate &candidate : *bucket) {
						if (candidate.volume == p_target || candidate.diagonal != opposite_diagonal) {
							continue;
						}
						if (_get_neighbor_diagonal(p_target, candidate.volume) == diagonal) {
							r_diagonals[diagonal] = candidate.volume;
							break;
						}
					}
				}
			}
		}
	}
}

struct VoxelNeighborIndexCache {
	VoxelNeighborFaceIndex faces;
	VoxelNeighborDiagonalIndex diagonals;
};

VoxelVolumeStreamingManager::VoxelVolumeStreamingManager() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = this;
	neighbor_index_cache = memnew(VoxelNeighborIndexCache);
	_register_voxel_forward_globals();
}

VoxelVolumeStreamingManager::~VoxelVolumeStreamingManager() {
	// SceneTree removes Object callables during teardown. At this initialization
	// level its signals may already be gone, so querying them here is invalid.
	connected_tree = nullptr;
	memdelete(neighbor_index_cache);
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
	streaming_evaluated = false;
	neighbors_dirty = true;
	_connect_tree(p_volume->get_tree());
}

void VoxelVolumeStreamingManager::unregister_volume(VoxelVolume3D *p_volume) {
	if (p_volume != nullptr) {
		volumes.erase(p_volume->get_instance_id());
		streaming_evaluated = false;
		neighbors_dirty = true;
	}
}

void VoxelVolumeStreamingManager::_process_frame() {
	// Residency does not need per-frame precision. This amortizes ranking costs
	// even with hundreds of 100^3 volumes.
	if (!streaming_evaluated || streaming_pending || ++frame_counter >= 12) {
		frame_counter = 0;
		_update_streaming();
	}
	// Loading a large resident set is deliberately spread over several frames.
	// Rebuilding all adjacency and occupancy data after every small batch would
	// turn that amortization back into a startup stall.
	if (!streaming_pending) {
		// Boundary edits must not wait behind a topology pass which animated
		// volumes can invalidate every frame.
		if (!neighbor_refresh_volumes.is_empty()) {
			_update_dirty_neighbors();
		}
		if (neighbors_dirty || !neighbor_rebuild_queue.is_empty()) {
			_update_neighbors();
		}
	}
}

void VoxelVolumeStreamingManager::_register_voxel_forward_globals() {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);

	PackedByteArray empty;
	empty.resize(1);
	empty.set(0, 0);
	Vector<Ref<Image>> slices;
	slices.push_back(Image::create_from_data(1, 1, false, Image::FORMAT_L8, empty));
	voxel_forward_fallback_texture.instantiate();
	ERR_FAIL_COND(voxel_forward_fallback_texture->create(Image::FORMAT_L8, 1, 1, 1, false, slices) != OK);
	voxel_forward_fallback_texture_2d = ImageTexture::create_from_image(slices[0]);
	ERR_FAIL_COND(voxel_forward_fallback_texture_2d.is_null());

	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_SHADOW_MASK_NAME, RSE::GLOBAL_VAR_TYPE_SAMPLER2D, RID());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_SHADOW_LIGHT_DIRECTION_NAME, RSE::GLOBAL_VAR_TYPE_VEC3, Vector3(0, 1, 0));
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_SHADOW_READY_NAME, RSE::GLOBAL_VAR_TYPE_BOOL, false);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_NEAR_NAME, RSE::GLOBAL_VAR_TYPE_SAMPLER3D, voxel_forward_fallback_texture->get_rid());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_FAR_NAME, RSE::GLOBAL_VAR_TYPE_SAMPLER3D, voxel_forward_fallback_texture->get_rid());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_DISTANT_NAME, RSE::GLOBAL_VAR_TYPE_SAMPLER3D, voxel_forward_fallback_texture->get_rid());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_NEAR_ORIGIN_NAME, RSE::GLOBAL_VAR_TYPE_VEC3, Vector3());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_FAR_ORIGIN_NAME, RSE::GLOBAL_VAR_TYPE_VEC3, Vector3());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_DISTANT_ORIGIN_NAME, RSE::GLOBAL_VAR_TYPE_VEC3, Vector3());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_NEAR_CELL_SIZE_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 1.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_FAR_CELL_SIZE_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 1.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_DISTANT_CELL_SIZE_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 1.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_RESOLUTION_NAME, RSE::GLOBAL_VAR_TYPE_INT, 1);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_TRANSITION_CELLS_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 1.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_INTENSITY_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_READY_NAME, RSE::GLOBAL_VAR_TYPE_BOOL, false);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_BACKEND_NAME, RSE::GLOBAL_VAR_TYPE_INT, 2);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_DEBUG_MODE_NAME, RSE::GLOBAL_VAR_TYPE_INT, 0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_CORNER_AO_STRENGTH_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 1.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_CORNER_AO_TINT_NAME, RSE::GLOBAL_VAR_TYPE_COLOR, Color(0.0, 0.0, 0.0));
	for (uint32_t cascade = 0; cascade < 3; cascade++) {
		rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_DIRTY_MIN_NAMES[cascade], RSE::GLOBAL_VAR_TYPE_VEC3, Vector3(1e20, 1e20, 1e20));
		rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_DIRTY_MAX_NAMES[cascade], RSE::GLOBAL_VAR_TYPE_VEC3, Vector3(-1e20, -1e20, -1e20));
	}
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_STAGING_MASK_NAME, RSE::GLOBAL_VAR_TYPE_INT, 0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_ACTIVE_REVISIONS_NAME, RSE::GLOBAL_VAR_TYPE_VEC3, Vector3(-1, -1, -1));
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_STAGING_REVISIONS_NAME, RSE::GLOBAL_VAR_TYPE_VEC3, Vector3(-1, -1, -1));
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_UPDATE_METRICS_NAME, RSE::GLOBAL_VAR_TYPE_VEC4, Vector4());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_TOPOLOGY_EDIT_NAME, RSE::GLOBAL_VAR_TYPE_VEC4, Vector4());
	const RID fallback_2d = voxel_forward_fallback_texture_2d->get_rid();
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_RESTIR_GI_NAME, RSE::GLOBAL_VAR_TYPE_SAMPLER2D, fallback_2d);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_RESTIR_READY_NAME, RSE::GLOBAL_VAR_TYPE_BOOL, false);
	for (uint32_t lod = 0; lod < 4; lod++) {
		rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_IRRADIANCE_NAMES[lod], RSE::GLOBAL_VAR_TYPE_SAMPLER2D, fallback_2d);
		rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_DEPTH_NAMES[lod], RSE::GLOBAL_VAR_TYPE_SAMPLER2D, fallback_2d);
		rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_METADATA_NAMES[lod], RSE::GLOBAL_VAR_TYPE_SAMPLER2D, fallback_2d);
		rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_ORIGIN_NAMES[lod], RSE::GLOBAL_VAR_TYPE_VEC3, Vector3());
		rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_CELL_SIZE_NAMES[lod], RSE::GLOBAL_VAR_TYPE_VEC3, Vector3(1, 1, 1));
		rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_PHASE_NAMES[lod], RSE::GLOBAL_VAR_TYPE_IVEC3, Vector3i());
		rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_LOGICAL_ORIGIN_NAMES[lod], RSE::GLOBAL_VAR_TYPE_IVEC3, Vector3i());
	}
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_PROBE_RESOLUTION_NAME, RSE::GLOBAL_VAR_TYPE_INT, 1);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_IRRADIANCE_ATLAS_SIZE_NAME, RSE::GLOBAL_VAR_TYPE_VEC2, Vector2(1, 1));
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_VISIBILITY_ATLAS_SIZE_NAME, RSE::GLOBAL_VAR_TYPE_VEC2, Vector2(1, 1));
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_SELF_SHADOW_BIAS_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.3);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_VIEW_BIAS_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_LOD_TRANSITION_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.15);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_CAMERA_POSITION_NAME, RSE::GLOBAL_VAR_TYPE_VEC3, Vector3());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_READY_NAME, RSE::GLOBAL_VAR_TYPE_BOOL, false);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_RESOLVE_NAME, RSE::GLOBAL_VAR_TYPE_SAMPLER2D, fallback_2d);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_RESOLVE_READY_NAME, RSE::GLOBAL_VAR_TYPE_BOOL, false);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_DEBUG_MODE_NAME, RSE::GLOBAL_VAR_TYPE_INT, 0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_SHADOW_FILL_STRENGTH_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_SHADOW_FILL_TINT_NAME, RSE::GLOBAL_VAR_TYPE_COLOR, Color(1.0, 1.0, 1.0));
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_SHADOW_FILL_REACH_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_COLOR_SATURATION_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 1.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_TOON_ENABLED_NAME, RSE::GLOBAL_VAR_TYPE_BOOL, false);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_TOON_BAND_COUNT_NAME, RSE::GLOBAL_VAR_TYPE_INT, 0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_TOON_BAND_SOFTNESS_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.05);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_DDGI_TOON_BAND_RANGE_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 1.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_TOON_SPECULAR_ENABLED_NAME, RSE::GLOBAL_VAR_TYPE_BOOL, false);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_TOON_SPECULAR_THRESHOLD_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.55);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_TOON_SPECULAR_SOFTNESS_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.04);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_TOON_SPECULAR_STRENGTH_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 1.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_AMBIENT_COLOR_NAME, RSE::GLOBAL_VAR_TYPE_COLOR, Color(0.22, 0.22, 0.22));
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_AMBIENT_ENERGY_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 1.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_REFLECTION_NAME, RSE::GLOBAL_VAR_TYPE_SAMPLER2D, RID());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_REFLECTION_READY_NAME, RSE::GLOBAL_VAR_TYPE_BOOL, false);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_REFLECTION_INTENSITY_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 1.0);
}
void VoxelVolumeStreamingManager::_update_streaming() {
	streaming_evaluated = true;
	selected_target_count = 0;
	const int max_resident = MAX(1, int(GLOBAL_GET("rendering/voxel_volume/max_resident_volumes")));
	const int configured_loads = CLAMP(int(GLOBAL_GET("rendering/voxel_volume/max_loads_per_frame")), 1, 64);
	const real_t global_distance = MAX(real_t(0.0), real_t(GLOBAL_GET("rendering/voxel_volume/streaming_distance")));
	const real_t hysteresis = MAX(real_t(0.0), real_t(GLOBAL_GET("rendering/voxel_volume/streaming_hysteresis")));
	streaming_pending = false;
	Vector<VoxelStreamingCandidate> candidates;
	Vector<ObjectID> stale;
	bool all_candidates_resident = true;
	for (const ObjectID &id : volumes) {
		VoxelVolume3D *volume = Object::cast_to<VoxelVolume3D>(ObjectDB::get_instance(id));
		if (volume == nullptr) {
			stale.push_back(id);
			continue;
		}
		const Ref<VoxelShapeData> voxel_data = volume->get_voxel_data();
		if (voxel_data.is_null() || voxel_data->get_brick_storage().get_occupied_voxel_count() == 0) {
			volume->set_streaming_resident(false);
			continue;
		}
		if (volume->get_streaming_mode() == VoxelVolume3D::STREAMING_ALWAYS_RESIDENT) {
			selected_target_count++;
			volume->set_streaming_resident(true);
			continue;
		}
		if (volume->get_streaming_mode() == VoxelVolume3D::STREAMING_MANUAL) {
			continue;
		}
		Viewport *viewport = volume->get_viewport();
		Camera3D *camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
		if (camera == nullptr) {
			// A scene can enter the tree before its camera becomes current. Waiting
			// one frame prevents every automatic volume from uploading at once.
			streaming_pending = true;
			continue;
		}
		const real_t distance_limit = volume->get_streaming_distance() > 0.0 ? volume->get_streaming_distance() : global_distance;
		const Vector3 center = volume->get_global_transform().xform(volume->get_aabb().get_center());
		const real_t distance_squared = center.distance_squared_to(camera->get_global_position());
		if (distance_limit <= 0.0 || distance_squared <= distance_limit * distance_limit) {
			VoxelStreamingCandidate candidate;
			candidate.volume = volume;
			candidate.distance_squared = distance_squared;
			const real_t distance = Math::sqrt(distance_squared);
			const real_t ranking_distance = volume->is_streaming_resident() ? MAX(real_t(0.0), distance - hysteresis) : distance;
			candidate.ranking_distance_squared = ranking_distance * ranking_distance;
			candidate.instance_id = volume->get_instance_id();
			candidates.push_back(candidate);
			all_candidates_resident = all_candidates_resident && volume->is_streaming_resident();
		} else {
			volume->set_streaming_resident(false);
		}
	}
	for (const ObjectID &id : stale) {
		volumes.erase(id);
	}
	selected_target_count += MIN(max_resident, candidates.size());
	if (candidates.size() <= max_resident && all_candidates_resident) {
		return;
	}
	candidates.sort();
	// Texture creation and material setup happen synchronously when residency is
	// enabled. Honor the configured budget during the initial fill as well as
	// later streaming so large scenes do not collapse all of that work into a
	// handful of long frames.
	// Release deselected resources before admitting replacements. This keeps the
	// configured cap true even during a transition and makes the result
	// independent of candidate iteration order.
	for (int i = max_resident; i < candidates.size(); i++) {
		candidates[i].volume->set_streaming_resident(false);
	}
	int remaining_loads = configured_loads;
	const int selected_count = MIN(max_resident, candidates.size());
	for (int i = 0; i < selected_count; i++) {
		VoxelVolume3D *volume = candidates[i].volume;
		if (volume->is_streaming_resident()) {
			continue;
		}
		if (remaining_loads > 0) {
			volume->set_streaming_resident(true);
			remaining_loads--;
		} else {
			streaming_pending = true;
		}
	}
}

void VoxelVolumeStreamingManager::_update_neighbors() {
	// Finding neighbors and uploading every face texture in one call can stall a
	// large streamed scene for more than a second. Reuse the residency budget so
	// adjacency finalization has the same bounded per-frame cost as volume loads.
	const int update_budget = CLAMP(int(GLOBAL_GET("rendering/voxel_volume/max_loads_per_frame")), 1, 64);
	if (neighbors_dirty) {
		Vector<ObjectID> stale;
		Vector<VoxelVolume3D *> live_volumes;
		for (const ObjectID &id : volumes) {
			VoxelVolume3D *volume = Object::cast_to<VoxelVolume3D>(ObjectDB::get_instance(id));
			if (volume == nullptr) {
				stale.push_back(id);
				continue;
			}
			if (volume->is_streaming_resident()) {
				live_volumes.push_back(volume);
			}
		}
		for (const ObjectID &id : stale) {
			volumes.erase(id);
		}
		neighbor_index_cache->faces.clear();
		neighbor_index_cache->diagonals.clear();
		_build_neighbor_face_index(live_volumes, neighbor_index_cache->faces);
		_build_neighbor_diagonal_index(live_volumes, neighbor_index_cache->diagonals);
		neighbors_dirty = false;
		if (neighbor_rebuild_queue.is_empty()) {
			neighbor_rebuild_index = 0;
			for (VoxelVolume3D *volume : live_volumes) {
				neighbor_rebuild_queue.push_back(volume->get_instance_id());
			}
		} else {
			// Keep making progress with the current topology index. Restarting at
			// zero on each animation tick starves everything after the first batch.
			// A subsequent pass also revisits volumes processed before this change
			// and includes any newly registered volumes.
			neighbor_rebuild_rescan = true;
		}
	}
	if (neighbor_rebuild_index >= neighbor_rebuild_queue.size()) {
		neighbor_rebuild_queue.clear();
		neighbor_rebuild_index = 0;
		return;
	}
	// Registration, removal, movement and residency changes invalidate this cache
	// before it is used again. Stable batches reuse the same lookup tables.

	int updated = 0;
	while (neighbor_rebuild_index < neighbor_rebuild_queue.size() && updated < update_budget) {
		VoxelVolume3D *target = Object::cast_to<VoxelVolume3D>(ObjectDB::get_instance(neighbor_rebuild_queue[neighbor_rebuild_index++]));
		if (target == nullptr) {
			continue;
		}
		VoxelVolume3D *faces[VoxelVolume3D::NEIGHBOR_FACE_COUNT] = {};
		VoxelVolume3D *diagonals[VoxelVolume3D::NEIGHBOR_DIAGONAL_COUNT] = {};
		_find_neighbor_faces(target, neighbor_index_cache->faces, faces);
		_find_neighbor_diagonals(target, neighbor_index_cache->diagonals, diagonals);
		target->update_neighbor_faces(faces, diagonals);
		updated++;
	}
	if (neighbor_rebuild_index >= neighbor_rebuild_queue.size()) {
		neighbor_rebuild_queue.clear();
		neighbor_rebuild_index = 0;
		neighbors_dirty = neighbors_dirty || neighbor_rebuild_rescan;
		neighbor_rebuild_rescan = false;
	}
}

void VoxelVolumeStreamingManager::mark_volume_neighbors_dirty(VoxelVolume3D *p_volume) {
	if (p_volume != nullptr) {
		neighbor_refresh_volumes.insert(p_volume->get_instance_id());
		streaming_evaluated = false;
	}
}

void VoxelVolumeStreamingManager::_update_dirty_neighbors() {
	HashSet<ObjectID> requested(neighbor_refresh_volumes);
	neighbor_refresh_volumes.clear();
	HashSet<ObjectID> affected;

	// Voxel edits do not change volume placement, so the adjacency discovered by
	// the last streaming/topology rebuild is still valid. Rebuilding both spatial
	// indexes here made a one-cell boundary edit scale with every resident volume.
	// Refresh only the edited volumes and their already-known touching neighbors.
	for (const ObjectID &id : requested) {
		VoxelVolume3D *edited = Object::cast_to<VoxelVolume3D>(ObjectDB::get_instance(id));
		if (edited == nullptr) {
			continue;
		}
		Vector<ObjectID> cached_neighbors;
		edited->append_cached_neighbor_ids(cached_neighbors);
		// Newly resident or isolated volumes have no cached shared boundaries.
		// Their first adjacency pass handles discovery; do not duplicate that
		// work for the entire initial load here.
		if (cached_neighbors.is_empty()) {
			continue;
		}
		affected.insert(id);
		for (const ObjectID &neighbor_id : cached_neighbors) {
			affected.insert(neighbor_id);
		}
	}

	for (const ObjectID &id : affected) {
		VoxelVolume3D *target = Object::cast_to<VoxelVolume3D>(ObjectDB::get_instance(id));
		if (target == nullptr) {
			continue;
		}
		target->refresh_cached_neighbor_faces();
	}
}

// Preparation/submission status. Callers must observe a rendered frame before
// revealing the world; this is not a GPU fence or a GI-convergence metric.
Dictionary VoxelVolumeStreamingManager::get_streaming_status() const {
	int resident = 0;
	for (const ObjectID &id : volumes) {
		const VoxelVolume3D *volume = Object::cast_to<VoxelVolume3D>(ObjectDB::get_instance(id));
		if (volume && volume->get_streaming_mode() != VoxelVolume3D::STREAMING_MANUAL && volume->is_streaming_resident()) {
			resident++;
		}
	}
	Dictionary result;
	result["resident"] = resident;
	result["target"] = selected_target_count;
	result["pending_neighbors"] = neighbor_rebuild_queue.size() - neighbor_rebuild_index;
	result["ready"] = streaming_evaluated && !streaming_pending && !neighbors_dirty && neighbor_rebuild_queue.is_empty() && neighbor_refresh_volumes.is_empty();
	return result;
}
