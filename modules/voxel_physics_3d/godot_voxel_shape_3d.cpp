// modules/voxel_physics_3d/godot_voxel_shape_3d.cpp

#include "godot_voxel_shape_3d.h"

GodotVoxelShape3D::GodotVoxelShape3D() :
		GodotShape3D() {
}

void GodotVoxelShape3D::set_data(const Variant &p_data) {
	Ref<VoxelShapeData> new_data = p_data;
	voxel_data = new_data;

	if (voxel_data.is_null()) {
		brick_caches.clear();
		solid_count = 0;
		cached_revision = UINT64_MAX;
		center_of_mass = Vector3();
		inertia_per_unit_mass = Vector3();
		occupied_volume = 0.0;
		mass_properties_valid = false;
		local_aabb = AABB();
		configure(local_aabb);
		return;
	}

	local_aabb = AABB(Vector3(), Vector3(voxel_data->get_dimensions()) * voxel_data->get_voxel_size());
	const real_t voxel_size = voxel_data->get_voxel_size();
	default_cube_feature.set_data(Vector3(1, 1, 1) * voxel_size * 0.5);
	default_corner_feature.set_data(voxel_size * 0.5);
	_invalidate_brick_caches();

	configure(local_aabb);
}

Variant GodotVoxelShape3D::get_data() const {
	return voxel_data;
}

void GodotVoxelShape3D::_rebuild_mass_properties() const {
	if (mass_properties_valid) {
		return;
	}
	_ensure_all_brick_caches();
	center_of_mass = Vector3();
	inertia_per_unit_mass = Vector3();
	if (voxel_data.is_null()) {
		mass_properties_valid = true;
		return;
	}

	const real_t voxel_size = voxel_data->get_voxel_size();
	Vector3 center_sum_grid;
	Vector3 center_squared_sum_grid;
	for (const PhysicsBrickCache &cache : brick_caches) {
		center_sum_grid += cache.center_sum_grid;
		center_squared_sum_grid += cache.center_squared_sum_grid;
	}

	if (solid_count == 0) {
		center_of_mass = local_aabb.get_center();
		mass_properties_valid = true;
		return;
	}

	const Vector3 mean_grid = center_sum_grid / real_t(solid_count);
	center_of_mass = mean_grid * voxel_size;
	const real_t cube_inertia = voxel_size * voxel_size / 6.0;
	const Vector3 variance_grid = center_squared_sum_grid / real_t(solid_count) - mean_grid * mean_grid;
	inertia_per_unit_mass = Vector3(
			cube_inertia + (variance_grid.y + variance_grid.z) * voxel_size * voxel_size,
			cube_inertia + (variance_grid.x + variance_grid.z) * voxel_size * voxel_size,
			cube_inertia + (variance_grid.x + variance_grid.y) * voxel_size * voxel_size);
	mass_properties_valid = true;
}

void GodotVoxelShape3D::_ensure_brick_cache(int p_brick_index) const {
	ERR_FAIL_INDEX(p_brick_index, brick_caches.size());
	if (brick_caches[p_brick_index].initialized) {
		return;
	}
	PhysicsBrickCache &cache = brick_caches.write[p_brick_index];
	cache = PhysicsBrickCache();
	cache.initialized = true;
	const VoxelBrickStorage &storage = voxel_data->get_brick_storage();
	const VoxelBrickStorage::Brick &brick = storage.get_brick(p_brick_index);
	if (brick.type == VoxelBrickStorage::BRICK_EMPTY) {
		return;
	}

	const Vector3i dimensions = voxel_data->get_dimensions();
	const Vector3i brick_origin = storage.brick_index_to_position(p_brick_index) * VoxelBrickStorage::BRICK_SIZE;
	const Vector3i valid_size = (dimensions - brick_origin).clamp(Vector3i(), Vector3i(VoxelBrickStorage::BRICK_SIZE, VoxelBrickStorage::BRICK_SIZE, VoxelBrickStorage::BRICK_SIZE));
	cache.uniform_solid = brick.type == VoxelBrickStorage::BRICK_UNIFORM && brick.uniform_value != 0;
	for (int z = 0; z < valid_size.z; z++) {
		for (int y = 0; y < valid_size.y; y++) {
			for (int x = 0; x < valid_size.x; x++) {
				const Vector3i position = brick_origin + Vector3i(x, y, z);
				if (!voxel_data->is_solid(position)) {
					continue;
				}
				const uint16_t local_index = uint16_t(x + y * VoxelBrickStorage::BRICK_SIZE + z * VoxelBrickStorage::BRICK_SIZE * VoxelBrickStorage::BRICK_SIZE);
				if (!cache.uniform_solid) {
					cache.solid_local_indices.push_back(local_index);
				}
				const int surface_class = voxel_data->get_surface_class(position);
				if (surface_class == 1) {
					cache.surface_local_indices.push_back(local_index);
					cache.face_local_indices.push_back(local_index);
				} else if (surface_class == 2) {
					cache.surface_local_indices.push_back(local_index);
					cache.edge_local_indices.push_back(local_index);
					cache.edge_bits[local_index >> 6] |= UINT64_C(1) << (local_index & 63);
				} else if (surface_class == 3) {
					cache.surface_local_indices.push_back(local_index);
					cache.corner_local_indices.push_back(local_index);
				}
				const Vector3 center_grid = Vector3(position) + Vector3(0.5, 0.5, 0.5);
				cache.center_sum_grid += center_grid;
				cache.center_squared_sum_grid += center_grid * center_grid;
				cache.occupied_count++;
			}
		}
	}
}

