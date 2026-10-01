/**************************************************************************/
/*  godot_voxel_collision_solver_3d.cpp                                   */
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

#include "godot_voxel_collision_solver_3d.h"

#include "gjk_epa.h"
#include "godot_collision_solver_3d_sat.h"
#include "godot_voxel_shape_3d.h"

#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"

namespace {

// Eight contacts are emitted at most. Keeping four times that many candidates
// preserves a well-spread manifold without spending a wall-slide query testing
// hundreds of redundant coplanar voxel contacts.
constexpr int MAX_VOXEL_CONTACT_CANDIDATES_PER_PASS = 32;
constexpr int MAX_VOXEL_CONTACT_CANDIDATES = MAX_VOXEL_CONTACT_CANDIDATES_PER_PASS * 3;
constexpr int MAX_VOXEL_MANIFOLD_CONTACTS = 8;
constexpr int MAX_EMBEDDED_EXIT_DEPTH = 8;

struct VoxelContactCandidate {
	Vector3 point_a;
	Vector3 point_b;
	Vector3 normal;
	real_t penetration = 0.0;
	int voxel_index_a = -1;
	int voxel_index_b = -1;
};

struct VoxelCallbackContext {
	LocalVector<VoxelContactCandidate> *candidates = nullptr;
	bool swap = false;
	bool collided = false;
	bool limit_reached = false;
	bool stop_at_limit = true;
	uint32_t candidate_limit = MAX_VOXEL_CONTACT_CANDIDATES_PER_PASS;
	int feature_index = -1;
	int other_index = -1;
};

uint64_t _voxel_pair_key(int p_index_a, int p_index_b) {
	return uint64_t(uint32_t(p_index_a)) |
			(uint64_t(uint32_t(p_index_b)) << 32);
}

bool _candidate_stable_less(const VoxelContactCandidate &p_a, const VoxelContactCandidate &p_b) {
	if (p_a.voxel_index_a != p_b.voxel_index_a) {
		return p_a.voxel_index_a < p_b.voxel_index_a;
	}
	if (p_a.voxel_index_b != p_b.voxel_index_b) {
		return p_a.voxel_index_b < p_b.voxel_index_b;
	}

	// A voxel-convex SAT pass can return several points for the same voxel.
	// Use the geometric values only after the discrete voxel identity so equal
	// candidates do not inherit traversal or container ordering.
	const Vector3 midpoint_a = (p_a.point_a + p_a.point_b) * 0.5;
	const Vector3 midpoint_b = (p_b.point_a + p_b.point_b) * 0.5;
	for (int axis = 0; axis < 3; axis++) {
		if (!Math::is_equal_approx(p_a.normal[axis], p_b.normal[axis])) {
			return p_a.normal[axis] < p_b.normal[axis];
		}
	}
	for (int axis = 0; axis < 3; axis++) {
		if (!Math::is_equal_approx(midpoint_a[axis], midpoint_b[axis])) {
			return midpoint_a[axis] < midpoint_b[axis];
		}
	}
	return false;
}

int _candidate_normal_bucket(const Vector3 &p_normal) {
	const Vector3 absolute_normal = p_normal.abs();
	int axis = 0;
	if (absolute_normal.y > absolute_normal.x) {
		axis = 1;
	}
	if (absolute_normal.z > absolute_normal[axis]) {
		axis = 2;
	}
	return axis * 2 + (p_normal[axis] < 0.0 ? 1 : 0);
}

void _store_candidate(LocalVector<VoxelContactCandidate> &r_candidates, const VoxelContactCandidate &p_candidate, uint32_t p_limit) {
	if (p_limit == 0) {
		return;
	}
	if (r_candidates.size() < p_limit) {
		r_candidates.push_back(p_candidate);
		return;
	}

	const int candidate_bucket = _candidate_normal_bucket(p_candidate.normal);
	int bucket_counts[6] = {};
	int shallowest_same_bucket = -1;
	for (uint32_t i = 0; i < r_candidates.size(); i++) {
		const int bucket = _candidate_normal_bucket(r_candidates[i].normal);
		bucket_counts[bucket]++;
		if (bucket == candidate_bucket &&
				(shallowest_same_bucket < 0 || r_candidates[i].penetration < r_candidates[shallowest_same_bucket].penetration)) {
			shallowest_same_bucket = i;
		}
	}

	if (shallowest_same_bucket >= 0) {
		if (p_candidate.penetration > r_candidates[shallowest_same_bucket].penetration) {
			r_candidates[shallowest_same_bucket] = p_candidate;
		}
		return;
	}

	int donor_bucket = 0;
	for (int bucket = 1; bucket < 6; bucket++) {
		if (bucket_counts[bucket] > bucket_counts[donor_bucket]) {
			donor_bucket = bucket;
		}
	}
	int replacement = -1;
	for (uint32_t i = 0; i < r_candidates.size(); i++) {
		if (_candidate_normal_bucket(r_candidates[i].normal) == donor_bucket &&
				(replacement < 0 || r_candidates[i].penetration < r_candidates[replacement].penetration)) {
			replacement = i;
		}
	}
	if (replacement >= 0) {
		r_candidates[replacement] = p_candidate;
	}
}

void _voxel_contact_callback(
		const Vector3 &p_point_feature,
		int,
		const Vector3 &p_point_other,
		int,
		const Vector3 &p_normal,
		void *p_userdata) {
	VoxelCallbackContext *context = static_cast<VoxelCallbackContext *>(p_userdata);
	context->collided = true;
	if (context->stop_at_limit && context->candidates->size() >= context->candidate_limit) {
		context->limit_reached = true;
		return;
	}

	VoxelContactCandidate candidate;
	if (context->swap) {
		candidate.point_a = p_point_other;
		candidate.point_b = p_point_feature;
		candidate.normal = -p_normal;
		candidate.voxel_index_a = context->other_index;
		candidate.voxel_index_b = context->feature_index;
	} else {
		candidate.point_a = p_point_feature;
		candidate.point_b = p_point_other;
		candidate.normal = p_normal;
		candidate.voxel_index_a = context->feature_index;
		candidate.voxel_index_b = context->other_index;
	}
	candidate.penetration = Math::abs(
			(candidate.point_a - candidate.point_b).dot(candidate.normal));
	_store_candidate(*context->candidates, candidate, context->candidate_limit);
	context->limit_reached = context->stop_at_limit && context->candidates->size() >= context->candidate_limit;
}

Vector3i _index_to_voxel(int p_index, const Vector3i &p_dimensions) {
	const int plane = p_dimensions.x * p_dimensions.y;
	return Vector3i(
			p_index % p_dimensions.x,
			(p_index / p_dimensions.x) % p_dimensions.y,
			p_index / plane);
}

Transform3D _voxel_proxy_transform(
		const Transform3D &p_shape_transform,
		const Vector3i &p_voxel,
		real_t p_voxel_size,
		const VoxelFeatureProxy3D &p_proxy) {
	return p_shape_transform *
			Transform3D(Basis(), Vector3(p_voxel) * p_voxel_size) *
			p_proxy.local_transform;
}

bool _voxel_range_from_world_aabb(
		const Ref<VoxelShapeData> &p_data,
		const Transform3D &p_voxel_transform,
		const AABB &p_world_aabb,
		Vector3i &r_lo,
		Vector3i &r_hi) {
	if (p_data.is_null() || p_world_aabb.size == Vector3()) {
		return false;
	}
	const Vector3i dimensions = p_data->get_dimensions();
	const real_t voxel_size = p_data->get_voxel_size();
	const AABB local_bounds = p_voxel_transform.affine_inverse().xform(p_world_aabb);
	const Vector3 local_end = local_bounds.get_end();
	r_lo = Vector3i(
			Math::floor(local_bounds.position.x / voxel_size),
			Math::floor(local_bounds.position.y / voxel_size),
			Math::floor(local_bounds.position.z / voxel_size));
	r_hi = Vector3i(
			Math::floor(local_end.x / voxel_size),
			Math::floor(local_end.y / voxel_size),
			Math::floor(local_end.z / voxel_size));
	if (r_hi.x < 0 || r_hi.y < 0 || r_hi.z < 0 ||
			r_lo.x >= dimensions.x || r_lo.y >= dimensions.y || r_lo.z >= dimensions.z) {
		return false;
	}
	r_lo = r_lo.clamp(Vector3i(), dimensions - Vector3i(1, 1, 1));
	r_hi = r_hi.clamp(Vector3i(), dimensions - Vector3i(1, 1, 1));
	return true;
}

Transform3D _solid_voxel_transform(
		const Transform3D &p_voxel_transform,
		const Vector3i &p_voxel,
		real_t p_voxel_size) {
	return p_voxel_transform * Transform3D(Basis(), (Vector3(p_voxel) + Vector3(0.5, 0.5, 0.5)) * p_voxel_size);
}

bool _distance_convex_to_voxel(
		const GodotShape3D *p_convex_shape,
		const Transform3D &p_convex_transform,
		const GodotVoxelShape3D *p_voxel_shape,
		const Transform3D &p_voxel_transform,
		Vector3 &r_point_convex,
		Vector3 &r_point_voxel,
		const AABB &p_swept_hint) {
	const Ref<VoxelShapeData> data = p_voxel_shape->get_voxel_data();
	if (data.is_null() || p_voxel_shape->get_solid_indices().is_empty()) {
		r_point_convex = p_convex_transform.origin;
		r_point_voxel = p_voxel_transform.origin;
		return true;
	}

	GodotBoxShape3D voxel_box;
	const real_t voxel_size = data->get_voxel_size();
	voxel_box.set_data(Vector3(1, 1, 1) * voxel_size * 0.5);
	real_t best_distance_squared = INFINITY;
	bool tested = false;
	const Vector3i dimensions = data->get_dimensions();
	auto test_indices = [&](const GodotVoxelShape3D::IndexRange &p_indices) {
		for (const int index : p_indices) {
			const Vector3i voxel = _index_to_voxel(index, dimensions);
			Vector3 point_convex;
			Vector3 point_voxel;
			const bool separated = gjk_epa_calculate_distance(
					p_convex_shape, p_convex_transform,
					&voxel_box, _solid_voxel_transform(p_voxel_transform, voxel, voxel_size),
					point_convex, point_voxel);
			if (!separated) {
				r_point_convex = point_convex;
				r_point_voxel = point_voxel;
				return false;
			}
			tested = true;
			const real_t distance_squared = point_convex.distance_squared_to(point_voxel);
			if (distance_squared < best_distance_squared) {
				best_distance_squared = distance_squared;
				r_point_convex = point_convex;
				r_point_voxel = point_voxel;
			}
		}
		return true;
	};

	Vector3i lo;
	Vector3i hi;
	AABB query_bounds = p_swept_hint;
	if (p_swept_hint != AABB()) {
		// The broad-phase hint covers the complete frame motion. A static
		// overlap query only needs the shape at its current transform, while a
		// motion query needs the shortened sweep used by this binary-search
		// iteration. Keeping the full hint here repeatedly scanned future floor
		// and wall cells while a CharacterBody3D was sliding against a wall.
		const GodotShape3D *base_shape = p_convex_shape;
		const GodotMotionShape3D *query_motion_shape = nullptr;
		if (p_convex_shape->is_motion_shape()) {
			query_motion_shape = static_cast<const GodotMotionShape3D *>(p_convex_shape);
			base_shape = query_motion_shape->shape;
		}
		if (base_shape != nullptr && base_shape->get_aabb().has_surface()) {
			query_bounds = p_convex_transform.xform(base_shape->get_aabb());
			if (query_motion_shape != nullptr) {
				const Vector3 world_motion = p_convex_transform.basis.xform(query_motion_shape->motion);
				query_bounds = query_bounds.merge(AABB(query_bounds.position + world_motion, query_bounds.size));
			}
		}
	}
	if (query_bounds != AABB()) {
		if (!_voxel_range_from_world_aabb(data, p_voxel_transform, query_bounds, lo, hi)) {
			r_point_convex = p_convex_transform.origin;
			r_point_voxel = p_voxel_transform.origin;
			return true;
		}
		if (!test_indices(p_voxel_shape->get_surface_indices(lo, hi))) {
			return false;
		}
		if (!tested && !test_indices(p_voxel_shape->get_solid_indices(lo, hi))) {
			return false;
		}
	} else {
		if (!test_indices(p_voxel_shape->get_surface_indices())) {
			return false;
		}
		if (!tested && !test_indices(p_voxel_shape->get_solid_indices())) {
			return false;
		}
	}

	if (!tested) {
		r_point_convex = p_convex_transform.origin;
		r_point_voxel = p_voxel_transform.origin;
	}
	return true;
}

bool _distance_voxel_to_voxel(
		const GodotVoxelShape3D *p_shape_a,
		const Transform3D &p_transform_a,
		const GodotVoxelShape3D *p_shape_b,
		const Transform3D &p_transform_b,
		Vector3 &r_point_a,
		Vector3 &r_point_b,
		const AABB &p_swept_hint) {
	const Ref<VoxelShapeData> data_a = p_shape_a->get_voxel_data();
	const Ref<VoxelShapeData> data_b = p_shape_b->get_voxel_data();
	if (data_a.is_null() || data_b.is_null() ||
			p_shape_a->get_solid_indices().is_empty() || p_shape_b->get_solid_indices().is_empty()) {
		r_point_a = p_transform_a.origin;
		r_point_b = p_transform_b.origin;
		return true;
	}

	LocalVector<int> candidate_b;
	const Vector3i dimensions_b = data_b->get_dimensions();
	if (p_swept_hint != AABB()) {
		Vector3i lo;
		Vector3i hi;
		if (!_voxel_range_from_world_aabb(data_b, p_transform_b, p_swept_hint, lo, hi)) {
			r_point_a = p_transform_a.origin;
			r_point_b = p_transform_b.origin;
			return true;
		}
		for (int z = lo.z; z <= hi.z; z++) {
			for (int y = lo.y; y <= hi.y; y++) {
				for (int x = lo.x; x <= hi.x; x++) {
					const Vector3i voxel(x, y, z);
					if (data_b->is_solid(voxel)) {
						candidate_b.push_back(x + y * dimensions_b.x + z * dimensions_b.x * dimensions_b.y);
					}
				}
			}
		}
	} else {
		for (const int index : p_shape_b->get_solid_indices()) {
			candidate_b.push_back(index);
		}
	}
	if (candidate_b.is_empty()) {
		r_point_a = p_transform_a.origin;
		r_point_b = p_transform_b.origin;
		return true;
	}

	GodotBoxShape3D box_a;
	GodotBoxShape3D box_b;
	const real_t voxel_size_a = data_a->get_voxel_size();
	const real_t voxel_size_b = data_b->get_voxel_size();
	box_a.set_data(Vector3(1, 1, 1) * voxel_size_a * 0.5);
	box_b.set_data(Vector3(1, 1, 1) * voxel_size_b * 0.5);
	const Vector3i dimensions_a = data_a->get_dimensions();
	real_t best_distance_squared = INFINITY;
	bool tested = false;
	for (const int index_a : p_shape_a->get_solid_indices()) {
		const Vector3i voxel_a = _index_to_voxel(index_a, dimensions_a);
		const Transform3D transform_a = _solid_voxel_transform(p_transform_a, voxel_a, voxel_size_a);
		for (const int index_b : candidate_b) {
			const Vector3i voxel_b = _index_to_voxel(index_b, dimensions_b);
			Vector3 point_a;
			Vector3 point_b;
			const bool separated = gjk_epa_calculate_distance(
					&box_a, transform_a,
					&box_b, _solid_voxel_transform(p_transform_b, voxel_b, voxel_size_b),
					point_a, point_b);
			if (!separated) {
				r_point_a = point_a;
				r_point_b = point_b;
				return false;
			}
			tested = true;
			const real_t distance_squared = point_a.distance_squared_to(point_b);
			if (distance_squared < best_distance_squared) {
				best_distance_squared = distance_squared;
				r_point_a = point_a;
				r_point_b = point_b;
			}
		}
	}
	if (!tested) {
		r_point_a = p_transform_a.origin;
		r_point_b = p_transform_b.origin;
	}
	return true;
}

bool _distance_motion_voxel_to_voxel(
		const GodotMotionShape3D *p_motion_shape,
		const Transform3D &p_motion_transform,
		const GodotVoxelShape3D *p_target_shape,
		const Transform3D &p_target_transform,
		Vector3 &r_point_motion,
		Vector3 &r_point_target,
		const AABB &p_swept_hint) {
	ERR_FAIL_NULL_V(p_motion_shape, true);
	ERR_FAIL_NULL_V(p_motion_shape->shape, true);
	ERR_FAIL_COND_V(p_motion_shape->shape->get_type() != PhysicsServer3D::SHAPE_CUSTOM, true);
	const GodotVoxelShape3D *moving_shape = static_cast<const GodotVoxelShape3D *>(p_motion_shape->shape);
	const Ref<VoxelShapeData> moving_data = moving_shape->get_voxel_data();
	const Ref<VoxelShapeData> target_data = p_target_shape->get_voxel_data();
	if (moving_data.is_null() || target_data.is_null() ||
			moving_shape->get_solid_indices().is_empty() || p_target_shape->get_solid_indices().is_empty()) {
		r_point_motion = p_motion_transform.origin;
		r_point_target = p_target_transform.origin;
		return true;
	}

	LocalVector<int> target_candidates;
	const Vector3i target_dimensions = target_data->get_dimensions();
	if (p_swept_hint != AABB()) {
		Vector3i lo;
		Vector3i hi;
		if (!_voxel_range_from_world_aabb(target_data, p_target_transform, p_swept_hint, lo, hi)) {
			r_point_motion = p_motion_transform.origin;
			r_point_target = p_target_transform.origin;
			return true;
		}
		for (int z = lo.z; z <= hi.z; z++) {
			for (int y = lo.y; y <= hi.y; y++) {
				for (int x = lo.x; x <= hi.x; x++) {
					const Vector3i voxel(x, y, z);
					if (target_data->is_solid(voxel)) {
						target_candidates.push_back(target_data->get_voxel_index(voxel));
					}
				}
			}
		}
	} else {
		for (const int index : p_target_shape->get_solid_indices()) {
			target_candidates.push_back(index);
		}
	}
	if (target_candidates.is_empty()) {
		r_point_motion = p_motion_transform.origin;
		r_point_target = p_target_transform.origin;
		return true;
	}

	GodotBoxShape3D moving_box;
	GodotBoxShape3D target_box;
	const real_t moving_voxel_size = moving_data->get_voxel_size();
	const real_t target_voxel_size = target_data->get_voxel_size();
	moving_box.set_data(Vector3(1, 1, 1) * moving_voxel_size * 0.5);
	target_box.set_data(Vector3(1, 1, 1) * target_voxel_size * 0.5);
	GodotMotionShape3D moving_voxel;
	moving_voxel.shape = &moving_box;
	moving_voxel.motion = p_motion_shape->motion;

	const Vector3i moving_dimensions = moving_data->get_dimensions();
	real_t best_distance_squared = INFINITY;
	bool tested = false;
	for (const int moving_index : moving_shape->get_solid_indices()) {
		const Transform3D moving_transform = _solid_voxel_transform(
				p_motion_transform, _index_to_voxel(moving_index, moving_dimensions), moving_voxel_size);
		for (const int target_index : target_candidates) {
			Vector3 point_motion;
			Vector3 point_target;
			const bool separated = gjk_epa_calculate_distance(
					&moving_voxel, moving_transform,
					&target_box, _solid_voxel_transform(p_target_transform, _index_to_voxel(target_index, target_dimensions), target_voxel_size),
					point_motion, point_target);
			if (!separated) {
				r_point_motion = point_motion;
				r_point_target = point_target;
				return false;
			}
			tested = true;
			const real_t distance_squared = point_motion.distance_squared_to(point_target);
			if (distance_squared < best_distance_squared) {
				best_distance_squared = distance_squared;
				r_point_motion = point_motion;
				r_point_target = point_target;
			}
		}
	}
	if (!tested) {
		r_point_motion = p_motion_transform.origin;
		r_point_target = p_target_transform.origin;
	}
	return true;
}

bool _distance_motion_voxel_to_convex(
		const GodotMotionShape3D *p_motion_shape,
		const Transform3D &p_motion_transform,
		const GodotShape3D *p_convex_shape,
		const Transform3D &p_convex_transform,
		Vector3 &r_point_motion,
		Vector3 &r_point_convex) {
	ERR_FAIL_NULL_V(p_motion_shape, true);
	ERR_FAIL_NULL_V(p_motion_shape->shape, true);
	ERR_FAIL_NULL_V(p_convex_shape, true);
	ERR_FAIL_COND_V(p_motion_shape->shape->get_type() != PhysicsServer3D::SHAPE_CUSTOM, true);
	ERR_FAIL_COND_V(p_convex_shape->is_concave(), true);
	const GodotVoxelShape3D *moving_shape = static_cast<const GodotVoxelShape3D *>(p_motion_shape->shape);
	const Ref<VoxelShapeData> moving_data = moving_shape->get_voxel_data();
	if (moving_data.is_null() || moving_shape->get_solid_indices().is_empty()) {
		r_point_motion = p_motion_transform.origin;
		r_point_convex = p_convex_transform.origin;
		return true;
	}

	GodotBoxShape3D moving_box;
	const real_t voxel_size = moving_data->get_voxel_size();
	moving_box.set_data(Vector3(1, 1, 1) * voxel_size * 0.5);
	GodotMotionShape3D moving_voxel;
	moving_voxel.shape = &moving_box;
	moving_voxel.motion = p_motion_shape->motion;
	const Vector3i dimensions = moving_data->get_dimensions();
	real_t best_distance_squared = INFINITY;
	for (const int index : moving_shape->get_solid_indices()) {
		Vector3 point_motion;
		Vector3 point_convex;
		const bool separated = gjk_epa_calculate_distance(
				&moving_voxel, _solid_voxel_transform(p_motion_transform, _index_to_voxel(index, dimensions), voxel_size),
				p_convex_shape, p_convex_transform,
				point_motion, point_convex);
		if (!separated) {
			r_point_motion = point_motion;
			r_point_convex = point_convex;
			return false;
		}
		const real_t distance_squared = point_motion.distance_squared_to(point_convex);
		if (distance_squared < best_distance_squared) {
			best_distance_squared = distance_squared;
			r_point_motion = point_motion;
			r_point_convex = point_convex;
		}
	}
	return true;
}

struct SolidExitNode {
	Vector3i voxel;
	int depth = 0;
};

bool _find_nearest_solid_exit(
		const Ref<VoxelShapeData> &p_solid_data,
		const Transform3D &p_solid_transform,
		const Vector3i &p_start_voxel,
		const Vector3 &p_center_local,
		Vector3 &r_surface_world,
		Vector3 &r_normal_world) {
	static const Vector3i directions[6] = {
		Vector3i(-1, 0, 0),
		Vector3i(1, 0, 0),
		Vector3i(0, -1, 0),
		Vector3i(0, 1, 0),
		Vector3i(0, 0, -1),
		Vector3i(0, 0, 1),
	};
	const Vector3i dimensions = p_solid_data->get_dimensions();
	const real_t voxel_size = p_solid_data->get_voxel_size();
	LocalVector<SolidExitNode> queue;
	HashSet<int> visited;
	queue.push_back({ p_start_voxel, 0 });
	visited.insert(p_start_voxel.x + p_start_voxel.y * dimensions.x + p_start_voxel.z * dimensions.x * dimensions.y);

	const Vector3 center_world = p_solid_transform.xform(p_center_local);
	real_t best_distance_squared = INFINITY;
	for (uint32_t cursor = 0; cursor < queue.size(); cursor++) {
		const SolidExitNode node = queue[cursor];
		const Vector3 box_min = Vector3(node.voxel) * voxel_size;
		const Vector3 box_max = box_min + Vector3(1, 1, 1) * voxel_size;

		for (const Vector3i &direction : directions) {
			const Vector3i neighbor = node.voxel + direction;
			if (p_solid_data->is_solid(neighbor)) {
				if (node.depth < MAX_EMBEDDED_EXIT_DEPTH) {
					const int index = neighbor.x + neighbor.y * dimensions.x + neighbor.z * dimensions.x * dimensions.y;
					if (!visited.has(index)) {
						visited.insert(index);
						queue.push_back({ neighbor, node.depth + 1 });
					}
				}
				continue;
			}

			Vector3 closest = p_center_local.clamp(box_min, box_max);
			if (direction.x != 0) {
				closest.x = direction.x > 0 ? box_max.x : box_min.x;
			} else if (direction.y != 0) {
				closest.y = direction.y > 0 ? box_max.y : box_min.y;
			} else {
				closest.z = direction.z > 0 ? box_max.z : box_min.z;
			}
			const Vector3 surface_world = p_solid_transform.xform(closest);
			const Vector3 to_surface = surface_world - center_world;
			const real_t distance_squared = to_surface.length_squared();
			if (distance_squared >= best_distance_squared) {
				continue;
			}
			best_distance_squared = distance_squared;
			r_surface_world = surface_world;
			r_normal_world = !to_surface.is_zero_approx() ? to_surface.normalized() : p_solid_transform.basis.xform(Vector3(direction)).normalized();
		}
	}

	return best_distance_squared < INFINITY && !r_normal_world.is_zero_approx();
}

bool _test_sphere_against_solid(
		const GodotSphereShape3D *p_sphere,
		const Transform3D &p_sphere_transform,
		const Ref<VoxelShapeData> &p_solid_data,
		const Transform3D &p_solid_transform,
		const Vector3i &p_solid_voxel,
		real_t p_sphere_margin,
		real_t p_solid_margin,
		VoxelCallbackContext &r_context) {
	const real_t voxel_size = p_solid_data->get_voxel_size();
	const Transform3D solid_inverse = p_solid_transform.affine_inverse();
	const Vector3 center_world = p_sphere_transform.origin;
	const Vector3 center_local = solid_inverse.xform(center_world);
	const Vector3 box_min = Vector3(p_solid_voxel) * voxel_size;
	const Vector3 box_max = box_min + Vector3(1, 1, 1) * voxel_size;
	const Vector3 closest_local = center_local.clamp(box_min, box_max);
	Vector3 closest_world = p_solid_transform.xform(closest_local);
	Vector3 from_solid = center_world - closest_world;
	const real_t radius = p_sphere->get_radius() * p_sphere_transform.basis[0].length();

	if (!from_solid.is_zero_approx()) {
		const real_t distance = from_solid.length();
		if (distance > radius + p_sphere_margin + p_solid_margin) {
			return false;
		}
		from_solid /= distance;
	} else if (!_find_nearest_solid_exit(
					   p_solid_data, p_solid_transform, p_solid_voxel, center_local,
					   closest_world, from_solid)) {
		return false;
	}

	// The callback normal follows SAT's convention (solid B towards sphere A).
	// BodyPair inverts it to its stored separating-normal convention.
	const Vector3 point_sphere = center_world - from_solid * (radius + p_sphere_margin);
	const Vector3 point_solid = closest_world + from_solid * p_solid_margin;
	_voxel_contact_callback(point_sphere, 0, point_solid, 0, from_solid, &r_context);
	return true;
}

bool _test_features_against_solids(
		VoxelFeatureType p_feature_type,
		const GodotVoxelShape3D *p_feature_shape,
		const Transform3D &p_feature_transform,
		const GodotVoxelShape3D *p_solid_shape,
		const Transform3D &p_solid_transform,
		bool p_swap,
		real_t p_feature_margin,
		real_t p_solid_margin,
		HashSet<uint64_t> &r_seen,
		LocalVector<VoxelContactCandidate> &r_candidates) {
	const Ref<VoxelShapeData> feature_data = p_feature_shape->get_voxel_data();
	const Ref<VoxelShapeData> solid_data = p_solid_shape->get_voxel_data();
	if (feature_data.is_null() || solid_data.is_null()) {
		return false;
	}

	if (p_feature_type != VoxelFeatureType::EDGE &&
			p_feature_type != VoxelFeatureType::CORNER) {
		return false;
	}
	const Vector3i feature_dimensions = feature_data->get_dimensions();
	const Vector3i solid_dimensions = solid_data->get_dimensions();
	const real_t feature_voxel_size = feature_data->get_voxel_size();
	const real_t solid_voxel_size = solid_data->get_voxel_size();
	const AABB solid_world_bounds = p_solid_transform.xform(AABB(Vector3(), Vector3(solid_dimensions) * solid_voxel_size)).grow(feature_voxel_size + p_feature_margin + p_solid_margin);
	Vector3i feature_lo;
	Vector3i feature_hi;
	if (!_voxel_range_from_world_aabb(feature_data, p_feature_transform, solid_world_bounds, feature_lo, feature_hi)) {
		return false;
	}
	const GodotVoxelShape3D::IndexRange feature_indices = p_feature_shape->get_feature_indices(p_feature_type, feature_lo, feature_hi);
	if (!p_solid_shape->has_solids() || feature_indices.is_empty()) {
		return false;
	}

	GodotBoxShape3D solid_voxel_box;
	solid_voxel_box.set_data(Vector3(1, 1, 1) * solid_voxel_size * 0.5);
	const Transform3D solid_inverse = p_solid_transform.affine_inverse();
	bool collided = false;
	const uint32_t candidate_limit = MIN(
			r_candidates.size() + MAX_VOXEL_CONTACT_CANDIDATES_PER_PASS,
			uint32_t(MAX_VOXEL_CONTACT_CANDIDATES));
	if (r_candidates.size() >= candidate_limit) {
		return false;
	}

	for (const int feature_index : feature_indices) {
		const Vector3i feature_voxel = _index_to_voxel(feature_index, feature_dimensions);
		const uint8_t feature_mask = p_feature_type == VoxelFeatureType::EDGE ? feature_data->get_edge_mask(feature_voxel) : feature_data->get_corner_mask(feature_voxel);
		const VoxelFeatureProxy3D &proxy = p_feature_shape->get_feature_proxy(
				p_feature_type, feature_mask);
		if (!proxy.is_valid()) {
			continue;
		}

		const Transform3D proxy_world_transform = _voxel_proxy_transform(
				p_feature_transform, feature_voxel, feature_voxel_size, proxy);
		AABB search_bounds = solid_inverse.xform(
				proxy_world_transform.xform(proxy.shape->get_aabb()));
		search_bounds = search_bounds.grow(p_feature_margin + p_solid_margin);
		const Vector3 search_end = search_bounds.get_end();
		Vector3i lo(
				Math::floor(search_bounds.position.x / solid_voxel_size),
				Math::floor(search_bounds.position.y / solid_voxel_size),
				Math::floor(search_bounds.position.z / solid_voxel_size));
		Vector3i hi(
				Math::floor(search_end.x / solid_voxel_size),
				Math::floor(search_end.y / solid_voxel_size),
				Math::floor(search_end.z / solid_voxel_size));

		if (hi.x < 0 || hi.y < 0 || hi.z < 0 ||
				lo.x >= solid_dimensions.x || lo.y >= solid_dimensions.y || lo.z >= solid_dimensions.z) {
			continue;
		}
		lo = lo.clamp(Vector3i(), solid_dimensions - Vector3i(1, 1, 1));
		hi = hi.clamp(Vector3i(), solid_dimensions - Vector3i(1, 1, 1));

		for (int z = lo.z; z <= hi.z; z++) {
			for (int y = lo.y; y <= hi.y; y++) {
				for (int x = lo.x; x <= hi.x; x++) {
					const Vector3i solid_voxel(x, y, z);
					const int solid_index = x + y * solid_dimensions.x +
							z * solid_dimensions.x * solid_dimensions.y;
					if (!solid_data->is_solid(solid_voxel)) {
						continue;
					}

					const uint64_t key = p_swap ? _voxel_pair_key(solid_index, feature_index) : _voxel_pair_key(feature_index, solid_index);
					if (r_seen.has(key)) {
						continue;
					}

					const Transform3D solid_world_transform = p_solid_transform * Transform3D(Basis(), (Vector3(solid_voxel) + Vector3(0.5, 0.5, 0.5)) * solid_voxel_size);
					VoxelCallbackContext context;
					context.candidates = &r_candidates;
					context.swap = p_swap;
					context.feature_index = feature_index;
					context.other_index = solid_index;
					context.candidate_limit = candidate_limit;
					if (proxy.shape->get_type() == PhysicsServer3D::SHAPE_SPHERE) {
						_test_sphere_against_solid(
								static_cast<const GodotSphereShape3D *>(proxy.shape), proxy_world_transform,
								solid_data, p_solid_transform, solid_voxel,
								p_feature_margin, p_solid_margin, context);
					} else {
						sat_calculate_penetration(
								proxy.shape, proxy_world_transform,
								&solid_voxel_box, solid_world_transform,
								_voxel_contact_callback, &context, false, nullptr,
								p_feature_margin, p_solid_margin);
					}
					if (context.collided) {
						r_seen.insert(key);
						collided = true;
					}
					if (context.limit_reached) {
						return collided;
					}
				}
			}
		}
	}

	return collided;
}

bool _test_edges_against_edges(
		const GodotVoxelShape3D *p_shape_a,
		const Transform3D &p_transform_a,
		const GodotVoxelShape3D *p_shape_b,
		const Transform3D &p_transform_b,
		real_t p_margin_a,
		real_t p_margin_b,
		HashSet<uint64_t> &r_seen,
		LocalVector<VoxelContactCandidate> &r_candidates) {
	const Ref<VoxelShapeData> data_a = p_shape_a->get_voxel_data();
	const Ref<VoxelShapeData> data_b = p_shape_b->get_voxel_data();
	if (data_a.is_null() || data_b.is_null()) {
		return false;
	}

	const Vector3i dimensions_a = data_a->get_dimensions();
	const Vector3i dimensions_b = data_b->get_dimensions();
	const real_t voxel_size_a = data_a->get_voxel_size();
	const real_t voxel_size_b = data_b->get_voxel_size();
	const Transform3D inverse_b = p_transform_b.affine_inverse();
	bool collided = false;
	const uint32_t candidate_limit = MIN(
			r_candidates.size() + MAX_VOXEL_CONTACT_CANDIDATES_PER_PASS,
			uint32_t(MAX_VOXEL_CONTACT_CANDIDATES));
	if (r_candidates.size() >= candidate_limit) {
		return false;
	}

	const AABB shape_b_world_bounds = p_transform_b.xform(AABB(Vector3(), Vector3(dimensions_b) * voxel_size_b)).grow(voxel_size_a + p_margin_a + p_margin_b);
	Vector3i edge_lo_a;
	Vector3i edge_hi_a;
	if (!_voxel_range_from_world_aabb(data_a, p_transform_a, shape_b_world_bounds, edge_lo_a, edge_hi_a)) {
		return false;
	}
	const GodotVoxelShape3D::IndexRange edge_indices_a = p_shape_a->get_feature_indices(VoxelFeatureType::EDGE, edge_lo_a, edge_hi_a);
	for (const int index_a : edge_indices_a) {
		const Vector3i voxel_a = _index_to_voxel(index_a, dimensions_a);
		const VoxelFeatureProxy3D &proxy_a = p_shape_a->get_feature_proxy(
				VoxelFeatureType::EDGE, data_a->get_edge_mask(voxel_a));
		if (!proxy_a.is_valid()) {
			continue;
		}
		const Transform3D world_a = _voxel_proxy_transform(
				p_transform_a, voxel_a, voxel_size_a, proxy_a);
		AABB bounds_b = inverse_b.xform(world_a.xform(proxy_a.shape->get_aabb()));
		bounds_b = bounds_b.grow(p_margin_a + p_margin_b);
		const Vector3 end_b = bounds_b.get_end();
		Vector3i lo(
				Math::floor(bounds_b.position.x / voxel_size_b),
				Math::floor(bounds_b.position.y / voxel_size_b),
				Math::floor(bounds_b.position.z / voxel_size_b));
		Vector3i hi(
				Math::floor(end_b.x / voxel_size_b),
				Math::floor(end_b.y / voxel_size_b),
				Math::floor(end_b.z / voxel_size_b));
		if (hi.x < 0 || hi.y < 0 || hi.z < 0 ||
				lo.x >= dimensions_b.x || lo.y >= dimensions_b.y || lo.z >= dimensions_b.z) {
			continue;
		}
		lo = lo.clamp(Vector3i(), dimensions_b - Vector3i(1, 1, 1));
		hi = hi.clamp(Vector3i(), dimensions_b - Vector3i(1, 1, 1));

		for (int z = lo.z; z <= hi.z; z++) {
			for (int y = lo.y; y <= hi.y; y++) {
				for (int x = lo.x; x <= hi.x; x++) {
					const Vector3i voxel_b(x, y, z);
					const int index_b = x + y * dimensions_b.x + z * dimensions_b.x * dimensions_b.y;
					if (!p_shape_b->is_edge_feature(voxel_b)) {
						continue;
					}
					const uint64_t key = _voxel_pair_key(index_a, index_b);
					if (r_seen.has(key)) {
						continue;
					}
					const VoxelFeatureProxy3D &proxy_b = p_shape_b->get_feature_proxy(
							VoxelFeatureType::EDGE, data_b->get_edge_mask(voxel_b));
					if (!proxy_b.is_valid()) {
						continue;
					}
					const Transform3D world_b = _voxel_proxy_transform(
							p_transform_b, voxel_b, voxel_size_b, proxy_b);
					VoxelCallbackContext context;
					context.candidates = &r_candidates;
					context.feature_index = index_a;
					context.other_index = index_b;
					context.candidate_limit = candidate_limit;
					sat_calculate_penetration(
							proxy_a.shape, world_a,
							proxy_b.shape, world_b,
							_voxel_contact_callback, &context, false, nullptr,
							p_margin_a, p_margin_b);
					if (context.collided) {
						r_seen.insert(key);
						collided = true;
					}
					if (context.limit_reached) {
						return collided;
					}
				}
			}
		}
	}
	return collided;
}

struct VoxelConcaveStaticContext {
	const GodotVoxelShape3D *voxel_shape = nullptr;
	const Transform3D *voxel_transform = nullptr;
	const Transform3D *concave_transform = nullptr;
	GodotCollisionSolver3D::CallbackResult result_callback = nullptr;
	void *userdata = nullptr;
	bool voxel_is_a = true;
	bool collided = false;
	real_t voxel_margin = 0.0;
	real_t concave_margin = 0.0;
};

bool _voxel_concave_static_callback(void *p_userdata, GodotShape3D *p_convex) {
	VoxelConcaveStaticContext *context = static_cast<VoxelConcaveStaticContext *>(p_userdata);
	const bool collided = GodotVoxelCollisionSolver3D::solve_voxel_convex(
			context->voxel_shape, *context->voxel_transform,
			p_convex, *context->concave_transform,
			context->voxel_is_a, context->result_callback, context->userdata,
			context->voxel_margin, context->concave_margin);
	context->collided |= collided;
	return collided && context->result_callback == nullptr;
}

struct VoxelConcaveDistanceContext {
	const GodotShape3D *voxel_query = nullptr;
	const Transform3D *voxel_transform = nullptr;
	const Transform3D *concave_transform = nullptr;
	Vector3 point_voxel;
	Vector3 point_concave;
	real_t best_distance_squared = INFINITY;
	bool tested = false;
	bool collided = false;
};

bool _voxel_concave_distance_callback(void *p_userdata, GodotShape3D *p_convex) {
	VoxelConcaveDistanceContext *context = static_cast<VoxelConcaveDistanceContext *>(p_userdata);
	Vector3 point_voxel;
	Vector3 point_concave;
	const bool separated = GodotVoxelCollisionSolver3D::solve_distance(
			context->voxel_query, *context->voxel_transform,
			p_convex, *context->concave_transform,
			point_voxel, point_concave);
	context->tested = true;
	if (!separated) {
		context->point_voxel = point_voxel;
		context->point_concave = point_concave;
		context->collided = true;
		return true;
	}
	const real_t distance_squared = point_voxel.distance_squared_to(point_concave);
	if (distance_squared < context->best_distance_squared) {
		context->best_distance_squared = distance_squared;
		context->point_voxel = point_voxel;
		context->point_concave = point_concave;
	}
	return false;
}

bool _distance_voxel_query_to_concave(
		const GodotShape3D *p_voxel_query,
		const Transform3D &p_voxel_transform,
		const GodotConcaveShape3D *p_concave_shape,
		const Transform3D &p_concave_transform,
		Vector3 &r_point_voxel,
		Vector3 &r_point_concave,
		const AABB &p_swept_hint) {
	const GodotShape3D *base_shape = p_voxel_query;
	Vector3 local_motion;
	if (p_voxel_query->is_motion_shape()) {
		const GodotMotionShape3D *motion_shape = static_cast<const GodotMotionShape3D *>(p_voxel_query);
		ERR_FAIL_NULL_V(motion_shape->shape, true);
		base_shape = motion_shape->shape;
		local_motion = motion_shape->motion;
	}

	AABB world_bounds;
	if (p_swept_hint != AABB()) {
		world_bounds = p_swept_hint;
	} else {
		world_bounds = p_voxel_transform.xform(base_shape->get_aabb());
		if (!local_motion.is_zero_approx()) {
			const Vector3 world_motion = p_voxel_transform.basis.xform(local_motion);
			world_bounds = world_bounds.merge(AABB(world_bounds.position + world_motion, world_bounds.size));
		}
	}

	VoxelConcaveDistanceContext context;
	context.voxel_query = p_voxel_query;
	context.voxel_transform = &p_voxel_transform;
	context.concave_transform = &p_concave_transform;
	const AABB local_bounds = p_concave_transform.affine_inverse().xform(world_bounds);
	p_concave_shape->cull(local_bounds, _voxel_concave_distance_callback, &context, false);
	if (!context.tested) {
		r_point_voxel = p_voxel_transform.origin;
		r_point_concave = p_concave_transform.origin;
		return true;
	}
	r_point_voxel = context.point_voxel;
	r_point_concave = context.point_concave;
	return !context.collided;
}

bool _candidate_is_better(
		const VoxelContactCandidate &p_candidate,
		real_t p_spread,
		const VoxelContactCandidate &p_best,
		real_t p_best_spread,
		bool p_selecting_deepest) {
	if (p_selecting_deepest) {
		if (!Math::is_equal_approx(p_candidate.penetration, p_best.penetration)) {
			return p_candidate.penetration > p_best.penetration;
		}
	} else {
		// Keep squared distance and penetration as separate comparison keys.
		// Adding them produces a scale-dependent score with incompatible units.
		if (!Math::is_equal_approx(p_spread, p_best_spread)) {
			return p_spread > p_best_spread;
		}
		if (!Math::is_equal_approx(p_candidate.penetration, p_best.penetration)) {
			return p_candidate.penetration > p_best.penetration;
		}
	}
	return _candidate_stable_less(p_candidate, p_best);
}

void _emit_manifold(
		const LocalVector<VoxelContactCandidate> &p_candidates,
		GodotCollisionSolver3D::CallbackResult p_callback,
		void *p_userdata) {
	if (p_callback == nullptr || p_candidates.is_empty()) {
		return;
	}

	int selected[MAX_VOXEL_MANIFOLD_CONTACTS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	int selected_count = 0;
	while (selected_count < MAX_VOXEL_MANIFOLD_CONTACTS && selected_count < int(p_candidates.size())) {
		int best = -1;
		real_t best_spread = 0.0;
		for (uint32_t i = 0; i < p_candidates.size(); i++) {
			bool already_selected = false;
			for (int j = 0; j < selected_count; j++) {
				already_selected |= selected[j] == int(i);
			}
			if (already_selected) {
				continue;
			}

			real_t spread = 0.0;
			if (selected_count > 0) {
				spread = INFINITY;
				const Vector3 midpoint = (p_candidates[i].point_a + p_candidates[i].point_b) * 0.5;
				for (int j = 0; j < selected_count; j++) {
					const VoxelContactCandidate &chosen = p_candidates[selected[j]];
					const Vector3 chosen_midpoint = (chosen.point_a + chosen.point_b) * 0.5;
					spread = MIN(spread, midpoint.distance_squared_to(chosen_midpoint));
				}
			}

			if (best < 0 || _candidate_is_better(p_candidates[i], spread, p_candidates[best], best_spread, selected_count == 0)) {
				best = i;
				best_spread = spread;
			}
		}
		if (best < 0) {
			break;
		}
		selected[selected_count++] = best;
	}

	for (int i = 0; i < selected_count; i++) {
		const VoxelContactCandidate &contact = p_candidates[selected[i]];
		p_callback(
				contact.point_a, contact.voxel_index_a,
				contact.point_b, contact.voxel_index_b,
				contact.normal, p_userdata);
	}
}

} // namespace

bool GodotVoxelCollisionSolver3D::solve_voxel_convex(
		const GodotVoxelShape3D *p_voxel_shape,
		const Transform3D &p_voxel_transform,
		const GodotShape3D *p_convex_shape,
		const Transform3D &p_convex_transform,
		bool p_voxel_is_a,
		GodotCollisionSolver3D::CallbackResult p_result_callback,
		void *p_userdata,
		real_t p_voxel_margin,
		real_t p_convex_margin) {
	ERR_FAIL_NULL_V(p_voxel_shape, false);
	ERR_FAIL_NULL_V(p_convex_shape, false);
	ERR_FAIL_COND_V(p_convex_shape->is_concave(), false);
	const Ref<VoxelShapeData> data = p_voxel_shape->get_voxel_data();
	if (data.is_null() || p_voxel_shape->get_solid_indices().is_empty()) {
		return false;
	}

	AABB convex_world_bounds;
	if (p_convex_shape->get_type() == PhysicsServer3D::SHAPE_CONCAVE_POLYGON) {
		// Concave culling supplies a temporary GodotFaceShape3D whose cached
		// AABB intentionally remains empty. Build its bounds from the vertices.
		const GodotFaceShape3D *face = static_cast<const GodotFaceShape3D *>(p_convex_shape);
		const Vector3 first_vertex = p_convex_transform.xform(face->get_vertex(0));
		convex_world_bounds = AABB(first_vertex, Vector3());
		convex_world_bounds.expand_to(p_convex_transform.xform(face->get_vertex(1)));
		convex_world_bounds.expand_to(p_convex_transform.xform(face->get_vertex(2)));
	} else {
		convex_world_bounds = p_convex_transform.xform(p_convex_shape->get_aabb());
	}
	convex_world_bounds = convex_world_bounds.grow(p_voxel_margin + p_convex_margin);
	Vector3i lo;
	Vector3i hi;
	if (!_voxel_range_from_world_aabb(data, p_voxel_transform, convex_world_bounds, lo, hi)) {
		return false;
	}

	GodotBoxShape3D voxel_box;
	const real_t voxel_size = data->get_voxel_size();
	voxel_box.set_data(Vector3(1, 1, 1) * voxel_size * 0.5);
	const Vector3i dimensions = data->get_dimensions();
	LocalVector<VoxelContactCandidate> candidates;
	const uint32_t candidate_limit = MAX_VOXEL_CONTACT_CANDIDATES_PER_PASS;
	bool collided = false;
	bool tested = false;
	auto test_indices = [&](const GodotVoxelShape3D::IndexRange &p_indices) {
		for (const int voxel_index : p_indices) {
			tested = true;
			const Vector3i voxel = _index_to_voxel(voxel_index, dimensions);
			VoxelCallbackContext context;
			context.candidates = &candidates;
			// Keep the solid voxel as SAT shape A. Face shapes produced by a
			// concave mesh use their winding when generating contacts, matching
			// Godot's normal convex-versus-concave argument order.
			context.swap = !p_voxel_is_a;
			context.feature_index = voxel_index;
			context.candidate_limit = candidate_limit;
			context.stop_at_limit = false;
			context.other_index = -1;
			sat_calculate_penetration(
					&voxel_box, _solid_voxel_transform(p_voxel_transform, voxel, voxel_size),
					p_convex_shape, p_convex_transform,
					_voxel_contact_callback, &context, false, nullptr,
					p_voxel_margin, p_convex_margin);
			collided |= context.collided;
			if (context.limit_reached) {
				return true;
			}
		}
		return false;
	};
	if (test_indices(p_voxel_shape->get_surface_indices(lo, hi)) ||
			(!tested && test_indices(p_voxel_shape->get_solid_indices(lo, hi)))) {
		_emit_manifold(candidates, p_result_callback, p_userdata);
		return true;
	}
	_emit_manifold(candidates, p_result_callback, p_userdata);
	return collided;
}

bool GodotVoxelCollisionSolver3D::solve_voxel_concave(
		const GodotVoxelShape3D *p_voxel_shape,
		const Transform3D &p_voxel_transform,
		const GodotConcaveShape3D *p_concave_shape,
		const Transform3D &p_concave_transform,
		bool p_voxel_is_a,
		GodotCollisionSolver3D::CallbackResult p_result_callback,
		void *p_userdata,
		real_t p_voxel_margin,
		real_t p_concave_margin) {
	ERR_FAIL_NULL_V(p_voxel_shape, false);
	ERR_FAIL_NULL_V(p_concave_shape, false);
	if (p_voxel_shape->get_voxel_data().is_null() || p_voxel_shape->get_solid_indices().is_empty()) {
		return false;
	}

	VoxelConcaveStaticContext context;
	context.voxel_shape = p_voxel_shape;
	context.voxel_transform = &p_voxel_transform;
	context.concave_transform = &p_concave_transform;
	context.result_callback = p_result_callback;
	context.userdata = p_userdata;
	context.voxel_is_a = p_voxel_is_a;
	context.voxel_margin = p_voxel_margin;
	context.concave_margin = p_concave_margin;

	AABB world_bounds = p_voxel_transform.xform(p_voxel_shape->get_aabb());
	world_bounds = world_bounds.grow(p_voxel_margin + p_concave_margin);
	const AABB local_bounds = p_concave_transform.affine_inverse().xform(world_bounds);
	p_concave_shape->cull(local_bounds, _voxel_concave_static_callback, &context, false);
	return context.collided;
}

bool GodotVoxelCollisionSolver3D::solve_distance(
		const GodotShape3D *p_shape_a,
		const Transform3D &p_transform_a,
		const GodotShape3D *p_shape_b,
		const Transform3D &p_transform_b,
		Vector3 &r_point_a,
		Vector3 &r_point_b,
		const AABB &p_swept_hint) {
	const bool voxel_a = p_shape_a->get_type() == PhysicsServer3D::SHAPE_CUSTOM;
	const bool voxel_b = p_shape_b->get_type() == PhysicsServer3D::SHAPE_CUSTOM;
	const GodotMotionShape3D *motion_a = p_shape_a->is_motion_shape() ? static_cast<const GodotMotionShape3D *>(p_shape_a) : nullptr;
	const GodotMotionShape3D *motion_b = p_shape_b->is_motion_shape() ? static_cast<const GodotMotionShape3D *>(p_shape_b) : nullptr;
	const bool motion_voxel_a = motion_a != nullptr && motion_a->shape != nullptr && motion_a->shape->get_type() == PhysicsServer3D::SHAPE_CUSTOM;
	const bool motion_voxel_b = motion_b != nullptr && motion_b->shape != nullptr && motion_b->shape->get_type() == PhysicsServer3D::SHAPE_CUSTOM;
	ERR_FAIL_COND_V(!voxel_a && !voxel_b && !motion_voxel_a && !motion_voxel_b, true);
	if ((voxel_a || motion_voxel_a) && p_shape_b->is_concave()) {
		return _distance_voxel_query_to_concave(
				p_shape_a, p_transform_a,
				static_cast<const GodotConcaveShape3D *>(p_shape_b), p_transform_b,
				r_point_a, r_point_b, p_swept_hint);
	}
	if (p_shape_a->is_concave() && (voxel_b || motion_voxel_b)) {
		return _distance_voxel_query_to_concave(
				p_shape_b, p_transform_b,
				static_cast<const GodotConcaveShape3D *>(p_shape_a), p_transform_a,
				r_point_b, r_point_a, p_swept_hint);
	}
	if (motion_voxel_a && voxel_b) {
		return _distance_motion_voxel_to_voxel(
				motion_a, p_transform_a,
				static_cast<const GodotVoxelShape3D *>(p_shape_b), p_transform_b,
				r_point_a, r_point_b, p_swept_hint);
	}
	if (voxel_a && motion_voxel_b) {
		return _distance_motion_voxel_to_voxel(
				motion_b, p_transform_b,
				static_cast<const GodotVoxelShape3D *>(p_shape_a), p_transform_a,
				r_point_b, r_point_a, p_swept_hint);
	}
	if (motion_voxel_a) {
		return _distance_motion_voxel_to_convex(
				motion_a, p_transform_a, p_shape_b, p_transform_b,
				r_point_a, r_point_b);
	}
	if (motion_voxel_b) {
		return _distance_motion_voxel_to_convex(
				motion_b, p_transform_b, p_shape_a, p_transform_a,
				r_point_b, r_point_a);
	}
	if (voxel_a && voxel_b) {
		return _distance_voxel_to_voxel(
				static_cast<const GodotVoxelShape3D *>(p_shape_a), p_transform_a,
				static_cast<const GodotVoxelShape3D *>(p_shape_b), p_transform_b,
				r_point_a, r_point_b, p_swept_hint);
	}
	if (voxel_b) {
		return _distance_convex_to_voxel(
				p_shape_a, p_transform_a,
				static_cast<const GodotVoxelShape3D *>(p_shape_b), p_transform_b,
				r_point_a, r_point_b, p_swept_hint);
	}
	const bool separated = _distance_convex_to_voxel(
			p_shape_b, p_transform_b,
			static_cast<const GodotVoxelShape3D *>(p_shape_a), p_transform_a,
			r_point_b, r_point_a, p_swept_hint);
	return separated;
}

bool GodotVoxelCollisionSolver3D::solve_voxel_voxel(
		const GodotVoxelShape3D *p_shape_a,
		const Transform3D &p_transform_a,
		const GodotVoxelShape3D *p_shape_b,
		const Transform3D &p_transform_b,
		GodotCollisionSolver3D::CallbackResult p_result_callback,
		void *p_userdata,
		real_t p_margin_a,
		real_t p_margin_b) {
	HashSet<uint64_t> seen;
	LocalVector<VoxelContactCandidate> candidates;
	bool collided = false;

	// Exact edge geometry gets first ownership of its voxel pair. The volume
	// passes then provide corner and edge coverage without duplicating it.
	collided |= _test_edges_against_edges(
			p_shape_a, p_transform_a, p_shape_b, p_transform_b,
			p_margin_a, p_margin_b, seen, candidates);
	collided |= _test_features_against_solids(
			VoxelFeatureType::CORNER,
			p_shape_a, p_transform_a, p_shape_b, p_transform_b, false,
			p_margin_a, p_margin_b, seen, candidates);
	collided |= _test_features_against_solids(
			VoxelFeatureType::CORNER,
			p_shape_b, p_transform_b, p_shape_a, p_transform_a, true,
			p_margin_b, p_margin_a, seen, candidates);

	_emit_manifold(candidates, p_result_callback, p_userdata);
	return collided;
}
