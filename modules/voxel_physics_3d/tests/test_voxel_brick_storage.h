#pragma once

#include "../voxel_brick_storage.h"

#include "tests/test_macros.h"

namespace TestVoxelBrickStorage {

TEST_CASE("[VoxelPhysics3D] Mixed brick pool preserves exact voxel values") {
	VoxelBrickStorage storage;
	storage.reset(Vector3i(24, 8, 8));

	CHECK(storage.set_voxel(Vector3i(1, 1, 1), 3));
	CHECK(storage.set_voxel(Vector3i(9, 2, 2), 5));
	CHECK(storage.set_voxel(Vector3i(17, 3, 3), 7));
	CHECK(storage.get_occupied_voxel_count() == 3);

	// Releasing the first slot swap-moves the last slot. Values in both remaining
	// bricks must still be addressed by their brick index, not their old slot.
	CHECK(storage.set_voxel(Vector3i(1, 1, 1), 0));
	CHECK(storage.get_voxel(Vector3i(9, 2, 2)) == 5);
	CHECK(storage.get_voxel(Vector3i(17, 3, 3)) == 7);
	CHECK(storage.get_occupied_voxel_count() == 2);

	CHECK(storage.fill_region(Vector3i(8, 0, 0), Vector3i(8, 8, 8), 9) == 512);
	CHECK(storage.get_brick(1).type == VoxelBrickStorage::BRICK_UNIFORM);
	CHECK(storage.get_voxel(Vector3i(9, 2, 2)) == 9);
	CHECK(storage.get_voxel(Vector3i(17, 3, 3)) == 7);
	CHECK(storage.get_occupied_voxel_count() == 513);
}

TEST_CASE("[VoxelPhysics3D] Dense and sparse storage round trips are exact") {
	const Vector3i dimensions(17, 10, 9);
	PackedByteArray dense;
	dense.resize(dimensions.x * dimensions.y * dimensions.z);
	for (int z = 0; z < dimensions.z; z++) {
		for (int y = 0; y < dimensions.y; y++) {
			for (int x = 0; x < dimensions.x; x++) {
				const int index = x + y * dimensions.x + z * dimensions.x * dimensions.y;
				dense.set(index, ((x * 17 + y * 5 + z * 3) % 11) == 0 ? uint8_t((x + y + z) % 254 + 1) : 0);
			}
		}
	}

	VoxelBrickStorage storage;
	storage.import_dense(dense, dimensions);
	CHECK(storage.export_dense() == dense);

	uint64_t expected_occupied = 0;
	for (uint8_t value : dense) {
		expected_occupied += value != 0 ? 1 : 0;
	}
	CHECK(storage.get_occupied_voxel_count() == expected_occupied);

	VoxelBrickStorage restored;
	CHECK(restored.deserialize_sparse(storage.serialize_sparse(), dimensions));
	CHECK(restored.export_dense() == dense);
	CHECK(restored.get_occupied_voxel_count() == expected_occupied);
}

} // namespace TestVoxelBrickStorage