void GodotVoxelShape3D::_ensure_all_brick_caches() const {
	for (int index = 0; index < brick_caches.size(); index++) {
		_ensure_brick_cache(index);
	}
}

bool GodotVoxelShape3D::is_edge_feature(const Vector3i &p_voxel) const {
	if (voxel_data.is_null() || !voxel_data->is_inside(p_voxel)) {
		return false;
	}

	const VoxelBrickStorage &storage = voxel_data->get_brick_storage();
	const int brick_index = storage.position_to_brick_index(p_voxel);
	if (brick_index < 0 || brick_index >= brick_caches.size()) {
		return false;
	}

	const int x = p_voxel.x % VoxelBrickStorage::BRICK_SIZE;
	const int y = p_voxel.y % VoxelBrickStorage::BRICK_SIZE;
	const int z = p_voxel.z % VoxelBrickStorage::BRICK_SIZE;
	const int local_index = x + y * VoxelBrickStorage::BRICK_SIZE +
			z * VoxelBrickStorage::BRICK_SIZE * VoxelBrickStorage::BRICK_SIZE;
	_ensure_brick_cache(brick_index);
	return (brick_caches[brick_index].edge_bits[local_index >> 6] &
			(UINT64_C(1) << (local_index & 63))) != 0;
}

void GodotVoxelShape3D::_invalidate_brick_caches() {
	if (voxel_data.is_null()) {
		return;
	}
	const VoxelBrickStorage &storage = voxel_data->get_brick_storage();
	brick_caches.clear();
	brick_caches.resize(storage.get_brick_count());
	solid_count = 0;
	for (int index = 0; index < storage.get_brick_count(); index++) {
		solid_count += storage.get_brick(index).occupied_count;
	}
	cached_dimensions = voxel_data->get_dimensions();
	cached_revision = voxel_data->get_revision();
	const real_t voxel_size = voxel_data->get_voxel_size();
	occupied_volume = real_t(solid_count) * voxel_size * voxel_size * voxel_size;
	center_of_mass = local_aabb.get_center();
	inertia_per_unit_mass = Vector3();
	mass_properties_valid = false;
}

void GodotVoxelShape3D::_build_feature_table(GodotShape3D *p_shape, const Vector3 &p_scale, VoxelFeatureProxy3D (&r_table)[64]) {
	const Vector3 voxel_half = Vector3(1, 1, 1) * voxel_data->get_voxel_size() * 0.5;
	Vector3 feature_half;
	if (p_shape != nullptr && p_shape->is_configured()) {
		feature_half = p_shape->get_aabb().size * p_scale * 0.5;
	}
	feature_half.x = MIN(feature_half.x, voxel_half.x);
	feature_half.y = MIN(feature_half.y, voxel_half.y);
	feature_half.z = MIN(feature_half.z, voxel_half.z);
	const Vector3 offset_limit = voxel_half - feature_half;

	for (int mask = 0; mask < 64; mask++) {
		VoxelFeatureProxy3D &proxy = r_table[mask];
		proxy.shape = p_shape;
		proxy.local_transform = Transform3D(Basis().scaled(p_scale), voxel_half);

		Vector3 offset;
		if ((mask & 0x03) != 0x03) {
			offset.x = (mask & 0x01) != 0 ? offset_limit.x : ((mask & 0x02) != 0 ? -offset_limit.x : 0.0);
		}
		if ((mask & 0x0C) != 0x0C) {
			offset.y = (mask & 0x04) != 0 ? offset_limit.y : ((mask & 0x08) != 0 ? -offset_limit.y : 0.0);
		}
		if ((mask & 0x30) != 0x30) {
			offset.z = (mask & 0x10) != 0 ? offset_limit.z : ((mask & 0x20) != 0 ? -offset_limit.z : 0.0);
		}
		proxy.local_transform.origin += offset;
	}
}

