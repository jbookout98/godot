#pragma once

#include "core/templates/vector.h"
#include "core/variant/variant.h"

class VoxelBrickStorage {
public:
	static constexpr int BRICK_SIZE = 8;
	static constexpr int BRICK_VOXEL_COUNT = BRICK_SIZE * BRICK_SIZE * BRICK_SIZE;

	enum BrickType : uint8_t {
		BRICK_EMPTY = 0,
		BRICK_UNIFORM = 1,
		BRICK_MIXED = 2,
	};

	enum DirtyFlag : uint8_t {
		DIRTY_NONE = 0,
		DIRTY_RENDER = 1 << 0,
		DIRTY_PHYSICS = 1 << 1,
		DIRTY_SAVE = 1 << 2,
		DIRTY_ALL = DIRTY_RENDER | DIRTY_PHYSICS | DIRTY_SAVE,
	};

	struct Brick {
		BrickType type = BRICK_EMPTY;
		uint8_t uniform_value = 0;
		uint32_t mixed_slot = UINT32_MAX;
		int occupied_count = 0;
		// Occupancy of the eight 4x4x4 subcells in this 8x8x8 brick.
		// Derived from voxel data and never serialized.
		uint8_t coarse_occupancy_4 = 0;
		uint8_t dirty_flags = DIRTY_NONE;
		uint64_t revision = 0;
	};

private:
	Vector3i dimensions = Vector3i(1, 1, 1);
	Vector3i brick_dimensions = Vector3i(1, 1, 1);
	Vector<Brick> bricks;
	PackedByteArray mixed_value_pool;
	Vector<int> mixed_slot_owners;
	uint64_t occupied_voxel_count = 0;
	uint64_t revision = 0;

	int _get_brick_index(const Vector3i &p_brick) const;
	int _get_local_index(const Vector3i &p_position) const;
	int _get_valid_voxel_count(const Vector3i &p_brick) const;
	uint8_t *_allocate_mixed_values(int p_brick_index, uint8_t p_fill_value);
	void _release_mixed_values(int p_brick_index);
	void _normalize_brick(int p_brick_index);
	void _rebuild_coarse_occupancy_4(int p_brick_index);

public:
	void reset(const Vector3i &p_dimensions);
	bool is_inside(const Vector3i &p_position) const;
	uint8_t get_voxel(const Vector3i &p_position) const;
	bool set_voxel(const Vector3i &p_position, uint8_t p_value);
	int fill_region(const Vector3i &p_position, const Vector3i &p_size, uint8_t p_value);

	void import_dense(const PackedByteArray &p_voxels, const Vector3i &p_dimensions);
	PackedByteArray export_dense() const;
	PackedByteArray serialize_sparse() const;
	bool deserialize_sparse(const PackedByteArray &p_data, const Vector3i &p_dimensions);

	Vector3i get_dimensions() const { return dimensions; }
	Vector3i get_brick_dimensions() const { return brick_dimensions; }
	int get_brick_count() const { return bricks.size(); }
	uint64_t get_occupied_voxel_count() const { return occupied_voxel_count; }
	const Brick &get_brick(int p_index) const { return bricks[p_index]; }
	Brick &get_brick_write(int p_index) { return bricks.write[p_index]; }
	const uint8_t *get_brick_mixed_values(int p_index) const;
	uint64_t get_revision() const { return revision; }

	Vector3i brick_index_to_position(int p_index) const;
	int position_to_brick_index(const Vector3i &p_position) const;
	void get_non_empty_brick_indices(Vector<int> &r_indices) const;
	void clear_dirty_flags(uint8_t p_flags = DIRTY_ALL);
};
