#pragma once

#include "godot_collision_solver_3d.h"

class GodotVoxelShape3D;

class GodotVoxelCollisionSolver3D {
public:
	static bool solve_voxel_convex(
			const GodotVoxelShape3D *p_voxel_shape,
			const Transform3D &p_voxel_transform,
			const GodotShape3D *p_convex_shape,
			const Transform3D &p_convex_transform,
			bool p_voxel_is_a,
			GodotCollisionSolver3D::CallbackResult p_result_callback,
			void *p_userdata,
			real_t p_voxel_margin = 0.0,
			real_t p_convex_margin = 0.0);

	static bool solve_voxel_concave(
			const GodotVoxelShape3D *p_voxel_shape,
			const Transform3D &p_voxel_transform,
			const GodotConcaveShape3D *p_concave_shape,
			const Transform3D &p_concave_transform,
			bool p_voxel_is_a,
			GodotCollisionSolver3D::CallbackResult p_result_callback,
			void *p_userdata,
			real_t p_voxel_margin = 0.0,
			real_t p_concave_margin = 0.0);

	// Uses occupied voxel cubes instead of the volume AABB. The return value
	// follows GodotCollisionSolver3D::solve_distance(): true means separated,
	// false means overlapping.
	static bool solve_distance(
			const GodotShape3D *p_shape_a,
			const Transform3D &p_transform_a,
			const GodotShape3D *p_shape_b,
			const Transform3D &p_transform_b,
			Vector3 &r_point_a,
			Vector3 &r_point_b,
			const AABB &p_swept_hint = AABB());

	static bool solve_voxel_voxel(
			const GodotVoxelShape3D *p_shape_a,
			const Transform3D &p_transform_a,
			const GodotVoxelShape3D *p_shape_b,
			const Transform3D &p_transform_b,
			GodotCollisionSolver3D::CallbackResult p_result_callback,
			void *p_userdata,
			real_t p_margin_a = 0.0,
			real_t p_margin_b = 0.0);
};