void GodotVoxelShape3D::set_feature_shapes(GodotShape3D *p_face_shape, GodotShape3D *p_edge_shape, GodotShape3D *p_corner_shape) {
	if (voxel_data.is_null()) {
		return;
	}
	_build_feature_table(p_face_shape != nullptr ? p_face_shape : &default_cube_feature, voxel_data->get_face_shape_scale(), face_feature_table);
	_build_feature_table(p_edge_shape != nullptr ? p_edge_shape : &default_cube_feature, voxel_data->get_edge_shape_scale(), edge_feature_table);
	_build_feature_table(p_corner_shape != nullptr ? p_corner_shape : &default_corner_feature, voxel_data->get_corner_shape_scale(), corner_feature_table);
}

const VoxelFeatureProxy3D &GodotVoxelShape3D::get_feature_proxy(VoxelFeatureType p_feature, uint8_t p_mask) const {
	const uint8_t mask = p_mask & 0x3F;
	switch (p_feature) {
		case VoxelFeatureType::FACE:
			return face_feature_table[mask];
		case VoxelFeatureType::EDGE:
			return edge_feature_table[mask];
		case VoxelFeatureType::CORNER:
			return corner_feature_table[mask];
		default:
			return face_feature_table[0];
	}
}

GodotVoxelShape3D::IndexRange GodotVoxelShape3D::get_feature_indices(VoxelFeatureType p_feature) const {
	return IndexRange(this, int(p_feature));
}

GodotVoxelShape3D::IndexRange GodotVoxelShape3D::get_feature_indices(VoxelFeatureType p_feature, const Vector3i &p_from, const Vector3i &p_to) const {
	return IndexRange(this, int(p_feature), p_from, p_to);
}

bool GodotVoxelShape3D::IndexRange::is_empty() const {
	if (shape == nullptr) {
		return true;
	}
	if (!bounded && feature == 0) {
		return shape->solid_count == 0;
	}
	return !(begin() != end());
}

GodotVoxelShape3D::IndexIterator::IndexIterator(const GodotVoxelShape3D *p_shape, int p_feature, bool p_bounded, const Vector3i &p_from, const Vector3i &p_to, bool p_end) {
	shape = p_shape;
	feature = p_feature;
	bounded = p_bounded;
	range_from = p_from;
	range_to = p_to;
	if (!p_end && shape != nullptr) {
		if (bounded && shape->voxel_data.is_valid()) {
			// Bounded collision queries usually touch only a few bricks. Starting
			// at brick zero made every CharacterBody3D sweep rescan all preceding
			// bricks, which was especially costly when a corner involved several
			// neighboring voxel chunks and repeated motion-search iterations.
			brick_index = shape->voxel_data->get_brick_storage().position_to_brick_index(range_from);
		}
		_advance();
	}
}

void GodotVoxelShape3D::IndexIterator::_advance() {
	if (shape == nullptr || !shape->_find_next_index(feature, bounded, range_from, range_to, brick_index, cursor, current)) {
		current = -1;
	}
}

