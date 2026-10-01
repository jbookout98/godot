/**************************************************************************/
/*  test_voxel_occupancy.h                                                */
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

#pragma once

#include "../voxel_shape_data.h"

#include "tests/test_macros.h"

namespace TestVoxelOccupancy {

static bool occupied(const VoxelShapeData::OccupancyData &p_data, const Vector3i &p_brick_dimensions, const Vector3i &p_position) {
	const Vector3i brick = p_position / VoxelBrickStorage::BRICK_SIZE;
	const int offset = (brick.x + brick.y * p_brick_dimensions.x + brick.z * p_brick_dimensions.x * p_brick_dimensions.y) * 4;
	const uint32_t code = uint32_t(p_data.directory[offset]) | (uint32_t(p_data.directory[offset + 1]) << 8) |
			(uint32_t(p_data.directory[offset + 2]) << 16) | (uint32_t(p_data.directory[offset + 3]) << 24);
	if (code < 2) {
		return code == 1;
	}
	const Vector3i local = p_position - brick * VoxelBrickStorage::BRICK_SIZE;
	const int index = local.x + local.y * 8 + local.z * 64;
	return (p_data.bricks[(code - 2) * 64 + (index >> 3)] & (1u << (index & 7))) != 0;
}

TEST_CASE("[VoxelShapeData] Packed occupancy matches mixed, uniform and edge bricks") {
	Ref<VoxelShapeData> data;
	data.instantiate();
	data->set_dimensions(Vector3i(19, 10, 9));
	data->fill_voxel_region(Vector3i(), Vector3i(8, 8, 8), 7);
	data->set_voxel(Vector3i(8, 0, 0), 2);
	data->set_voxel(Vector3i(15, 7, 7), 3);
	data->set_voxel(Vector3i(18, 9, 8), 4);
	const VoxelShapeData::OccupancyData snapshot = data->get_occupancy_data();
	CHECK(snapshot.occupied_brick_count == 3);
	CHECK(snapshot.bricks.size() == 128);
	for (int z = 0; z < 9; z++) {
		for (int y = 0; y < 10; y++) {
			for (int x = 0; x < 19; x++) {
				const Vector3i cell(x, y, z);
				CHECK(occupied(snapshot, data->get_brick_storage().get_brick_dimensions(), cell) == data->is_solid(cell));
			}
		}
	}
}

TEST_CASE("[VoxelShapeData] Occupancy cache reuses buffers and preserves queued snapshots") {
	Ref<VoxelShapeData> data;
	data.instantiate();
	data->set_dimensions(Vector3i(8, 8, 8));
	data->set_voxel(Vector3i(1, 2, 3), 1);
	const VoxelShapeData::OccupancyData before = data->get_occupancy_data();
	Ref<VoxelShapeData> shared = data;
	CHECK(shared->get_occupancy_data().directory.ptr() == before.directory.ptr());
	CHECK(shared->get_occupancy_data().bricks.ptr() == before.bricks.ptr());
	data->set_voxel_size(0.25);
	CHECK(data->get_occupancy_data().bricks.ptr() == before.bricks.ptr());
	data->begin_edit();
	data->set_voxel(Vector3i(1, 2, 3), 0);
	data->set_voxel(Vector3i(7, 7, 7), 9);
	const VoxelShapeData::OccupancyData during = data->get_occupancy_data();
	CHECK(occupied(before, Vector3i(1, 1, 1), Vector3i(1, 2, 3)));
	CHECK_FALSE(occupied(before, Vector3i(1, 1, 1), Vector3i(7, 7, 7)));
	CHECK_FALSE(occupied(during, Vector3i(1, 1, 1), Vector3i(1, 2, 3)));
	CHECK(occupied(during, Vector3i(1, 1, 1), Vector3i(7, 7, 7)));
	data->end_edit();
	CHECK(data->get_occupancy_data().bricks.ptr() == during.bricks.ptr());
	data->fill_voxel_region(Vector3i(), Vector3i(8, 8, 8), 0);
	CHECK(data->get_occupancy_data().occupied_brick_count == 0);
	CHECK(data->get_occupancy_data().bricks.is_empty());
}

TEST_CASE("[VoxelShapeData] Loading and resizing invalidate occupancy independently of other resources") {
	Ref<VoxelShapeData> first;
	first.instantiate();
	first->set_dimensions(Vector3i(8, 8, 8));
	first->set_voxel(Vector3i(2, 3, 4), 1);
	const VoxelShapeData::OccupancyData before = first->get_occupancy_data();
	Ref<VoxelShapeData> second;
	second.instantiate();
	second->set_dimensions(Vector3i(8, 8, 8));
	second->set_voxel(Vector3i(5, 6, 7), 1);
	CHECK(first->get_revision() == second->get_revision());
	CHECK_FALSE(occupied(second->get_occupancy_data(), Vector3i(1, 1, 1), Vector3i(2, 3, 4)));
	first->set_sparse_brick_data(second->get_sparse_brick_data());
	CHECK_FALSE(occupied(first->get_occupancy_data(), Vector3i(1, 1, 1), Vector3i(2, 3, 4)));
	CHECK(occupied(first->get_occupancy_data(), Vector3i(1, 1, 1), Vector3i(5, 6, 7)));
	CHECK(occupied(before, Vector3i(1, 1, 1), Vector3i(2, 3, 4)));
	first->set_dimensions(Vector3i(16, 8, 8));
	CHECK(first->get_occupancy_data().directory.size() == 8);
	CHECK(first->get_occupancy_data().occupied_brick_count == 0);
	PackedByteArray dense;
	dense.resize(16 * 8 * 8);
	dense.fill(1);
	first->set_voxel_data(dense);
	CHECK(first->get_occupancy_data().occupied_brick_count == 2);
	CHECK(first->get_occupancy_data().bricks.is_empty());
}

} // namespace TestVoxelOccupancy
