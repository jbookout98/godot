#pragma once

#include "../godot_voxel_collision_solver_3d.h"
#include "../godot_voxel_shape_3d.h"

#include "tests/test_macros.h"

namespace TestVoxelMotion {

Ref<VoxelShapeData> make_wall_data(bool p_with_floor = false, real_t p_voxel_size = 1.0, int p_depth = 8) {
	const Vector3i dimensions(4, 6, p_depth);
	PackedByteArray dense;
	dense.resize(dimensions.x * dimensions.y * dimensions.z);
	dense.fill(0);
	for (int z = 0; z < dimensions.z; z++) {
		for (int y = 0; y < dimensions.y; y++) {
			dense.set(2 + y * dimensions.x + z * dimensions.x * dimensions.y, 1);
		}
		if (p_with_floor) {
			for (int x = 0; x < dimensions.x; x++) {
				dense.set(x + z * dimensions.x * dimensions.y, 1);
			}
		}
	}

	Ref<VoxelShapeData> data;
	data.instantiate();
	data->set_dimensions(dimensions);
	data->set_voxel_size(p_voxel_size);
	data->set_voxel_data(dense);
	return data;
}

Ref<VoxelShapeData> make_ceiling_data() {
	const Vector3i dimensions(4, 5, 4);
	PackedByteArray dense;
	dense.resize(dimensions.x * dimensions.y * dimensions.z);
	dense.fill(0);
	for (int z = 0; z < dimensions.z; z++) {
		for (int x = 0; x < dimensions.x; x++) {
			dense.set(x + 3 * dimensions.x + z * dimensions.x * dimensions.y, 1);
		}
	}
	Ref<VoxelShapeData> data;
	data.instantiate();
	data->set_dimensions(dimensions);
	data->set_voxel_size(1.0);
	data->set_voxel_data(dense);
	return data;
}

Ref<VoxelShapeData> make_corner_data(bool p_column_only = false) {
	const Vector3i dimensions(4, 6, 4);
	PackedByteArray dense;
	dense.resize(dimensions.x * dimensions.y * dimensions.z);
	dense.fill(0);
	for (int y = 0; y < dimensions.y; y++) {
		if (p_column_only) {
			dense.set(2 + y * dimensions.x + 2 * dimensions.x * dimensions.y, 1);
			continue;
		}
		for (int z = 0; z < dimensions.z; z++) {
			dense.set(2 + y * dimensions.x + z * dimensions.x * dimensions.y, 1);
		}
		for (int x = 0; x < dimensions.x; x++) {
			dense.set(x + y * dimensions.x + 2 * dimensions.x * dimensions.y, 1);
		}
	}
	Ref<VoxelShapeData> data;
	data.instantiate();
	data->set_dimensions(dimensions);
	data->set_voxel_size(1.0);
	data->set_voxel_data(dense);
	return data;
}

void configure_cylinder(GodotCylinderShape3D &r_cylinder, real_t p_radius = 0.5, real_t p_height = 2.0) {
	Dictionary data;
	data["radius"] = p_radius;
	data["height"] = p_height;
	r_cylinder.set_data(data);
}

bool cast_cylinder(
		GodotCylinderShape3D &p_cylinder,
		GodotVoxelShape3D &p_voxel_shape,
		const Vector3 &p_origin,
		const Vector3 &p_motion,
		const Transform3D &p_voxel_transform = Transform3D()) {
	GodotMotionShape3D motion_shape;
	motion_shape.shape = &p_cylinder;
	motion_shape.motion = p_motion;
	Vector3 point_cylinder;
	Vector3 point_voxel;
	return GodotVoxelCollisionSolver3D::solve_distance(
			&motion_shape, Transform3D(Basis(), p_origin),
			&p_voxel_shape, p_voxel_transform,
			point_cylinder, point_voxel);
}

struct ContactDirections {
	bool floor = false;
	bool wall = false;
	real_t deepest = 0.0;
};

void collect_contact_direction(
		const Vector3 &p_point_A,
		int,
		const Vector3 &p_point_B,
		int,
		const Vector3 &p_normal,
		void *p_userdata) {
	ContactDirections *directions = static_cast<ContactDirections *>(p_userdata);
	directions->floor |= p_normal.dot(Vector3(0.0, 1.0, 0.0)) > 0.9;
	directions->wall |= p_normal.dot(Vector3(-1.0, 0.0, 0.0)) > 0.9;
	directions->deepest = MAX(directions->deepest, p_point_A.distance_to(p_point_B));
}

TEST_CASE("[VoxelPhysics3D] Initial voxel overlap respects motion direction") {
	GodotVoxelShape3D voxel_shape;
	voxel_shape.set_data(make_wall_data());
	GodotCylinderShape3D cylinder;
	configure_cylinder(cylinder);
	const Vector3 penetrating_origin(1.51, 2.0, 1.5);

	CHECK(cast_cylinder(cylinder, voxel_shape, penetrating_origin, Vector3(-0.2, 0.0, 0.0)));
	CHECK(cast_cylinder(cylinder, voxel_shape, penetrating_origin, Vector3(0.0, 0.0, 0.2)));
	CHECK_FALSE(cast_cylinder(cylinder, voxel_shape, penetrating_origin, Vector3(0.2, 0.0, 0.0)));
	CHECK_FALSE(cast_cylinder(cylinder, voxel_shape, penetrating_origin, Vector3(0.05, 0.0, 0.2)));
}

TEST_CASE("[VoxelPhysics3D] Tangent floor contact does not block wall sliding") {
	GodotVoxelShape3D voxel_shape;
	voxel_shape.set_data(make_wall_data(true));
	GodotCylinderShape3D cylinder;
	configure_cylinder(cylinder);

	CHECK(cast_cylinder(cylinder, voxel_shape, Vector3(1.51, 2.0, 1.5), Vector3(-0.2, 0.0, 0.0)));
	CHECK(cast_cylinder(cylinder, voxel_shape, Vector3(1.51, 2.0, 1.5), Vector3(0.0, 0.0, 0.2)));
	CHECK_FALSE(cast_cylinder(cylinder, voxel_shape, Vector3(1.51, 2.0, 1.5), Vector3(0.05, 0.0, 0.2)));
}

TEST_CASE("[VoxelPhysics3D] Voxel sweep still blocks a future collision") {
	GodotVoxelShape3D voxel_shape;
	voxel_shape.set_data(make_wall_data());
	GodotCylinderShape3D cylinder;
	configure_cylinder(cylinder);

	CHECK_FALSE(cast_cylinder(cylinder, voxel_shape, Vector3(1.4, 2.0, 1.5), Vector3(0.2, 0.0, 0.0)));
}

TEST_CASE("[VoxelPhysics3D] Floor contacts do not hide a simultaneous wall contact") {
	GodotVoxelShape3D voxel_shape;
	voxel_shape.set_data(make_wall_data(true));
	GodotCylinderShape3D cylinder;
	configure_cylinder(cylinder);
	ContactDirections directions;

	CHECK(GodotVoxelCollisionSolver3D::solve_voxel_convex(
			&voxel_shape, Transform3D(),
			&cylinder, Transform3D(Basis(), Vector3(1.51, 2.00094, 2.035)),
			false, collect_contact_direction, &directions, 0.0, 0.001));
	CHECK(directions.floor);
	CHECK(directions.wall);
}

TEST_CASE("[VoxelPhysics3D] Cylinder motion beneath a voxel ceiling respects direction") {
	GodotVoxelShape3D voxel_shape;
	voxel_shape.set_data(make_ceiling_data());
	GodotCylinderShape3D cylinder;
	configure_cylinder(cylinder);
	const Vector3 touching_origin(1.5, 2.0, 1.5);
	ContactDirections end_contacts;
	CHECK(GodotVoxelCollisionSolver3D::solve_voxel_convex(
			&voxel_shape, Transform3D(),
			&cylinder, Transform3D(Basis(), touching_origin + Vector3(0.0, 0.2, 0.0)),
			false, collect_contact_direction, &end_contacts, 0.0, 0.0));

	CHECK_FALSE(cast_cylinder(cylinder, voxel_shape, touching_origin, Vector3(0.0, 0.2, 0.0)));
	CHECK(cast_cylinder(cylinder, voxel_shape, touching_origin, Vector3(0.0, -0.2, 0.0)));
	CHECK(cast_cylinder(cylinder, voxel_shape, touching_origin, Vector3(0.2, 0.0, 0.0)));
}

TEST_CASE("[VoxelPhysics3D] Cylinder motion at an inside voxel corner remains stable") {
	GodotVoxelShape3D voxel_shape;
	voxel_shape.set_data(make_corner_data());
	GodotCylinderShape3D cylinder;
	configure_cylinder(cylinder);
	const Vector3 corner_origin(1.51, 2.0, 1.51);

	CHECK(cast_cylinder(cylinder, voxel_shape, corner_origin, Vector3(-0.2, 0.0, -0.2)));
	CHECK(cast_cylinder(cylinder, voxel_shape, corner_origin, Vector3(0.0, 0.2, 0.0)));
	CHECK_FALSE(cast_cylinder(cylinder, voxel_shape, corner_origin, Vector3(0.2, 0.0, 0.0)));
}

TEST_CASE("[VoxelPhysics3D] Cylinder motion around an outside voxel corner respects direction") {
	GodotVoxelShape3D voxel_shape;
	voxel_shape.set_data(make_corner_data(true));
	GodotCylinderShape3D cylinder;
	configure_cylinder(cylinder);
	const real_t touching_coordinate = 2.0 - Math::SQRT12 * 0.5;
	const Vector3 corner_origin(touching_coordinate, 2.0, touching_coordinate);

	CHECK(cast_cylinder(cylinder, voxel_shape, corner_origin, Vector3(-0.1, 0.0, -0.1)));
	CHECK(cast_cylinder(cylinder, voxel_shape, corner_origin, Vector3(-0.1, 0.0, 0.1)));
	CHECK_FALSE(cast_cylinder(cylinder, voxel_shape, corner_origin, Vector3(0.1, 0.0, 0.1)));
}

TEST_CASE("[VoxelPhysics3D] Tangent motion crosses aligned voxel volume boundaries") {
	GodotVoxelShape3D first_volume;
	GodotVoxelShape3D second_volume;
	first_volume.set_data(make_wall_data(false, 1.0, 2));
	second_volume.set_data(make_wall_data(false, 1.0, 2));
	GodotCylinderShape3D cylinder;
	configure_cylinder(cylinder);
	const Vector3 boundary_origin(1.499, 2.0, 1.95);
	const Vector3 tangent_motion(0.0, 0.0, 0.2);

	CHECK(cast_cylinder(cylinder, first_volume, boundary_origin, tangent_motion));
	CHECK(cast_cylinder(cylinder, second_volume, boundary_origin, tangent_motion, Transform3D(Basis(), Vector3(0.0, 0.0, 2.0))));
}

TEST_CASE("[VoxelPhysics3D] Motion classification scales with voxel and cylinder size") {
	GodotVoxelShape3D voxel_shape;
	voxel_shape.set_data(make_wall_data(false, 0.25));
	GodotCylinderShape3D cylinder;
	configure_cylinder(cylinder, 0.125, 0.5);
	const Vector3 penetrating_origin(0.377, 0.5, 0.375);

	CHECK(cast_cylinder(cylinder, voxel_shape, penetrating_origin, Vector3(-0.05, 0.0, 0.0)));
	CHECK(cast_cylinder(cylinder, voxel_shape, penetrating_origin, Vector3(0.0, 0.0, 0.05)));
	CHECK_FALSE(cast_cylinder(cylinder, voxel_shape, penetrating_origin, Vector3(0.05, 0.0, 0.0)));
}

TEST_CASE("[VoxelPhysics3D] Deep wall overlap retains bounded outward contact") {
	GodotVoxelShape3D voxel_shape;
	voxel_shape.set_data(make_wall_data());
	GodotCylinderShape3D cylinder;
	configure_cylinder(cylinder);
	ContactDirections directions;

	CHECK(GodotVoxelCollisionSolver3D::solve_voxel_convex(
			&voxel_shape, Transform3D(),
			&cylinder, Transform3D(Basis(), Vector3(1.75, 2.0, 1.5)),
			false, collect_contact_direction, &directions, 0.0, 0.0));
	CHECK(directions.wall);
	CHECK(directions.deepest > 0.0);
	CHECK(directions.deepest <= 0.253);
}

TEST_CASE("[VoxelPhysics3D] Repeated tangent and inward casts are deterministic") {
	GodotVoxelShape3D voxel_shape;
	voxel_shape.set_data(make_wall_data(true));
	GodotCylinderShape3D cylinder;
	configure_cylinder(cylinder);
	const Vector3 origin(1.51, 2.0, 1.5);

	for (int iteration = 0; iteration < 1000; iteration++) {
		CHECK(cast_cylinder(cylinder, voxel_shape, origin, Vector3(0.0, 0.0, 0.2)));
		CHECK_FALSE(cast_cylinder(cylinder, voxel_shape, origin, Vector3(0.05, 0.0, 0.2)));
	}
}

} // namespace TestVoxelMotion
