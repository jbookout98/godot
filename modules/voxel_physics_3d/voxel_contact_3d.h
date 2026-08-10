#pragma once

#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/typedefs.h"

enum class VoxelFeatureType : uint8_t {
	NONE,
	FACE,
	EDGE,
	CORNER,
};

struct VoxelContact3D {
	Vector3 point_a;
	Vector3 point_b;
	Vector3 normal;
	real_t penetration = 0.0;

	Vector3i voxel_a;
	Vector3i voxel_b;
	VoxelFeatureType feature_a = VoxelFeatureType::NONE;
	VoxelFeatureType feature_b = VoxelFeatureType::NONE;
	uint8_t mask_a = 0;
	uint8_t mask_b = 0;
};