bool GodotVoxelShape3D::_find_next_index(int p_feature, bool p_bounded, const Vector3i &p_from, const Vector3i &p_to, int &r_brick_index, int &r_cursor, int &r_index) const {
	if (voxel_data.is_null()) {
		return false;
	}
	const VoxelBrickStorage &storage = voxel_data->get_brick_storage();
	const Vector3i dimensions = voxel_data->get_dimensions();
	while (r_brick_index < brick_caches.size()) {
		const Vector3i brick_origin = storage.brick_index_to_position(r_brick_index) * VoxelBrickStorage::BRICK_SIZE;
		if (p_bounded) {
			// Bricks are stored with Z as the outermost coordinate. Once this is
			// past the requested Z range, no later brick can intersect the query.
			if (brick_origin.z > p_to.z) {
				return false;
			}
			const Vector3i brick_end = (brick_origin + Vector3i(VoxelBrickStorage::BRICK_SIZE - 1, VoxelBrickStorage::BRICK_SIZE - 1, VoxelBrickStorage::BRICK_SIZE - 1)).clamp(Vector3i(), dimensions - Vector3i(1, 1, 1));
			if (brick_end.x < p_from.x || brick_end.y < p_from.y || brick_end.z < p_from.z ||
					brick_origin.x > p_to.x || brick_origin.y > p_to.y || brick_origin.z > p_to.z) {
				r_brick_index++;
				r_cursor = 0;
				continue;
			}
		}
		// Reject non-overlapping bricks before materializing their collision cache.
		// CharacterBody3D floor/wall sweeps usually touch only a handful of bricks.
		_ensure_brick_cache(r_brick_index);
		const PhysicsBrickCache &cache = brick_caches[r_brick_index];
		const Vector<uint16_t> *indices = nullptr;
		if (p_feature == 1) {
			indices = &cache.face_local_indices;
		} else if (p_feature == 2) {
			indices = &cache.edge_local_indices;
		} else if (p_feature == 3) {
			indices = &cache.corner_local_indices;
		} else if (p_feature == 4) {
			indices = &cache.surface_local_indices;
		} else if (!cache.uniform_solid) {
			indices = &cache.solid_local_indices;
		}

		if (indices != nullptr) {
			while (r_cursor < indices->size()) {
				const int local = (*indices)[r_cursor++];
				const Vector3i position = brick_origin + Vector3i(
						local % VoxelBrickStorage::BRICK_SIZE,
						(local / VoxelBrickStorage::BRICK_SIZE) % VoxelBrickStorage::BRICK_SIZE,
						local / (VoxelBrickStorage::BRICK_SIZE * VoxelBrickStorage::BRICK_SIZE));
				if (p_bounded && (position.x < p_from.x || position.y < p_from.y || position.z < p_from.z || position.x > p_to.x || position.y > p_to.y || position.z > p_to.z)) {
					continue;
				}
				r_index = position.x + position.y * dimensions.x + position.z * dimensions.x * dimensions.y;
				return true;
			}
		} else {
			while (r_cursor < VoxelBrickStorage::BRICK_VOXEL_COUNT) {
				const int local = r_cursor++;
				const Vector3i position = brick_origin + Vector3i(
						local % VoxelBrickStorage::BRICK_SIZE,
						(local / VoxelBrickStorage::BRICK_SIZE) % VoxelBrickStorage::BRICK_SIZE,
						local / (VoxelBrickStorage::BRICK_SIZE * VoxelBrickStorage::BRICK_SIZE));
				if (position.x >= dimensions.x || position.y >= dimensions.y || position.z >= dimensions.z) {
					continue;
				}
				if (p_bounded && (position.x < p_from.x || position.y < p_from.y || position.z < p_from.z || position.x > p_to.x || position.y > p_to.y || position.z > p_to.z)) {
					continue;
				}
				r_index = position.x + position.y * dimensions.x + position.z * dimensions.x * dimensions.y;
				return true;
			}
		}
		r_brick_index++;
		r_cursor = 0;
	}
	return false;
}

void GodotVoxelShape3D::project_range(const Vector3 &p_normal, const Transform3D &p_transform, real_t &r_min, real_t &r_max) const {
	Vector3 corners[8];

	for (int i = 0; i < 8; i++) {
		corners[i] = p_transform.xform(local_aabb.get_endpoint(i));
	}

	r_min = r_max = p_normal.dot(corners[0]);

	for (int i = 1; i < 8; i++) {
		const real_t projection = p_normal.dot(corners[i]);
		r_min = MIN(r_min, projection);
		r_max = MAX(r_max, projection);
	}
}

void GodotVoxelShape3D::get_supports(const Vector3 &p_normal, int p_max, Vector3 *r_supports, int &r_amount, FeatureType &r_type) const {
	if (p_max <= 0) {
		r_amount = 0;
		return;
	}

	const Vector3 end = local_aabb.position + local_aabb.size;
	r_supports[0] = Vector3(
			p_normal.x < 0.0 ? local_aabb.position.x : end.x,
			p_normal.y < 0.0 ? local_aabb.position.y : end.y,
			p_normal.z < 0.0 ? local_aabb.position.z : end.z);
	r_amount = 1;
	r_type = FEATURE_POINT;
}

Vector3 GodotVoxelShape3D::get_closest_point_to(const Vector3 &p_point) const {
	const Vector3 end = local_aabb.position + local_aabb.size;
	return p_point.clamp(local_aabb.position, end);
}

bool GodotVoxelShape3D::intersect_segment(const Vector3 &p_begin, const Vector3 &p_end, Vector3 &r_point, Vector3 &r_normal, int &r_face_index, bool p_hit_back_faces) const {
	r_face_index = -1;
	if (voxel_data.is_null() || !has_solids()) {
		return false;
	}

	const Vector3 segment = p_end - p_begin;
	const real_t segment_length = segment.length();
	if (segment_length <= CMP_EPSILON) {
		return false;
	}

	const Vector3 bounds_end = local_aabb.get_end();
	real_t enter_t = 0.0;
	real_t exit_t = 1.0;
	Vector3 entry_normal;
	for (int axis = 0; axis < 3; axis++) {
		if (Math::abs(segment[axis]) <= CMP_EPSILON) {
			if (p_begin[axis] < local_aabb.position[axis] || p_begin[axis] > bounds_end[axis]) {
				return false;
			}
			continue;
		}

		const real_t inverse_direction = 1.0 / segment[axis];
		real_t near_t = (local_aabb.position[axis] - p_begin[axis]) * inverse_direction;
		real_t far_t = (bounds_end[axis] - p_begin[axis]) * inverse_direction;
		Vector3 near_normal;
		near_normal[axis] = segment[axis] > 0.0 ? -1.0 : 1.0;
		if (near_t > far_t) {
			SWAP(near_t, far_t);
		}
		if (near_t > enter_t) {
			enter_t = near_t;
			entry_normal = near_normal;
		}
		exit_t = MIN(exit_t, far_t);
		if (enter_t > exit_t) {
			return false;
		}
	}

	if (exit_t < 0.0 || enter_t > 1.0) {
		return false;
	}

	const real_t voxel_size = voxel_data->get_voxel_size();
	const Vector3i dimensions = voxel_data->get_dimensions();
	real_t current_t = MAX(enter_t, 0.0);
	const real_t nudge_t = MIN(exit_t - current_t, voxel_size * 0.00001 / segment_length);
	const Vector3 sample_position = p_begin + segment * (current_t + MAX(nudge_t, real_t(0.0)));
	Vector3i voxel(
			Math::floor(sample_position.x / voxel_size),
			Math::floor(sample_position.y / voxel_size),
			Math::floor(sample_position.z / voxel_size));
	voxel = voxel.clamp(Vector3i(), dimensions - Vector3i(1, 1, 1));

	Vector3i step;
	Vector3 next_t(INFINITY, INFINITY, INFINITY);
	Vector3 delta_t(INFINITY, INFINITY, INFINITY);
	for (int axis = 0; axis < 3; axis++) {
		if (segment[axis] > CMP_EPSILON) {
			step[axis] = 1;
			const real_t boundary = real_t(voxel[axis] + 1) * voxel_size;
			next_t[axis] = (boundary - p_begin[axis]) / segment[axis];
			delta_t[axis] = voxel_size / segment[axis];
		} else if (segment[axis] < -CMP_EPSILON) {
			step[axis] = -1;
			const real_t boundary = real_t(voxel[axis]) * voxel_size;
			next_t[axis] = (boundary - p_begin[axis]) / segment[axis];
			delta_t[axis] = -voxel_size / segment[axis];
		}
	}

	Vector3 hit_normal = entry_normal;
	while (current_t <= exit_t + CMP_EPSILON) {
		if (voxel_data->is_solid(voxel)) {
			r_point = p_begin + segment * current_t;
			r_normal = hit_normal;
			if (r_normal.is_zero_approx()) {
				const Vector3 absolute_direction = segment.abs();
				int normal_axis = 0;
				if (absolute_direction.y > absolute_direction.x) {
					normal_axis = 1;
				}
				if (absolute_direction.z > absolute_direction[normal_axis]) {
					normal_axis = 2;
				}
				r_normal[normal_axis] = segment[normal_axis] > 0.0 ? -1.0 : 1.0;
			}
			r_face_index = voxel_data->get_voxel_index(voxel);
			return true;
		}

		int axis = 0;
		if (next_t.y < next_t.x) {
			axis = 1;
		}
		if (next_t.z < next_t[axis]) {
			axis = 2;
		}
		current_t = next_t[axis];
		if (current_t > exit_t + CMP_EPSILON || step[axis] == 0) {
			break;
		}
		next_t[axis] += delta_t[axis];
		voxel[axis] += step[axis];
		if (voxel[axis] < 0 || voxel[axis] >= dimensions[axis]) {
			break;
		}
		hit_normal = Vector3();
		hit_normal[axis] = -step[axis];
	}

	return false;
}

Vector3 GodotVoxelShape3D::get_center_of_mass() const {
	_rebuild_mass_properties();
	return center_of_mass;
}

bool GodotVoxelShape3D::intersect_point(const Vector3 &p_point) const {
	if (voxel_data.is_null() || !local_aabb.has_point(p_point)) {
		return false;
	}
	const Vector3i voxel = Vector3i(p_point / voxel_data->get_voxel_size());
	return voxel_data->is_solid(voxel);
}

Vector3 GodotVoxelShape3D::get_moment_of_inertia(real_t p_mass) const {
	_rebuild_mass_properties();
	return inertia_per_unit_mass * p_mass;
}
